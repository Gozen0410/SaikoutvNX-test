#pragma once

#include <borealis.hpp>
#include <curl/curl.h>
#include "kaa_crypto.hpp"
#include "anikoto_provider.hpp"
#include <switch/applets/swkbd.h>
#include <switch/services/nifm.h>
#include <switch.h>
#include "api_sources.hpp"
#include "mpv_player.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static void log_stage(const char* stage);
static std::string replace_all(std::string value, const std::string& from, const std::string& to)
{
    if (from.empty()) return value;
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos)
    {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}


static const auto g_perfStart = std::chrono::steady_clock::now();

static long long perf_elapsed_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_perfStart).count();
}

static void perf_log(const char* stage)
{
    char marker[224];
    std::snprintf(marker, sizeof(marker), "PERF %lldms | %s", perf_elapsed_ms(), stage);
    log_stage(marker);
}

static void perf_log_count(const char* stage, size_t count)
{
    char marker[224];
    std::snprintf(marker, sizeof(marker), "PERF %lldms | %s count=%zu",
        perf_elapsed_ms(), stage, count);
    log_stage(marker);
}

struct SaikouAnime
{
    int id = 0;
    int score = 0;
    int episodes = 0;
    std::string title;
    std::string englishTitle;
    std::string romajiTitle;
    std::string nativeTitle;
    std::string userPreferredTitle;
    std::string coverUrl;
    std::string bannerUrl;
    std::string description;
    std::string format;
    std::string status;
    std::string posterPath;
};

static bool g_providerEnabled[kApiSourceCount] = { true };
static int g_selectedApiSource = static_cast<int>(ApiSourceId::KickAssAnime);
static bool g_restoreGlobalQuitAfterKeyboard = false;
static std::atomic<unsigned int> g_anilistAccountRevision{ 0 };
static bool g_socketOwned = false;
static bool g_curlOwned = false;
static constexpr const char* kSettingsPath = "sdmc:/switch/SaikouTV/settings.ini";
static constexpr const char* kAniListTokenPath = "sdmc:/switch/SaikouTV/anilistToken";
static constexpr const char* kCacheDir = "sdmc:/switch/SaikouTV/cache";
static constexpr const char* kLocalContinuePath = "sdmc:/switch/SaikouTV/continue.ini";
static constexpr size_t kLocalContinueCapacity = 120;

static void register_page_back_action(brls::View* root)
{
    if (!root)
        return;

    root->registerAction("Back", brls::BUTTON_B, [](brls::View*) {
        log_stage("NAV ACTION: Back");
        // Pop after the input traversal has released its current View pointers.
        brls::sync([] {
            const bool popped = brls::Application::popActivity(brls::TransitionAnimation::NONE, [] {}, true);
            log_stage(popped ? "NAV BACK: returned to previous screen" : "NAV BACK: no previous screen");
        });
        return true;
    });
}

static bool ensure_network_ready()
{
    static std::once_flag once;
    static Result socketResult = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    std::call_once(once, [] {
        socketResult = socketInitializeDefault();
        g_socketOwned = R_SUCCEEDED(socketResult);
    });
    return R_SUCCEEDED(socketResult) ||
        socketResult == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
}

static bool ensure_curl_ready()
{
    static std::once_flag once;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(once, [] {
        result = curl_global_init(CURL_GLOBAL_DEFAULT);
        g_curlOwned = result == CURLE_OK;
    });
    return result == CURLE_OK;
}

static void shutdown_network()
{
    // All activity-owned network workers are joined during Borealis Application::exit()
    // before libnx calls userAppExit(). Release cURL first, then our socket service.
    if (g_curlOwned)
    {
        curl_global_cleanup();
        g_curlOwned = false;
    }

    if (g_socketOwned)
    {
        socketExit();
        g_socketOwned = false;
    }
}

extern "C" void userAppExit(void)
{
    shutdown_network();
}

static size_t append_http_data(char* data, size_t size, size_t count, void* userdata)
{
    std::string* output = static_cast<std::string*>(userdata);
    const size_t bytes = size * count;
    static constexpr size_t maxResponseBytes = 2 * 1024 * 1024;
    if (!output || output->size() + bytes > maxResponseBytes)
        return 0;
    output->append(data, bytes);
    return bytes;
}

static bool http_request(const std::string& url, const std::string* postBody,
    std::string& response, long timeoutSeconds = 10, const std::string* bearerToken = nullptr,
    const char* userAgent = nullptr,
    const std::vector<std::string>* extraHeaders = nullptr)
{
    if (!ensure_network_ready() || !ensure_curl_ready())
        return false;

    static constexpr int kMaxAttempts = 2;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt)
    {
        response.clear();

        CURL* curl = curl_easy_init();
        if (!curl)
            return false;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/json");
        if (postBody)
            headers = curl_slist_append(headers, "Content-Type: application/json");
        if (bearerToken && !bearerToken->empty())
        {
            const std::string authHeader = "Authorization: Bearer " + *bearerToken;
            headers = curl_slist_append(headers, authHeader.c_str());
        }
        if (extraHeaders)
        {
            for (const std::string& extra : *extraHeaders)
                headers = curl_slist_append(headers, extra.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
            (userAgent && *userAgent) ? userAgent : "SaikouTV-NX/0.3");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_http_data);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        if (postBody)
        {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody->size()));
        }

        const CURLcode requestResult = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        const bool success =
            requestResult == CURLE_OK && httpCode >= 200 && httpCode < 300;

        const bool retryableHttp =
            httpCode == 429 || httpCode >= 500;

        const bool retryableCurl =
            requestResult == CURLE_OPERATION_TIMEDOUT ||
            requestResult == CURLE_COULDNT_CONNECT ||
            requestResult == CURLE_COULDNT_RESOLVE_HOST ||
            requestResult == CURLE_RECV_ERROR ||
            requestResult == CURLE_SEND_ERROR ||
            requestResult == CURLE_GOT_NOTHING;

        char marker[256];
        std::snprintf(marker, sizeof(marker),
            "HTTP ATTEMPT %d/%d result=%d http=%ld bytes=%zu retry=%d url=%.120s",
            attempt, kMaxAttempts, static_cast<int>(requestResult), httpCode,
            response.size(),
            (!success && attempt < kMaxAttempts && (retryableHttp || retryableCurl)) ? 1 : 0,
            url.c_str());
        log_stage(marker);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (success)
            return true;

        if (attempt < kMaxAttempts && (retryableHttp || retryableCurl))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            continue;
        }

        return false;
    }

    return false;
}

static void append_utf8(std::string& out, unsigned int cp)
{
    if (cp <= 0x7F)
        out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0xFFFF)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static unsigned int read_hex4(const std::string& text, size_t at)
{
    unsigned int value = 0;
    for (size_t i = 0; i < 4 && at + i < text.size(); ++i)
    {
        const char c = text[at + i];
        value <<= 4;
        if (c >= '0' && c <= '9') value += static_cast<unsigned int>(c - '0');
        else if (c >= 'a' && c <= 'f') value += static_cast<unsigned int>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value += static_cast<unsigned int>(c - 'A' + 10);
    }
    return value;
}

static std::string decode_json_string(const std::string& json, size_t quoteAt, size_t* endAt = nullptr)
{
    std::string out;
    if (quoteAt >= json.size() || json[quoteAt] != '"')
        return out;

    for (size_t i = quoteAt + 1; i < json.size(); ++i)
    {
        const char c = json[i];
        if (c == '"')
        {
            if (endAt) *endAt = i + 1;
            return out;
        }
        if (c != '\\')
        {
            out.push_back(c);
            continue;
        }
        if (++i >= json.size())
            break;
        switch (json[i])
        {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
            {
                unsigned int cp = read_hex4(json, i + 1);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < json.size() &&
                    json[i + 1] == '\\' && json[i + 2] == 'u')
                {
                    const unsigned int low = read_hex4(json, i + 3);
                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                append_utf8(out, cp);
                break;
            }
            default: out.push_back(json[i]); break;
        }
    }
    if (endAt) *endAt = json.size();
    return out;
}

static size_t find_json_string_end(const std::string& json, size_t start)
{
    bool escaped = false;
    for (size_t i = start + 1; i < json.size(); ++i)
    {
        if (escaped) { escaped = false; continue; }
        if (json[i] == '\\') { escaped = true; continue; }
        if (json[i] == '"') return i;
    }
    return std::string::npos;
}

static size_t json_field_value(const std::string& json, const std::string& key, size_t from = 0)
{
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle, from);
    if (p == std::string::npos) return p;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return p;
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    return p;
}

static std::string json_string_field(const std::string& json, const std::string& key, size_t from = 0)
{
    const size_t p = json_field_value(json, key, from);
    return p < json.size() && json[p] == '"' ? decode_json_string(json, p) : std::string();
}

static int json_int_field(const std::string& json, const std::string& key, size_t from = 0)
{
    const size_t p = json_field_value(json, key, from);
    if (p >= json.size() || json[p] == 'n' || json[p] == 't' || json[p] == 'f') return 0;
    return std::atoi(json.c_str() + p);
}

static std::string json_object_field(const std::string& json, const std::string& key)
{
    size_t p = json_field_value(json, key);
    if (p >= json.size() || json[p] != '{') return std::string();
    const size_t start = p++;
    int depth = 1;
    bool inString = false;
    bool escaped = false;
    for (; p < json.size(); ++p)
    {
        const char c = json[p];
        if (inString)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return json.substr(start, p - start + 1);
    }
    return std::string();
}

static std::string json_array_field(const std::string& json, const std::string& key)
{
    size_t p = json_field_value(json, key);
    if (p >= json.size() || json[p] != '[') return std::string();
    const size_t start = p++;
    int depth = 1;
    bool inString = false;
    bool escaped = false;
    for (; p < json.size(); ++p)
    {
        const char c = json[p];
        if (inString)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '[') ++depth;
        else if (c == ']' && --depth == 0) return json.substr(start, p - start + 1);
    }
    return std::string();
}

static std::vector<std::string> json_object_array(const std::string& array)
{
    std::vector<std::string> objects;
    size_t p = array.find('[');
    if (p == std::string::npos) return objects;
    for (++p; p < array.size();)
    {
        while (p < array.size() && (std::isspace(static_cast<unsigned char>(array[p])) || array[p] == ',')) ++p;
        if (p >= array.size() || array[p] == ']') break;
        if (array[p] != '{') { ++p; continue; }
        const size_t start = p++;
        int depth = 1;
        bool inString = false;
        bool escaped = false;
        for (; p < array.size() && depth > 0; ++p)
        {
            const char c = array[p];
            if (inString)
            {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') inString = true;
            else if (c == '{') ++depth;
            else if (c == '}') --depth;
        }
        if (depth == 0) objects.push_back(array.substr(start, p - start));
    }
    return objects;
}

static std::string json_quote(const std::string& text)
{
    std::string out = "\"";
    for (unsigned char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    static const char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0x0F]);
                    out.push_back(hex[c & 0x0F]);
                }
                else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
    return out;
}

static SaikouAnime parse_anime_object(const std::string& object)
{
    SaikouAnime anime;
    anime.id = json_int_field(object, "id");
    anime.score = json_int_field(object, "averageScore");
    anime.episodes = json_int_field(object, "episodes");
    anime.format = json_string_field(object, "format");
    anime.status = json_string_field(object, "status");
    anime.description = json_string_field(object, "description");
    anime.bannerUrl = json_string_field(object, "bannerImage");

    const std::string title = json_object_field(object, "title");
    anime.englishTitle = json_string_field(title, "english");
    anime.romajiTitle = json_string_field(title, "romaji");
    anime.nativeTitle = json_string_field(title, "native");
    anime.userPreferredTitle = json_string_field(title, "userPreferred");
    anime.title = anime.englishTitle;
    if (anime.title.empty()) anime.title = anime.userPreferredTitle;
    if (anime.title.empty()) anime.title = anime.romajiTitle;
    if (anime.title.empty()) anime.title = anime.nativeTitle;

    const std::string cover = json_object_field(object, "coverImage");
    anime.coverUrl = json_string_field(cover, "extraLarge");
    if (anime.coverUrl.empty()) anime.coverUrl = json_string_field(cover, "large");
    if (anime.title.empty()) anime.title = "Untitled anime";
    return anime;
}

static std::vector<SaikouAnime> parse_anilist_media(
    const std::string& response, size_t maxItems = 24)
{
    std::vector<SaikouAnime> media;
    size_t arrayAt = response.find("\"media\"");
    if (arrayAt == std::string::npos) return media;
    arrayAt = response.find('[', arrayAt);
    if (arrayAt == std::string::npos) return media;

    for (size_t p = arrayAt + 1; p < response.size() && media.size() < maxItems;)
    {
        while (p < response.size() && (std::isspace(static_cast<unsigned char>(response[p])) || response[p] == ',')) ++p;
        if (p >= response.size() || response[p] == ']') break;
        if (response[p] != '{') { ++p; continue; }

        const size_t start = p++;
        int depth = 1;
        bool inString = false;
        bool escaped = false;
        for (; p < response.size() && depth > 0; ++p)
        {
            const char c = response[p];
            if (inString)
            {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') inString = true;
            else if (c == '{') ++depth;
            else if (c == '}') --depth;
        }
        if (depth != 0) break;
        SaikouAnime anime = parse_anime_object(response.substr(start, p - start));
        if (anime.id > 0) media.push_back(anime);
    }
    return media;
}

static std::vector<SaikouAnime> fetch_anilist_media(
    const std::string& search, int pageSize, std::string& status, int page = 1)
{
    static const std::string endpoint = "https://graphql.anilist.co";
    const std::string query =
        "query ($page: Int, $perPage: Int, $search: String) { "
        "Page(page: $page, perPage: $perPage) { "
        "media(type: ANIME, sort: TRENDING_DESC, search: $search) { "
        "id title { english romaji native userPreferred } "
        "coverImage { large extraLarge } bannerImage averageScore format status episodes "
        "description(asHtml: false) "
        "} } }";

    std::string body = "{\"query\":" + json_quote(query) +
        ",\"variables\":{\"page\":" + std::to_string(page) + ",\"perPage\":" + std::to_string(pageSize) +
        ",\"search\":" + (search.empty() ? "null" : json_quote(search)) + "}}";
    std::string response;
    if (!http_request(endpoint, &body, response, 12))
    {
        status = "AniList could not be reached. Check the Switch internet connection.";
        log_stage("ANILIST MEDIA REQUEST FAILED");
        return {};
    }

    std::vector<SaikouAnime> result = parse_anilist_media(response);
    char marker[96];
    std::snprintf(marker, sizeof(marker),
        "ANILIST MEDIA PAGE %d FOUND %zu ITEMS", page, result.size());
    log_stage(marker);
    status = result.empty() ? "AniList returned no anime." : "Live AniList data";
    return result;
}

static std::vector<SaikouAnime> fetch_anilist_trending_page(
    int page, int pageSize, std::string& status)
{
    static const std::string endpoint = "https://graphql.anilist.co";
    const std::string query =
        "query ($page: Int, $perPage: Int) { "
        "Page(page: $page, perPage: $perPage) { "
        "media(type: ANIME, sort: TRENDING_DESC) { "
        "id title { english romaji native userPreferred } "
        "coverImage { large extraLarge } bannerImage averageScore format status episodes "
        "description(asHtml: false) "
        "} } }";

    const std::string body = "{\"query\":" + json_quote(query) +
        ",\"variables\":{\"page\":" + std::to_string(page) +
        ",\"perPage\":" + std::to_string(pageSize) + "}}";
    std::string response;
    if (!http_request(endpoint, &body, response, 12))
    {
        status = "AniList could not be reached. Check the Switch internet connection.";
        log_stage("ANILIST TRENDING PAGE REQUEST FAILED");
        return {};
    }

    std::vector<SaikouAnime> result =
        parse_anilist_media(response, static_cast<size_t>(pageSize));
    char marker[128];
    std::snprintf(marker, sizeof(marker),
        "ANILIST TRENDING PAGE %d FOUND %zu ITEMS", page, result.size());
    log_stage(marker);
    status = result.empty() ? "AniList returned no more trending anime." : "Live AniList trending data";
    return result;
}

static std::vector<SaikouAnime> fetch_currently_airing_page(
    int page, int pageSize, std::string& status)
{
    static const std::string endpoint = "https://graphql.anilist.co";
    const std::string query =
        "query ($page: Int, $perPage: Int) { "
        "Page(page: $page, perPage: $perPage) { "
        "media(type: ANIME, status: RELEASING, sort: POPULARITY_DESC) { "
        "id title { english romaji native userPreferred } "
        "coverImage { large extraLarge } bannerImage averageScore format status episodes "
        "description(asHtml: false) "
        "} } }";

    const std::string body = "{\"query\":" + json_quote(query) +
        ",\"variables\":{\"page\":" + std::to_string(page) +
        ",\"perPage\":" + std::to_string(pageSize) + "}}";
    std::string response;
    if (!http_request(endpoint, &body, response, 12))
    {
        status = "AniList could not be reached. Check the Switch internet connection.";
        log_stage("ANILIST AIRING PAGE REQUEST FAILED");
        return {};
    }

    std::vector<SaikouAnime> result =
        parse_anilist_media(response, static_cast<size_t>(pageSize));
    char marker[128];
    std::snprintf(marker, sizeof(marker),
        "ANILIST AIRING PAGE %d FOUND %zu ITEMS", page, result.size());
    log_stage(marker);
    status = result.empty()
        ? "AniList returned no more currently airing anime."
        : "Live AniList currently airing data";
    return result;
}

static std::vector<SaikouAnime> fetch_currently_airing_media(int pageSize, std::string& status)
{
    static const std::string endpoint = "https://graphql.anilist.co";
    const std::string query =
        "query ($page: Int, $perPage: Int) { "
        "Page(page: $page, perPage: $perPage) { "
        "media(type: ANIME, status: RELEASING, sort: POPULARITY_DESC) { "
        "id title { english romaji native userPreferred } "
        "coverImage { large extraLarge } bannerImage averageScore format status episodes "
        "description(asHtml: false) "
        "} } }";

    std::string body = "{\"query\":" + json_quote(query) +
        ",\"variables\":{\"page\":1,\"perPage\":" + std::to_string(pageSize) + "}}";
    std::string response;
    if (!http_request(endpoint, &body, response, 12))
    {
        status = "AniList could not be reached. Check the Switch internet connection.";
        log_stage("ANILIST AIRING REQUEST FAILED");
        return {};
    }

    std::vector<SaikouAnime> result = parse_anilist_media(response);
    char marker[128];
    std::snprintf(marker, sizeof(marker), "ANILIST AIRING FOUND %zu ITEMS", result.size());
    log_stage(marker);
    status = result.empty() ? "AniList returned no currently airing anime." : "Live AniList currently airing data";
    return result;
}

struct AniListEntry
{
    std::string listStatus;
    std::string listName;
    int progress = 0;
    SaikouAnime anime;
};

static std::vector<AniListEntry> parse_anilist_library(const std::string& response)
{
    std::vector<AniListEntry> entries;
    const std::string collection = json_object_field(response, "MediaListCollection");
    const std::string lists = json_array_field(collection, "lists");
    for (const std::string& list : json_object_array(lists))
    {
        const std::string listStatus = json_string_field(list, "status");
        const std::string listName = json_string_field(list, "name");
        const std::string entryArray = json_array_field(list, "entries");
        for (const std::string& entry : json_object_array(entryArray))
        {
            const std::string media = json_object_field(entry, "media");
            SaikouAnime anime = parse_anime_object(media);
            if (anime.id <= 0) continue;
            AniListEntry item;
            item.listStatus = json_string_field(entry, "status");
            if (item.listStatus.empty()) item.listStatus = listStatus;
            item.listName = listName;
            item.progress = json_int_field(entry, "progress");
            item.anime = anime;
            entries.push_back(item);
        }
    }
    return entries;
}

static std::string load_anilist_token()
{
    FILE* file = std::fopen(kAniListTokenPath, "r");
    if (!file) return std::string();
    char buffer[4096] = {};
    const size_t count = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    std::string token(buffer, count);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
        token.pop_back();
    size_t first = 0;
    while (first < token.size() && std::isspace(static_cast<unsigned char>(token[first]))) ++first;
    if (first > 0) token.erase(0, first);
    return token;
}

static bool save_anilist_token(const std::string& token)
{
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SaikouTV", 0777);
    FILE* file = std::fopen(kAniListTokenPath, "w");
    if (!file) return false;
    const bool ok = std::fwrite(token.data(), 1, token.size(), file) == token.size();
    std::fclose(file);
    return ok;
}

static bool validate_anilist_token(const std::string& token, std::string& username)
{
    const std::string query = "{ Viewer { id name } }";
    const std::string body = "{\"query\":" + json_quote(query) + "}";
    std::string response;
    if (!http_request("https://graphql.anilist.co", &body, response, 12, &token))
        return false;
    if (json_int_field(response, "id") <= 0)
        return false;
    username = json_string_field(response, "name");
    return !username.empty();
}

static std::vector<AniListEntry> fetch_anilist_library(
    const std::string& token, std::string& username, std::string& status)
{
    const std::string viewerQuery = "{ Viewer { id name } }";
    const std::string viewerBody = "{\"query\":" + json_quote(viewerQuery) + "}";
    std::string viewerResponse;
    if (!http_request("https://graphql.anilist.co", &viewerBody, viewerResponse, 12, &token))
    {
        status = "Could not load the AniList account.";
        return {};
    }

    const int userId = json_int_field(viewerResponse, "id");
    username = json_string_field(viewerResponse, "name");
    if (userId <= 0 || username.empty())
    {
        status = "AniList rejected the saved account token. Pair again in Settings.";
        return {};
    }

    const std::string query =
        "query ($userId: Int) { MediaListCollection(userId: $userId, type: ANIME) { "
        "lists { name status entries { status progress media { "
        "id title { english romaji native userPreferred } "
        "coverImage { large extraLarge } bannerImage averageScore format status episodes "
        "description(asHtml: false) "
        "} } } } }";
    const std::string body = "{\"query\":" + json_quote(query) +
        ",\"variables\":{\"userId\":" + std::to_string(userId) + "}}";
    std::string response;
    if (!http_request("https://graphql.anilist.co", &body, response, 12, &token))
    {
        status = "Could not load AniList lists.";
        return {};
    }

    std::vector<AniListEntry> result = parse_anilist_library(response);
    status = result.empty()
        ? (username + " is linked. No anime entries were returned.")
        : (username + " — " + std::to_string(result.size()) + " anime entries");
    return result;
}

static std::string get_switch_local_ip()
{
    if (!ensure_network_ready()) return std::string();
    Result nifmResult = nifmInitialize(NifmServiceType_User);
    const bool nifmOwned = R_SUCCEEDED(nifmResult);
    if (!nifmOwned && nifmResult != MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized))
        return std::string();

    u32 rawAddress = 0;
    const Result ipResult = nifmGetCurrentIpAddress(&rawAddress);
    if (nifmOwned) nifmExit();
    if (R_FAILED(ipResult)) return std::string();

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&rawAddress);
    if (bytes[0] == 0 || bytes[0] == 127)
        return std::string();
    char ip[32];
    std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
        static_cast<unsigned int>(bytes[0]), static_cast<unsigned int>(bytes[1]),
        static_cast<unsigned int>(bytes[2]), static_cast<unsigned int>(bytes[3]));
    return ip;
}

static std::string cached_cover_path(int id)
{
    return std::string(kCacheDir) + "/anilist_" + std::to_string(id) + "_cover.jpg";
}

static std::string cached_banner_path(int id)
{
    return std::string(kCacheDir) + "/anilist_" + std::to_string(id) + "_banner.jpg";
}

static bool download_image(const std::string& url, const std::string& path)
{
    if (url.empty() || !ensure_network_ready() || !ensure_curl_ready())
        return false;

    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 256)
        return true;

    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string bytes;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 7L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SaikouTV-NX/0.3");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_http_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);

    const CURLcode result = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || httpCode < 200 || httpCode >= 300 || bytes.size() <= 256)
        return false;

    mkdir(kCacheDir, 0777);
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return written == bytes.size();
}

static void initialize_source_settings()
{
    for (size_t i = 0; i < kApiSourceCount; ++i)
        g_providerEnabled[i] = kApiSources[i].enabled;

    FILE* file = std::fopen(kSettingsPath, "r");
    if (!file) return;
    char line[512];
    while (std::fgets(line, sizeof(line), file))
    {
        std::string value(line);
        for (size_t i = 0; i < kApiSourceCount; ++i)
        {
            const std::string key = std::string(kApiSources[i].slug) + "=";
            if (value.find(key) == 0)
                g_providerEnabled[i] = value[key.size()] == '1';
        }
        if (value.find("selected=") == 0)
            g_selectedApiSource = std::atoi(value.c_str() + 9);
    }
    std::fclose(file);
    if (!api_source_is_valid(g_selectedApiSource))
        g_selectedApiSource = static_cast<int>(ApiSourceId::KickAssAnime);
}

static void save_source_settings()
{
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SaikouTV", 0777);
    FILE* file = std::fopen(kSettingsPath, "w");
    if (!file) return;
    for (size_t i = 0; i < kApiSourceCount; ++i)
        std::fprintf(file, "%s=%d\n", kApiSources[i].slug, g_providerEnabled[i] ? 1 : 0);
    std::fprintf(file, "selected=%d\n", g_selectedApiSource);
    std::fclose(file);
}


struct ProviderEpisode
{
    int number = 0;
    std::string title;
    std::string id;
    std::string provider;
    std::string category;
};

struct ProviderStream
{
    std::string url;
    std::string quality;
    std::string type;
    std::vector<std::string> headers;
};

static std::string encode_url_component(const std::string& input)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : input)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            output.push_back(static_cast<char>(c));
        else
        {
            output.push_back('%');
            output.push_back(hex[c >> 4]);
            output.push_back(hex[c & 15]);
        }
    }
    return output;
}


static std::string first_array(const std::string& json,
    const std::vector<std::string>& fields)
{
    for (const std::string& field : fields)
    {
        std::string value = json_array_field(json, field);
        if (!value.empty())
            return value;
    }
    return {};
}

static std::string first_string(const std::string& json,
    const std::vector<std::string>& fields)
{
    for (const std::string& field : fields)
    {
        std::string value = json_string_field(json, field);
        if (!value.empty())
            return value;
    }
    return {};
}



static size_t json_value_end(const std::string& json, size_t start)
{
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
    if (start >= json.size()) return start;
    if (json[start] == '"')
    {
        bool escaped = false;
        for (size_t i = start + 1; i < json.size(); ++i)
        {
            if (escaped) escaped = false;
            else if (json[i] == '\\') escaped = true;
            else if (json[i] == '"') return i + 1;
        }
        return json.size();
    }
    if (json[start] == '{' || json[start] == '[')
    {
        int depth = 0;
        bool quoted = false, escaped = false;
        for (size_t i = start; i < json.size(); ++i)
        {
            const char c = json[i];
            if (quoted)
            {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
                continue;
            }
            if (c == '"') quoted = true;
            else if (c == '{' || c == '[') ++depth;
            else if ((c == '}' || c == ']') && --depth == 0) return i + 1;
        }
        return json.size();
    }
    size_t end = start;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']') ++end;
    return end;
}

static std::vector<std::pair<std::string, std::string>> json_object_members(const std::string& object)
{
    std::vector<std::pair<std::string, std::string>> members;
    if (object.empty() || object.front() != '{') return members;
    size_t pos = 1;
    while (pos < object.size())
    {
        while (pos < object.size() && (std::isspace(static_cast<unsigned char>(object[pos])) ||
            object[pos] == ',')) ++pos;
        if (pos >= object.size() || object[pos] == '}') break;
        if (object[pos] != '"') break;
        size_t keyEnd = pos + 1;
        bool escaped = false;
        for (; keyEnd < object.size(); ++keyEnd)
        {
            if (escaped) escaped = false;
            else if (object[keyEnd] == '\\') escaped = true;
            else if (object[keyEnd] == '"') break;
        }
        if (keyEnd >= object.size()) break;
        const std::string key = decode_json_string(object, pos);
        pos = keyEnd + 1;
        while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) ++pos;
        if (pos >= object.size() || object[pos++] != ':') break;
        while (pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) ++pos;
        const size_t end = json_value_end(object, pos);
        if (end <= pos) break;
        members.emplace_back(key, object.substr(pos, end - pos));
        pos = end;
    }
    return members;
}



static std::string kaa_host(const std::string& url)
{
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t end = url.find_first_of("/?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static std::string kaa_query_value(const std::string& url, const std::string& key)
{
    const size_t q = url.find('?');
    if (q == std::string::npos) return {};
    size_t p = q + 1;
    while (p < url.size())
    {
        const size_t amp = url.find('&', p);
        const std::string part = url.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
        const std::string needle = key + "=";
        if (part.rfind(needle, 0) == 0) return part.substr(needle.size());
        if (amp == std::string::npos) break;
        p = amp + 1;
    }
    return {};
}

static std::string kaa_unescape_url(std::string value)
{
    value = replace_all(value, "\\/", "/");
    value = replace_all(value, "\\u0026", "&");
    return value;
}

static std::string kaa_fix_url(const std::string& raw, const std::string& base)
{
    std::string value = kaa_unescape_url(raw);
    if (value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0) return value;
    if (value.rfind("//", 0) == 0) return "https:" + value;
    if (value.rfind("/", 0) == 0) return "https://" + kaa_host(base) + value;
    return value;
}

static std::string kaa_signature_url(const std::string& serverUrl, const std::string& serverName,
    const std::string& html)
{
    const size_t cidAt = html.find("cid: '");
    if (cidAt == std::string::npos) return {};
    const size_t cidStart = cidAt + 6;
    const size_t cidEnd = html.find('\'', cidStart);
    if (cidEnd == std::string::npos) return {};
    const std::string cidHex = html.substr(cidStart, cidEnd - cidStart);
    const std::string cidRaw = crypto::fromHex(cidHex);
    const size_t sep = cidRaw.find('|');
    if (sep == std::string::npos) return {};
    const std::string ip = cidRaw.substr(0, sep);
    const size_t routeStart = sep + 1;
    const std::string playerRoute = cidRaw.substr(routeStart);
    if (ip.empty() || playerRoute.empty()) return {};

    const std::string route = replace_all(playerRoute, "player.php", "source.php");
    const std::string mid = serverName == "DuckStream" ? "mid" : "id";
    const std::string midValue = kaa_query_value(serverUrl, mid);
    if (midValue.empty()) return {};

    const std::string key =
        serverName == "VidStreaming" ? "e13d38099bf562e8b9851a652d2043d3" :
        serverName == "DuckStream" ? "4504447b74641ad972980a6b8ffd7631" :
        serverName == "BirdStream" ? "4b14d0ff625163e3c9c7a47926484bf2" : "";
    if (key.empty()) return {};

    const std::string timestamp = std::to_string(static_cast<long long>(std::time(nullptr)) + 60);
    std::string data = ip + "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/129.0.0.0 Mobile Safari/537.36" + route + midValue;
    if (serverName != "BirdStream") data += timestamp;
    data += key;

    std::string sourceUrl = "https://" + kaa_host(serverUrl) + route + "?" + mid + "=" + midValue;
    if (serverName != "BirdStream") sourceUrl += "&e=" + timestamp;
    sourceUrl += "&s=" + crypto::toHex(crypto::sha1(data));
    return sourceUrl;
}

static std::vector<ProviderStream> kaa_hls_streams(const std::string& masterUrl, const std::string& serverName,
    const std::string& referer, const std::string& playlist)
{
    std::vector<ProviderStream> out;
    out.push_back({masterUrl, serverName + " - Auto", "HLS", {"Referer: " + referer, "Origin: https://" + kaa_host(referer)}});
    size_t pos = 0;
    while ((pos = playlist.find("#EXT-X-STREAM-INF", pos)) != std::string::npos)
    {
        const size_t lineEnd = playlist.find('\n', pos);
        const std::string info = playlist.substr(pos, lineEnd == std::string::npos ? std::string::npos : lineEnd - pos);
        const size_t next = lineEnd == std::string::npos ? playlist.size() : lineEnd + 1;
        size_t u = next;
        while (u < playlist.size() && (playlist[u] == '\r' || playlist[u] == '\n' || playlist[u] == ' ')) ++u;
        const size_t uriEnd = playlist.find_first_of("\r\n", u);
        if (u >= playlist.size()) break;
        const std::string uri = playlist.substr(u, uriEnd == std::string::npos ? std::string::npos : uriEnd - u);
        const size_t res = info.find("RESOLUTION=");
        int height = 0;
        if (res != std::string::npos)
        {
            const size_t x = info.find('x', res);
            if (x != std::string::npos) height = std::atoi(info.c_str() + x + 1);
        }
        if (!uri.empty())
        {
            ProviderStream item;
            item.url = kaa_fix_url(uri, masterUrl);
            item.quality = serverName + " - " + (height > 0 ? std::to_string(height) + "p" : "Video");
            item.type = "HLS";
            item.headers = {"Referer: " + referer, "Origin: https://" + kaa_host(referer)};
            out.push_back(std::move(item));
        }
        pos = next;
    }
    return out;
}

static std::vector<ProviderStream> kaa_extract_server(const std::string& serverUrl,
    const std::string& serverName, std::string& status)
{
    static constexpr const char* kVideoUA =
        "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/129.0.0.0 Mobile Safari/537.36";
    std::vector<std::string> pageHeaders = {
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
    };
    std::string html;
    if (!http_request(serverUrl, nullptr, html, 25, nullptr, kVideoUA, &pageHeaders))
    {
        log_stage(("KAA EXTRACT PAGE FAILED server=" + serverName).c_str());
        return {};
    }
    log_stage(("KAA EXTRACT PAGE OK server=" + serverName + " bytes=" + std::to_string(html.size())).c_str());

    // New Astro-style KAA player: the page embeds manifest:[0,"//..."].
    std::string clean = replace_all(html, "&quot;", "\"");
    const std::string manifestMarker = "manifest\":[0,\"";
    const size_t manifestAt = clean.find(manifestMarker);
    if (manifestAt != std::string::npos)
    {
        size_t p = manifestAt + manifestMarker.size();
        size_t e = clean.find('"', p);
        if (e != std::string::npos)
        {
            const std::string manifest = kaa_fix_url(clean.substr(p, e - p), serverUrl);
            std::vector<std::string> h = {
                "Accept: */*",
                "Referer: " + serverUrl,
                "Origin: https://" + kaa_host(serverUrl)
            };
            std::string playlist;
            if (http_request(manifest, nullptr, playlist, 25, nullptr, kVideoUA, &h))
            {
                log_stage(("KAA EXTRACT MANIFEST OK server=" + serverName + " bytes=" + std::to_string(playlist.size())).c_str());
                return kaa_hls_streams(manifest, serverName, serverUrl, playlist);
            }
            return {{manifest, serverName + " - Auto", "HLS", {"Referer: " + serverUrl, "Origin: https://" + kaa_host(serverUrl)}}};
        }
    }

    const std::string sourceUrl = kaa_signature_url(serverUrl, serverName, html);
    if (sourceUrl.empty())
    {
        log_stage(("KAA EXTRACT UNSUPPORTED server=" + serverName).c_str());
        return {};
    }

    std::vector<std::string> sourceHeaders = {
        "Accept: */*",
        "Referer: " + serverUrl,
        "Origin: https://" + kaa_host(serverUrl)
    };
    std::string response;
    if (!http_request(sourceUrl, nullptr, response, 25, nullptr, kVideoUA, &sourceHeaders))
    {
        log_stage(("KAA EXTRACT SOURCE FAILED server=" + serverName).c_str());
        return {};
    }
    log_stage(("KAA EXTRACT SOURCE OK server=" + serverName + " bytes=" + std::to_string(response.size())).c_str());

    const size_t q = response.find(":\"");
    if (q == std::string::npos) return {};
    const size_t valueStart = q + 2;
    const size_t valueEnd = response.find("\"", valueStart);
    if (valueEnd == std::string::npos) return {};
    std::string payload = response.substr(valueStart, valueEnd - valueStart);
    payload = replace_all(payload, "\\\\", "\\");
    const size_t colon = payload.find(':');
    if (colon == std::string::npos) return {};
    const std::string encrypted = payload.substr(0, colon);
    const std::string ivHex = payload.substr(colon + 1);
    try
    {
        const std::string decrypted = crypto::aesCbcDecrypt(
            crypto::base64Decode(encrypted), 
            serverName == "VidStreaming" ? "e13d38099bf562e8b9851a652d2043d3" :
            serverName == "DuckStream" ? "4504447b74641ad972980a6b8ffd7631" :
            "4b14d0ff625163e3c9c7a47926484bf2",
            crypto::fromHex(ivHex));
        const std::string hls = json_string_field(decrypted, "hls");
        const std::string dash = json_string_field(decrypted, "dash");
        const std::string playlistUrl = kaa_fix_url(hls.empty() ? dash : hls, serverUrl);
        if (playlistUrl.empty()) return {};
        log_stage(("KAA EXTRACT DECRYPTED stream=" + (hls.empty() ? dash : hls)).c_str());
        if (!hls.empty())
        {
            std::vector<std::string> ph = {"Accept: */*", "Referer: " + serverUrl,
                "Origin: https://" + kaa_host(serverUrl)};
            std::string playlist;
            if (http_request(playlistUrl, nullptr, playlist, 25, nullptr, kVideoUA, &ph))
                return kaa_hls_streams(playlistUrl, serverName, serverUrl, playlist);
        }
        return {{playlistUrl, serverName + " - Auto", hls.empty() ? "DASH" : "HLS", {"Referer: " + serverUrl, "Origin: https://" + kaa_host(serverUrl)}}};
    }
    catch (const std::exception&)
    {
        log_stage(("KAA EXTRACT DECRYPT FAILED server=" + serverName).c_str());
        return {};
    }
}

static std::vector<ProviderStream> fetch_kaa_sources(
    const ProviderEpisode& episode, std::string& status)
{
    if (episode.id.empty())
    {
        status = "KickAssAnime did not provide an episode route.";
        return {};
    }

    // AnikkuNX's native KAA flow:
    // /api/show/{anime-slug}/episode/ep-{number}-{slug}
    // returns { "servers": [ { "name": ..., "src": ... }, ... ] }.
    // AnikkuNX's actual KAA video endpoint inserts /episode before /ep-*.
    // episode.id is the episode path returned by /episodes (e.g. /ep-1-5d81fc).
    std::string episodePath = episode.id;
    if (episodePath.rfind("/episode/", 0) != 0)
    {
        const size_t ep = episodePath.find("/ep-");
        if (ep != std::string::npos)
            episodePath.insert(ep, "/episode");
        else if (episodePath.rfind("ep-", 0) == 0)
            episodePath = "/episode/" + episodePath;
    }

    const std::string route =
        "https://kaa.lt/api/show" + episodePath;
    std::string response;

    log_stage(("KAA SOURCE REQUEST route=" + route).c_str());
    static constexpr const char* kKaaBrowserUA =
        "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/129.0.0.0 Mobile Safari/537.36";

    if (!http_request(route, nullptr, response, 25, nullptr, kKaaBrowserUA))
    {
        status = "KickAssAnime source request failed (see HTTP log).";
        log_stage("KAA SOURCE REQUEST FAILED");
        return {};
    }

    const std::string servers = first_array(response, { "servers" });
    if (servers.empty())
    {
        status = "KickAssAnime returned no server sources.";
        log_stage("KAA SOURCE RESPONSE HAD NO SERVERS");
        return {};
    }
    log_stage(("KAA SOURCE RESPONSE bytes=" + std::to_string(response.size())).c_str());

    std::vector<ProviderStream> sources;
    for (const std::string& object : json_object_array(servers))
    {
        const std::string name = first_string(object, { "name", "server", "provider" });
        const std::string src = first_string(object, { "src", "url", "source" });

        if (name.empty() || src.empty())
            continue;

        ProviderStream item;
        item.url = src;
        item.quality = name;
        item.type = "source";
        sources.push_back(std::move(item));

        log_stage(("KAA SOURCE FOUND name=" + name).c_str());
    }

    if (sources.empty())
    {
        status = "KickAssAnime returned no usable source servers.";
        return {};
    }

    std::vector<ProviderStream> resolved;
    for (const ProviderStream& source : sources)
    {
        log_stage(("KAA EXTRACT START server=" + source.quality).c_str());
        std::vector<ProviderStream> videos = kaa_extract_server(source.url, source.quality, status);
        resolved.insert(resolved.end(), videos.begin(), videos.end());
        if (!videos.empty())
            log_stage(("KAA EXTRACT READY server=" + source.quality + " count=" + std::to_string(videos.size())).c_str());
    }

    status = resolved.empty()
        ? "KickAssAnime found servers, but could not extract a playable stream."
        : "KickAssAnime extracted " + std::to_string(resolved.size()) + " playable stream option(s).";
    char marker[128];
    std::snprintf(marker, sizeof(marker),
        "KAA STREAMS FINAL count=%zu", resolved.size());
    log_stage(marker);
    return resolved;
}



static std::vector<std::string> provider_search_titles(const SaikouAnime& anime)
{
    // Match the Android title lookup behavior: search the English name first,
    // then retry with the AniList romaji/Japanese names if that search has no hit.
    const std::string candidates[] = {
        anime.englishTitle,
        anime.title,
        anime.romajiTitle,
        anime.nativeTitle,
        anime.userPreferredTitle
    };
    std::vector<std::string> titles;
    for (const std::string& candidate : candidates)
    {
        if (candidate.empty()) continue;
        bool duplicate = false;
        for (const std::string& existing : titles)
            if (existing == candidate) duplicate = true;
        if (!duplicate) titles.push_back(candidate);
    }
    return titles;
}

static std::vector<ProviderEpisode> fetch_provider_episodes(
    const SaikouAnime& anime, int sourceId, std::string& status)
{
    if (!api_source_is_valid(sourceId))
    {
        status = "Invalid episode provider.";
        return {};
    }

    std::string response;
    std::vector<ProviderEpisode> episodes;

    switch (static_cast<ApiSourceId>(sourceId))
    {
        case ApiSourceId::KickAssAnime:
        {

            static constexpr const char* kKaaBase = "https://kaa.lt";

            // KickAssAnime's current native flow is JSON-based:
            // POST /api/fsearch -> slug -> /api/show/{slug}
            // -> /language -> /episodes?page=N&lang=...
            std::string slug;

            for (const std::string& title : provider_search_titles(anime))
            {
                const std::string searchUrl = std::string(kKaaBase) + "/api/fsearch";
                const std::string body =
                    std::string("{\"page\":1,\"query\":") + json_quote(title) + "}";
                log_stage(("KAA SEARCH title=" + title).c_str());

                if (!http_request(searchUrl, &body, response, 20))
                    continue;

                const std::string results = first_array(response, { "result" });
                for (const std::string& object : json_object_array(results))
                {
                    slug = first_string(object, { "slug" });
                    if (!slug.empty()) break;
                }

                if (!slug.empty())
                    break;

                log_stage("KAA SEARCH HAD NO MATCH; trying next AniList title");
            }

            if (slug.empty())
            {
                status = "KickAssAnime search returned no matching show.";
                break;
            }

            {
                const std::string showUrl =
                    std::string(kKaaBase) + "/api/show/" + encode_url_component(slug);
                if (!http_request(showUrl, nullptr, response, 20))
                {
                    status = "KickAssAnime show lookup failed.";
                    break;
                }
                log_stage(("KAA SHOW READY slug=" + slug).c_str());
            }

            // Mirror AnikkuNX's language preference: Japanese first, English second.
            std::string lang = "ja-JP";
            {
                const std::string languageUrl =
                    std::string(kKaaBase) + "/api/show/" + encode_url_component(slug) + "/language";
                if (http_request(languageUrl, nullptr, response, 20))
                {
                    const bool hasJapanese = response.find("ja-JP") != std::string::npos;
                    const bool hasEnglish = response.find("en-US") != std::string::npos;
                    if (hasJapanese)
                        lang = "ja-JP";
                    else if (hasEnglish)
                        lang = "en-US";
                    log_stage(("KAA LANGUAGE selected=" + lang).c_str());
                }
                else
                {
                    log_stage("KAA LANGUAGE REQUEST FAILED; trying ja-JP directly");
                }
            }

            for (const char* candidate : { lang.c_str(), "ja-JP", "en-US" })
            {
                bool duplicate = false;
                if (std::string(candidate) == "ja-JP" && lang == "ja-JP" && candidate != lang.c_str())
                    duplicate = true;
                if (std::string(candidate) == "en-US" && lang == "en-US" && candidate != lang.c_str())
                    duplicate = true;
                if (duplicate) continue;

                episodes.clear();
                bool pageFailed = false;
                int pageCount = 1;

                for (int page = 1; page <= pageCount; ++page)
                {
                    const std::string episodeUrl =
                        std::string(kKaaBase) + "/api/show/" +
                        encode_url_component(slug) + "/episodes?page=" +
                        std::to_string(page) + "&lang=" + encode_url_component(candidate);

                    char marker[192];
                    std::snprintf(marker, sizeof(marker),
                        "KAA EPISODES REQUEST page=%d lang=%s",
                        page, candidate);
                    log_stage(marker);

                    if (!http_request(episodeUrl, nullptr, response, 20))
                    {
                        pageFailed = true;
                        break;
                    }

                    const std::string result = first_array(response, { "result" });
                    const std::vector<std::string> objects = json_object_array(result);
                    if (page == 1)
                    {
                        const std::string pages = json_array_field(response, "pages");
                        if (!pages.empty())
                        {
                            // KAA's \`pages\` field is an array; AnikkuNX uses
                            // its element count as the number of episode pages.
                            int discovered = 1;
                            int depth = 0;
                            bool inString = false;
                            bool escaped = false;
                            for (size_t pi = 1; pi + 1 < pages.size(); ++pi)
                            {
                                const char ch = pages[pi];
                                if (inString)
                                {
                                    if (escaped) escaped = false;
                                    else if (ch == '\\') escaped = true;
                                    else if (ch == '"') inString = false;
                                    continue;
                                }
                                if (ch == '"') inString = true;
                                else if (ch == '[' || ch == '{') ++depth;
                                else if (ch == ']' || ch == '}') --depth;
                                else if (ch == ',' && depth == 0) ++discovered;
                            }
                            if (discovered > 0) pageCount = discovered;
                        }
                    }

                    for (const std::string& object : objects)
                    {
                        const std::string numberText = first_string(
                            object, { "episode_string", "episodeNumber", "episode" });
                        const std::string episodeSlug = first_string(object, { "slug" });
                        if (numberText.empty() || episodeSlug.empty())
                            continue;

                        ProviderEpisode item;
                        item.number = std::atoi(numberText.c_str());
                        if (item.number <= 0)
                            continue;
                        item.title = first_string(object, { "title" });
                        if (item.title.empty())
                            item.title = "Episode " + std::to_string(item.number);
                        item.id = "/" + slug + "/ep-" + numberText + "-" + episodeSlug;
                        item.provider = "KickAssAnime";
                        item.category = candidate;
                        episodes.push_back(std::move(item));
                    }

                    std::snprintf(marker, sizeof(marker),
                        "KAA EPISODES PAGE DONE page=%d count=%zu totalPages=%d",
                        page, objects.size(), pageCount);
                    log_stage(marker);
                }

                if (!pageFailed && !episodes.empty())
                    break;
            }

            std::stable_sort(episodes.begin(), episodes.end(),
                [](const ProviderEpisode& a, const ProviderEpisode& b) {
                    return a.number < b.number;
                });

            episodes.erase(std::unique(episodes.begin(), episodes.end(),
                [](const ProviderEpisode& a, const ProviderEpisode& b) {
                    return a.number == b.number && a.id == b.id;
                }), episodes.end());

            char marker[160];
            std::snprintf(marker, sizeof(marker),
                "KAA EPISODES FINAL count=%zu slug=%s",
                episodes.size(), slug.c_str());
            log_stage(marker);
            break;
        }
        case ApiSourceId::Anichi:
        case ApiSourceId::Anikoto:
        {
            const int sourceIndex =
                sourceId - static_cast<int>(ApiSourceId::Anichi);
            const char* baseUrl = anikoto::base_for_source(sourceIndex);
            if (!baseUrl || !*baseUrl)
            {
                status = std::string(api_source_name(sourceId)) + " has no configured site URL.";
                break;
            }

            for (const std::string& title : provider_search_titles(anime))
            {
                std::string sourceStatus;
                const std::vector<anikoto::Episode> found =
                    anikoto::fetch_episodes(title, baseUrl, sourceStatus);
                if (found.empty())
                {
                    log_stage((std::string(api_source_name(sourceId)) +
                        " title lookup miss; trying next AniList title").c_str());
                    continue;
                }

                for (const anikoto::Episode& ep : found)
                {
                    ProviderEpisode item;
                    item.number = ep.number;
                    item.title = ep.title;
                    item.id = ep.id;
                    item.provider = api_source_name(sourceId);
                    item.category = "Sub";
                    episodes.push_back(std::move(item));
                }
                break;
            }
            break;
        }
    }

    if (!episodes.empty())
    {
        status = std::string(api_source_name(sourceId)) + " returned " +
            std::to_string(episodes.size()) + " episodes.";
        char marker[160];
        std::snprintf(marker, sizeof(marker), "EPISODE PROVIDER READY source=%d count=%zu",
            sourceId, episodes.size());
        log_stage(marker);
        return episodes;
    }

    if (status.empty())
        status = std::string(api_source_name(sourceId)) +
            " returned no episodes. Check the title match and network.";
    log_stage("EPISODE PROVIDER REQUEST EMPTY OR FAILED");
    return {};
}

static void clear_box(brls::Box* box)
{
    if (!box) return;
    const auto children = box->getChildren();
    for (brls::View* child : children)
        box->removeView(child);
}


class PlaybackPreviewActivity : public brls::Activity
{
public:
    PlaybackPreviewActivity(SaikouAnime anime, int episode, int sourceId, std::string quality)
        : m_anime(std::move(anime)), m_episode(episode), m_sourceId(sourceId), m_quality(std::move(quality))
    {
    }

    brls::View* createContentView() override
    {
        brls::Box* root = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(root);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);
        root->setPadding(30.0f);
        root->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText("PLAYER");
        heading->setFontSize(28.0f);
        heading->setTextColor(nvgRGB(244, 246, 250));
        root->addView(heading);

        char episodeText[64];
        std::snprintf(episodeText, sizeof(episodeText), "%s - Episode %d",
            m_anime.title.c_str(), m_episode);
        brls::Label* details = new brls::Label();
        details->setText(episodeText);
        details->setFontSize(17.0f);
        details->setTextColor(nvgRGB(174, 184, 200));
        details->setMargins(0, 5, 0, 0);
        root->addView(details);

        brls::Box* viewport = new brls::Box();
        viewport->setDimensions(1160.0f, 470.0f);
        viewport->setMargins(0, 18, 0, 0);
        viewport->setBackgroundColor(nvgRGB(0, 0, 0));
        viewport->setFocusable(false);
        root->addView(viewport);

        const std::string message =
            "The player surface is ready for " + std::string(api_source_name(m_sourceId)) +
            " at " + m_quality +
            ". A stream URL and video playback backend are not connected yet.";
        brls::Label* status = new brls::Label();
        status->setText(message);
        status->setFontSize(16.0f);
        status->setLineHeight(22.0f);
        status->setTextColor(nvgRGB(174, 184, 200));
        status->setMargins(0, 12, 0, 0);
        status->setFocusable(false);
        root->addView(status);

        brls::Label* back = new brls::Label();
        back->setText("Press B to return to episode selection.");
        back->setFontSize(14.0f);
        back->setTextColor(nvgRGB(135, 147, 166));
        back->setMargins(0, 12, 0, 0);
        back->setFocusable(false);
        root->addView(back);
        return root;
    }

private:
    SaikouAnime m_anime;
    int m_episode = 0;
    int m_sourceId = 0;
    std::string m_quality;
};

class EpisodeStreamActivity;
static EpisodeStreamActivity* g_episodeStreamActivity = nullptr;

class EpisodeStreamActivity : public brls::Activity
{
public:
    EpisodeStreamActivity(SaikouAnime anime, ProviderEpisode episode, int sourceId)
        : m_anime(std::move(anime)), m_providerEpisode(std::move(episode)), m_sourceId(sourceId)
    {
        g_episodeStreamActivity = this;
    }

    ~EpisodeStreamActivity() override
    {
        m_lifetime->store(false, std::memory_order_release);
        if (m_worker.joinable()) m_worker.join();
        if (g_episodeStreamActivity == this) g_episodeStreamActivity = nullptr;
    }

    brls::View* createContentView() override
    {
        brls::Box* root = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(root);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);
        root->setPadding(30.0f);
        root->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText(m_providerEpisode.title);
        heading->setFontSize(28.0f);
        heading->setTextColor(nvgRGB(244, 246, 250));
        root->addView(heading);

        brls::Label* animeTitle = new brls::Label();
        animeTitle->setText(m_anime.title);
        animeTitle->setFontSize(18.0f);
        animeTitle->setTextColor(nvgRGB(174, 184, 200));
        animeTitle->setMargins(0, 5, 0, 0);
        root->addView(animeTitle);

        m_sourceStatus = new brls::Label();
        m_sourceStatus->setFontSize(15.0f);
        m_sourceStatus->setLineHeight(21.0f);
        m_sourceStatus->setTextColor(nvgRGB(174, 184, 200));
        m_sourceStatus->setMargins(0, 16, 0, 0);
        m_sourceStatus->setFocusable(false);
        if (m_sourceId == static_cast<int>(ApiSourceId::KickAssAnime) ||
            m_sourceId == static_cast<int>(ApiSourceId::Anichi) ||
            m_sourceId == static_cast<int>(ApiSourceId::Anikoto))
        {
            m_sourceStatus->setText(
                "Loading available sources from " +
                std::string(api_source_name(m_sourceId)) + "...");
        }
        else
        {
            m_sourceStatus->setText("Source extraction for this provider is not connected yet.");
        }
        root->addView(m_sourceStatus);

        // While source extraction runs, trap focus inside this activity.
        m_focusSink = new brls::Padding();
        m_focusSink->setWidth(1.0f);
        m_focusSink->setHeight(1.0f);
        m_focusSink->alpha = 0.0f;
        m_focusSink->setFocusable(true);
        root->addView(m_focusSink);

        m_streamRow = new brls::Box(brls::Axis::ROW);
        m_streamRow->setWidthPercentage(100.0f);
        m_streamRow->setHeight(70.0f);
        m_streamRow->setMargins(0, 12, 0, 0);
        root->addView(m_streamRow);

        brls::Label* playerStatus = new brls::Label();
        playerStatus->setText(
            "Select a stream with A to start native playback. Press B to return.");
        playerStatus->setFontSize(14.0f);
        playerStatus->setTextColor(nvgRGB(135, 147, 166));
        playerStatus->setMargins(0, 18, 0, 0);
        playerStatus->setFocusable(false);
        root->addView(playerStatus);

        if (m_sourceId == static_cast<int>(ApiSourceId::KickAssAnime) ||
            m_sourceId == static_cast<int>(ApiSourceId::Anichi) ||
            m_sourceId == static_cast<int>(ApiSourceId::Anikoto))
            start_load();
        brls::Application::giveFocus(m_focusSink);
        return root;
    }

    void tick()
    {
        if (!m_ready.load(std::memory_order_acquire)) return;
        if (m_worker.joinable()) m_worker.join();
        m_ready.store(false, std::memory_order_release);
        if (m_sourceStatus) m_sourceStatus->setText(m_statusText);
        clear_box(m_streamRow);
        m_streamChoices.clear();
        if (m_streams.empty())
        {
            // Do not render an unfocusable fake button. The status label above is the
            // authoritative result of extraction, and the invisible sink keeps focus
            // from leaking back into the episode list underneath.
            if (m_focusSink)
                m_focusSink->setFocusable(true);
            return;
        }

        if (m_focusSink)
            m_focusSink->setFocusable(false);

        for (size_t i = 0; i < m_streams.size(); ++i)
        {
            const ProviderStream& stream = m_streams[i];
            const std::string label =
                (stream.quality.empty() ? std::string("Stream") : stream.quality) +
                (stream.type.empty() ? std::string() : "  " + stream.type);
            brls::Box* choice = make_option(label, i == m_selectedStream);
            choice->registerAction("Play scraper stream option", brls::BUTTON_A,
                [this, i, label](brls::View*) {
                    m_selectedStream = i;
                    const ProviderStream& selected = m_streams[m_selectedStream];
                    log_stage("NATIVE PLAYER OPEN: selected scraper stream");
                    brls::Application::pushActivity(
                        new SaikouMpvPlayerActivity(
                            m_anime.title,
                            m_providerEpisode.title,
                            label,
                            selected.url,
                            selected.headers),
                        brls::TransitionAnimation::NONE);
                    return true;
                });
            m_streamChoices.push_back(choice);
            m_streamRow->addView(choice);
        }
        if (!m_streamChoices.empty())
            brls::Application::giveFocus(m_streamChoices[0]);
    }

private:
    SaikouAnime m_anime;
    ProviderEpisode m_providerEpisode;
    int m_sourceId = 0;
    size_t m_selectedStream = 0;
    brls::Box* m_streamRow = nullptr;
    brls::Label* m_sourceStatus = nullptr;
    std::vector<brls::Box*> m_streamChoices;
    std::vector<ProviderStream> m_streams;
    std::string m_statusText;
    brls::Padding* m_focusSink = nullptr;
    std::thread m_worker;
    std::atomic<bool> m_ready{ false };
    std::shared_ptr<std::atomic<bool>> m_lifetime =
        std::make_shared<std::atomic<bool>>(true);

    brls::Box* make_option(const std::string& text, bool selected)
    {
        brls::Box* option = new brls::Box(brls::Axis::COLUMN);
        option->setWidth(210.0f);
        option->setHeight(58.0f);
        option->setMargins(0, 0, 9, 0);
        option->setPadding(9.0f);
        option->setBackgroundColor(selected ? nvgRGB(31, 64, 79) : nvgRGB(27, 34, 48));
        option->setBorderColor(selected ? nvgRGB(67, 190, 218) : nvgRGB(48, 57, 74));
        option->setBorderThickness(selected ? 2.0f : 1.0f);
        option->setCornerRadius(8.0f);
        option->setFocusable(true);
        brls::Label* label = new brls::Label();
        label->setText(text);
        label->setFontSize(15.0f);
        label->setTextColor(nvgRGB(244, 246, 250));
        label->setFocusable(false);
        option->addView(label);
        return option;
    }

    void start_load()
    {
        const auto lifetime = m_lifetime;
        const ProviderEpisode episode = m_providerEpisode;
        m_worker = std::thread([this, lifetime, episode] {
            if (m_sourceId == static_cast<int>(ApiSourceId::KickAssAnime))
            {
                perf_log("KAA SOURCE REQUEST START");
                m_streams = fetch_kaa_sources(episode, m_statusText);
            }
            else if (m_sourceId == static_cast<int>(ApiSourceId::Anichi) ||
                     m_sourceId == static_cast<int>(ApiSourceId::Anikoto))
            {
                const int sourceIndex =
                    m_sourceId - static_cast<int>(ApiSourceId::Anichi);
                const char* baseUrl = anikoto::base_for_source(sourceIndex);
                perf_log("ANIKOTO STREAM REQUEST START");
                anikoto::Episode sourceEpisode;
                sourceEpisode.number = episode.number;
                sourceEpisode.title = episode.title;
                sourceEpisode.id = episode.id;
                const std::vector<anikoto::Stream> found =
                    anikoto::fetch_streams(sourceEpisode, baseUrl, m_statusText);
                m_streams.clear();
                for (const anikoto::Stream& stream : found)
                    m_streams.push_back({stream.url, stream.quality, stream.type, stream.headers});
                char marker[160];
                std::snprintf(marker, sizeof(marker), "%s STREAMS READY count=%zu",
                    api_source_name(m_sourceId), m_streams.size());
                log_stage(marker);
            }
            else
            {
                m_statusText = "No stream handler is registered for this provider.";
            }
            if (!lifetime->load(std::memory_order_acquire)) return;
            m_ready.store(true, std::memory_order_release);
        });
    }
};

class EpisodeListActivity;
static EpisodeListActivity* g_episodeListActivity = nullptr;

class EpisodeListActivity : public brls::Activity
{
public:
    EpisodeListActivity(SaikouAnime anime, int sourceId)
        : m_anime(std::move(anime)), m_sourceId(sourceId)
    {
        g_episodeListActivity = this;
    }

    ~EpisodeListActivity() override
    {
        m_lifetime->store(false, std::memory_order_release);
        if (m_worker.joinable()) m_worker.join();
        if (g_episodeListActivity == this) g_episodeListActivity = nullptr;
    }

    brls::View* createContentView() override
    {
        m_content = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(m_content);
        m_content->setWidthPercentage(100.0f);
        m_content->setHeightPercentage(100.0f);
        m_content->setPadding(30.0f);
        m_content->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText("EPISODES");
        heading->setFontSize(28.0f);
        heading->setTextColor(nvgRGB(244, 246, 250));
        m_content->addView(heading);

        brls::Label* animeTitle = new brls::Label();
        animeTitle->setText(m_anime.title);
        animeTitle->setFontSize(17.0f);
        animeTitle->setTextColor(nvgRGB(174, 184, 200));
        animeTitle->setMargins(0, 4, 0, 0);
        m_content->addView(animeTitle);

        m_status = new brls::Label();
        m_status->setFontSize(14.0f);
        m_status->setTextColor(nvgRGB(135, 147, 166));
        m_status->setMargins(0, 8, 0, 0);
        m_status->setFocusable(false);
        m_content->addView(m_status);

        // Keep focus inside this activity while the async provider request is running.
        // Otherwise Borealis can fall back to the previous activity's focused provider buttons.
        m_focusSink = new brls::Padding();
        m_focusSink->setWidth(1.0f);
        m_focusSink->setHeight(1.0f);
        m_focusSink->alpha = 0.0f;
        m_focusSink->setFocusable(true);
        m_content->addView(m_focusSink);

        m_scroll = new brls::ScrollingFrame();
        m_scroll->setWidthPercentage(100.0f);
        m_scroll->setGrow(1.0f);
        m_scroll->setMargins(0, 10, 0, 0);
        m_scroll->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);
        m_rows = new brls::Box(brls::Axis::COLUMN);
        m_rows->setWidth(1160.0f);
        m_rows->setHeight(600.0f);
        m_scroll->setContentView(m_rows);
        m_content->addView(m_scroll);

        m_status->setText("Loading episode list from " + std::string(api_source_name(m_sourceId)) + "...");
        start_load();
        brls::Application::giveFocus(m_focusSink);
        return m_content;
    }

    void tick()
    {
        if (!m_ready.load(std::memory_order_acquire))
            return;
        if (m_worker.joinable()) m_worker.join();
        m_ready.store(false, std::memory_order_release);
        if (m_status) m_status->setText(m_statusText);

        clear_box(m_rows);
        if (m_focusSink)
            m_focusSink->setFocusable(false);
        if (m_episodes.empty())
        {
            brls::Box* back = new brls::Box(brls::Axis::ROW);
            back->setWidth(300.0f);
            back->setHeight(56.0f);
            back->setMargins(0, 18, 0, 0);
            back->setPadding(12.0f);
            back->setAlignItems(brls::AlignItems::CENTER);
            back->setBackgroundColor(nvgRGB(27, 34, 48));
            back->setBorderColor(nvgRGB(48, 57, 74));
            back->setBorderThickness(1.0f);
            back->setCornerRadius(8.0f);
            back->setFocusable(true);
            brls::Label* backLabel = new brls::Label();
            backLabel->setText("BACK TO ANIME");
            backLabel->setFontSize(16.0f);
            backLabel->setTextColor(nvgRGB(244, 246, 250));
            backLabel->setFocusable(false);
            back->addView(backLabel);
            back->registerAction("Return to anime details", brls::BUTTON_A, [](brls::View*) {
                brls::sync([] {
                    brls::Application::popActivity(brls::TransitionAnimation::NONE, [] {}, true);
                });
                return true;
            });
            m_rows->addView(back);
            brls::Application::giveFocus(back);
            return;
        }

        for (size_t begin = 0; begin < m_episodes.size(); begin += 50)
        {
            const size_t finish = std::min(begin + 50, m_episodes.size());
            const size_t batch = finish - begin;
            brls::Label* rowHeading = new brls::Label();
            rowHeading->setText("EPISODES " + std::to_string(begin + 1) +
                "–" + std::to_string(finish));
            rowHeading->setFontSize(17.0f);
            rowHeading->setTextColor(nvgRGB(220, 228, 240));
            rowHeading->setFocusable(false);
            rowHeading->setMargins(0, begin == 0 ? 0 : 12, 0, 0);
            m_rows->addView(rowHeading);

            brls::HScrollingFrame* horizontal = new brls::HScrollingFrame();
            horizontal->setWidth(1160.0f);
            horizontal->setHeight(70.0f);
            horizontal->setMargins(0, 5, 0, 0);
            horizontal->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);
            brls::Box* row = new brls::Box(brls::Axis::ROW);
            row->setWidth(std::max(1160.0f, static_cast<float>(batch) * 190.0f));
            row->setHeight(62.0f);
            for (size_t i = begin; i < finish; ++i)
            {
                const ProviderEpisode& episode = m_episodes[i];
                brls::Box* tile = new brls::Box(brls::Axis::COLUMN);
                tile->setWidth(182.0f);
                tile->setHeight(54.0f);
                tile->setPadding(8.0f);
                tile->setMargins(0, 0, 8, 0);
                tile->setBackgroundColor(nvgRGB(27, 34, 48));
                tile->setBorderColor(nvgRGB(48, 57, 74));
                tile->setBorderThickness(1.0f);
                tile->setCornerRadius(7.0f);
                tile->setFocusable(true);
                brls::Label* label = new brls::Label();
                std::string episodeLabel = "Episode " + std::to_string(episode.number);
                if (!episode.provider.empty())
                    episodeLabel += "  " + episode.provider;
                if (!episode.category.empty())
                    episodeLabel += "  " + episode.category;
                if (!episode.title.empty() && episode.title != "Episode " + std::to_string(episode.number))
                    episodeLabel += " — " + episode.title;
                label->setText(episodeLabel);
                label->setFontSize(15.0f);
                label->setTextColor(nvgRGB(244, 246, 250));
                label->setFocusable(false);
                tile->addView(label);
                tile->registerAction("Select provider episode", brls::BUTTON_A,
                    [this, episode](brls::View*) {
                        log_stage("EPISODE SELECTED FROM PROVIDER");
                        brls::Application::pushActivity(
                            new EpisodeStreamActivity(m_anime, episode, m_sourceId),
                            brls::TransitionAnimation::NONE);
                        return true;
                    });
                row->addView(tile);
            }
            row->setDefaultFocusedIndex(0);
            horizontal->setContentView(row);
            m_rows->addView(horizontal);
        }

        m_rows->setHeight(std::max(600.0f,
            static_cast<float>((m_episodes.size() + 49) / 50) * 112.0f + 80.0f));
        if (m_episodes.size() && m_rows)
        {
            m_rows->setDefaultFocusedIndex(1);
            if (m_rows->getDefaultFocus())
                brls::Application::giveFocus(m_rows->getDefaultFocus());
        }
        perf_log_count("PROVIDER EPISODE TILES RENDERED", m_episodes.size());
    }

private:
    SaikouAnime m_anime;
    int m_sourceId = 0;
    brls::Box* m_content = nullptr;
    brls::Label* m_status = nullptr;
    brls::ScrollingFrame* m_scroll = nullptr;
    brls::Box* m_rows = nullptr;
    std::thread m_worker;
    std::atomic<bool> m_ready{ false };
    std::shared_ptr<std::atomic<bool>> m_lifetime =
        std::make_shared<std::atomic<bool>>(true);
    std::vector<ProviderEpisode> m_episodes;
    std::string m_statusText;
    brls::Padding* m_focusSink = nullptr;

    void start_load()
    {
        if (m_worker.joinable()) m_worker.join();
        m_ready.store(false, std::memory_order_release);
        const auto lifetime = m_lifetime;
        const SaikouAnime anime = m_anime;
        const int sourceId = m_sourceId;
        m_worker = std::thread([this, lifetime, anime, sourceId] {
            perf_log("PROVIDER EPISODE REQUEST START");
            m_episodes = fetch_provider_episodes(anime, sourceId, m_statusText);
            if (!lifetime->load(std::memory_order_acquire)) return;
            m_ready.store(true, std::memory_order_release);
        });
    }
};

class AnimeDetailsActivity;
static AnimeDetailsActivity* g_animeDetailsActivity = nullptr;

class AnimeDetailsActivity : public brls::Activity
{
public:
    explicit AnimeDetailsActivity(SaikouAnime anime) : anime(std::move(anime))
    {
        g_animeDetailsActivity = this;
    }

    ~AnimeDetailsActivity() override
    {
        if (m_bannerWorker.joinable())
            m_bannerWorker.join();
        if (g_animeDetailsActivity == this)
            g_animeDetailsActivity = nullptr;
    }

    brls::View* createContentView() override
    {
        log_stage("ACTIVITY OPEN: anime details");
        m_content = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(m_content);
        m_content->setWidthPercentage(100.0f);
        m_content->setHeightPercentage(100.0f);
        m_content->setPadding(30.0f);
        m_content->setBackgroundColor(nvgRGB(16, 20, 29));
        log_stage("DETAIL VIEW BUILT");
        return m_content;
    }

    void onContentAvailable() override
    {
        brls::Box* root = m_content;
        if (!root) return;
        log_stage("DETAIL CONTENT BUILT");

        brls::Label* heading = new brls::Label();
        heading->setText(anime.title);
        heading->setFontSize(30.0f);
        heading->setTextColor(nvgRGB(244, 246, 250));
        root->addView(heading);

        std::string meta;
        if (anime.score > 0)
            meta = "AniList score " + std::to_string(anime.score) + "/100";
        if (!anime.format.empty())
            meta += (meta.empty() ? "" : "    ") + anime.format;
        // AniList episode counts are metadata only and can be absent for long-running series.
        // Episode rows must be populated by the selected streaming provider.

        brls::Label* metaLabel = new brls::Label();
        metaLabel->setText(meta);
        metaLabel->setFontSize(15.0f);
        metaLabel->setTextColor(nvgRGB(174, 184, 200));
        metaLabel->setMargins(0, 6, 0, 0);
        root->addView(metaLabel);

        if (!anime.bannerUrl.empty())
        {
            m_bannerSlot = new brls::Box();
            m_bannerSlot->setDimensions(1160.0f, 150.0f);
            m_bannerSlot->setMargins(0, 12, 0, 0);
            m_bannerSlot->setBackgroundColor(nvgRGB(27, 34, 48));
            m_bannerSlot->setFocusable(false);
            root->addView(m_bannerSlot);
            m_bannerPath = cached_banner_path(anime.id);
            m_bannerWorker = std::thread([this] {
                m_bannerDownloaded = download_image(anime.bannerUrl, m_bannerPath);
                m_bannerReady.store(true, std::memory_order_release);
            });
        }

        brls::Box* summary = new brls::Box(brls::Axis::ROW);
        summary->setHeight(190.0f);
        summary->setMargins(0, 12, 0, 0);
        if (anime.posterPath.empty())
            anime.posterPath = cached_cover_path(anime.id);
        struct stat posterStat;
        if (stat(anime.posterPath.c_str(), &posterStat) == 0 && posterStat.st_size > 256)
        {
            brls::Image* poster = new brls::Image();
            poster->setDimensions(126.0f, 184.0f);
            poster->setScalingType(brls::ImageScalingType::FIT);
            poster->setImageFromFile(anime.posterPath);
            poster->setFocusable(false);
            summary->addView(poster);
        }

        brls::Box* text = new brls::Box(brls::Axis::COLUMN);
        text->setWidth(1010.0f);
        text->setMargins(18, 0, 0, 0);
        summary->addView(text);

        brls::Label* details = new brls::Label();
        std::string description = anime.description;
        if (description.size() > 840)
        {
            size_t cut = 840;
            while (cut > 0 && (static_cast<unsigned char>(description[cut]) & 0xC0) == 0x80) --cut;
            description.resize(cut);
            description += "...";
        }
        details->setText(description.empty() ? "No description is available from AniList." : description);
        details->setFontSize(16.0f);
        details->setLineHeight(21.0f);
        details->setTextColor(nvgRGB(220, 228, 240));
        details->setFocusable(false);
        text->addView(details);
        root->addView(summary);

        brls::Label* sourcesHeading = new brls::Label();
        sourcesHeading->setText("WATCH SOURCES");
        sourcesHeading->setFontSize(20.0f);
        sourcesHeading->setTextColor(nvgRGB(220, 228, 240));
        sourcesHeading->setMargins(0, 12, 0, 4);
        root->addView(sourcesHeading);

        // This is a horizontal focus row, not a page-wide ScrollingFrame.
        // A ScrollingFrame does not reliably move focus when the user presses
        // RIGHT on a child, so the old implementation stopped at KickAssAnime.
        // Let the Box own the focus chain and explicitly scroll the viewport
        // whenever the focused provider moves beyond the visible area.
        brls::Box* sourceViewport = new brls::Box(brls::Axis::ROW);
        sourceViewport->setWidthPercentage(100.0f);
        sourceViewport->setHeight(58.0f);
        sourceViewport->setMargins(0, 0, 0, 0);

        brls::Box* sources = new brls::Box(brls::Axis::ROW);
        sources->setWidth(1160.0f + static_cast<float>(kApiSourceCount) * 185.0f);
        sources->setHeight(46.0f);
        for (size_t i = 0; i < kApiSourceCount; ++i)
        {
            if (!g_providerEnabled[i]) continue;
            const ApiSourceInfo& source = kApiSources[i];
            brls::Box* choice = make_source_choice(source);
            sources->addView(choice);
        }
        if (sources->getChildren().empty())
        {
            brls::Label* empty = new brls::Label();
            empty->setText("Enable a source API in Settings to continue.");
            empty->setFontSize(16.0f);
            empty->setTextColor(nvgRGB(174, 184, 200));
            sources->addView(empty);
        }
        sourceViewport->addView(sources);
        root->addView(sourceViewport);

        m_sourceStatus = new brls::Label();
        m_sourceStatus->setFontSize(14.0f);
        m_sourceStatus->setTextColor(nvgRGB(174, 184, 200));
        m_sourceStatus->setMargins(0, 7, 0, 0);
        m_sourceStatus->setText("Choose a provider. Episode and stream lookup are not connected in this build yet.");
        root->addView(m_sourceStatus);
    }

    void tick()
    {
        if (!m_bannerReady.load(std::memory_order_acquire))
            return;
        if (m_bannerWorker.joinable())
            m_bannerWorker.join();
        m_bannerReady.store(false, std::memory_order_release);
        if (m_bannerDownloaded && m_bannerSlot)
        {
            brls::Image* banner = new brls::Image();
            banner->setDimensions(1160.0f, 150.0f);
            banner->setScalingType(brls::ImageScalingType::FILL);
            banner->setImageFromFile(m_bannerPath);
            banner->setFocusable(false);
            m_bannerSlot->addView(banner);
            log_stage("DETAIL BANNER READY");
        }
        else
        {
            log_stage("DETAIL BANNER UNAVAILABLE");
        }
    }

private:
    SaikouAnime anime;
    brls::Box* m_content = nullptr;
    brls::Box* m_bannerSlot = nullptr;
    brls::Label* m_sourceStatus = nullptr;
    std::string m_bannerPath;
    std::thread m_bannerWorker;
    std::atomic<bool> m_bannerReady{ false };
    bool m_bannerDownloaded = false;

    brls::Box* make_source_choice(const ApiSourceInfo& source)
    {
        brls::Box* choice = new brls::Box(brls::Axis::COLUMN);
        choice->setWidth(175.0f);
        choice->setHeight(42.0f);
        choice->setMargins(0, 10, 0, 0);
        choice->setPadding(8.0f);
        choice->setBackgroundColor(nvgRGB(27, 34, 48));
        choice->setBorderColor(nvgRGB(48, 57, 74));
        choice->setBorderThickness(1.0f);
        choice->setCornerRadius(8.0f);
        choice->setFocusable(true);

        brls::Label* label = new brls::Label();
        label->setText(source.name);
        label->setFontSize(15.0f);
        choice->addView(label);
        choice->registerAction("Select source", brls::BUTTON_A,
            [this, id = static_cast<int>(source.id), name = std::string(source.name)](brls::View*) {
                g_selectedApiSource = id;
                save_source_settings();
                if (m_sourceStatus)
                    m_sourceStatus->setText("Selected " + name + ". Opening episode list.");
                brls::Application::pushActivity(
                    new EpisodeListActivity(anime, id),
                    brls::TransitionAnimation::NONE);
                log_stage("DETAIL SOURCE SELECTED");
                return true;
            });
        return choice;
    }
};

static void open_anime_details(const SaikouAnime& anime)
{
    char marker[128];
    const auto stack = brls::Application::getActivitiesStack();
    std::snprintf(marker, sizeof(marker),
        "ANIME CARD OPENED: id=%d stack_before=%zu", anime.id, stack.size());
    log_stage(marker);
    brls::Application::pushActivity(
        new AnimeDetailsActivity(anime),
        brls::TransitionAnimation::NONE);
    log_stage("DETAIL ACTIVITY PUSH RETURNED");
}

static std::string compact_card_title(const std::string& title)
{
    if (title.size() <= 34) return title;
    size_t cut = 34;
    while (cut > 0 && (static_cast<unsigned char>(title[cut]) & 0xC0) == 0x80) --cut;
    return title.substr(0, cut) + "...";
}

static brls::Box* make_anime_card(
    const SaikouAnime& anime,
    const std::string& subtitle = std::string(),
    brls::Image** imageOut = nullptr)
{
    brls::Box* card = new brls::Box(brls::Axis::COLUMN);
    card->setWidth(184.0f);
    card->setHeight(subtitle.empty() ? 244.0f : 260.0f);
    card->setPadding(6.0f);
    card->setMargins(3, 7, 3, 0);
    card->setBackgroundColor(nvgRGB(27, 34, 48));
    card->setBorderColor(nvgRGB(48, 57, 74));
    card->setBorderThickness(1.0f);
    card->setCornerRadius(9.0f);
    card->setFocusable(true);
    card->setHighlightPadding(4.0f);

    const std::string imagePath = anime.posterPath.empty()
        ? cached_cover_path(anime.id)
        : anime.posterPath;
    struct stat st;
    const float posterHeight = subtitle.empty() ? 168.0f : 148.0f;

    brls::Image* poster = new brls::Image();
    poster->setDimensions(168.0f, posterHeight);
    poster->setScalingType(brls::ImageScalingType::FIT);
    poster->setBackgroundColor(nvgRGB(35, 45, 62));
    poster->setFocusable(false);

    if (stat(imagePath.c_str(), &st) == 0 && st.st_size > 256)
        poster->setImageFromFile(imagePath);

    if (imageOut)
        *imageOut = poster;

    card->addView(poster);

    brls::Label* score = new brls::Label();
    score->setText(anime.score > 0 ? "AniList  " + std::to_string(anime.score) + "/100" : "AniList score unavailable");
    score->setFontSize(12.0f);
    score->setTextColor(nvgRGB(97, 207, 226));
    score->setMargins(0, 3, 0, 0);
    score->setFocusable(false);
    card->addView(score);

    brls::Label* title = new brls::Label();
    title->setText(compact_card_title(anime.title));
    title->setFontSize(14.0f);
    title->setSingleLine(false);
    title->setTextColor(nvgRGB(244, 246, 250));
    title->setMargins(0, 2, 0, 0);
    title->setFocusable(false);
    card->addView(title);

    if (!subtitle.empty())
    {
        brls::Label* progress = new brls::Label();
        progress->setText(subtitle);
        progress->setFontSize(11.0f);
        progress->setTextColor(nvgRGB(174, 184, 200));
        progress->setMargins(0, 2, 0, 0);
        progress->setFocusable(false);
        card->addView(progress);
    }

    card->registerAction("Open anime details", brls::BUTTON_A, [anime](brls::View*) {
        open_anime_details(anime);
        return true;
    });
    return card;
}

static void render_horizontal_anime_cards(brls::Box* container, const std::vector<SaikouAnime>& items)
{
    if (!container) return;
    clear_box(container);

    // Keep the HScrollingFrame content view wide enough to contain the complete
    // card footprint, including its horizontal margins, so the final card remains
    // reachable by focus/navigation.
    const float contentWidth = std::max(1160.0f, static_cast<float>(items.size()) * 192.0f);
    container->setWidth(contentWidth);

    brls::Box* row = new brls::Box(brls::Axis::ROW);
    row->setWidth(contentWidth);
    row->setHeight(252.0f);
    row->setAlignItems(brls::AlignItems::FLEX_START);

    for (const SaikouAnime& anime : items)
        row->addView(make_anime_card(anime));

    container->addView(row);

    if (items.empty())
    {
        brls::Label* empty = new brls::Label();
        empty->setText("No titles to show.");
        empty->setFontSize(16.0f);
        empty->setTextColor(nvgRGB(174, 184, 200));
        row->addView(empty);
    }
}

static void render_anime_cards(brls::Box* container, const std::vector<SaikouAnime>& items)
{
    if (!container) return;
    clear_box(container);

    constexpr size_t perRow = 6;
    for (size_t start = 0; start < items.size(); start += perRow)
    {
        brls::Box* row = new brls::Box(brls::Axis::ROW);
        row->setWidth(1160.0f);
        row->setHeight(252.0f);
        row->setAlignItems(brls::AlignItems::FLEX_START);
        const size_t stop = std::min(items.size(), start + perRow);
        for (size_t i = start; i < stop; ++i)
            row->addView(make_anime_card(items[i]));
        container->addView(row);
    }

    if (items.empty())
    {
        brls::Label* empty = new brls::Label();
        empty->setText("No titles to show.");
        empty->setFontSize(16.0f);
        empty->setTextColor(nvgRGB(174, 184, 200));
        container->addView(empty);
    }
}

// Shared by Search, Trending, Currently Airing, and Continue Watching.
// Keep the implementation below the activity definitions, but declare it here
// so earlier activity classes can safely reference the shared Load More UI.
static brls::Box* make_home_load_more_card(std::function<void()> callback);

class SearchActivity;
static SearchActivity* g_searchActivity = nullptr;

class SearchActivity : public brls::Activity
{
public:
    SearchActivity() { g_searchActivity = this; }

    ~SearchActivity() override
    {
        m_lifetime->store(false, std::memory_order_release);
        if (m_worker.joinable())
            m_worker.join();
        if (m_imageWorker.joinable())
            m_imageWorker.join();
        if (g_searchActivity == this)
            g_searchActivity = nullptr;
    }

    brls::View* createContentView() override
    {
        log_stage("ACTIVITY OPEN: Search");
        brls::Box* root = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(root);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);
        root->setPadding(30.0f);
        root->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText("SEARCH ANIME");
        heading->setFontSize(28.0f);
        root->addView(heading);

        m_searchButton = new brls::Box(brls::Axis::ROW);
        m_searchButton->setWidth(500.0f);
        m_searchButton->setHeight(52.0f);
        m_searchButton->setMargins(0, 15, 0, 0);
        m_searchButton->setPadding(12.0f);
        m_searchButton->setBackgroundColor(nvgRGB(27, 34, 48));
        m_searchButton->setBorderColor(nvgRGB(48, 57, 74));
        m_searchButton->setBorderThickness(1.0f);
        m_searchButton->setCornerRadius(8.0f);
        m_searchButton->setFocusable(true);

        m_queryLabel = new brls::Label();
        m_queryLabel->setText("Press A to enter a title with the Switch keyboard");
        m_queryLabel->setFontSize(16.0f);
        m_searchButton->addView(m_queryLabel);
        m_searchButton->registerAction("Enter search query", brls::BUTTON_A, [this](brls::View*) {
            run_search();
            return true;
        });
        root->addView(m_searchButton);

        m_status = new brls::Label();
        m_status->setText("Search uses live AniList anime data.");
        m_status->setFontSize(15.0f);
        m_status->setTextColor(nvgRGB(174, 184, 200));
        m_status->setMargins(0, 12, 0, 0);
        root->addView(m_status);

        m_scroll = new brls::ScrollingFrame();
        m_scroll->setWidthPercentage(100.0f);
        m_scroll->setHeight(575.0f);
        m_scroll->setMargins(0, 12, 0, 0);
        m_scroll->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);

        m_results = new brls::Box(brls::Axis::COLUMN);
        m_results->setWidth(1160.0f);
        m_results->setHeight(600.0f);
        m_scroll->setContentView(m_results);
        root->addView(m_scroll);

        log_stage("SEARCH VIEW BUILT");
        return root;
    }

    void tick()
    {
        if (!m_resultReady.load(std::memory_order_acquire))
            return;

        if (m_worker.joinable())
            m_worker.join();

        m_loading = false;
        m_resultReady.store(false, std::memory_order_release);

        ++m_renderGeneration;
        render_results();
        perf_log_count("SEARCH FIRST VISIBLE CARDS RENDERED", m_resultItems.size());
        start_progressive_image_load();

        if (m_status)
        {
            std::string text = m_resultStatus;
            text += " — " + std::to_string(m_resultItems.size()) +
                " results";
            if (m_hasMore)
                text += " — select LOAD MORE for another 24.";
            m_status->setText(text);
        }

        log_stage("SEARCH RESULTS ATTACHED");
    }

private:
    brls::Label* m_queryLabel = nullptr;
    brls::Box* m_searchButton = nullptr;
    brls::Label* m_status = nullptr;
    brls::ScrollingFrame* m_scroll = nullptr;
    brls::Box* m_results = nullptr;

    std::string m_pendingQuery;
    std::string m_resultStatus;
    std::vector<SaikouAnime> m_resultItems;
    std::thread m_worker;
    std::thread m_imageWorker;
    std::atomic<bool> m_resultReady{ false };
    std::atomic<uint64_t> m_renderGeneration{ 0 };
    std::shared_ptr<std::atomic<bool>> m_lifetime =
        std::make_shared<std::atomic<bool>>(true);

    bool m_loading = false;
    bool m_hasMore = true;
    int m_page = 0;

    void run_search()
    {
        if (m_loading)
        {
            if (m_status)
                m_status->setText("Search is still loading. Please wait.");
            return;
        }

        log_stage("SEARCH SWITCH KEYBOARD OPEN");

        SwkbdConfig keyboard{};
        Result rc = swkbdCreate(&keyboard, 0);
        if (R_FAILED(rc))
        {
            if (m_status)
                m_status->setText("Could not open the Switch keyboard.");
            log_stage("SEARCH SWITCH KEYBOARD CREATE FAILED");
            return;
        }

        swkbdConfigMakePresetDefault(&keyboard);
        swkbdConfigSetHeaderText(&keyboard, "Search anime on AniList");
        swkbdConfigSetGuideText(&keyboard, "Type an anime title");
        swkbdConfigSetSubText(&keyboard, "Press Search to run the query; Cancel closes the keyboard");
        swkbdConfigSetOkButtonText(&keyboard, "Search");

        if (!m_pendingQuery.empty())
            swkbdConfigSetInitialText(&keyboard, m_pendingQuery.c_str());

        brls::Application::setGlobalQuit(false);
        g_restoreGlobalQuitAfterKeyboard = true;
        log_stage("SEARCH KEYBOARD GLOBAL QUIT DISABLED");

        char query[256] = {};
        rc = swkbdShow(&keyboard, query, sizeof(query));
        swkbdClose(&keyboard);

        if (R_FAILED(rc) || query[0] == '\0')
        {
            if (m_status)
                m_status->setText("No title entered. Press A to search, or B to leave Search.");
            log_stage("SEARCH KEYBOARD CANCELED OR EMPTY");
            return;
        }

        if (m_worker.joinable())
            m_worker.join();

        m_pendingQuery = query;
        m_page = 1;
        m_hasMore = true;
        ++m_renderGeneration;
        if (m_imageWorker.joinable())
            m_imageWorker.join();
        m_resultItems.clear();

        if (m_queryLabel)
            m_queryLabel->setText(m_pendingQuery);
        if (m_status)
            m_status->setText("Searching AniList...");
        clear_box(m_results);

        m_loading = true;
        m_resultReady.store(false, std::memory_order_release);
        log_stage("SEARCH REQUEST STARTED");

        start_load_page(1);
    }

    void start_load_page(int page)
    {
        if (m_loading && page != 1)
            return;
        if (page != 1 && !m_hasMore)
            return;

        if (m_worker.joinable())
            m_worker.join();

        m_loading = true;
        m_resultReady.store(false, std::memory_order_release);
        m_page = page;

        if (m_status)
            m_status->setText(page == 1
                ? "Searching AniList..."
                : "Loading more search results...");

        m_worker = std::thread([this, page] {
            char stage[96];
            std::snprintf(stage, sizeof(stage), "SEARCH PAGE %d API START", page);
            perf_log(stage);

            std::string status;
            std::vector<SaikouAnime> newItems =
                fetch_anilist_media(m_pendingQuery, 24, status, page);
            std::snprintf(stage, sizeof(stage), "SEARCH PAGE %d METADATA RECEIVED", page);
            perf_log_count(stage, newItems.size());

            for (SaikouAnime& anime : newItems)
                anime.posterPath = cached_cover_path(anime.id);

            if (newItems.size() < 24)
                m_hasMore = false;

            m_resultItems.insert(m_resultItems.end(), newItems.begin(), newItems.end());
            m_resultReady.store(true, std::memory_order_release);
        });
    }

    void start_progressive_image_load()
    {
        if (m_imageWorker.joinable())
            m_imageWorker.join();

        if (m_resultItems.empty() || !m_results)
            return;

        std::vector<brls::Image*> posters;
        posters.reserve(m_resultItems.size());

        for (brls::View* rowView : m_results->getChildren())
        {
            brls::Box* row = dynamic_cast<brls::Box*>(rowView);
            if (!row) continue;

            for (brls::View* cardView : row->getChildren())
            {
                brls::Box* card = dynamic_cast<brls::Box*>(cardView);
                if (!card) continue;

                const auto children = card->getChildren();
                if (children.empty()) continue;

                brls::Image* poster =
                    dynamic_cast<brls::Image*>(children.front());
                if (poster && posters.size() < m_resultItems.size())
                    posters.push_back(poster);
            }
        }

        // The final row contains the Load More card, which has no poster Image,
        // so only the anime-card image slots should have been collected.
        if (posters.size() != m_resultItems.size())
        {
            perf_log_count("SEARCH PROGRESSIVE IMAGE SLOT MISMATCH", posters.size());
            return;
        }

        const uint64_t generation =
            m_renderGeneration.load(std::memory_order_acquire);
        const auto lifetime = m_lifetime;
        const std::vector<SaikouAnime> items = m_resultItems;

        perf_log_count("SEARCH PROGRESSIVE IMAGE LOAD START", items.size());

        m_imageWorker = std::thread(
            [this, lifetime, generation, items, posters] {
                size_t completed = 0;

                for (size_t index = 0; index < items.size(); ++index)
                {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;
                    if (m_renderGeneration.load(std::memory_order_acquire) != generation)
                        return;

                    const SaikouAnime& anime = items[index];
                    const std::string path = cached_cover_path(anime.id);

                    if (!download_image(anime.coverUrl, path))
                        continue;

                    ++completed;
                    brls::sync([this, lifetime, generation,
                        poster = posters[index], path, completed] {
                        if (!lifetime->load(std::memory_order_acquire))
                            return;
                        if (m_renderGeneration.load(std::memory_order_acquire) != generation)
                            return;

                        poster->setImageFromFile(path);

                        char stage[128];
                        std::snprintf(stage, sizeof(stage),
                            "SEARCH PROGRESSIVE IMAGE READY %zu", completed);
                        perf_log(stage);
                    });
                }

                brls::sync([this, lifetime, generation, completed] {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;
                    if (m_renderGeneration.load(std::memory_order_acquire) != generation)
                        return;

                    perf_log_count(
                        "SEARCH PROGRESSIVE IMAGE LOAD DONE", completed);
                });
            });
    }

    brls::Box* make_search_load_more_card()
    {
        return make_home_load_more_card([this] {
            remove_load_more_row();
            start_load_page(m_page + 1);
        });
    }

    void remove_load_more_row()
    {
        if (!m_results || m_results->getChildren().empty())
            return;

        brls::View* last = m_results->getChildren().back();
        if (dynamic_cast<brls::Box*>(last))
            m_results->removeView(last);
    }

    void render_results()
    {
        clear_box(m_results);

        constexpr size_t perRow = 6;
        constexpr float rowWidth = 1160.0f;
        constexpr float rowHeight = 252.0f;

        size_t index = 0;
        while (index < m_resultItems.size())
        {
            brls::Box* row = new brls::Box(brls::Axis::ROW);
            row->setWidth(rowWidth);
            row->setHeight(rowHeight);
            row->setAlignItems(brls::AlignItems::FLEX_START);

            const size_t stop = std::min(m_resultItems.size(), index + perRow);
            for (; index < stop; ++index)
                row->addView(make_anime_card(m_resultItems[index], std::string(), nullptr));

            m_results->addView(row);
        }

        if (m_hasMore)
        {
            brls::Box* moreRow = new brls::Box(brls::Axis::ROW);
            moreRow->setWidth(rowWidth);
            moreRow->setHeight(rowHeight);
            moreRow->setAlignItems(brls::AlignItems::FLEX_START);
            moreRow->addView(make_search_load_more_card());
            m_results->addView(moreRow);
        }

        const size_t rows = (m_resultItems.size() + perRow - 1) / perRow +
            (m_hasMore ? 1 : 0);
        m_results->setHeight(
            std::max(600.0f, static_cast<float>(rows) * rowHeight + 20.0f));
    }
};


class PairingActivity;
static PairingActivity* g_pairingActivity = nullptr;

class PairingActivity : public brls::Activity
{
public:
    PairingActivity() { g_pairingActivity = this; }

    ~PairingActivity() override
    {
        m_stopping.store(true, std::memory_order_release);
        if (m_listener.joinable())
            m_listener.join();
        if (g_pairingActivity == this)
            g_pairingActivity = nullptr;
    }

    brls::View* createContentView() override
    {
        brls::Box* root = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(root);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);
        root->setPadding(34.0f);
        root->setBackgroundColor(nvgRGB(16, 20, 29));

        m_pairCode = 10000u +
            static_cast<uint32_t>(randomGet64() % 90000u);

        brls::Label* heading = new brls::Label();
        heading->setText("LINK YOUR ANILIST ACCOUNT");
        heading->setFontSize(28.0f);
        heading->setFocusable(false);
        heading->setTextColor(nvgRGB(244, 246, 250));
        root->addView(heading);

        brls::Label* instructions = new brls::Label();
        instructions->setText(
            "This Switch generates a 5-digit pairing code for the current session. "
            "Use the code with the phone pairing flow while both devices are on the same Wi-Fi.");
        instructions->setFontSize(17.0f);
        instructions->setLineHeight(24.0f);
        instructions->setFocusable(false);
        instructions->setTextColor(nvgRGB(174, 184, 200));
        instructions->setMargins(0, 14, 0, 0);
        root->addView(instructions);

        brls::Label* codeTitle = new brls::Label();
        codeTitle->setText("PAIRING CODE");
        codeTitle->setFontSize(18.0f);
        codeTitle->setFocusable(false);
        codeTitle->setTextColor(nvgRGB(174, 184, 200));
        codeTitle->setMargins(0, 22, 0, 0);
        root->addView(codeTitle);

        m_pairCodeLabel = new brls::Label();
        char pairCodeText[16];
        std::snprintf(
            pairCodeText,
            sizeof(pairCodeText),
            "%05u",
            static_cast<unsigned>(m_pairCode));
        m_pairCodeLabel->setText(pairCodeText);
        m_pairCodeLabel->setFontSize(42.0f);
        m_pairCodeLabel->setFocusable(false);
        m_pairCodeLabel->setTextColor(nvgRGB(97, 207, 226));
        m_pairCodeLabel->setMargins(0, 4, 0, 0);
        root->addView(m_pairCodeLabel);

        m_address = get_switch_local_ip();
        brls::Label* address = new brls::Label();
        if (m_address.empty())
            address->setText("Switch IP unavailable. Connect it to Wi-Fi.\nPhone-code pairing is not available yet.");
        else
            address->setText("Switch IP: " + m_address +
                "\nThe 3-digit phone-code exchange is not available in this build.");
        address->setFontSize(25.0f);
        address->setFocusable(false);
        address->setTextColor(nvgRGB(97, 207, 226));
        address->setMargins(0, 24, 0, 0);
        root->addView(address);

        m_statusLabel = new brls::Label();
        m_statusLabel->setText("Starting the phone link listener...");
        m_statusLabel->setFontSize(17.0f);
        m_statusLabel->setFocusable(false);
        m_statusLabel->setTextColor(nvgRGB(220, 228, 240));
        m_statusLabel->setMargins(0, 18, 0, 0);
        root->addView(m_statusLabel);

        brls::Label* back = new brls::Label();
        back->setText("Press B to return to Settings.");
        back->setFontSize(14.0f);
        back->setFocusable(false);
        back->setTextColor(nvgRGB(135, 147, 166));
        back->setMargins(0, 24, 0, 0);
        root->addView(back);

        m_focusSink = new brls::Padding();
        m_focusSink->setWidth(1.0f);
        m_focusSink->setHeight(1.0f);
        m_focusSink->alpha = 0.0f;
        m_focusSink->setFocusable(true);
        m_focusSink->registerAction(
            "Pairing no-op",
            brls::BUTTON_A,
            [](brls::View*) {
                return true;
            });
        root->addView(m_focusSink);

        return root;
    }

    void onContentAvailable() override
    {
        if (m_focusSink)
            brls::Application::giveFocus(m_focusSink);

        if (!m_listener.joinable())
            m_listener = std::thread([this] { listen_for_phone(); });
    }

    void tick()
    {
        if (!m_statusLabel) return;
        const std::string message = status_snapshot();
        if (message != m_lastDisplayed)
        {
            m_statusLabel->setText(message);
            m_lastDisplayed = message;
        }
    }

private:
    static constexpr int kPairingPort = 2413;
    std::atomic<bool> m_stopping{ false };
    std::thread m_listener;
    std::mutex m_statusMutex;
    std::string m_status = "Starting the phone link listener...";
    std::string m_lastDisplayed;
    std::string m_address;
    brls::Label* m_statusLabel = nullptr;
    brls::Padding* m_focusSink = nullptr;
    brls::Label* m_pairCodeLabel = nullptr;
    uint32_t m_pairCode = 0;

    void set_status(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status = message;
    }

    std::string status_snapshot()
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        return m_status;
    }

    bool same_local_subnet(const sockaddr_in& peer) const
    {
        if (m_address.empty())
            return true;

        in_addr local{};
        if (inet_pton(AF_INET, m_address.c_str(), &local) != 1)
            return false;
        const uint32_t peerHost = ntohl(peer.sin_addr.s_addr);
        const uint32_t localHost = ntohl(local.s_addr);
        return (peerHost & 0xFFFFFF00u) == (localHost & 0xFFFFFF00u);
    }

    bool receive_token(int client, std::string& token)
    {
        char buffer[256];
        while (!m_stopping.load(std::memory_order_acquire) && token.size() < 4096)
        {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(client, &readSet);
            timeval timeout{ 1, 0 };
            const int ready = select(client + 1, &readSet, nullptr, nullptr, &timeout);
            if (ready == 0)
                continue;
            if (ready < 0)
                return false;

            const ssize_t received = recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0)
                return !token.empty();

            for (ssize_t i = 0; i < received; ++i)
            {
                if (buffer[i] == '\n')
                    return !token.empty();
                if (buffer[i] != '\r')
                    token.push_back(buffer[i]);
            }
        }
        return !token.empty();
    }

    void listen_for_phone()
    {
        const int server = socket(AF_INET, SOCK_STREAM, 0);
        if (server < 0)
        {
            set_status("Could not create the local pairing socket.");
            return;
        }

        int reuse = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kPairingPort);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
            listen(server, 1) < 0)
        {
            close(server);
            set_status("Could not listen on port 2413. Close another Saikou TV session and retry.");
            return;
        }

        set_status(m_address.empty()
            ? "Listener on port 2413; no Switch IP is available. Phone-code pairing is not ready."
            : "Local token listener on port 2413; phone-code validation is not connected.");
        while (!m_stopping.load(std::memory_order_acquire))
        {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(server, &readSet);
            timeval timeout{ 0, 500000 };
            const int ready = select(server + 1, &readSet, nullptr, nullptr, &timeout);
            if (ready == 0)
                continue;
            if (ready < 0)
                break;

            sockaddr_in peer{};
            socklen_t peerLength = sizeof(peer);
            const int client = accept(server, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (client < 0)
                continue;
            if (!same_local_subnet(peer))
            {
                close(client);
                set_status("Pairing request was outside the Switch's local /24 network. Still waiting...");
                continue;
            }

            std::string token;
            if (!receive_token(client, token))
            {
                close(client);
                if (!m_stopping.load(std::memory_order_acquire))
                    set_status("No token was received. Keep this screen open and try again from the phone.");
                continue;
            }
            close(client);

            set_status("Phone connected. Verifying the AniList account...");
            std::string username;
            if (!validate_anilist_token(token, username))
            {
                if (!m_stopping.load(std::memory_order_acquire))
                    set_status("AniList rejected that token. Check the phone login and try again.");
                continue;
            }
            if (!save_anilist_token(token))
            {
                set_status("Account verified, but the token could not be saved to the SD card.");
                continue;
            }

            g_anilistAccountRevision.fetch_add(1, std::memory_order_acq_rel);
            log_stage("ANILIST PHONE PAIRING SUCCEEDED");
            set_status("Linked to @" + username + ". Return to Settings or open Library.");
            break;
        }
        close(server);
    }
};

class LibraryActivity;
static LibraryActivity* g_libraryActivity = nullptr;


static constexpr const char* kLibraryStatusNames[6] = {
    "WATCHING", "PLANNING", "COMPLETED", "PAUSED", "DROPPED", "REWATCHING"
};

static constexpr const char* kLibraryStatusValues[6] = {
    "CURRENT", "PLANNING", "COMPLETED", "PAUSED", "DROPPED", "REPEATING"
};

static std::vector<AniListEntry> filter_library_entries(
    const std::vector<AniListEntry>& entries, size_t category)
{
    std::vector<AniListEntry> result;
    if (category >= 6)
        return result;

    for (const AniListEntry& entry : entries)
    {
        if (entry.listStatus == kLibraryStatusValues[category])
        {
            AniListEntry item = entry;
            item.anime.posterPath = cached_cover_path(item.anime.id);
            result.push_back(std::move(item));
        }
    }

    return result;
}

class LibraryCategoryActivity : public brls::Activity
{
public:
    LibraryCategoryActivity(
        size_t category,
        const std::vector<AniListEntry>& entries)
        : m_category(category),
          m_items(filter_library_entries(entries, category))
    {
    }

    ~LibraryCategoryActivity() override
    {
        m_coverLifetime->store(false, std::memory_order_release);

        if (m_coverWorker.joinable())
            m_coverWorker.join();
    }

    brls::View* createContentView() override
    {
        brls::Box* root = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(root);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);
        root->setPadding(30.0f);
        root->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText(
            m_category < 6 ? kLibraryStatusNames[m_category] : "LIBRARY");
        heading->setFontSize(30.0f);
        heading->setTextColor(nvgRGB(244, 246, 250));
        root->addView(heading);

        m_status = new brls::Label();
        m_status->setText(
            "Loaded " + std::to_string(m_items.size()) + " entries");
        m_status->setFontSize(14.0f);
        m_status->setTextColor(nvgRGB(174, 184, 200));
        m_status->setMargins(0, 7, 0, 0);
        root->addView(m_status);

        m_scroll = new brls::ScrollingFrame();
        m_scroll->setWidthPercentage(100.0f);
        m_scroll->setHeight(600.0f);
        m_scroll->setMargins(0, 12, 0, 0);
        m_scroll->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);

        m_grid = new brls::Box(brls::Axis::COLUMN);
        m_grid->setWidth(1160.0f);
        m_grid->setHeight(900.0f);
        m_scroll->setContentView(m_grid);
        root->addView(m_scroll);

        append_batch();
        return root;
    }

private:
    struct CoverJob
    {
        brls::Image* image = nullptr;
        std::string url;
        std::string path;
    };

    size_t m_category = 0;
    std::vector<AniListEntry> m_items;
    size_t m_renderedCount = 0;

    brls::Label* m_status = nullptr;
    brls::ScrollingFrame* m_scroll = nullptr;
    brls::Box* m_grid = nullptr;

    std::vector<CoverJob> m_pendingCoverJobs;
    std::thread m_coverWorker;
    bool m_coverWorkerDone = true;
    std::shared_ptr<std::atomic<bool>> m_coverLifetime =
        std::make_shared<std::atomic<bool>>(true);

    static constexpr size_t kBatchSize = 24;

    void append_batch()
    {
        if (!m_grid)
            return;

        constexpr size_t perRow = 6;
        constexpr float rowWidth = 1160.0f;
        constexpr float rowHeight = 252.0f;

        // Remove only the old Load More row. Existing anime rows/cards remain
        // intact, so focus/navigation stays stable.
        if (!m_grid->getChildren().empty() && m_renderedCount > 0)
        {
            brls::View* last = m_grid->getChildren().back();
            brls::Box* moreRow = dynamic_cast<brls::Box*>(last);

            if (moreRow && moreRow->getChildren().size() == 1)
            {
                brls::View* moreCard = moreRow->getChildren().front();

                if (brls::Application::getCurrentFocus() == moreCard)
                {
                    const auto& rows = m_grid->getChildren();
                    if (rows.size() >= 2)
                    {
                        brls::Box* previousRow =
                            dynamic_cast<brls::Box*>(rows[rows.size() - 2]);

                        if (previousRow && !previousRow->getChildren().empty())
                        {
                            brls::Application::giveFocus(
                                previousRow->getChildren().back());
                        }
                    }
                }

                m_grid->removeView(moreRow);
            }
        }

        const size_t start = m_renderedCount;
        const size_t end =
            std::min(m_items.size(), start + kBatchSize);

        size_t index = start;

        while (index < end)
        {
            brls::Box* row = nullptr;

            if (!m_grid->getChildren().empty())
            {
                brls::View* last = m_grid->getChildren().back();
                row = dynamic_cast<brls::Box*>(last);

                if (row && row->getChildren().size() >= perRow)
                    row = nullptr;
            }

            if (!row)
            {
                row = new brls::Box(brls::Axis::ROW);
                row->setWidth(rowWidth);
                row->setHeight(rowHeight);
                row->setAlignItems(brls::AlignItems::FLEX_START);
                m_grid->addView(row);
            }

            while (index < end && row->getChildren().size() < perRow)
            {
                const AniListEntry& entry = m_items[index];

                std::string subtitle;
                if (entry.listStatus == "CURRENT" ||
                    entry.listStatus == "REPEATING")
                {
                    subtitle = "Episode " +
                        std::to_string(entry.progress);

                    if (entry.anime.episodes > 0)
                        subtitle += " / " +
                            std::to_string(entry.anime.episodes);
                }
                else
                {
                    subtitle = entry.listName.empty()
                        ? entry.listStatus
                        : entry.listName;
                }

                brls::Image* image = nullptr;
                row->addView(
                    make_anime_card(entry.anime, subtitle, &image));

                struct stat st;
                const std::string path =
                    cached_cover_path(entry.anime.id);

                if (image && !entry.anime.coverUrl.empty() &&
                    (stat(path.c_str(), &st) != 0 ||
                     st.st_size <= 256))
                {
                    m_pendingCoverJobs.push_back({
                        image,
                        entry.anime.coverUrl,
                        path
                    });
                }

                ++index;
            }
        }

        m_renderedCount = end;

        const bool hasMore = m_renderedCount < m_items.size();

        if (hasMore)
        {
            brls::Box* moreRow = new brls::Box(brls::Axis::ROW);
            moreRow->setWidth(rowWidth);
            moreRow->setHeight(rowHeight);
            moreRow->setAlignItems(brls::AlignItems::FLEX_START);

            moreRow->addView(make_home_load_more_card([this] {
                append_batch();
            }));
            m_grid->addView(moreRow);
        }

        const size_t dataRows =
            (m_renderedCount + perRow - 1) / perRow;
        const size_t totalRows = dataRows + (hasMore ? 1 : 0);

        m_grid->setHeight(
            std::max(600.0f,
                static_cast<float>(totalRows) * rowHeight + 20.0f));

        update_status();
        start_cover_worker();
    }

    void update_status()
    {
        if (!m_status)
            return;

        std::string text =
            "Loaded " + std::to_string(m_renderedCount) +
            " / " + std::to_string(m_items.size());

        if (m_renderedCount < m_items.size())
            text += "  |  Select LOAD MORE for another 24.";

        m_status->setText(text);
    }

    void start_cover_worker()
    {
        if (m_pendingCoverJobs.empty())
            return;

        if (m_coverWorker.joinable())
        {
            if (!m_coverWorkerDone)
                return;

            m_coverWorker.join();
        }

        std::vector<CoverJob> jobs;
        jobs.swap(m_pendingCoverJobs);

        m_coverWorkerDone = false;
        const auto lifetime = m_coverLifetime;
        const size_t total = jobs.size();

        m_coverWorker = std::thread(
            [this, lifetime, jobs = std::move(jobs), total] {
                size_t completed = 0;

                for (const CoverJob& job : jobs)
                {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;

                    if (!download_image(job.url, job.path))
                        continue;

                    ++completed;

                    brls::sync([this, lifetime,
                        image = job.image, path = job.path,
                        completed, total] {
                        if (!lifetime->load(std::memory_order_acquire))
                            return;

                        image->setImageFromFile(path);

                        if (m_status)
                        {
                            m_status->setText(
                                "Loading posters: " +
                                std::to_string(completed) +
                                " / " +
                                std::to_string(total));
                        }
                    });
                }

                brls::sync([this, lifetime, completed, total] {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;

                    m_coverWorkerDone = true;

                    std::string text =
                        "Loaded " +
                        std::to_string(m_renderedCount) +
                        " / " +
                        std::to_string(m_items.size());

                    if (m_renderedCount < m_items.size())
                        text += "  |  Select LOAD MORE for another 24.";

                    m_status->setText(text);

                    perf_log_count(
                        "LIBRARY CATEGORY PROGRESSIVE COVERS DONE",
                        completed);
                });
            });
    }
};

class LibraryActivity : public brls::Activity
{
public:
    LibraryActivity() { g_libraryActivity = this; }

    ~LibraryActivity() override
    {
        m_coverLifetime->store(false, std::memory_order_release);

        if (m_coverWorker.joinable())
            m_coverWorker.join();

        if (m_worker.joinable())
            m_worker.join();

        if (g_libraryActivity == this)
            g_libraryActivity = nullptr;
    }

    brls::View* createContentView() override
    {
        log_stage("LIBRARY VIEW CREATE START");

        m_content = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(m_content);
        m_content->setWidthPercentage(100.0f);
        m_content->setHeightPercentage(100.0f);
        m_content->setPadding(30.0f);
        m_content->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText("ANILIST LIBRARY");
        heading->setFontSize(28.0f);
        heading->setTextColor(nvgRGB(244, 246, 250));
        m_content->addView(heading);

        m_statusLabel = new brls::Label();
        m_statusLabel->setText("Loading your AniList account...");
        m_statusLabel->setFontSize(15.0f);
        m_statusLabel->setTextColor(nvgRGB(174, 184, 200));
        m_statusLabel->setMargins(0, 6, 0, 0);
        m_content->addView(m_statusLabel);

        brls::Label* libraryHint = new brls::Label();
        libraryHint->setText(
            "AniList library is managed from Settings. "
            "Local library lists are planned for a future update.");
        libraryHint->setFontSize(14.0f);
        libraryHint->setTextColor(nvgRGB(135, 147, 166));
        libraryHint->setMargins(0, 8, 0, 0);
        libraryHint->setFocusable(false);
        m_content->addView(libraryHint);

        m_scroll = new brls::ScrollingFrame();
        m_scroll->setWidthPercentage(100.0f);
        m_scroll->setHeight(570.0f);
        m_scroll->setMargins(0, 14, 0, 0);
        m_scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

        m_sections = new brls::Box(brls::Axis::COLUMN);
        m_sections->setWidth(1160.0f);
        m_sections->setHeight(1900.0f);
        m_scroll->setContentView(m_sections);
        m_content->addView(m_scroll);

        log_stage("LIBRARY VIEW BUILT");
        return m_content;
    }

    void onContentAvailable() override
    {
        log_stage("ACTIVITY OPEN: Library");

        m_token = load_anilist_token();

        if (m_token.empty())
        {
            m_loaded = true;
            m_loadStatus =
                "No AniList account is linked. Use Settings to connect AniList. "
                "Local library lists are planned for a future update.";

            if (m_statusLabel)
                m_statusLabel->setText(m_loadStatus);

            build_sections();
            return;
        }

        m_loading = true;
        log_stage("LIBRARY REQUEST STARTED");

        m_worker = std::thread([this] {
            m_entries =
                fetch_anilist_library(m_token, m_username, m_loadStatus);

            m_ready.store(true, std::memory_order_release);
        });
    }

    void tick()
    {
        // Match Home: when a horizontal scrolling frame itself receives focus,
        // immediately transfer focus to its first actual anime card.
        brls::View* currentFocus = brls::Application::getCurrentFocus();

        for (brls::HScrollingFrame* sectionScroll : m_categoryScrolls)
        {
            if (!sectionScroll || currentFocus != sectionScroll)
                continue;

            brls::View* card = sectionScroll->getDefaultFocus();
            if (card)
                brls::Application::giveFocus(card);
            break;
        }

        if (!m_ready.load(std::memory_order_acquire))
            return;

        if (m_worker.joinable())
            m_worker.join();

        m_ready.store(false, std::memory_order_release);
        m_loading = false;
        m_loaded = true;

        build_sections();
        log_stage("LIBRARY SECTIONS ATTACHED");
    }

private:
    static constexpr size_t kCategoryCount = 6;
    static constexpr size_t kHomeBatchSize = 12;

    brls::Box* m_content = nullptr;
    brls::Label* m_statusLabel = nullptr;
    brls::ScrollingFrame* m_scroll = nullptr;
    brls::Box* m_sections = nullptr;

    std::string m_token;
    std::string m_username;
    std::string m_loadStatus;
    std::vector<AniListEntry> m_entries;

    std::thread m_worker;
    std::atomic<bool> m_ready{ false };
    bool m_loading = false;
    bool m_loaded = false;

    struct CoverJob
    {
        brls::Image* image = nullptr;
        std::string url;
        std::string path;
    };

    std::vector<brls::HScrollingFrame*> m_categoryScrolls;
    std::vector<CoverJob> m_pendingCoverJobs;
    std::thread m_coverWorker;
    bool m_coverWorkerDone = true;
    std::shared_ptr<std::atomic<bool>> m_coverLifetime =
        std::make_shared<std::atomic<bool>>(true);

    void build_sections()
    {
        if (!m_sections)
            return;

        clear_box(m_sections);
        m_categoryScrolls.clear();

        std::vector<AniListEntry> continueEntries;
        size_t currentCount = 0;

        for (const AniListEntry& entry : m_entries)
        {
            if (entry.listStatus != "CURRENT")
                continue;

            ++currentCount;

            if (continueEntries.size() < kHomeBatchSize)
                continueEntries.push_back(entry);
        }

        if (!continueEntries.empty())
        {
            brls::Box* continueHeader =
                new brls::Box(brls::Axis::ROW);
            continueHeader->setWidth(1160.0f);
            continueHeader->setHeight(28.0f);
            continueHeader->setAlignItems(
                brls::AlignItems::CENTER);

            brls::Label* continueTitle =
                new brls::Label();
            continueTitle->setText("CONTINUE WATCHING");
            continueTitle->setFontSize(19.0f);
            continueTitle->setTextColor(
                nvgRGB(220, 228, 240));
            continueHeader->addView(continueTitle);

            brls::Label* continueCount =
                new brls::Label();
            continueCount->setText(
                "  " +
                std::to_string(continueEntries.size()));
            continueCount->setFontSize(13.0f);
            continueCount->setTextColor(
                nvgRGB(135, 147, 166));
            continueHeader->addView(continueCount);

            m_sections->addView(continueHeader);

            brls::HScrollingFrame* continueScroll =
                new brls::HScrollingFrame();
            continueScroll->setWidth(1160.0f);
            continueScroll->setHeight(260.0f);
            continueScroll->setMargins(0, 5, 0, 0);
            continueScroll->setScrollingBehavior(
                brls::ScrollingBehavior::CENTERED);

            brls::Box* continueRow =
                new brls::Box(brls::Axis::ROW);

            const bool hasContinueMore =
                currentCount > kHomeBatchSize;

            const size_t totalCards =
                continueEntries.size() +
                (hasContinueMore ? 1 : 0);

            continueRow->setWidth(
                std::max(
                    1160.0f,
                    static_cast<float>(totalCards) * 192.0f));
            continueRow->setHeight(252.0f);
            continueRow->setAlignItems(
                brls::AlignItems::FLEX_START);

            for (const AniListEntry& entry : continueEntries)
            {
                std::string subtitle =
                    "Episode " +
                    std::to_string(entry.progress);

                if (entry.anime.episodes > 0)
                    subtitle +=
                        " / " +
                        std::to_string(entry.anime.episodes);

                brls::Image* poster = nullptr;

                continueRow->addView(
                    make_anime_card(
                        entry.anime,
                        subtitle,
                        &poster));

                struct stat st;
                const std::string coverPath =
                    cached_cover_path(entry.anime.id);

                if (poster &&
                    !entry.anime.coverUrl.empty() &&
                    (stat(
                        coverPath.c_str(),
                        &st) != 0 ||
                     st.st_size <= 256))
                {
                    m_pendingCoverJobs.push_back({
                        poster,
                        entry.anime.coverUrl,
                        coverPath
                    });
                }
            }

            if (hasContinueMore)
            {
                continueRow->addView(
                    make_home_load_more_card([this] {
                        brls::Application::pushActivity(
                            new LibraryCategoryActivity(
                                0,
                                m_entries),
                            brls::TransitionAnimation::NONE);
                    }));
            }

            continueRow->setDefaultFocusedIndex(0);
            continueScroll->setContentView(continueRow);
            m_sections->addView(continueScroll);
            m_categoryScrolls.push_back(
                continueScroll);

            brls::Padding* continueSpacer =
                new brls::Padding();
            continueSpacer->setHeight(14.0f);
            m_sections->addView(continueSpacer);

            char marker[128];
            std::snprintf(
                marker,
                sizeof(marker),
                "LIBRARY CONTINUE ROW BUILT count=%zu",
                continueEntries.size());
            log_stage(marker);
        }
        else
        {
            log_stage(
                "LIBRARY CONTINUE ROW BUILT count=0");
        }

        for (size_t category = 1;
             category < kCategoryCount;
             ++category)
        {
            const std::vector<AniListEntry> matching =
                filter_library_entries(m_entries, category);

            brls::Box* header =
                new brls::Box(brls::Axis::ROW);
            header->setWidth(1160.0f);
            header->setHeight(28.0f);
            header->setAlignItems(brls::AlignItems::CENTER);

            brls::Label* title = new brls::Label();
            title->setText(kLibraryStatusNames[category]);
            title->setFontSize(19.0f);
            title->setTextColor(nvgRGB(220, 228, 240));
            header->addView(title);

            brls::Label* count = new brls::Label();
            count->setText(
                "  " + std::to_string(matching.size()));
            count->setFontSize(13.0f);
            count->setTextColor(nvgRGB(135, 147, 166));
            header->addView(count);

            m_sections->addView(header);

            if (matching.empty())
            {
                brls::Label* empty = new brls::Label();
                empty->setText("No titles in this list.");
                empty->setFontSize(15.0f);
                empty->setTextColor(nvgRGB(135, 147, 166));
                empty->setFocusable(false);
                empty->setMargins(0, 5, 0, 9);
                m_sections->addView(empty);
                continue;
            }

            brls::HScrollingFrame* scroll =
                new brls::HScrollingFrame();
            scroll->setWidth(1160.0f);
            scroll->setHeight(260.0f);
            scroll->setMargins(0, 5, 0, 0);
            scroll->setScrollingBehavior(
                brls::ScrollingBehavior::CENTERED);

            brls::Box* row =
                new brls::Box(brls::Axis::ROW);

            const size_t visibleCount =
                std::min(kHomeBatchSize, matching.size());

            const bool hasMore =
                matching.size() > kHomeBatchSize;

            const size_t totalCards =
                visibleCount + (hasMore ? 1 : 0);

            row->setWidth(
                std::max(
                    1160.0f,
                    static_cast<float>(totalCards) * 192.0f));
            row->setHeight(252.0f);
            row->setAlignItems(brls::AlignItems::FLEX_START);

            for (size_t index = 0;
                 index < visibleCount;
                 ++index)
            {
                const AniListEntry& entry = matching[index];

                std::string subtitle;

                if (entry.listStatus == "CURRENT" ||
                    entry.listStatus == "REPEATING")
                {
                    subtitle =
                        "Episode " +
                        std::to_string(entry.progress);

                    if (entry.anime.episodes > 0)
                        subtitle +=
                            " / " +
                            std::to_string(entry.anime.episodes);
                }
                else
                {
                    subtitle =
                        entry.listName.empty()
                            ? entry.listStatus
                            : entry.listName;
                }

                brls::Image* poster = nullptr;

                row->addView(
                    make_anime_card(
                        entry.anime,
                        subtitle,
                        &poster));

                struct stat st;
                const std::string coverPath =
                    cached_cover_path(entry.anime.id);

                if (poster &&
                    !entry.anime.coverUrl.empty() &&
                    (stat(coverPath.c_str(), &st) != 0 ||
                     st.st_size <= 256))
                {
                    m_pendingCoverJobs.push_back({
                        poster,
                        entry.anime.coverUrl,
                        coverPath
                    });
                }
            }

            if (hasMore)
            {
                const size_t categoryCopy = category;

                row->addView(
                    make_home_load_more_card(
                        [this, categoryCopy] {
                            brls::Application::pushActivity(
                                new LibraryCategoryActivity(
                                    categoryCopy,
                                    m_entries),
                                brls::TransitionAnimation::NONE);
                        }));
            }

            scroll->setContentView(row);
            m_sections->addView(scroll);
            m_categoryScrolls.push_back(scroll);

            brls::Padding* spacer = new brls::Padding();
            spacer->setHeight(14.0f);
            m_sections->addView(spacer);
        }

        brls::Padding* bottomSpacer =
            new brls::Padding();
        bottomSpacer->setHeight(140.0f);
        m_sections->addView(bottomSpacer);

        // Keep enough real content extent for the last REWATCHING row
        // to scroll completely into the 570px viewport.
        m_sections->setHeight(
            std::max(
                2400.0f,
                static_cast<float>(
                    (kCategoryCount - 1) * 304 +
                    (!continueEntries.empty()
                        ? 304
                        : 0) +
                    300)));

        if (m_statusLabel)
        {
            std::string status = m_loadStatus;

            if (!m_username.empty())
                status += "  |  @" + m_username;

            status +=
                "  |  " +
                std::to_string(m_entries.size()) +
                " library entries";

            m_statusLabel->setText(status);
        }

        start_cover_worker();

        log_stage("LIBRARY HOME ROWS BUILT");
    }

    void start_cover_worker()
    {
        if (m_pendingCoverJobs.empty())
        {
            log_stage(
                "LIBRARY PROGRESSIVE COVERS: no uncached covers");
            return;
        }

        if (m_coverWorker.joinable())
        {
            if (!m_coverWorkerDone)
                return;

            m_coverWorker.join();
        }

        std::vector<CoverJob> jobs;
        jobs.swap(m_pendingCoverJobs);

        m_coverWorkerDone = false;
        const auto lifetime = m_coverLifetime;
        const size_t total = jobs.size();

        char startMarker[128];
        std::snprintf(
            startMarker,
            sizeof(startMarker),
            "LIBRARY PROGRESSIVE COVERS START count=%zu",
            total);
        log_stage(startMarker);

        m_coverWorker = std::thread(
            [this, lifetime, jobs = std::move(jobs), total] {
                size_t completed = 0;

                for (const CoverJob& job : jobs)
                {
                    if (!lifetime->load(
                            std::memory_order_acquire))
                        return;

                    if (!download_image(job.url, job.path))
                        continue;

                    ++completed;

                    brls::sync(
                        [this,
                         lifetime,
                         image = job.image,
                         path = job.path,
                         completed,
                         total] {
                            if (!lifetime->load(
                                    std::memory_order_acquire))
                                return;

                            image->setImageFromFile(path);

                            char marker[128];
                            std::snprintf(
                                marker,
                                sizeof(marker),
                                "LIBRARY PROGRESSIVE COVER READY %zu/%zu",
                                completed,
                                total);
                            log_stage(marker);

                            if (m_statusLabel)
                            {
                                m_statusLabel->setText(
                                    "Loading posters: " +
                                    std::to_string(completed) +
                                    " / " +
                                    std::to_string(total));
                            }
                        });
                }

                brls::sync(
                    [this, lifetime, completed, total] {
                        if (!lifetime->load(
                                std::memory_order_acquire))
                            return;

                        m_coverWorkerDone = true;

                        std::string status = m_loadStatus;
                        if (!m_username.empty())
                            status += "  |  @" + m_username;

                        status +=
                            "  |  " +
                            std::to_string(m_entries.size()) +
                            " library entries";

                        if (m_statusLabel)
                            m_statusLabel->setText(status);

                        char marker[128];
                        std::snprintf(
                            marker,
                            sizeof(marker),
                            "LIBRARY PROGRESSIVE COVERS DONE %zu/%zu",
                            completed,
                            total);
                        log_stage(marker);
                    });
            });
    }


};


class SettingsActivity : public brls::Activity
{
public:
    brls::View* createContentView() override
    {
        log_stage("ACTIVITY OPEN: Settings");

        brls::Box* root =
            new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(root);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);
        root->setPadding(30.0f);
        root->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText("SETTINGS");
        heading->setFontSize(28.0f);
        heading->setFocusable(false);
        root->addView(heading);

        m_scroll = new brls::ScrollingFrame();
        m_scroll->setGrow(1.0f);
        m_scroll->setScrollingBehavior(
            brls::ScrollingBehavior::CENTERED);
        m_scroll->setMargins(0, 8, 0, 0);

        brls::Box* content =
            new brls::Box(brls::Axis::COLUMN);
        content->setWidthPercentage(100.0f);
        content->setPadding(0.0f, 0.0f, 80.0f, 0.0f);

        m_scroll->setContentView(content);
        root->addView(m_scroll);

        brls::Label* accountHeading = new brls::Label();
        accountHeading->setText("ANILIST ACCOUNT");
        accountHeading->setFontSize(19.0f);
        accountHeading->setTextColor(nvgRGB(220, 228, 240));
        accountHeading->setFocusable(false);
        content->addView(accountHeading);

        brls::Padding* accountTitleGap = new brls::Padding();
        accountTitleGap->setHeight(8.0f);
        content->addView(accountTitleGap);

        brls::Label* account = new brls::Label();
        account->setText(load_anilist_token().empty()
            ? "Not linked to AniList."
            : "AniList account token is saved on this Switch.");
        account->setFontSize(15.0f);
        account->setTextColor(nvgRGB(174, 184, 200));
        account->setFocusable(false);
        content->addView(account);

        brls::Padding* accountButtonGap = new brls::Padding();
        accountButtonGap->setHeight(18.0f);
        content->addView(accountButtonGap);

        brls::Box* pair =
            new brls::Box(brls::Axis::ROW);
        pair->setWidthPercentage(100.0f);
        pair->setHeight(46.0f);
        pair->setPadding(10.0f);
        pair->setAlignItems(brls::AlignItems::CENTER);
        pair->setBackgroundColor(nvgRGB(27, 34, 48));
        pair->setBorderColor(nvgRGB(48, 57, 74));
        pair->setBorderThickness(1.0f);
        pair->setCornerRadius(6.0f);
        pair->setFocusable(true);

        brls::Label* pairLabel = new brls::Label();
        pairLabel->setText(load_anilist_token().empty()
            ? "LINK ANILIST FROM PHONE"
            : "PAIR AGAIN / CHANGE ANILIST ACCOUNT");
        pairLabel->setFontSize(17.0f);
        pairLabel->setTextColor(nvgRGB(97, 207, 226));
        pairLabel->setFocusable(false);
        pair->addView(pairLabel);

        pair->registerAction(
            "Link AniList account",
            brls::BUTTON_A,
            [](brls::View*) {
                log_stage("SETTINGS OPEN PAIRING");
                brls::Application::pushActivity(
                    new PairingActivity(),
                    brls::TransitionAnimation::NONE);
                return true;
            });
        content->addView(pair);

        brls::Padding* pairSourceGap = new brls::Padding();
        pairSourceGap->setHeight(26.0f);
        content->addView(pairSourceGap);

        brls::Label* sourceHeading = new brls::Label();
        sourceHeading->setText("EPISODE SOURCES");
        sourceHeading->setFontSize(20.0f);
        sourceHeading->setTextColor(nvgRGB(220, 228, 240));
        sourceHeading->setFocusable(false);
        content->addView(sourceHeading);

        brls::Padding* sourceHeaderGap = new brls::Padding();
        sourceHeaderGap->setHeight(12.0f);
        content->addView(sourceHeaderGap);

        content->addView(make_preferred_source());

        brls::Padding* preferredSourceGap =
            new brls::Padding();
        preferredSourceGap->setHeight(14.0f);
        content->addView(preferredSourceGap);

        brls::Label* sourceHint = new brls::Label();
        sourceHint->setText(
            "Enable the sources you want available for episode lookup.");
        sourceHint->setFontSize(13.0f);
        sourceHint->setTextColor(nvgRGB(135, 147, 166));
        sourceHint->setFocusable(false);
        brls::Padding* sourceHintGap = new brls::Padding();
        sourceHintGap->setHeight(10.0f);
        content->addView(sourceHintGap);
        content->addView(sourceHint);

        brls::Label* aboutHeading = new brls::Label();
        aboutHeading->setText("APP");
        aboutHeading->setFontSize(19.0f);
        aboutHeading->setMargins(0, 22, 0, 6);
        aboutHeading->setTextColor(nvgRGB(220, 228, 240));
        aboutHeading->setFocusable(false);
        content->addView(aboutHeading);

        m_about = new brls::Label();
        m_about->setText(
            "SaikouTV NX  |  Live AniList data  |  "
            "Episode sources are saved to the Switch.");
        m_about->setFontSize(13.0f);
        m_about->setTextColor(nvgRGB(135, 147, 166));
        m_about->setFocusable(false);
        content->addView(m_about);

        log_stage("SETTINGS VIEW BUILT");
        return root;
    }

private:
    brls::ScrollingFrame* m_scroll = nullptr;
    brls::Label* m_selectedSource = nullptr;
    brls::Label* m_about = nullptr;

    size_t enabled_source_count() const
    {
        size_t count = 0;
        for (size_t i = 0; i < kApiSourceCount; ++i)
        {
            if (g_providerEnabled[i])
                ++count;
        }
        return count;
    }

    void refresh_selected_source()
    {
        if (!m_selectedSource)
            return;

        std::string text = "PREFERRED: ";

        const size_t selectedIndex = api_source_index(g_selectedApiSource);
        if (selectedIndex < kApiSourceCount && g_providerEnabled[selectedIndex])
        {
            text += kApiSources[selectedIndex].name;
        }
        else
        {
            text += "NONE";
        }

        text +=
            "    (" +
            std::to_string(enabled_source_count()) +
            "/" +
            std::to_string(kApiSourceCount) +
            " enabled)";

        m_selectedSource->setText(text);
    }

    brls::Box* make_preferred_source()
    {
        brls::Box* row =
            new brls::Box(brls::Axis::ROW);
        row->setWidthPercentage(100.0f);
        row->setHeight(46.0f);
        row->setMargins(0, 0, 0, 0);
        row->setPadding(10.0f);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setBackgroundColor(nvgRGB(27, 34, 48));
        row->setBorderColor(nvgRGB(48, 57, 74));
        row->setBorderThickness(1.0f);
        row->setCornerRadius(6.0f);
        row->setFocusable(true);

        m_selectedSource = new brls::Label();
        m_selectedSource->setFontSize(16.0f);
        m_selectedSource->setTextColor(nvgRGB(214, 222, 235));
        m_selectedSource->setFocusable(false);
        row->addView(m_selectedSource);
        refresh_selected_source();

        row->registerAction(
            "Cycle preferred episode source",
            brls::BUTTON_A,
            [this](brls::View*) {
                if (enabled_source_count() == 0)
                {
                    refresh_selected_source();
                    return true;
                }

                const size_t start = api_source_index(g_selectedApiSource);
                for (size_t offset = 1; offset <= kApiSourceCount; ++offset)
                {
                    const size_t candidate =
                        ((start < kApiSourceCount ? start : kApiSourceCount - 1) + offset) %
                        kApiSourceCount;
                    if (!g_providerEnabled[candidate]) continue;
                    g_selectedApiSource = static_cast<int>(kApiSources[candidate].id);
                    save_source_settings();
                    refresh_selected_source();
                    char marker[128];
                    std::snprintf(marker, sizeof(marker),
                        "SETTINGS PREFERRED SOURCE id=%d", g_selectedApiSource);
                    log_stage(marker);
                    break;
                }

                return true;
            });

        return row;
    }

    brls::Box* make_toggle(size_t index)
    {
        brls::Box* toggle =
            new brls::Box(brls::Axis::ROW);
        toggle->setWidthPercentage(100.0f);
        toggle->setHeight(46.0f);
        toggle->setMargins(0, 0, 0, 0);
        toggle->setPadding(10.0f);
        toggle->setAlignItems(brls::AlignItems::CENTER);
        toggle->setBackgroundColor(nvgRGB(27, 34, 48));
        toggle->setBorderColor(nvgRGB(48, 57, 74));
        toggle->setBorderThickness(1.0f);
        toggle->setCornerRadius(6.0f);
        toggle->setFocusable(true);

        brls::Label* label = new brls::Label();
        label->setText(toggle_text(index));
        label->setFontSize(17.0f);
        label->setTextColor(nvgRGB(214, 222, 235));
        label->setFocusable(false);
        toggle->addView(label);

        toggle->registerAction(
            "Toggle source API",
            brls::BUTTON_A,
            [this, label, index](brls::View*) {
                log_stage("SETTINGS SOURCE TOGGLE");
                g_providerEnabled[index] = !g_providerEnabled[index];
                const int sourceId = static_cast<int>(kApiSources[index].id);
                if (g_providerEnabled[index] && !api_source_is_valid(g_selectedApiSource))
                    g_selectedApiSource = sourceId;
                else if (!g_providerEnabled[index] && g_selectedApiSource == sourceId)
                    g_selectedApiSource = -1;
                save_source_settings();
                label->setText(toggle_text(index));
                refresh_selected_source();
                return true;
            });

        return toggle;
    }

    
    std::string toggle_text(size_t index) const
    {
        return std::string(
            g_providerEnabled[index] ? "[ON]  " : "[OFF] ") +
            kApiSources[index].name +
            "    (press A to toggle)";
    }
};

struct ContinueWatchItem
{
    SaikouAnime anime;
    int progress = 0;
};

static std::vector<ContinueWatchItem> fetch_anilist_continue_watching(
    const std::string& token, std::string& message, size_t maxItems = 24)
{
    std::vector<ContinueWatchItem> items;
    if (token.empty())
    {
        message = "Link an AniList account to sync your watching list.";
        return items;
    }

    std::string username;
    std::string libraryStatus;
    const std::vector<AniListEntry> library = fetch_anilist_library(token, username, libraryStatus);

    for (const AniListEntry& entry : library)
    {
        if (entry.listStatus != "CURRENT")
            continue;

        ContinueWatchItem item;
        item.anime = entry.anime;
        item.progress = entry.progress;
        item.anime.posterPath = cached_cover_path(item.anime.id);
        items.push_back(item);

        if (maxItems > 0 && items.size() >= maxItems)
            break;
    }

    if (!items.empty())
        message = "Watching on AniList  |  @" + username;
    else
        message = "@" + username + " is linked. No Watching entries yet.";

    return items;
}

static std::vector<ContinueWatchItem> load_local_continue_watching(size_t maxItems = 24)
{
    std::vector<ContinueWatchItem> items;
    FILE* file = std::fopen(kLocalContinuePath, "r");
    if (!file)
        return items;

    ContinueWatchItem legacy;
    bool sawLegacy = false;
    std::map<int, ContinueWatchItem> byIndex;
    char line[2048] = {};

    while (std::fgets(line, sizeof(line), file))
    {
        std::string value(line);
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
            value.pop_back();

        const size_t split = value.find('=');
        if (split == std::string::npos)
            continue;

        const std::string key = value.substr(0, split);
        const std::string data = value.substr(split + 1);

        auto parseIndexed = [&](const std::string& prefix, int& index, std::string& field) -> bool {
            if (key.rfind(prefix, 0) != 0)
                return false;
            const size_t dot = key.find('.', prefix.size());
            if (dot == std::string::npos)
                return false;
            index = std::atoi(key.substr(prefix.size(), dot - prefix.size()).c_str());
            field = key.substr(dot + 1);
            return index >= 0 && index < static_cast<int>(kLocalContinueCapacity);
        };

        int index = 0;
        std::string field;
        if (parseIndexed("item", index, field))
        {
            ContinueWatchItem& item = byIndex[index];
            if (field == "id")
                item.anime.id = std::atoi(data.c_str());
            else if (field == "title")
                item.anime.title = data;
            else if (field == "coverUrl")
                item.anime.coverUrl = data;
            else if (field == "episodes")
                item.anime.episodes = std::atoi(data.c_str());
            else if (field == "progress")
                item.progress = std::atoi(data.c_str());
            continue;
        }

        // Backward compatibility with the original single-entry file format.
        sawLegacy = true;
        if (key == "id")
            legacy.anime.id = std::atoi(data.c_str());
        else if (key == "title")
            legacy.anime.title = data;
        else if (key == "coverUrl")
            legacy.anime.coverUrl = data;
        else if (key == "episodes")
            legacy.anime.episodes = std::atoi(data.c_str());
        else if (key == "progress")
            legacy.progress = std::atoi(data.c_str());
    }

    std::fclose(file);

    if (sawLegacy && legacy.anime.id > 0 && !legacy.anime.title.empty())
    {
        legacy.anime.posterPath = cached_cover_path(legacy.anime.id);
        items.push_back(legacy);
    }

    for (const auto& entry : byIndex)
    {
        if (entry.second.anime.id <= 0 || entry.second.anime.title.empty())
            continue;

        ContinueWatchItem item = entry.second;
        item.anime.posterPath = cached_cover_path(item.anime.id);
        items.push_back(item);

        if (maxItems > 0 && items.size() >= maxItems)
            break;
    }

    return items;
}

// Called by the future video-player path whenever playback actually starts/resumes.
// Upserts the title, keeps the newest progress, and caps local history at 24 titles.
static bool save_local_continue_watching(const ContinueWatchItem& item)
{
    if (item.anime.id <= 0 || item.anime.title.empty())
        return false;

    std::vector<ContinueWatchItem> items = load_local_continue_watching(0);
    bool replaced = false;

    for (ContinueWatchItem& existing : items)
    {
        if (existing.anime.id != item.anime.id)
            continue;

        existing = item;
        replaced = true;
        break;
    }

    if (!replaced)
        items.insert(items.begin(), item);

    if (items.size() > kLocalContinueCapacity)
        items.resize(kLocalContinueCapacity);

    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SaikouTV", 0777);

    FILE* file = std::fopen(kLocalContinuePath, "w");
    if (!file)
        return false;

    for (size_t i = 0; i < items.size() && i < kLocalContinueCapacity; ++i)
    {
        const ContinueWatchItem& current = items[i];
        std::fprintf(file, "item%zu.id=%d\n", i, current.anime.id);
        std::fprintf(file, "item%zu.title=%s\n", i, current.anime.title.c_str());
        std::fprintf(file, "item%zu.coverUrl=%s\n", i, current.anime.coverUrl.c_str());
        std::fprintf(file, "item%zu.episodes=%d\n", i, current.anime.episodes);
        std::fprintf(file, "item%zu.progress=%d\n", i, current.progress);
    }

    std::fclose(file);
    return true;
}


class TrendingCatalogActivity;
static TrendingCatalogActivity* g_trendingCatalogActivity = nullptr;

static brls::Box* make_home_load_more_card(std::function<void()> callback)
{
    brls::Box* card = new brls::Box(brls::Axis::COLUMN);
    card->setWidth(184.0f);
    card->setHeight(244.0f);
    card->setPadding(10.0f);
    card->setMargins(3, 7, 3, 0);
    card->setBackgroundColor(nvgRGB(27, 34, 48));
    card->setBorderColor(nvgRGB(48, 57, 74));
    card->setBorderThickness(1.0f);
    card->setCornerRadius(9.0f);
    card->setFocusable(true);
    card->setJustifyContent(brls::JustifyContent::CENTER);
    card->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* label = new brls::Label();
    label->setText("LOAD MORE");
    label->setFontSize(16.0f);
    label->setTextColor(nvgRGB(244, 246, 250));
    label->setSingleLine(false);
    label->setFocusable(false);
    card->addView(label);

    card->registerAction("Load more trending anime", brls::BUTTON_A, [callback](brls::View*) {
        callback();
        return true;
    });
    return card;
}

static void render_home_trending_cards(
    brls::Box* container,
    const std::vector<SaikouAnime>& items,
    std::function<void()> loadMoreCallback)
{
    if (!container) return;
    clear_box(container);

    const float itemWidth = 192.0f;
    const float contentWidth =
        std::max(1160.0f, static_cast<float>(items.size() + 1) * itemWidth);
    container->setWidth(contentWidth);

    brls::Box* row = new brls::Box(brls::Axis::ROW);
    row->setWidth(contentWidth);
    row->setHeight(252.0f);
    row->setAlignItems(brls::AlignItems::FLEX_START);

    for (const SaikouAnime& anime : items)
        row->addView(make_anime_card(anime));

    row->addView(make_home_load_more_card(std::move(loadMoreCallback)));
    container->addView(row);
}

class TrendingCatalogActivity : public brls::Activity
{
public:
    TrendingCatalogActivity()
    {
        g_trendingCatalogActivity = this;
    }

    ~TrendingCatalogActivity() override
    {
        m_coverLifetime->store(false, std::memory_order_release);

        if (m_worker.joinable())
            m_worker.join();
        if (m_coverWorker.joinable())
            m_coverWorker.join();

        if (g_trendingCatalogActivity == this)
            g_trendingCatalogActivity = nullptr;
    }

    brls::View* createContentView() override
    {
        brls::Box* root = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(root);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);
        root->setPadding(30.0f);
        root->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText("TRENDING ANIME");
        heading->setFontSize(30.0f);
        heading->setTextColor(nvgRGB(244, 246, 250));
        root->addView(heading);

        m_status = new brls::Label();
        m_status->setText("Loading trending anime...");
        m_status->setFontSize(14.0f);
        m_status->setTextColor(nvgRGB(174, 184, 200));
        m_status->setMargins(0, 7, 0, 0);
        root->addView(m_status);

        m_scroll = new brls::ScrollingFrame();
        m_scroll->setWidthPercentage(100.0f);
        m_scroll->setHeight(600.0f);
        m_scroll->setMargins(0, 12, 0, 0);
        m_scroll->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);

        m_grid = new brls::Box(brls::Axis::COLUMN);
        m_grid->setWidth(1160.0f);
        m_grid->setHeight(900.0f);
        m_scroll->setContentView(m_grid);
        root->addView(m_scroll);
        return root;
    }

    void onContentAvailable() override
    {
        start_load(1);
    }

    void tick()
    {
        if (m_ready.load(std::memory_order_acquire))
        {
            if (m_worker.joinable())
                m_worker.join();

            append_loaded_items();
            m_ready.store(false, std::memory_order_release);
            m_loading = false;

            if (m_status)
            {
                std::string label = "Loaded " +
                    std::to_string(m_items.size()) + " trending anime";
                if (m_hasMore)
                    label += " — select LOAD MORE for another 30";
                else
                    label += " — end of results";
                m_status->setText(label);
            }
        }

        start_pending_cover_worker();
    }

private:
    struct CoverJob
    {
        brls::Image* image = nullptr;
        std::string url;
        std::string path;
    };

    brls::Label* m_status = nullptr;
    brls::ScrollingFrame* m_scroll = nullptr;
    brls::Box* m_grid = nullptr;

    std::vector<SaikouAnime> m_items;
    std::thread m_worker;
    std::thread m_coverWorker;
    std::atomic<bool> m_ready{ false };
    std::shared_ptr<std::atomic<bool>> m_coverLifetime =
        std::make_shared<std::atomic<bool>>(true);

    std::vector<CoverJob> m_pendingCoverJobs;
    size_t m_coverCompleted = 0;
    size_t m_coverTotal = 0;
    bool m_coverWorkerDone = true;

    bool m_loading = false;
    bool m_hasMore = true;
    int m_page = 0;
    size_t m_renderedCount = 0;

    void start_load(int page)
    {
        if (m_loading || !m_hasMore)
            return;

        if (m_worker.joinable())
            m_worker.join();

        m_loading = true;
        m_ready.store(false, std::memory_order_release);

        m_worker = std::thread([this, page] {
            std::string status;
            std::vector<SaikouAnime> newItems =
                fetch_anilist_trending_page(page, 30, status);

            for (SaikouAnime& anime : newItems)
                anime.posterPath = cached_cover_path(anime.id);

            if (newItems.size() < 30)
                m_hasMore = false;

            m_items.insert(m_items.end(), newItems.begin(), newItems.end());
            m_page = page;
            m_ready.store(true, std::memory_order_release);
        });
    }

    void append_loaded_items()
    {
        if (!m_grid)
            return;

        constexpr size_t perRow = 6;
        constexpr float rowWidth = 1160.0f;
        constexpr float rowHeight = 252.0f;

        size_t index = m_renderedCount;

        // The Load More row stays alive while fetching metadata. Remove it only
        // after the new metadata is ready, and move focus off it first if needed.
        if (!m_grid->getChildren().empty() && m_renderedCount > 0)
        {
            brls::View* last = m_grid->getChildren().back();
            brls::Box* moreRow = dynamic_cast<brls::Box*>(last);
            if (moreRow && moreRow->getChildren().size() == 1 &&
                moreRow->getChildren().front()->isFocusable())
            {
                brls::View* currentFocus =
                    brls::Application::getCurrentFocus();
                if (currentFocus == moreRow->getChildren().front())
                {
                    const auto& rows = m_grid->getChildren();
                    if (rows.size() >= 2)
                    {
                        brls::Box* previousRow =
                            dynamic_cast<brls::Box*>(rows[rows.size() - 2]);
                        if (previousRow && !previousRow->getChildren().empty())
                            brls::Application::giveFocus(
                                previousRow->getChildren().back());
                    }
                }

                m_grid->removeView(moreRow);
            }
        }

        while (index < m_items.size())
        {
            brls::Box* row = nullptr;

            if (!m_grid->getChildren().empty())
            {
                brls::View* last = m_grid->getChildren().back();
                row = dynamic_cast<brls::Box*>(last);
                if (row && row->getChildren().size() >= perRow)
                    row = nullptr;
            }

            if (!row)
            {
                row = new brls::Box(brls::Axis::ROW);
                row->setWidth(rowWidth);
                row->setHeight(rowHeight);
                row->setAlignItems(brls::AlignItems::FLEX_START);
                m_grid->addView(row);
            }

            while (index < m_items.size() &&
                   row->getChildren().size() < perRow)
            {
                brls::Image* image = nullptr;
                row->addView(
                    make_anime_card(m_items[index], std::string(), &image));

                struct stat st;
                const std::string path =
                    cached_cover_path(m_items[index].id);
                if (image && !m_items[index].coverUrl.empty() &&
                    (stat(path.c_str(), &st) != 0 || st.st_size <= 256))
                {
                    m_pendingCoverJobs.push_back(
                        { image, m_items[index].coverUrl, path });
                }

                ++index;
            }
        }

        m_renderedCount = m_items.size();

        if (m_hasMore)
        {
            brls::Box* moreRow = new brls::Box(brls::Axis::ROW);
            moreRow->setWidth(rowWidth);
            moreRow->setHeight(rowHeight);
            moreRow->setAlignItems(brls::AlignItems::FLEX_START);

            moreRow->addView(make_home_load_more_card([this] {
                start_load(m_page + 1);
            }));
            m_grid->addView(moreRow);
        }

        const size_t dataRows =
            (m_renderedCount + perRow - 1) / perRow;
        const size_t totalRows = dataRows + (m_hasMore ? 1 : 0);
        m_grid->setHeight(
            std::max(600.0f, static_cast<float>(totalRows) * rowHeight + 20.0f));

        if (m_pendingCoverJobs.empty())
        {
            if (m_status)
                m_status->setText("Trending ready — select a poster for details.");
        }
        else if (m_status)
        {
            m_status->setText(
                "Loading posters: 0 / " +
                std::to_string(m_pendingCoverJobs.size()));
        }
    }

    void start_pending_cover_worker()
    {
        if (m_pendingCoverJobs.empty())
            return;

        if (m_coverWorker.joinable())
        {
            if (!m_coverWorkerDone)
                return;

            m_coverWorker.join();
        }

        std::vector<CoverJob> jobs;
        jobs.swap(m_pendingCoverJobs);

        m_coverCompleted = 0;
        m_coverTotal = jobs.size();
        m_coverWorkerDone = false;

        const auto lifetime = m_coverLifetime;
        perf_log_count("TRENDING PROGRESSIVE COVERS START", jobs.size());

        m_coverWorker = std::thread(
            [this, lifetime, jobs = std::move(jobs)] {
                size_t completed = 0;

                for (const CoverJob& job : jobs)
                {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;

                    if (!download_image(job.url, job.path))
                        continue;

                    ++completed;

                    brls::sync([this, lifetime, image = job.image,
                        path = job.path, completed, total = jobs.size()] {
                        if (!lifetime->load(std::memory_order_acquire))
                            return;

                        image->setImageFromFile(path);
                        m_coverCompleted = completed;

                        if (m_status)
                        {
                            m_status->setText(
                                "Loading posters: " +
                                std::to_string(completed) + " / " +
                                std::to_string(total));
                        }
                    });
                }

                brls::sync([this, lifetime, completed, total = jobs.size()] {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;

                    m_coverCompleted = completed;
                    m_coverWorkerDone = true;

                    if (m_status)
                    {
                        m_status->setText(
                            "Trending ready — " +
                            std::to_string(completed) + " / " +
                            std::to_string(total) + " posters loaded.");
                    }

                    perf_log_count(
                        "TRENDING PROGRESSIVE COVERS DONE", completed);
                });
            });
    }
};


class AiringCatalogActivity;
static AiringCatalogActivity* g_airingCatalogActivity = nullptr;

static void render_home_airing_cards(
    brls::Box* container,
    const std::vector<SaikouAnime>& items,
    std::function<void()> loadMoreCallback)
{
    if (!container) return;
    clear_box(container);

    const float itemWidth = 192.0f;
    const float contentWidth =
        std::max(1160.0f, static_cast<float>(items.size() + 1) * itemWidth);
    container->setWidth(contentWidth);

    brls::Box* row = new brls::Box(brls::Axis::ROW);
    row->setWidth(contentWidth);
    row->setHeight(252.0f);
    row->setAlignItems(brls::AlignItems::FLEX_START);

    for (const SaikouAnime& anime : items)
        row->addView(make_anime_card(anime));

    row->addView(make_home_load_more_card(std::move(loadMoreCallback)));
    container->addView(row);
}

class AiringCatalogActivity : public brls::Activity
{
public:
    AiringCatalogActivity()
    {
        g_airingCatalogActivity = this;
    }

    ~AiringCatalogActivity() override
    {
        m_coverLifetime->store(false, std::memory_order_release);

        if (m_worker.joinable())
            m_worker.join();
        if (m_coverWorker.joinable())
            m_coverWorker.join();

        if (g_airingCatalogActivity == this)
            g_airingCatalogActivity = nullptr;
    }

    brls::View* createContentView() override
    {
        brls::Box* root = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(root);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);
        root->setPadding(30.0f);
        root->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText("CURRENTLY AIRING");
        heading->setFontSize(30.0f);
        heading->setTextColor(nvgRGB(244, 246, 250));
        root->addView(heading);

        m_status = new brls::Label();
        m_status->setText("Loading currently airing anime...");
        m_status->setFontSize(14.0f);
        m_status->setTextColor(nvgRGB(174, 184, 200));
        m_status->setMargins(0, 7, 0, 0);
        root->addView(m_status);

        m_scroll = new brls::ScrollingFrame();
        m_scroll->setWidthPercentage(100.0f);
        m_scroll->setHeight(600.0f);
        m_scroll->setMargins(0, 12, 0, 0);
        m_scroll->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);

        m_grid = new brls::Box(brls::Axis::COLUMN);
        m_grid->setWidth(1160.0f);
        m_grid->setHeight(900.0f);
        m_scroll->setContentView(m_grid);
        root->addView(m_scroll);
        return root;
    }

    void onContentAvailable() override
    {
        start_load(1);
    }

    void tick()
    {
        if (m_ready.load(std::memory_order_acquire))
        {
            if (m_worker.joinable())
                m_worker.join();

            append_loaded_items();
            m_ready.store(false, std::memory_order_release);
            m_loading = false;

            if (m_status)
            {
                std::string label = "Loaded " +
                    std::to_string(m_items.size()) + " currently airing anime";
                if (m_hasMore)
                    label += " — select LOAD MORE for another 30";
                else
                    label += " — end of results";
                m_status->setText(label);
            }
        }

        start_pending_cover_worker();
    }

private:
    struct CoverJob
    {
        brls::Image* image = nullptr;
        std::string url;
        std::string path;
    };

    brls::Label* m_status = nullptr;
    brls::ScrollingFrame* m_scroll = nullptr;
    brls::Box* m_grid = nullptr;

    std::vector<SaikouAnime> m_items;
    std::thread m_worker;
    std::thread m_coverWorker;
    std::atomic<bool> m_ready{ false };
    std::shared_ptr<std::atomic<bool>> m_coverLifetime =
        std::make_shared<std::atomic<bool>>(true);

    std::vector<CoverJob> m_pendingCoverJobs;
    size_t m_coverCompleted = 0;
    size_t m_coverTotal = 0;
    bool m_coverWorkerDone = true;

    bool m_loading = false;
    bool m_hasMore = true;
    int m_page = 0;
    size_t m_renderedCount = 0;

    void start_load(int page)
    {
        if (m_loading || !m_hasMore)
            return;

        if (m_worker.joinable())
            m_worker.join();

        m_loading = true;
        m_ready.store(false, std::memory_order_release);

        m_worker = std::thread([this, page] {
            std::string status;
            std::vector<SaikouAnime> newItems =
                fetch_currently_airing_page(page, 30, status);

            for (SaikouAnime& anime : newItems)
                anime.posterPath = cached_cover_path(anime.id);

            if (newItems.size() < 30)
                m_hasMore = false;

            m_items.insert(m_items.end(), newItems.begin(), newItems.end());
            m_page = page;
            m_ready.store(true, std::memory_order_release);
        });
    }

    void append_loaded_items()
    {
        if (!m_grid)
            return;

        constexpr size_t perRow = 6;
        constexpr float rowWidth = 1160.0f;
        constexpr float rowHeight = 252.0f;

        // Remove only the previous Load More row after metadata is ready.
        // If it was focused, move focus to a stable existing card first.
        if (!m_grid->getChildren().empty() && m_renderedCount > 0)
        {
            brls::View* last = m_grid->getChildren().back();
            brls::Box* moreRow = dynamic_cast<brls::Box*>(last);
            if (moreRow && moreRow->getChildren().size() == 1 &&
                moreRow->getChildren().front()->isFocusable())
            {
                brls::View* currentFocus =
                    brls::Application::getCurrentFocus();
                if (currentFocus == moreRow->getChildren().front())
                {
                    const auto& rows = m_grid->getChildren();
                    if (rows.size() >= 2)
                    {
                        brls::Box* previousRow =
                            dynamic_cast<brls::Box*>(rows[rows.size() - 2]);
                        if (previousRow && !previousRow->getChildren().empty())
                            brls::Application::giveFocus(
                                previousRow->getChildren().back());
                    }
                }

                m_grid->removeView(moreRow);
            }
        }

        size_t index = m_renderedCount;

        while (index < m_items.size())
        {
            brls::Box* row = nullptr;

            if (!m_grid->getChildren().empty())
            {
                brls::View* last = m_grid->getChildren().back();
                row = dynamic_cast<brls::Box*>(last);
                if (row && row->getChildren().size() >= perRow)
                    row = nullptr;
            }

            if (!row)
            {
                row = new brls::Box(brls::Axis::ROW);
                row->setWidth(rowWidth);
                row->setHeight(rowHeight);
                row->setAlignItems(brls::AlignItems::FLEX_START);
                m_grid->addView(row);
            }

            while (index < m_items.size() &&
                   row->getChildren().size() < perRow)
            {
                brls::Image* image = nullptr;
                row->addView(
                    make_anime_card(m_items[index], std::string(), &image));

                struct stat st;
                const std::string path =
                    cached_cover_path(m_items[index].id);
                if (image && !m_items[index].coverUrl.empty() &&
                    (stat(path.c_str(), &st) != 0 || st.st_size <= 256))
                {
                    m_pendingCoverJobs.push_back(
                        { image, m_items[index].coverUrl, path });
                }

                ++index;
            }
        }

        m_renderedCount = m_items.size();

        if (m_hasMore)
        {
            brls::Box* moreRow = new brls::Box(brls::Axis::ROW);
            moreRow->setWidth(rowWidth);
            moreRow->setHeight(rowHeight);
            moreRow->setAlignItems(brls::AlignItems::FLEX_START);

            moreRow->addView(make_home_load_more_card([this] {
                start_load(m_page + 1);
            }));
            m_grid->addView(moreRow);
        }

        const size_t dataRows =
            (m_renderedCount + perRow - 1) / perRow;
        const size_t totalRows = dataRows + (m_hasMore ? 1 : 0);
        m_grid->setHeight(
            std::max(600.0f, static_cast<float>(totalRows) * rowHeight + 20.0f));

        if (m_pendingCoverJobs.empty())
        {
            if (m_status)
                m_status->setText(
                    "Currently airing ready — select a poster for details.");
        }
        else if (m_status)
        {
            m_status->setText(
                "Loading posters: 0 / " +
                std::to_string(m_pendingCoverJobs.size()));
        }
    }

    void start_pending_cover_worker()
    {
        if (m_pendingCoverJobs.empty())
            return;

        if (m_coverWorker.joinable())
        {
            if (!m_coverWorkerDone)
                return;

            m_coverWorker.join();
        }

        std::vector<CoverJob> jobs;
        jobs.swap(m_pendingCoverJobs);

        m_coverCompleted = 0;
        m_coverTotal = jobs.size();
        m_coverWorkerDone = false;

        const auto lifetime = m_coverLifetime;
        perf_log_count("AIRING PROGRESSIVE COVERS START", jobs.size());

        m_coverWorker = std::thread(
            [this, lifetime, jobs = std::move(jobs)] {
                size_t completed = 0;

                for (const CoverJob& job : jobs)
                {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;

                    if (!download_image(job.url, job.path))
                        continue;

                    ++completed;

                    brls::sync([this, lifetime, image = job.image,
                        path = job.path, completed, total = jobs.size()] {
                        if (!lifetime->load(std::memory_order_acquire))
                            return;

                        image->setImageFromFile(path);
                        m_coverCompleted = completed;

                        if (m_status)
                        {
                            m_status->setText(
                                "Loading posters: " +
                                std::to_string(completed) + " / " +
                                std::to_string(total));
                        }
                    });
                }

                brls::sync([this, lifetime, completed, total = jobs.size()] {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;

                    m_coverCompleted = completed;
                    m_coverWorkerDone = true;

                    if (m_status)
                    {
                        m_status->setText(
                            "Currently airing ready — " +
                            std::to_string(completed) + " / " +
                            std::to_string(total) + " posters loaded.");
                    }

                    perf_log_count(
                        "AIRING PROGRESSIVE COVERS DONE", completed);
                });
            });
    }
};


class ContinueCatalogActivity;
static ContinueCatalogActivity* g_continueCatalogActivity = nullptr;

class ContinueCatalogActivity : public brls::Activity
{
public:
    ContinueCatalogActivity()
    {
        g_continueCatalogActivity = this;
    }

    ~ContinueCatalogActivity() override
    {
        m_coverLifetime->store(false, std::memory_order_release);

        if (m_worker.joinable())
            m_worker.join();
        if (m_coverWorker.joinable())
            m_coverWorker.join();

        if (g_continueCatalogActivity == this)
            g_continueCatalogActivity = nullptr;
    }

    brls::View* createContentView() override
    {
        brls::Box* root = new brls::Box(brls::Axis::COLUMN);
        register_page_back_action(root);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);
        root->setPadding(30.0f);
        root->setBackgroundColor(nvgRGB(16, 20, 29));

        brls::Label* heading = new brls::Label();
        heading->setText("CONTINUE WATCHING");
        heading->setFontSize(30.0f);
        heading->setTextColor(nvgRGB(244, 246, 250));
        root->addView(heading);

        m_status = new brls::Label();
        m_status->setText("Loading watch history...");
        m_status->setFontSize(14.0f);
        m_status->setTextColor(nvgRGB(174, 184, 200));
        m_status->setMargins(0, 7, 0, 0);
        root->addView(m_status);

        m_scroll = new brls::ScrollingFrame();
        m_scroll->setWidthPercentage(100.0f);
        m_scroll->setHeight(600.0f);
        m_scroll->setMargins(0, 12, 0, 0);
        m_scroll->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);

        m_grid = new brls::Box(brls::Axis::COLUMN);
        m_grid->setWidth(1160.0f);
        m_grid->setHeight(900.0f);
        m_scroll->setContentView(m_grid);
        root->addView(m_scroll);
        return root;
    }

    void onContentAvailable() override
    {
        start_load();
    }

    void tick()
    {
        if (m_ready.load(std::memory_order_acquire))
        {
            if (m_worker.joinable())
                m_worker.join();

            render_first_batch();
            m_ready.store(false, std::memory_order_release);
            m_loading = false;
        }

        start_pending_cover_worker();
    }

private:
    struct CoverJob
    {
        brls::Image* image = nullptr;
        std::string url;
        std::string path;
    };

    brls::Label* m_status = nullptr;
    brls::ScrollingFrame* m_scroll = nullptr;
    brls::Box* m_grid = nullptr;

    std::vector<ContinueWatchItem> m_allItems;
    std::thread m_worker;
    std::thread m_coverWorker;
    std::atomic<bool> m_ready{ false };
    std::shared_ptr<std::atomic<bool>> m_coverLifetime =
        std::make_shared<std::atomic<bool>>(true);

    std::vector<CoverJob> m_pendingCoverJobs;
    size_t m_renderedCount = 0;
    size_t m_coverCompleted = 0;
    size_t m_coverTotal = 0;
    bool m_coverWorkerDone = true;

    bool m_loading = false;
    bool m_hasMore = false;

    void start_load()
    {
        if (m_loading)
            return;

        m_loading = true;
        m_ready.store(false, std::memory_order_release);

        m_worker = std::thread([this] {
            const std::string token = load_anilist_token();

            if (!token.empty())
            {
                m_allItems =
                    fetch_anilist_continue_watching(token, m_statusText, 0);

                for (ContinueWatchItem& item : m_allItems)
                    item.anime.posterPath = cached_cover_path(item.anime.id);

                if (m_statusText.empty())
                    m_statusText = m_allItems.empty()
                        ? "No anime currently in your AniList watching history."
                        : "Live AniList watch history";
            }
            else
            {
                m_allItems = load_local_continue_watching(0);

                for (ContinueWatchItem& item : m_allItems)
                    item.anime.posterPath = cached_cover_path(item.anime.id);

                m_statusText = m_allItems.empty()
                    ? "Watch something and it will appear here."
                    : "Local watch progress on this Switch.";
            }

            m_hasMore = m_allItems.size() > 30;
            m_ready.store(true, std::memory_order_release);
        });
    }

    void render_first_batch()
    {
        clear_box(m_grid);
        m_renderedCount = 0;
        m_pendingCoverJobs.clear();
        append_batch();
    }

    void append_batch()
    {
        if (!m_grid)
            return;

        // Remove only the previous Load More row. If it is focused, hand focus
        // to an existing card before freeing the old row.
        if (!m_grid->getChildren().empty() && m_renderedCount > 0)
        {
            brls::View* last = m_grid->getChildren().back();
            brls::Box* moreRow = dynamic_cast<brls::Box*>(last);

            if (moreRow && moreRow->getChildren().size() == 1)
            {
                brls::View* moreCard = moreRow->getChildren().front();
                if (brls::Application::getCurrentFocus() == moreCard)
                {
                    const auto& rows = m_grid->getChildren();
                    if (rows.size() >= 2)
                    {
                        brls::Box* previousRow =
                            dynamic_cast<brls::Box*>(rows[rows.size() - 2]);
                        if (previousRow && !previousRow->getChildren().empty())
                            brls::Application::giveFocus(
                                previousRow->getChildren().back());
                    }
                }

                m_grid->removeView(moreRow);
            }
        }

        constexpr size_t perRow = 6;
        constexpr size_t batchSize = 30;
        constexpr float rowWidth = 1160.0f;
        constexpr float rowHeight = 252.0f;

        const size_t start = m_renderedCount;
        const size_t end =
            std::min(m_allItems.size(), start + batchSize);

        size_t index = start;
        while (index < end)
        {
            brls::Box* row = nullptr;

            if (!m_grid->getChildren().empty())
            {
                brls::View* last = m_grid->getChildren().back();
                row = dynamic_cast<brls::Box*>(last);
                if (row && row->getChildren().size() >= perRow)
                    row = nullptr;
            }

            if (!row)
            {
                row = new brls::Box(brls::Axis::ROW);
                row->setWidth(rowWidth);
                row->setHeight(rowHeight);
                row->setAlignItems(brls::AlignItems::FLEX_START);
                m_grid->addView(row);
            }

            while (index < end && row->getChildren().size() < perRow)
            {
                ContinueWatchItem& item = m_allItems[index];

                brls::Image* image = nullptr;
                std::string subtitle =
                    "Episode " + std::to_string(item.progress);
                if (item.anime.episodes > 0)
                    subtitle += " / " + std::to_string(item.anime.episodes);

                row->addView(
                    make_anime_card(item.anime, subtitle, &image));

                struct stat st;
                const std::string path = cached_cover_path(item.anime.id);
                if (image && !item.anime.coverUrl.empty() &&
                    (stat(path.c_str(), &st) != 0 || st.st_size <= 256))
                {
                    m_pendingCoverJobs.push_back(
                        { image, item.anime.coverUrl, path });
                }

                ++index;
            }
        }

        m_renderedCount = end;
        m_hasMore = m_renderedCount < m_allItems.size();

        if (m_hasMore)
        {
            brls::Box* moreRow = new brls::Box(brls::Axis::ROW);
            moreRow->setWidth(rowWidth);
            moreRow->setHeight(rowHeight);
            moreRow->setAlignItems(brls::AlignItems::FLEX_START);

            moreRow->addView(make_home_load_more_card([this] {
                append_batch();
            }));
            m_grid->addView(moreRow);
        }

        const size_t dataRows =
            (m_renderedCount + perRow - 1) / perRow;
        const size_t totalRows = dataRows + (m_hasMore ? 1 : 0);
        m_grid->setHeight(
            std::max(600.0f, static_cast<float>(totalRows) * rowHeight + 20.0f));

        if (m_status)
        {
            std::string label = m_statusText.empty()
                ? "Continue Watching"
                : m_statusText;

            if (!m_pendingCoverJobs.empty())
            {
                label += " — Loading posters: 0 / " +
                    std::to_string(m_pendingCoverJobs.size());
            }
            else if (m_hasMore)
            {
                label += " — select LOAD MORE for another 30";
            }
            else
            {
                label += " — " +
                    std::to_string(m_allItems.size()) +
                    " watch entries";
            }

            m_status->setText(label);
        }
    }

    void start_pending_cover_worker()
    {
        if (m_pendingCoverJobs.empty())
            return;

        if (m_coverWorker.joinable())
        {
            if (!m_coverWorkerDone)
                return;

            m_coverWorker.join();
        }

        std::vector<CoverJob> jobs;
        jobs.swap(m_pendingCoverJobs);

        m_coverCompleted = 0;
        m_coverTotal = jobs.size();
        m_coverWorkerDone = false;

        const auto lifetime = m_coverLifetime;
        perf_log_count("CONTINUE PROGRESSIVE COVERS START", jobs.size());

        m_coverWorker = std::thread(
            [this, lifetime, jobs = std::move(jobs)] {
                size_t completed = 0;

                for (const CoverJob& job : jobs)
                {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;

                    if (!download_image(job.url, job.path))
                        continue;

                    ++completed;

                    brls::sync([this, lifetime, image = job.image,
                        path = job.path, completed, total = jobs.size()] {
                        if (!lifetime->load(std::memory_order_acquire))
                            return;

                        image->setImageFromFile(path);
                        m_coverCompleted = completed;

                        if (m_status)
                        {
                            m_status->setText(
                                "Loading posters: " +
                                std::to_string(completed) + " / " +
                                std::to_string(total));
                        }
                    });
                }

                brls::sync([this, lifetime, completed, total = jobs.size()] {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;

                    m_coverCompleted = completed;
                    m_coverWorkerDone = true;

                    if (m_status)
                    {
                        std::string label = m_statusText.empty()
                            ? "Continue Watching"
                            : m_statusText;
                        label += " — " + std::to_string(completed) +
                            " / " + std::to_string(total) +
                            " posters loaded.";
                        if (m_hasMore)
                            label += " Select LOAD MORE for another 30.";
                        m_status->setText(label);
                    }

                    perf_log_count(
                        "CONTINUE PROGRESSIVE COVERS DONE", completed);
                });
            });
    }

    std::string m_statusText;
};


class HomeActivity : public brls::Activity
{
public:
    ~HomeActivity() override
    {
        m_coverLifetime->store(false, std::memory_order_release);

        if (m_loader.joinable())
            m_loader.join();
        if (m_airingLoader.joinable())
            m_airingLoader.join();
        if (m_accountLoader.joinable())
            m_accountLoader.join();
        if (m_coverLoader.joinable())
            m_coverLoader.join();
    }

    brls::View* createContentView() override
    {
        log_stage("HomeActivity createContentView START");
        brls::View* view = brls::View::createFromXMLResource("activity/main.xml");
        log_stage(view ? "Home XML returned a view" : "Home XML returned NULL");
        if (!view)
            return create_xml_failure_view();

        view->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
        view->setBackgroundColor(nvgRGB(16, 20, 29));
        g_homeView = view;
        return view;
    }

    void onContentAvailable() override
    {
        m_status = dynamic_cast<brls::Label*>(getView("home/status"));
        m_cards = dynamic_cast<brls::Box*>(getView("home/trending/cards"));
        m_latestCards = dynamic_cast<brls::Box*>(getView("home/latest/cards"));
        m_continueBox = dynamic_cast<brls::Box*>(getView("home/card/continue"));
        m_continueScroll = getView("home/continue/scroll");
        m_continueTitle = dynamic_cast<brls::Label*>(getView("home/continue/title"));
        m_continueSubtitle = dynamic_cast<brls::Label*>(getView("home/continue/subtitle"));
        m_accountRevision = g_anilistAccountRevision.load(std::memory_order_acquire);

        if (m_continueBox)
            m_continueBox->setFocusable(false);

        connect_navigation("nav/search", "Open Search", [] {
            brls::Application::pushActivity(new SearchActivity(), brls::TransitionAnimation::NONE);
        });
        connect_navigation("nav/library", "Open Library", [] {
            brls::Application::pushActivity(new LibraryActivity(), brls::TransitionAnimation::NONE);
        });
        connect_navigation("nav/settings", "Open Settings", [] {
            brls::Application::pushActivity(new SettingsActivity(), brls::TransitionAnimation::NONE);
        });
        connect_navigation("nav/home", "Home", [] {});
        if (m_status) m_status->setText("Loading live AniList trending titles...");
        if (!m_loader.joinable())
        {
            m_loader = std::thread([this] {
                perf_log("HOME TRENDING API START");
                m_items = fetch_anilist_media("", 24, m_loadStatus);
                for (SaikouAnime& anime : m_items)
                    anime.posterPath = cached_cover_path(anime.id);
                perf_log_count("HOME TRENDING METADATA RECEIVED", m_items.size());

                const std::string token = load_anilist_token();
                if (!token.empty())
                {
                    perf_log("HOME CONTINUE API START");
                    m_continueItems = fetch_anilist_continue_watching(token, m_continueMessage);
                    for (ContinueWatchItem& item : m_continueItems)
                        item.anime.posterPath = cached_cover_path(item.anime.id);
                    perf_log_count("HOME CONTINUE METADATA RECEIVED", m_continueItems.size());
                }
                else
                {
                    perf_log("HOME CONTINUE LOCAL START");
                    m_continueItems = load_local_continue_watching();
                    m_continueMessage = m_continueItems.empty()
                        ? "Watch something and it will appear here."
                        : "Local watch progress on this Switch.";
                    for (ContinueWatchItem& item : m_continueItems)
                        item.anime.posterPath = cached_cover_path(item.anime.id);
                    perf_log_count("HOME CONTINUE LOCAL READY", m_continueItems.size());
                }

                m_ready.store(true, std::memory_order_release);
                perf_log("HOME PRIMARY METADATA READY");
            });
        }
        if (m_latestCards && !m_airingLoader.joinable())
        {
            m_airingLoader = std::thread([this] {
                perf_log("HOME AIRING API START");
                m_airingItems = fetch_currently_airing_media(24, m_airingStatus);
                for (SaikouAnime& anime : m_airingItems)
                    anime.posterPath = cached_cover_path(anime.id);
                perf_log_count("HOME AIRING METADATA RECEIVED", m_airingItems.size());

                m_airingReady.store(true, std::memory_order_release);
                perf_log("HOME AIRING METADATA READY");
            });
        }
    }

    void tick()
    {
        // HScrollingFrame can receive focus itself when entering the section.
        // Once its dynamic card row exists, explicitly hand focus to the first
        // actual anime card so A/LEFT/RIGHT operate on the card, not the frame.
        if (m_continueScroll && !m_continueItems.empty())
        {
            brls::View* currentFocus = brls::Application::getCurrentFocus();
            if (currentFocus == m_continueScroll && !m_continueBox->getChildren().empty())
            {
                brls::View* row = m_continueBox->getChildren().front();
                if (row)
                {
                    brls::View* card = row->getDefaultFocus();
                    if (card)
                        brls::Application::giveFocus(card);
                }
            }
        }

        if (!m_attached && m_ready.load(std::memory_order_acquire))
        {
            if (m_loader.joinable())
                m_loader.join();
            render_home_trending_cards(m_cards, m_items, [] {
                brls::Application::pushActivity(
                    new TrendingCatalogActivity(),
                    brls::TransitionAnimation::NONE);
            });
            perf_log_count("HOME TRENDING CARDS RENDERED", m_items.size());

            render_continue_cards();
            perf_log_count("HOME CONTINUE CARDS RENDERED", m_continueItems.size());
            if (m_status)
                m_status->setText("Home ready — loading posters...");

            m_attached = true;
            log_stage("ANILIST HOME CARDS ATTACHED");

            // Wait until the Airing row has also been attached so one Home
            // cover worker can own a complete, stable set of card targets.
            if (m_airingAttached && !m_coverStarted)
            {
                m_coverStarted = true;
                start_cover_loading();
            }
        }

        if (m_airingReady.load(std::memory_order_acquire))
        {
            if (m_airingLoader.joinable())
                m_airingLoader.join();
            render_home_airing_cards(m_latestCards, m_airingItems, [] {
                brls::Application::pushActivity(
                    new AiringCatalogActivity(),
                    brls::TransitionAnimation::NONE);
            });
            perf_log_count("HOME AIRING CARDS RENDERED", m_airingItems.size());
            m_airingReady.store(false, std::memory_order_release);
            m_airingAttached = true;

            if (m_attached && !m_coverStarted)
            {
                m_coverStarted = true;
                start_cover_loading();
            }
        }

        if (m_attached && m_accountReady.load(std::memory_order_acquire))
        {
            if (m_accountLoader.joinable())
                m_accountLoader.join();
            m_accountLoading = false;
            m_accountReady.store(false, std::memory_order_release);
            update_continue_card();
        }

        const unsigned int revision = g_anilistAccountRevision.load(std::memory_order_acquire);
        if (m_attached && revision != m_accountRevision && !m_accountLoading)
        {
            m_accountRevision = revision;
            m_accountLoading = true;
            m_accountReady.store(false, std::memory_order_release);
            m_accountLoader = std::thread([this] {
                m_continueItems.clear();
                const std::string token = load_anilist_token();
                if (!token.empty())
                {
                    m_continueItems = fetch_anilist_continue_watching(token, m_continueMessage);
                    for (ContinueWatchItem& item : m_continueItems)
                    {
                        if (!download_image(item.anime.coverUrl, item.anime.posterPath))
                            item.anime.posterPath.clear();
                    }
                }
                else
                {
                    m_continueItems = load_local_continue_watching();
                    m_continueMessage = m_continueItems.empty()
                        ? "Watch something and it will appear here."
                        : "Local watch progress on this Switch.";
                }
                m_accountReady.store(true, std::memory_order_release);
            });
        }
    }

private:
    std::thread m_loader;
    std::thread m_airingLoader;
    std::thread m_accountLoader;
    std::thread m_coverLoader;
    std::atomic<bool> m_ready{ false };
    std::atomic<bool> m_airingReady{ false };
    std::atomic<bool> m_accountReady{ false };
    bool m_attached = false;
    bool m_accountLoading = false;
    std::vector<ContinueWatchItem> m_continueItems;
    unsigned int m_accountRevision = 0;
    std::vector<SaikouAnime> m_items;
    std::vector<SaikouAnime> m_airingItems;
    SaikouAnime m_continueAnime;
    std::string m_loadStatus;
    std::string m_airingStatus;
    std::string m_continueMessage;
    brls::Label* m_status = nullptr;
    brls::Box* m_cards = nullptr;
    brls::Box* m_latestCards = nullptr;
    brls::View* m_continueScroll = nullptr;
    brls::Box* m_continueBox = nullptr;
    brls::Label* m_continueTitle = nullptr;
    brls::Label* m_continueSubtitle = nullptr;

    struct CoverTarget
    {
        int animeId = 0;
        std::string url;
        std::string path;
        brls::Image* image = nullptr;
    };

    std::shared_ptr<std::atomic<bool>> m_coverLifetime =
        std::make_shared<std::atomic<bool>>(true);
    std::atomic<uint64_t> m_coverGeneration{ 0 };
    std::vector<CoverTarget> m_coverTargets;
    size_t m_coverCompleted = 0;
    size_t m_coverTotal = 0;
    bool m_airingAttached = false;
    bool m_coverStarted = false;

    void render_continue_cards()
    {
        if (!m_continueBox)
            return;

        clear_box(m_continueBox);

        if (m_continueItems.empty())
        {
            if (m_continueTitle)
                m_continueTitle->setText("Nothing to resume yet");
            if (m_continueSubtitle)
                m_continueSubtitle->setText(m_continueMessage);
            return;
        }

        brls::Box* row = new brls::Box(brls::Axis::ROW);
        row->setWidth(std::max(1160.0f, static_cast<float>(m_continueItems.size()) * 192.0f));
        row->setHeight(252.0f);
        row->setAlignItems(brls::AlignItems::FLEX_START);

        for (const ContinueWatchItem& item : m_continueItems)
        {
            std::string subtitle = "Episode " + std::to_string(item.progress);
            if (item.anime.episodes > 0)
                subtitle += " / " + std::to_string(item.anime.episodes);
            row->addView(make_anime_card(item.anime, subtitle, nullptr));
        }

        row->setDefaultFocusedIndex(0);
        m_continueBox->setFocusable(false);
        m_continueBox->setDefaultFocusedIndex(0);
        if (m_continueItems.size() >= 24)
        {
            row->addView(make_home_load_more_card([] {
                brls::Application::pushActivity(
                    new ContinueCatalogActivity(),
                    brls::TransitionAnimation::NONE);
            }));
        }

        const size_t visibleCount = std::min<size_t>(24, m_continueItems.size());
        const float contentWidth =
            static_cast<float>((visibleCount + (m_continueItems.size() >= 24 ? 1 : 0)) * 192.0f);
        row->setWidth(std::max(1160.0f, contentWidth));

        m_continueBox->addView(row);

        if (m_continueTitle)
            m_continueTitle->setText("CONTINUE WATCHING");
        if (m_continueSubtitle)
            m_continueSubtitle->setText(m_continueMessage);
    }

    void collect_cover_targets(
        brls::Box* container,
        size_t expectedCount,
        std::vector<CoverTarget>& targets)
    {
        if (!container)
            return;

        for (brls::View* rowView : container->getChildren())
        {
            brls::Box* row = dynamic_cast<brls::Box*>(rowView);
            if (!row)
                continue;

            for (brls::View* cardView : row->getChildren())
            {
                if (targets.size() >= expectedCount)
                    return;

                brls::Box* card = dynamic_cast<brls::Box*>(cardView);
                if (!card)
                    continue;

                const auto children = card->getChildren();
                if (children.empty())
                    continue;

                brls::Image* image =
                    dynamic_cast<brls::Image*>(children.front());
                if (!image)
                    continue;

                targets.push_back(CoverTarget());
                targets.back().image = image;
            }
        }
    }

    void start_cover_loading()
    {
        if (m_coverLoader.joinable())
            m_coverLoader.join();

        m_coverTargets.clear();
        m_coverCompleted = 0;

        for (const SaikouAnime& anime : m_items)
            m_coverTargets.push_back({ anime.id, anime.coverUrl,
                cached_cover_path(anime.id), nullptr });

        for (const ContinueWatchItem& item : m_continueItems)
            m_coverTargets.push_back({ item.anime.id, item.anime.coverUrl,
                cached_cover_path(item.anime.id), nullptr });

        for (const SaikouAnime& anime : m_airingItems)
            m_coverTargets.push_back({ anime.id, anime.coverUrl,
                cached_cover_path(anime.id), nullptr });

        size_t offset = 0;
        if (m_cards)
        {
            std::vector<CoverTarget> targets;
            collect_cover_targets(m_cards, m_items.size(), targets);
            for (size_t i = 0; i < targets.size() && offset + i < m_coverTargets.size(); ++i)
                m_coverTargets[offset + i].image = targets[i].image;
            offset += targets.size();
        }

        if (m_continueBox)
        {
            std::vector<CoverTarget> targets;
            collect_cover_targets(m_continueBox, m_continueItems.size(), targets);
            for (size_t i = 0; i < targets.size() && offset + i < m_coverTargets.size(); ++i)
                m_coverTargets[offset + i].image = targets[i].image;
            offset += targets.size();
        }

        if (m_latestCards)
        {
            std::vector<CoverTarget> targets;
            collect_cover_targets(m_latestCards, m_airingItems.size(), targets);
            for (size_t i = 0; i < targets.size() && offset + i < m_coverTargets.size(); ++i)
                m_coverTargets[offset + i].image = targets[i].image;
        }

        std::vector<CoverTarget> jobs;
        for (const CoverTarget& target : m_coverTargets)
        {
            if (!target.image || target.url.empty())
                continue;

            struct stat st;
            if (stat(target.path.c_str(), &st) == 0 && st.st_size > 256)
                continue;

            jobs.push_back(target);
        }

        m_coverTotal = jobs.size();
        if (m_coverTotal == 0)
        {
            if (m_status)
                m_status->setText("Home ready — select a poster for details.");
            return;
        }

        const uint64_t generation =
            m_coverGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        const auto lifetime = m_coverLifetime;
        const size_t totalJobs = jobs.size();
        perf_log_count("HOME PROGRESSIVE COVERS START", totalJobs);

        m_coverLoader = std::thread(
            [this, lifetime, generation, jobs, totalJobs] {
                size_t completed = 0;

                for (const CoverTarget& job : jobs)
                {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;
                    if (m_coverGeneration.load(std::memory_order_acquire) != generation)
                        return;

                    if (!download_image(job.url, job.path))
                        continue;

                    ++completed;
                    brls::sync([this, lifetime, generation, totalJobs,
                        image = job.image, path = job.path, completed] {
                        if (!lifetime->load(std::memory_order_acquire))
                            return;
                        if (m_coverGeneration.load(std::memory_order_acquire) != generation)
                            return;

                        image->setImageFromFile(path);
                        m_coverCompleted = completed;

                        if (m_status)
                        {
                            m_status->setText(
                                "Loading posters: " +
                                std::to_string(m_coverCompleted) + " / " +
                                std::to_string(m_coverTotal));
                        }

                        char marker[160];
                        std::snprintf(marker, sizeof(marker),
                            "HOME PROGRESSIVE COVER READY %zu/%zu",
                            completed, totalJobs);
                        perf_log(marker);
                    });
                }

                brls::sync([this, lifetime, generation, completed] {
                    if (!lifetime->load(std::memory_order_acquire))
                        return;
                    if (m_coverGeneration.load(std::memory_order_acquire) != generation)
                        return;

                    m_coverCompleted = completed;
                    if (m_status)
                        m_status->setText("Home ready — select a poster for details.");

                    perf_log_count("HOME PROGRESSIVE COVERS DONE", completed);
                });
            });
    }

    void update_continue_card()
    {
        // Account changes rebuild the Continue row. Invalidate pending cover
        // callbacks before replacing those views, then synchronize the worker
        // before collecting the new image targets.
        m_coverGeneration.fetch_add(1, std::memory_order_acq_rel);
        render_continue_cards();

        if (m_attached)
        {
            if (m_coverLoader.joinable())
                m_coverLoader.join();
            start_cover_loading();
        }
    }

    void connect_navigation(const char* id, const char* name, std::function<void()> callback)
    {
        brls::View* view = getView(id);
        if (!view) return;
        view->registerAction(name, brls::BUTTON_A, [callback, name](brls::View*) {
            char marker[128];
            std::snprintf(marker, sizeof(marker), "NAV ACTION: %s", name);
            log_stage(marker);
            callback();
            char completeMarker[128];
            const auto stack = brls::Application::getActivitiesStack();
            std::snprintf(completeMarker, sizeof(completeMarker),
                "NAV ACTION COMPLETE: %s stack=%zu", name, stack.size());
            log_stage(completeMarker);
            return true;
        });
    }
};

static void tick_live_ui_activities()
{
    if (g_restoreGlobalQuitAfterKeyboard &&
        !brls::Application::getControllerState().buttons[brls::BUTTON_START])
    {
        brls::Application::setGlobalQuit(true);
        g_restoreGlobalQuitAfterKeyboard = false;
        log_stage("SEARCH KEYBOARD GLOBAL QUIT RESTORED");
    }

    if (g_episodeListActivity)
        g_episodeListActivity->tick();
    if (g_episodeStreamActivity)
        g_episodeStreamActivity->tick();
    if (g_pairingActivity)
        g_pairingActivity->tick();
    if (g_libraryActivity)
        g_libraryActivity->tick();
    if (g_searchActivity)
        g_searchActivity->tick();
    if (g_animeDetailsActivity)
        g_animeDetailsActivity->tick();
    if (g_trendingCatalogActivity)
        g_trendingCatalogActivity->tick();
    if (g_airingCatalogActivity)
        g_airingCatalogActivity->tick();
    if (g_continueCatalogActivity)
        g_continueCatalogActivity->tick();
}