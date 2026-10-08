// Fake-image HLS segment repair proxy.
//
// Port of AnikkuNX's switch/src/net/hls_proxy.cpp (hardware-verified there with
// the same libmpv 0.36.0 / FFmpeg 7.1 packages this project uses), adapted to
// SaikouTVNX:
//   - headers are the app's std::vector<std::string> "Name: Value" style,
//   - HTTP uses the app's curl settings,
//   - base64url comes from kaa_crypto,
//   - diagnostics go to saikou_debug.log and sdmc:/switch/SaikouTV/proxy.log.
//
// The proxy is strictly opt-in at the call site: nothing here changes any
// stream unless needsProxy() proved the stream serves disguised segments.

#include "hls_proxy.hpp"

#include <curl/curl.h>
#include "kaa_crypto.hpp"
#include <switch.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern void saikou_debug_log(const char* stage);

namespace hlsproxy {

namespace {

constexpr const char* kVideoUA =
    "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/129.0.0.0 Mobile Safari/537.36";

std::mutex mtx;
int serverPort = 0;
bool serverFailed = false;
int nextSession = 1;
std::map<int, std::vector<std::string>> sessions; // sid -> provider headers
std::atomic<int> activeClients{0};
std::string logPath;
std::mutex logMutex;

void proxyLog(const std::string& msg)
{
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (!logPath.empty())
        {
            FILE* f = std::fopen(logPath.c_str(), "a");
            if (f)
            {
                std::fprintf(f, "%s\n", msg.c_str());
                std::fclose(f);
            }
        }
    }
    const std::string marker = "PROXY " + msg;
    saikou_debug_log(marker.c_str());
}

bool startsWith(const std::string& s, const char* p)
{
    return s.compare(0, std::strlen(p), p) == 0;
}

std::string lowerCopy(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Resolve a (possibly relative) playlist URI against the playlist URL.
std::string resolve(const std::string& base, const std::string& rel)
{
    if (rel.empty()) return base;
    if (startsWith(rel.c_str(), "http://") || startsWith(rel.c_str(), "https://")) return rel;
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) return rel;
    if (rel.rfind("//", 0) == 0) return base.substr(0, scheme + 1) + rel;
    const size_t hostEnd = base.find('/', scheme + 3);
    const std::string origin = base.substr(0, hostEnd == std::string::npos ? base.size() : hostEnd);
    if (rel[0] == '/') return origin + rel;
    std::string dir = base;
    const size_t q = dir.find('?');
    if (q != std::string::npos) dir.resize(q);
    const size_t hash = dir.find('#');
    if (hash != std::string::npos) dir.resize(hash);
    const size_t slash = dir.rfind('/');
    if (slash != std::string::npos) dir.resize(slash + 1);
    return dir + rel;
}

// ------------------------------------------------------------- curl helpers

struct Fetch
{
    long code = 0;
    std::string body;
    std::string finalUrl;
};

static size_t fetchWriteCb(char* p, size_t size, size_t count, void* userdata)
{
    std::string* out = static_cast<std::string*>(userdata);
    const size_t n = size * count;
    // Segments can be several MB; keep a generous cap (the UI helper caps at 2 MB).
    constexpr size_t kMaxBytes = 32 * 1024 * 1024;
    if (!out || out->size() + n > kMaxBytes) return 0;
    out->append(p, n);
    return n;
}

void ensureReady()
{
    static std::once_flag once;
    std::call_once(once, [] {
        // The app already owns these; libnx tolerates a second init attempt.
        const Result sr = socketInitializeDefault();
        (void)sr;
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

Fetch fetch(const std::string& url, const std::vector<std::string>& headers,
            const std::string& range = std::string(), long timeoutSeconds = 20)
{
    ensureReady();
    Fetch r;
    CURL* c = curl_easy_init();
    if (!c) return r;
    curl_slist* list = nullptr;
    bool hasUA = false;
    for (const std::string& h : headers)
    {
        if (lowerCopy(h).rfind("user-agent:", 0) == 0) hasUA = true;
        list = curl_slist_append(list, h.c_str());
    }
    if (!hasUA)
        list = curl_slist_append(list, (std::string("User-Agent: ") + kVideoUA).c_str());
    if (!range.empty())
        list = curl_slist_append(list, ("Range: " + range).c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fetchWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.code);
    char* eff = nullptr;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff) r.finalUrl = eff;
    curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return r;
}

// ------------------------------------------------------ disguised-segment logic

bool looksLikeImage(const std::string& b)
{
    auto at = [&](size_t off, const char* m) {
        const size_t n = std::strlen(m);
        return b.size() >= off + n && b.compare(off, n, m) == 0;
    };
    // Already MPEG-TS: healthy.
    if (b.size() >= 4 && static_cast<unsigned char>(b[0]) == 0x47) return false;
    return at(0, "\x89PNG") || at(0, "\xFF\xD8\xFF") || at(0, "GIF8") || at(0, "BM") || at(0, "RIFF");
}

// Remove fake bytes prepended to a real media segment (TS sync or fMP4 box).
std::string stripFakeHeader(const std::string& b)
{
    if (!looksLikeImage(b)) return b;
    const size_t limit = std::min<size_t>(b.size(), 256 * 1024);
    for (size_t i = 1; i + 376 < limit; ++i)
        if (static_cast<unsigned char>(b[i]) == 0x47 &&
            static_cast<unsigned char>(b[i + 188]) == 0x47 &&
            static_cast<unsigned char>(b[i + 376]) == 0x47)
            return b.substr(i);
    // fMP4 disguised as an image
    for (size_t i = 1; i + 8 < limit; ++i)
        if (b.compare(i + 4, 4, "ftyp") == 0 || b.compare(i + 4, 4, "styp") == 0 ||
            b.compare(i + 4, 4, "moof") == 0)
            return b.substr(i);
    return b;
}

// ------------------------------------------------------------- proxy plumbing

std::string b64enc(const std::string& in) { return crypto::base64Encode(in, true, false); }
std::string b64dec(const std::string& in) { return crypto::base64Decode(in, true); }

std::string proxyUrl(int sid, const std::string& target, bool playlist)
{
    return "http://127.0.0.1:" + std::to_string(serverPort) + (playlist ? "/p/" : "/s/") +
           std::to_string(sid) + "/" + b64enc(target) + (playlist ? ".m3u8" : ".ts");
}

// Rewrite every URI of a playlist so libmpv fetches it through the proxy.
std::string rewrite(const std::string& body, const std::string& base, int sid)
{
    std::istringstream in(body);
    std::string line, out;
    bool nextIsPlaylist = false;
    auto rewriteAttr = [&](std::string l, bool playlist) {
        const size_t p = l.find("URI=\"");
        if (p == std::string::npos) return l;
        const size_t s = p + 5, e = l.find('"', s);
        if (e == std::string::npos) return l;
        const std::string abs = resolve(base, l.substr(s, e - s));
        return l.substr(0, s) + proxyUrl(sid, abs, playlist) + l.substr(e);
    };
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty())
        {
            out += "\n";
            continue;
        }
        if (line[0] == '#')
        {
            if (startsWith(line, "#EXT-X-STREAM-INF")) nextIsPlaylist = true;
            if (startsWith(line, "#EXT-X-MEDIA:") || startsWith(line, "#EXT-X-I-FRAME-STREAM-INF"))
                line = rewriteAttr(line, true);
            else if (startsWith(line, "#EXT-X-KEY") || startsWith(line, "#EXT-X-MAP") ||
                     startsWith(line, "#EXT-X-SESSION-KEY"))
                line = rewriteAttr(line, false);
            out += line + "\n";
            continue;
        }
        const std::string abs = resolve(base, line);
        out += proxyUrl(sid, abs, nextIsPlaylist) + "\n";
        nextIsPlaylist = false;
    }
    return out;
}

void sendAll(int fd, const std::string& data)
{
    size_t off = 0;
    while (off < data.size())
    {
        const long n = send(fd, data.data() + off,
                            static_cast<int>(std::min<size_t>(data.size() - off, 64 * 1024)), 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

void reply(int fd, int status, const std::string& type, const std::string& body, bool headOnly)
{
    const std::string head = "HTTP/1.1 " + std::to_string(status) +
                             (status == 200 ? " OK" : " Error") +
                             "\r\nContent-Type: " + type +
                             "\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\nConnection: close\r\n\r\n";
    sendAll(fd, head);
    if (!headOnly) sendAll(fd, body);
}

void handleClientImpl(int fd)
{
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384)
    {
        const long n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
    }
    const bool headOnly = startsWith(req, "HEAD ");
    const size_t sp1 = req.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
    const std::string path =
        (sp1 != std::string::npos && sp2 != std::string::npos) ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "";

    // /p/<sid>/<b64url>.m3u8  or  /s/<sid>/<b64url>.ts
    bool ok = false;
    if (path.size() > 4 && path[0] == '/' && (path[1] == 'p' || path[1] == 's') && path[2] == '/')
    {
        const size_t slash = path.find('/', 3);
        if (slash != std::string::npos)
        {
            const int sid = std::atoi(path.substr(3, slash - 3).c_str());
            std::string enc = path.substr(slash + 1);
            const size_t dot = enc.find('.');
            if (dot != std::string::npos) enc = enc.substr(0, dot);
            const std::string target = b64dec(enc);
            proxyLog(std::string(path[1] == 'p' ? "playlist " : "segment ") + target.substr(0, 120));
            std::vector<std::string> headers;
            {
                std::lock_guard<std::mutex> lock(mtx);
                auto it = sessions.find(sid);
                if (it != sessions.end()) headers = it->second;
            }
            Fetch r;
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                r = fetch(target, headers, std::string(), 60);
                if (r.code < 500 && r.code != 0) break;
            }
            proxyLog("  HTTP " + std::to_string(r.code) + ", " + std::to_string(r.body.size()) + " byte");
            if (r.code >= 200 && r.code < 300)
            {
                const std::string& body = r.body;
                const std::string base = r.finalUrl.empty() ? target : r.finalUrl;
                size_t skip = 0;
                while (skip < body.size() && static_cast<unsigned char>(body[skip]) <= ' ') ++skip;
                if (body.compare(skip, 7, "#EXTM3U") == 0 || body.compare(skip, 10, "\xEF\xBB\xBF#EXTM3U") == 0)
                {
                    reply(fd, 200, "application/vnd.apple.mpegurl", rewrite(body, base, sid), headOnly);
                }
                else
                {
                    const std::string clean = stripFakeHeader(body);
                    proxyLog("  sent " + std::to_string(clean.size()) + " byte (stripped " +
                             std::to_string(body.size() - clean.size()) + ")");
                    reply(fd, 200, "video/mp2t", clean, headOnly);
                }
            }
            else
            {
                reply(fd, r.code ? static_cast<int>(r.code) : 502, "text/plain", "upstream error", headOnly);
            }
            ok = true;
        }
    }
    if (!ok) reply(fd, 404, "text/plain", "not found", headOnly);
}

void handleClient(int fd)
{
    activeClients++;
    try
    {
        handleClientImpl(fd);
    }
    catch (...)
    {
    }
    shutdown(fd, SHUT_RDWR);
    close(fd);
    activeClients--;
}

// A few fixed threads do accept()+serve forever: no thread is created per
// segment (thread resources are limited on the Switch).
void workerLoop(int sock)
{
    int failures = 0;
    while (true)
    {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        const int fd = accept(sock, reinterpret_cast<sockaddr*>(&cli), &len);
        if (fd < 0)
        {
            if (++failures > 50) break; // socket closed (app exit)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        failures = 0;
        handleClient(fd);
    }
}

bool startWorkers(int sock)
{
    int started = 0;
    for (int i = 0; i < 3; ++i)
    {
        try
        {
            std::thread(workerLoop, sock).detach();
            ++started;
        }
        catch (...)
        {
        }
    }
    return started > 0;
}

bool ensureServer()
{
    if (serverPort) return true;
    if (serverFailed) return false;
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        serverFailed = true;
        return false;
    }
    const int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Fixed high port range: getsockname with port 0 is not always reliable on the Switch.
    for (int port = 48620; port < 48660; ++port)
    {
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(sock, 8) == 0)
        {
            serverPort = port;
            break;
        }
    }
    if (!serverPort)
    {
        close(sock);
        serverFailed = true;
        return false;
    }
    if (!startWorkers(sock))
    {
        close(sock);
        serverPort = 0;
        serverFailed = true;
        return false;
    }
    proxyLog("listening on 127.0.0.1:" + std::to_string(serverPort));
    return true;
}

// ------------------------------------------------------------- public surface

bool needsProxy(const std::string& url, const std::vector<std::string>& headers)
{
    try
    {
        Fetch pl = fetch(url, headers);
        if (pl.code < 200 || pl.code >= 300) return false;
        size_t skip = 0;
        while (skip < pl.body.size() && static_cast<unsigned char>(pl.body[skip]) <= ' ') ++skip;
        if (pl.body.compare(skip, 7, "#EXTM3U") != 0 && pl.body.compare(skip, 10, "\xEF\xBB\xBF#EXTM3U") != 0)
            return false;

        std::string base = pl.finalUrl.empty() ? url : pl.finalUrl;
        std::string body = pl.body;
        if (body.find("#EXT-X-STREAM-INF") != std::string::npos)
        {
            // Master playlist: probe the first variant instead.
            std::istringstream in(body);
            std::string line, variant;
            bool want = false;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (line[0] == '#')
                {
                    if (startsWith(line, "#EXT-X-STREAM-INF")) want = true;
                    continue;
                }
                if (want)
                {
                    variant = line;
                    break;
                }
            }
            if (variant.empty()) return false;
            const std::string vurl = resolve(base, variant);
            Fetch vr = fetch(vurl, headers);
            if (vr.code < 200 || vr.code >= 300) return false;
            body = vr.body;
            base = vr.finalUrl.empty() ? vurl : vr.finalUrl;
        }

        // Encrypted playlists must not pass through the proxy (key rewriting
        // would break segment decryption).
        if (body.find("#EXT-X-KEY:METHOD=AES") != std::string::npos) return false;

        std::istringstream in(body);
        std::string line, seg;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line[0] == '#') continue;
            seg = line;
            break;
        }
        if (seg.empty()) return false;

        Fetch sr = fetch(resolve(base, seg), headers, "bytes=0-2047");
        if (sr.code < 200 || sr.code >= 300) return false;
        return looksLikeImage(sr.body);
    }
    catch (...)
    {
        return false;
    }
}

std::string wrap(const std::string& url, const std::vector<std::string>& headers)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (!ensureServer())
    {
        proxyLog("server not started");
        return {};
    }
    proxyLog("new stream: " + url.substr(0, 120));
    const int sid = nextSession++;
    sessions[sid] = headers;
    while (sessions.size() > 32) sessions.erase(sessions.begin());
    return proxyUrl(sid, url, true);
}

void setLogPath(const std::string& path)
{
    std::lock_guard<std::mutex> lock(logMutex);
    logPath = path;
    FILE* f = std::fopen(path.c_str(), "w");
    if (f) std::fclose(f);
}

} // namespace

} // namespace hlsproxy
