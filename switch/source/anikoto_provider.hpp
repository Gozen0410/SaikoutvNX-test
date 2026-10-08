#pragma once

#include "kaa_crypto.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <ctime>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern void saikou_debug_log(const char* stage);

namespace anikoto {

struct Episode {
    int number = 0;
    std::string title;
    std::string id;
};

struct Stream {
    std::string url;
    std::string quality;
    std::string type;
    std::vector<std::string> headers;
};

namespace detail {
struct Response {
    long code = 0;
    std::string body;
    std::string contentType;
};

static std::mutex& cookie_share_mutex() { static std::mutex m; return m; }
static void cookie_share_lock(CURL*, curl_lock_data, curl_lock_access, void*) { cookie_share_mutex().lock(); }
static void cookie_share_unlock(CURL*, curl_lock_data, void*) { cookie_share_mutex().unlock(); }
static CURLSH* cookie_share() {
    static CURLSH* share = [] {
        CURLSH* h = curl_share_init();
        if (!h) return static_cast<CURLSH*>(nullptr);
        curl_share_setopt(h, CURLSHOPT_LOCKFUNC, cookie_share_lock);
        curl_share_setopt(h, CURLSHOPT_UNLOCKFUNC, cookie_share_unlock);
        curl_share_setopt(h, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
        return h;
    }();
    return share;
}

static size_t write_cb(char* p, size_t size, size_t count, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * count;
    if (out) out->append(p, n);
    return n;
}

static std::string url_encode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static std::string url_decode(const std::string& value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto hexValue = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hexValue(value[i + 1]), lo = hexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

static Response get(const std::string& url, const std::vector<std::string>& headers = {}, long timeout = 25, bool forceHttp11 = false) {
    Response r;
    CURL* c = curl_easy_init();
    if (!c) return r;
    curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    if (CURLSH* sharedCookies = cookie_share()) {
        curl_easy_setopt(c, CURLOPT_SHARE, sharedCookies);
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
    }
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    // The Android Anikoto extractor pins its playlist client to HTTP/1.1.
    // Some MegaPlay CDNs return opaque error payloads over negotiated HTTP/2.
    if (forceHttp11) curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(c, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/129.0.0.0 Safari/537.36");
    if (list) curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.code);
    char* contentType = nullptr;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &contentType);
    if (contentType) r.contentType = contentType;
    curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return r;
}

static std::string trim(std::string s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string html_decode(std::string s) {
    const std::pair<const char*, const char*> entities[] = {
        {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
        {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}
    };
    for (const auto& e : entities) {
        size_t p = 0;
        while ((p = s.find(e.first, p)) != std::string::npos) {
            s.replace(p, std::strlen(e.first), e.second);
            p += std::strlen(e.second);
        }
    }
    return s;
}

static std::string strip_tags(const std::string& html) {
    std::string out;
    bool tag = false;
    for (char c : html) {
        if (c == '<') { tag = true; continue; }
        if (c == '>') { tag = false; out.push_back(' '); continue; }
        if (!tag) out.push_back(c);
    }
    return trim(html_decode(out));
}

static std::string tag_attr(const std::string& tag, const std::string& attr) {
    const std::string needle = attr + "=";
    size_t p = 0;
    while ((p = tag.find(needle, p)) != std::string::npos) {
        if (p > 0 && (std::isalnum(static_cast<unsigned char>(tag[p - 1])) ||
                      tag[p - 1] == '-' || tag[p - 1] == '_')) {
            p += needle.size();
            continue;
        }
        p += needle.size();
        while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
        if (p >= tag.size()) return {};
        if (tag[p] == '"' || tag[p] == '\'') {
            const char q = tag[p++];
            const size_t e = tag.find(q, p);
            return e == std::string::npos ? std::string() : html_decode(tag.substr(p, e - p));
        }
        size_t e = p;
        while (e < tag.size() && !std::isspace(static_cast<unsigned char>(tag[e])) && tag[e] != '>') ++e;
        return html_decode(tag.substr(p, e - p));
    }
    return {};
}

static bool has_class(const std::string& tag, const std::string& wanted) {
    const std::string classes = " " + tag_attr(tag, "class") + " ";
    return classes.find(" " + wanted + " ") != std::string::npos;
}

static std::string find_href_for_name(const std::string& html) {
    size_t p = 0;
    while ((p = html.find("<a", p)) != std::string::npos) {
        const size_t e = html.find('>', p);
        if (e == std::string::npos) break;
        const std::string tag = html.substr(p, e - p + 1);
        if (has_class(tag, "name")) {
            const std::string href = tag_attr(tag, "href");
            if (!href.empty()) return href;
        }
        p = e + 1;
    }
    return {};
}

static std::string find_data_id(const std::string& html) {
    for (const std::string marker : { std::string("id=\"watch-main\""), std::string("id='watch-main'") }) {
        const size_t at = html.find(marker);
        if (at == std::string::npos) continue;
        const size_t start = html.rfind('<', at), end = html.find('>', at);
        if (start == std::string::npos || end == std::string::npos) continue;
        const std::string id = tag_attr(html.substr(start, end - start + 1), "data-id");
        if (!id.empty()) return id;
    }
    for (const char* attr : { "data-id", "data-tip" }) {
        size_t pos = 0;
        while ((pos = html.find(attr, pos)) != std::string::npos) {
            if (pos > 0 && (std::isalnum(static_cast<unsigned char>(html[pos - 1])) ||
                            html[pos - 1] == '-' || html[pos - 1] == '_')) {
                pos += std::strlen(attr); continue;
            }
            const size_t start = html.rfind('<', pos), end = html.find('>', pos);
            if (start == std::string::npos || end == std::string::npos) break;
            const std::string value = tag_attr(html.substr(start, end - start + 1), attr);
            if (!value.empty()) return value;
            pos = end + 1;
        }
    }
    return {};
}

static std::string result_html(const std::string& json) {
    size_t p = json.find("\"result\"");
    if (p == std::string::npos) return {};
    p = json.find(':', p);
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    std::string out;
    bool escaped = false;
    for (++p; p < json.size(); ++p) {
        const char c = json[p];
        if (escaped) {
            switch (c) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                default: out.push_back(c); break;
            }
            escaped = false;
        } else if (c == '\\') escaped = true;
        else if (c == '"') return out;
        else out.push_back(c);
    }
    return {};
}

static std::string json_string(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    if (p >= json.size() || json[p] != '"') return {};
    std::string out;
    bool escaped = false;
    for (++p; p < json.size(); ++p) {
        const char c = json[p];
        if (escaped) {
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else if (c == 't') out.push_back('\t');
            else if (c == '/') out.push_back('/');
            else out.push_back(c);
            escaped = false;
        } else if (c == '\\') escaped = true;
        else if (c == '"') return out;
        else out.push_back(c);
    }
    return {};
}

struct Server { std::string type; std::string id; std::string name; };

static std::string episode_param(const std::string& id, const std::string& name) {
    const std::string marker = "&" + name + "=";
    size_t p = id.find(marker);
    if (p == std::string::npos) return {};
    p += marker.size();
    const size_t end = id.find('&', p);
    return url_decode(id.substr(p, end == std::string::npos ? std::string::npos : end - p));
}

static std::vector<Server> mapper_servers(const std::string& episodeId, const std::string& base) {
    const std::string mal = episode_param(episodeId, "mal");
    const std::string slug = episode_param(episodeId, "slug");
    const std::string timestamp = episode_param(episodeId, "ts");
    if (mal.empty() || slug.empty() || timestamp.empty()) return {};

    const std::string url = "https://mapper.nekostream.site/api/mal/" +
        url_encode(mal) + "/" + url_encode(slug) + "/" + url_encode(timestamp);
    const Response response = get(url, {
        "Accept: application/json, text/javascript, */*; q=0.01",
        "Origin: " + base,
        "Referer: " + base + "/"
    });
    char marker[160];
    if (response.code < 200 || response.code >= 300) {
        std::snprintf(marker, sizeof(marker), "ANICHI MAPPER REJECT status=%ld bytes=%zu",
            response.code, response.body.size());
        ::saikou_debug_log(marker);
        return {};
    }

    std::vector<Server> out;
    std::set<std::string> seen;
    size_t p = 0;
    while ((p = response.body.find("\"url\"", p)) != std::string::npos) {
        const size_t dub = response.body.rfind("\"dub\"", p);
        const size_t sub = response.body.rfind("\"sub\"", p);
        const bool isDub = dub != std::string::npos && (sub == std::string::npos || dub > sub);
        const size_t colon = response.body.find(':', p + 5);
        if (colon == std::string::npos) break;
        const std::string streamUrl = json_string(response.body.substr(p), "url");
        if (streamUrl.rfind("http", 0) == 0 && seen.insert(streamUrl).second)
            out.push_back({isDub ? "Dub" : "Sub", streamUrl, "Mapper"});
        p = colon + 1;
    }
    std::snprintf(marker, sizeof(marker), "ANICHI MAPPER READY status=%ld bytes=%zu links=%zu",
        response.code, response.body.size(), out.size());
    ::saikou_debug_log(marker);
    return out;
}

static std::string resolve_url(const std::string& base, const std::string& rel) {
    if (rel.empty()) return {};
    if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0) return rel;
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) return rel;
    if (rel.rfind("//", 0) == 0) return base.substr(0, scheme + 1) + rel;
    const size_t hostEnd = base.find('/', scheme + 3);
    const std::string origin = base.substr(0, hostEnd == std::string::npos ? base.size() : hostEnd);
    if (rel[0] == '/') return origin + rel;
    std::string dir = base;
    const size_t q = dir.find('?');
    if (q != std::string::npos) dir.resize(q);
    const size_t slash = dir.rfind('/');
    if (slash != std::string::npos) dir.resize(slash + 1);
    return dir + rel;
}

static const char* source_tag(const std::string& url) {
    return lower(url).find("anichi") != std::string::npos ? "ANICHI" : "ANIKOTO";
}

static std::string host_of(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t end = url.find_first_of("/?#", scheme + 3);
    return url.substr(scheme + 3, end == std::string::npos ? std::string::npos : end - scheme - 3);
}

static std::string origin(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t slash = url.find('/', scheme + 3);
    return url.substr(0, slash == std::string::npos ? url.size() : slash);
}

static std::string path_only(std::string url) {
    const size_t q = url.find('?');
    if (q != std::string::npos) url.resize(q);
    const size_t scheme = url.find("://");
    const size_t slash = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    return slash == std::string::npos ? "/" : url.substr(slash);
}

static std::string strip_ep_suffix(std::string path) {
    const size_t p = path.rfind("/ep-");
    if (p == std::string::npos) return path;
    for (size_t i = p + 4; i < path.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(path[i]))) return path;
    return path.substr(0, p);
}

static std::string vrf_exchange(const std::string& in, const std::string& k1, const std::string& k2) {
    std::string out = in;
    for (char& c : out) {
        const size_t p = k1.find(c);
        if (p != std::string::npos) c = k2[p];
    }
    return out;
}

static std::string vrf_encrypt(const std::string& input) {
    std::string v = input;
    v = vrf_exchange(v, "AP6GeR8H0lwUz1", "UAz8Gwl10P6ReH");
    v = crypto::base64Encode(crypto::rc4("ItFKjuWokn4ZpB", v), true, true);
    v = crypto::base64Encode(crypto::rc4("fOyt97QWFB3", v), true, true);
    v = vrf_exchange(v, "1majSlPQd2M5", "da1l2jSmP5QM");
    v = vrf_exchange(v, "CPYvHj09Au3", "0jHA9CPYu3v");
    std::reverse(v.begin(), v.end());
    v = crypto::base64Encode(crypto::rc4("736y1uTJpBLUX", v), true, true);
    return url_encode(v);
}

static std::vector<Server> parse_servers(const std::string& html) {
    std::vector<Server> out;
    std::set<std::string> seen;
    size_t p = 0;
    while ((p = html.find("data-link-id=", p)) != std::string::npos) {
        const size_t tagStart = html.rfind('<', p);
        const size_t tagEnd = html.find('>', p);
        if (tagStart == std::string::npos || tagEnd == std::string::npos) break;
        const std::string tag = html.substr(tagStart, tagEnd - tagStart + 1);
        const std::string id = tag_attr(tag, "data-link-id");
        if (id.empty() || seen.count(id)) { p = tagEnd + 1; continue; }

        std::string name;
        const size_t close = html.find("</", tagEnd + 1);
        if (close != std::string::npos) name = strip_tags(html.substr(tagEnd + 1, close - tagEnd - 1));
        if (name.empty()) name = "Server";

        std::string type = tag_attr(tag, "data-type");
        if (type.empty()) {
            const size_t parent = html.rfind("<div", tagStart);
            if (parent != std::string::npos) {
                const size_t pe = html.find('>', parent);
                if (pe != std::string::npos && pe < tagStart) {
                    const std::string parentTag = html.substr(parent, pe - parent + 1);
                    type = tag_attr(parentTag, "data-type");
                    if (type.empty() && parentTag.find("type") != std::string::npos) {
                        const size_t label = html.find("<label", parent);
                        if (label != std::string::npos && label < tagStart) {
                            const size_t le = html.find('>', label);
                            const size_t lc = le == std::string::npos ? std::string::npos : html.find("</label>", le);
                            if (le != std::string::npos && lc != std::string::npos && lc < tagStart)
                                type = strip_tags(html.substr(le + 1, lc - le - 1));
                        }
                    }
                }
            }
        }
        if (type.empty()) type = "Sub";
        out.push_back({trim(type), id, trim(name)});
        seen.insert(id);
        p = tagEnd + 1;
    }
    return out;
}

static std::string parse_server_embed(const std::string& base, const Server& s, const std::string& epUrl) {
    if (s.id.rfind("http://", 0) == 0 || s.id.rfind("https://", 0) == 0) return s.id;
    const Response r = get(base + "/ajax/server?get=" + url_encode(s.id), {
        "Accept: application/json, text/javascript, */*; q=0.01",
        "Referer: " + base + epUrl,
        "X-Requested-With: XMLHttpRequest"
    });
    if (r.code < 200 || r.code >= 300) {
        char marker[128];
        std::snprintf(marker, sizeof(marker), "%s SERVER API REJECT status=%ld bytes=%zu",
            source_tag(base), r.code, r.body.size());
        ::saikou_debug_log(marker);
        return {};
    }
    // The site's server endpoint returns {"result":{"url":"..."}}. Older
    // versions returned an HTML string, so accept both response shapes.
    std::string embed = json_string(r.body, "url");
    if (embed.empty()) {
        const std::string result = result_html(r.body);
        embed = json_string(result, "url");
        if (embed.empty()) {
            size_t iframe = result.find("<iframe");
            if (iframe != std::string::npos) {
                const size_t end = result.find('>', iframe);
                if (end != std::string::npos)
                    embed = tag_attr(result.substr(iframe, end - iframe + 1), "src");
            }
        }
    }
    return resolve_url(base + epUrl, embed);
}

static std::string mega_source(const std::string& embed, const std::string& /*serverType*/) {
    const std::string o = origin(embed);
    if (o.empty()) return {};
    const Response page = get(embed, {
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Origin: " + o,
        "Referer: " + o + "/",
        "X-Requested-With: XMLHttpRequest"
    });
    if (page.code < 200 || page.code >= 300) {
        char marker[160];
        std::snprintf(marker, sizeof(marker), "%s PLAYER PAGE REJECT status=%ld bytes=%zu host=%s",
            source_tag(embed), page.code, page.body.size(), host_of(embed).c_str());
        ::saikou_debug_log(marker);
        return {};
    }

    std::string mediaId;
    size_t p = 0;
    while ((p = page.body.find("data-id=", p)) != std::string::npos) {
        const size_t e = page.body.find('>', p);
        if (e == std::string::npos) break;
        const size_t start = page.body.rfind('<', p);
        if (start == std::string::npos) break;
        mediaId = tag_attr(page.body.substr(start, e - start + 1), "data-id");
        if (!mediaId.empty()) break;
        p = e + 1;
    }
    if (mediaId.empty()) {
        char marker[160];
        std::snprintf(marker, sizeof(marker), "%s PLAYER PAGE NO DATA ID bytes=%zu host=%s content=%s",
            source_tag(embed), page.body.size(), host_of(embed).c_str(), page.contentType.c_str());
        ::saikou_debug_log(marker);
        return {};
    }

    // The shared Anikoto extractor requests only the media ID (and optional s).
    // Repeated id/type query keys can make MegaPlay pick an inconsistent source.
    std::string api = o + "/stream/getSources?id=" + url_encode(mediaId);
    const size_t sp = embed.find("?s=");
    if (sp != std::string::npos) {
        std::string s = embed.substr(sp + 3);
        const size_t amp = s.find('&');
        if (amp != std::string::npos) s.resize(amp);
        if (!s.empty()) api += "&s=" + url_encode(s);
    }

    const std::vector<std::string> apiHeaders = {
        "Accept: application/json,*/*",
        "X-Requested-With: XMLHttpRequest",
        "Referer: " + embed
    };
    auto extractSource = [&](const Response& source) {
        auto isHlsUrl = [](const std::string& value) {
            return lower(value).find(".m3u8") != std::string::npos;
        };
        // MegaPlay's "tracks" array also contains fields named "file" (usually .vtt).
        // Only accept an HLS manifest as the playback URL; otherwise MPV is handed
        // a subtitle file and reports that no audio/video data was played.
        auto firstHlsFile = [&](const std::string& body) {
            size_t p = 0;
            while ((p = body.find("\\"file\\"", p)) != std::string::npos) {
                const std::string candidate = json_string(body.substr(p), "file");
                if (isHlsUrl(candidate)) return candidate;
                p += 6;
            }
            return std::string();
        };

        std::string result;
        const std::string enc = json_string(source.body, "enc");
        if (!enc.empty()) {
            try {
                std::string key = "i?LMTAx0Q6,:}50U";
                key.resize(32, '\0');
                const std::string raw = crypto::base64Decode(enc);
                if (!raw.empty() && raw.size() % 16 == 0)
                    result = firstHlsFile(
                        crypto::aesCbcDecrypt(raw, key, "W0;27ToaUpl_P%'c"));
            } catch (...) {}
        }
        // Prefer a real manifest from the plain sources field if the encrypted
        // payload only exposed subtitle tracks.
        if (result.empty()) result = firstHlsFile(source.body);
        if (result.empty()) {
            const std::string sources = json_string(source.body, "sources");
            if (isHlsUrl(sources)) result = sources;
        }
        return result;
    };

    Response source = get(api, apiHeaders);
    std::string m3u8 = source.code >= 200 && source.code < 300 ? extractSource(source) : std::string();
    char marker[192];
    std::snprintf(marker, sizeof(marker), "%s SOURCES API status=%ld bytes=%zu content=%s parsed=%d",
        source_tag(embed), source.code, source.body.size(), source.contentType.c_str(), m3u8.empty() ? 0 : 1);
    ::saikou_debug_log(marker);
    if (m3u8.empty()) {
        const std::string fallback = o + "/stream/getSourcesNew?id=" + url_encode(mediaId);
        source = get(fallback, apiHeaders);
        if (source.code >= 200 && source.code < 300) m3u8 = extractSource(source);
        std::snprintf(marker, sizeof(marker), "%s SOURCES NEW status=%ld bytes=%zu content=%s parsed=%d",
            source_tag(embed), source.code, source.body.size(), source.contentType.c_str(), m3u8.empty() ? 0 : 1);
        ::saikou_debug_log(marker);
    }
    if (m3u8.empty()) return {};
    m3u8 = resolve_url(embed, m3u8);

    const std::string low = lower(m3u8);
    if (low.find("?token=") == std::string::npos && low.find("&token=") == std::string::npos) {
        for (size_t i = 0; i + 67 < low.size(); ++i) {
            if (low[i] != '/') continue;
            bool a = true, b = true;
            for (size_t k = i + 1; k < i + 33; ++k)
                a = a && std::isxdigit(static_cast<unsigned char>(low[k]));
            for (size_t k = i + 34; k < i + 66; ++k)
                b = b && std::isxdigit(static_cast<unsigned char>(low[k]));
            if (a && b && low[i + 33] == '/' && low[i + 66] == '/') {
                const std::string pathKey = low.substr(i + 1, 32) + "/" + low.substr(i + 34, 32);
                const std::string payload =
                    std::to_string(static_cast<long long>(std::time(nullptr)) + 90) + "|" + pathKey;
                const std::string sig = crypto::base64Encode(
                    crypto::hmacSha256("MpCdnT0k3n!9f2K#xQ7vL5mR8wN1pY4s", payload), true, false);
                const std::string token = crypto::base64Encode(payload, true, false) + "." + sig;
                m3u8 += (m3u8.find('?') == std::string::npos ? "?" : "&");
                m3u8 += "token=" + url_encode(token);
                break;
            }
        }
    }
    return m3u8;
}

static std::string mewcdn_source(const std::string& embed) {
    const size_t hash = embed.find('#');
    if (hash == std::string::npos) return {};
    const std::string raw = trim(crypto::base64Decode(embed.substr(hash + 1)));
    if (raw.rfind("http", 0) != 0) return {};
    std::string m3u8 = raw;
    const Response page = get(embed, {"Referer: " + origin(embed) + "/"});
    const size_t hm = page.body.find("var HOST_MAP");
    if (hm != std::string::npos) {
        const size_t ob = page.body.find('{', hm);
        const size_t cb = ob == std::string::npos ? std::string::npos : page.body.find('}', ob);
        if (ob != std::string::npos && cb != std::string::npos) {
            std::vector<std::string> q;
            const std::string map = page.body.substr(ob + 1, cb - ob - 1);
            size_t p = 0;
            while ((p = map.find('\'', p)) != std::string::npos) {
                const size_t e = map.find('\'', p + 1);
                if (e == std::string::npos) break;
                q.push_back(map.substr(p + 1, e - p - 1));
                p = e + 1;
            }
            for (size_t i = 0; i + 1 < q.size(); i += 2) {
                const size_t at = m3u8.find(q[i]);
                if (at != std::string::npos) { m3u8.replace(at, q[i].size(), q[i + 1]); break; }
            }
        }
    }
    return m3u8;
}

static std::vector<Stream> hls(const std::string& master, const std::string& prefix,
                               const std::string& referer, const std::vector<std::string>& extraHeaders,
                               const char* sourceName, bool allowOpaqueMedia) {
    std::vector<std::string> requestHeaders = extraHeaders;
    requestHeaders.push_back("Referer: " + referer);
    const Response r = get(master, requestHeaders, 25, true);
    std::vector<Stream> out;
    const bool hasPlaylistSignature = r.body.find("#EXTM3U") != std::string::npos;
    char signature[25] = {};
    const size_t signatureBytes = std::min<size_t>(8, r.body.size());
    for (size_t i = 0; i < signatureBytes; ++i)
        std::snprintf(signature + i * 2, 3, "%02X",
            static_cast<unsigned char>(r.body[i]));
    const bool isMp4 = r.body.size() >= 8 && r.body.compare(4, 4, "ftyp") == 0;
    const bool isTransportStream = r.body.size() >= 377 &&
        static_cast<unsigned char>(r.body[0]) == 0x47 &&
        static_cast<unsigned char>(r.body[188]) == 0x47 &&
        static_cast<unsigned char>(r.body[376]) == 0x47;
    char probe[224];
    std::snprintf(probe, sizeof(probe),
        "%s HLS PROBE status=%ld bytes=%zu content=%s extm3u=%d sig=%s host=%s",
        sourceName, r.code, r.body.size(), r.contentType.c_str(),
        hasPlaylistSignature ? 1 : 0, signature, host_of(master).c_str());
    ::saikou_debug_log(probe);
    const bool urlLooksLikePlaylist = lower(path_only(master)).find(".m3u8") != std::string::npos;
    const bool opaqueMedia = allowOpaqueMedia && r.code >= 200 && r.code < 300 && !r.body.empty() &&
        (lower(r.contentType).find("video/") != std::string::npos ||
         lower(r.contentType).find("audio/") != std::string::npos ||
         isMp4 || isTransportStream);
    if (r.code < 200 || r.code >= 300 ||
        (!hasPlaylistSignature && !urlLooksLikePlaylist && !opaqueMedia)) {
        char marker[224];
        std::snprintf(marker, sizeof(marker),
            "%s HLS REJECT status=%ld bytes=%zu extm3u=%d content=%s host=%s path_m3u8=%d",
            sourceName, r.code, r.body.size(), hasPlaylistSignature ? 1 : 0,
            r.contentType.c_str(), host_of(master).c_str(), urlLooksLikePlaylist ? 1 : 0);
        ::saikou_debug_log(marker);
        return out;
    }
    if (!hasPlaylistSignature) {
        // Anikoto-family MegaPlay endpoints may expose extensionless opaque
        // media routes instead of a text playlist. Let libmpv probe those.
        out.push_back({master, "Auto", prefix, extraHeaders});
        char marker[192];
        std::snprintf(marker, sizeof(marker),
            "%s OPAQUE MEDIA CANDIDATE; PASSING URL TO MPV host=%s",
            sourceName, host_of(master).c_str());
        ::saikou_debug_log(marker);
        return out;
    }
    std::vector<std::string> lines;
    size_t p = 0;
    while (p < r.body.size()) {
        const size_t e = r.body.find('\n', p);
        lines.push_back(trim(r.body.substr(p, e == std::string::npos ? std::string::npos : e - p)));
        p = e == std::string::npos ? r.body.size() : e + 1;
    }
    bool masterPlaylist = false;
    for (const std::string& line : lines)
        if (line.find("#EXT-X-STREAM-INF") != std::string::npos) masterPlaylist = true;
    if (!masterPlaylist) {
        out.push_back({master, "Auto", prefix, extraHeaders});
        return out;
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find("#EXT-X-STREAM-INF") == std::string::npos) continue;
        std::string resolution;
        const size_t rp = lines[i].find("RESOLUTION=");
        if (rp != std::string::npos) {
            size_t q = rp + 11;
            if (q < lines[i].size() && lines[i][q] == '"') ++q;
            const size_t end = lines[i].find('"', q);
            resolution = lines[i].substr(q, end == std::string::npos ? std::string::npos : end - q);
        }
        std::string variant;
        for (size_t k = i + 1; k < lines.size(); ++k) {
            if (lines[k].empty() || lines[k][0] == '#') continue;
            variant = lines[k];
            break;
        }
        if (variant.empty()) continue;
        const size_t x = resolution.find('x');
        const std::string quality = x == std::string::npos ? "Auto" : resolution.substr(x + 1) + "p";
        out.push_back({resolve_url(master, variant), quality, prefix, extraHeaders});
    }
    if (out.empty()) out.push_back({master, "Auto", prefix, extraHeaders});
    char marker[144];
    std::snprintf(marker, sizeof(marker),
        "%s HLS ACCEPT status=%ld bytes=%zu streams=%zu",
        sourceName, r.code, r.body.size(), out.size());
    ::saikou_debug_log(marker);
    return out;
}

static std::string find_m3u8_url(std::string text) {
    size_t escaped = 0;
    while ((escaped = text.find("\\/", escaped)) != std::string::npos) text.replace(escaped, 2, "/");
    while ((escaped = text.find("&amp;")) != std::string::npos) text.replace(escaped, 5, "&");
    size_t p = 0;
    while ((p = text.find(".m3u8", p)) != std::string::npos) {
        size_t start = p;
        while (start > 0) {
            const char c = text[start - 1];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' ||
                c == '<' || c == '>') break;
            --start;
        }
        size_t end = p + 5;
        while (end < text.size()) {
            const char c = text[end];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' ||
                c == '<' || c == '>') break;
            ++end;
        }
        const std::string url = text.substr(start, end - start);
        if (url.find("://") != std::string::npos || url.rfind("//", 0) == 0) return url;
        p += 5;
    }
    return {};
}

static std::string player_page_source(const std::string& embed, const std::string& serverType,
                                      int depth = 0) {
    if (embed.empty() || depth > 2) return {};
    const std::string pageOrigin = origin(embed);
    if (pageOrigin.empty()) return {};
    const Response page = get(embed, {
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Origin: " + pageOrigin,
        "Referer: " + pageOrigin + "/"
    });
    if (page.code < 200 || page.code >= 300) {
        char marker[160];
        std::snprintf(marker, sizeof(marker), "%s PLAYER PAGE REJECT status=%ld bytes=%zu host=%s",
            source_tag(embed), page.code, page.body.size(), host_of(embed).c_str());
        ::saikou_debug_log(marker);
        return {};
    }

    if (page.body.find("#EXTM3U") != std::string::npos) return embed;

    const std::string dataId = find_data_id(page.body);
    if (!dataId.empty()) {
        const std::string result = mega_source(embed, serverType);
        if (!result.empty()) return result;
    }

    const std::string direct = find_m3u8_url(page.body);
    if (!direct.empty()) return resolve_url(embed, direct);

    size_t p = page.body.find("<iframe");
    if (p != std::string::npos) {
        const size_t end = page.body.find('>', p);
        if (end != std::string::npos) {
            const std::string tag = page.body.substr(p, end - p + 1);
            const std::string src = tag_attr(tag, "src");
            if (!src.empty()) {
                const std::string nested = resolve_url(embed, src);
                if (host_of(nested) != host_of(embed))
                    return player_page_source(nested, serverType, depth + 1);
            }
        }
    }
    char marker[160];
    std::snprintf(marker, sizeof(marker), "%s PLAYER PAGE NO PLAYABLE URL bytes=%zu host=%s content=%s",
        source_tag(embed), page.body.size(), host_of(embed).c_str(), page.contentType.c_str());
    ::saikou_debug_log(marker);
    return {};
}

static std::string search_path(const std::string& title, const std::string& base) {
    const std::string vrf = vrf_encrypt(title);
    const std::string url = base + "/filter?keyword=" + url_encode(title) +
        "&page=1&vrf=" + url_encode(vrf);
    const Response r = get(url, {"Referer: " + base + "/"});
    const std::string href = find_href_for_name(r.body);
    char marker[176];
    std::snprintf(marker, sizeof(marker),
        "%s SEARCH PROBE status=%ld bytes=%zu content=%s title_link=%d",
        source_tag(base), r.code, r.body.size(), r.contentType.c_str(), href.empty() ? 0 : 1);
    ::saikou_debug_log(marker);
    if (r.code < 200 || r.code >= 300) return {};
    return resolve_url(url, href);
}

} // namespace detail

inline std::vector<Episode> fetch_episodes(const std::string& title, const std::string& base, std::string& status) {
    const std::string search = detail::search_path(title, base);
    if (search.empty()) { status = "Search returned no result."; return {}; }

    const detail::Response page = detail::get(search, {"Referer: " + base + "/"});
    if (page.code < 200 || page.code >= 300) { status = "Anime page request failed."; return {}; }

    const std::string animeId = detail::find_data_id(page.body);
    if (animeId.empty()) { status = "Anime ID was not found on the source page."; return {}; }

    const std::string path = detail::strip_ep_suffix(detail::path_only(search));
    const std::string vrf = detail::vrf_encrypt(animeId);
    const detail::Response eps = detail::get(
        base + "/ajax/episode/list/" + detail::url_encode(animeId) +
            "?style=grid&vrf=" + detail::url_encode(vrf),
        {
            "Accept: application/json, text/javascript, */*; q=0.01",
            "Referer: " + base + path,
            "X-Requested-With: XMLHttpRequest"
        });
    if (eps.code < 200 || eps.code >= 300) {
        status = "Episode list request failed (HTTP " + std::to_string(eps.code) + ").";
        return {};
    }

    const std::string body = detail::result_html(eps.body);
    if (body.empty()) { status = "Episode endpoint returned no HTML."; return {}; }

    std::vector<Episode> out;
    size_t p = 0;
    std::set<int> seen;
    while ((p = body.find("<a", p)) != std::string::npos) {
        const size_t e = body.find('>', p);
        if (e == std::string::npos) break;
        const std::string tag = body.substr(p, e - p + 1);
        const std::string epNum = detail::tag_attr(tag, "data-num");
        const std::string ids = detail::tag_attr(tag, "data-ids");
        if (!epNum.empty() && !ids.empty()) {
            const int number = std::atoi(epNum.c_str());
            if (number > 0 && !seen.count(number)) {
                std::string titleText;
                const size_t close = body.find("</a>", e);
                if (close != std::string::npos) titleText = detail::strip_tags(body.substr(e + 1, close - e - 1));
                Episode ep;
                ep.number = number;
                ep.title = "Episode " + std::to_string(number);
                if (!titleText.empty() && titleText != ep.title) ep.title += ": " + titleText;
                ep.id = ids + "&epurl=" + path + "/ep-" + std::to_string(number);
                const std::string mal = detail::tag_attr(tag, "data-mal");
                const std::string slug = detail::tag_attr(tag, "data-slug");
                const std::string ts = detail::tag_attr(tag, "data-timestamp");
                if (!mal.empty()) ep.id += "&mal=" + detail::url_encode(mal);
                if (!slug.empty()) ep.id += "&slug=" + detail::url_encode(slug);
                if (!ts.empty()) ep.id += "&ts=" + detail::url_encode(ts);
                out.push_back(std::move(ep));
                seen.insert(number);
            }
        }
        p = e + 1;
    }

    std::sort(out.begin(), out.end(), [](const Episode& a, const Episode& b) { return a.number < b.number; });
    status = out.empty() ? "Source returned no episodes." :
        "Source returned " + std::to_string(out.size()) + " episodes.";
    return out;
}

inline std::vector<Stream> fetch_streams(const Episode& episode, const std::string& base, std::string& status) {
    const size_t amp = episode.id.find("&epurl=");
    if (amp == std::string::npos) { status = "Episode has no source route."; return {}; }
    const std::string ids = episode.id.substr(0, amp);
    const std::string epUrl = episode.id.substr(amp + 1);
    const size_t nextAmp = epUrl.find('&');
    const std::string pagePath = nextAmp == std::string::npos ? epUrl.substr(7) : epUrl.substr(7, nextAmp - 7);

    // The site requires a watch-page session before its server AJAX requests.
    detail::get(base + pagePath, { "Referer: " + base + "/" });
    const detail::Response list = detail::get(base + "/ajax/server/list?servers=" + ids, {
        "Accept: application/json, text/javascript, */*; q=0.01",
        "Referer: " + base + pagePath,
        "X-Requested-With: XMLHttpRequest"
    });
    if (list.code < 200 || list.code >= 300) {
        status = "Server list request failed (HTTP " + std::to_string(list.code) + ").";
        return {};
    }

    const std::string html = detail::result_html(list.body);
    std::vector<detail::Server> servers = detail::parse_servers(html);
    if (detail::lower(base).find("anichi") != std::string::npos) {
        std::vector<detail::Server> mapped = detail::mapper_servers(episode.id, base);
        servers.insert(servers.end(), mapped.begin(), mapped.end());
    }
    if (servers.empty()) { status = "Source returned no server entries."; return {}; }

    std::vector<Stream> out;
    std::set<std::string> seen;
    for (const auto& server : servers) {
        const std::string embed = detail::parse_server_embed(base, server, pagePath);
        if (embed.empty()) continue;

        std::string m3u8;
        std::vector<std::string> headers;
        std::string referer = base + "/";
        bool allowOpaqueMedia = false;
        const std::string low = detail::lower(embed);

        if ((low.find("megaplay.") != std::string::npos || low.find("vidtube.site") != std::string::npos) &&
            low.find("/stream/") != std::string::npos) {
            allowOpaqueMedia = true;
            m3u8 = detail::mega_source(embed, server.type);
            referer = detail::origin(embed) + "/";
            headers.push_back("Origin: " + detail::origin(embed));
        } else if (low.find("mewcdn.online/player/plyr.php") != std::string::npos) {
            m3u8 = detail::mewcdn_source(embed);
            referer = "https://mewcdn.online/";
            headers.push_back("Origin: https://mewcdn.online");
        } else if (low.find(".m3u8") != std::string::npos) {
            m3u8 = embed;
            referer = base + "/";
        } else {
            m3u8 = detail::player_page_source(embed, server.type);
            referer = detail::origin(embed) + "/";
            if (!detail::origin(embed).empty())
                headers.push_back("Origin: " + detail::origin(embed));
        }

        if (m3u8.empty()) {
            char marker[192];
            std::snprintf(marker, sizeof(marker), "%s SERVER UNRESOLVED name=%s type=%s host=%s",
                detail::source_tag(base), server.name.c_str(), server.type.c_str(),
                detail::host_of(embed).c_str());
            ::saikou_debug_log(marker);
            continue;
        }
        std::vector<Stream> variants = detail::hls(
            m3u8, detail::trim(server.name) + " - " + detail::trim(server.type),
            referer, headers, detail::source_tag(base), allowOpaqueMedia);
        for (auto& v : variants) {
            v.type = detail::trim(server.name); if (!detail::trim(server.type).empty()) v.type += "  " + detail::trim(server.type);
            v.headers.push_back("Referer: " + referer);
            const std::string key = v.url + "|" + v.quality + "|" + v.type;
            if (seen.insert(key).second) out.push_back(std::move(v));
        }
    }

    status = out.empty() ? "Servers were found, but no playable streams were extracted." :
        "Extracted " + std::to_string(out.size()) + " playable stream option(s).";
    return out;
}

inline const char* base_for_source(int sourceId) {
    switch (sourceId) {
        case 0: return "https://anichi.to";
        case 1: return "https://anikototv.to";
        case 2: return "https://animewave.to";
        case 3: return "https://animesogo.to";
        case 4: return "https://animekaitv.to";
        default: return "";
    }
}

inline const char* name_for_source(int sourceId) {
    switch (sourceId) {
        case 0: return "Anichi";
        case 1: return "Anikoto";
        case 2: return "AniWave (Unoriginal)";
        case 3: return "AnimeSogo";
        case 4: return "AnimeKai (Unoriginal)";
        default: return "Anikoto source";
    }
}

} // namespace anikoto
