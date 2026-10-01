#pragma once
#include <cstddef>

enum class ApiSourceId : int
{
    KickAssAnime = 5,
};

struct ApiSourceInfo
{
    ApiSourceId id;
    const char* name;
    const char* slug;
    bool enabled;
};

static constexpr ApiSourceInfo kApiSources[] =
{
    { ApiSourceId::KickAssAnime, "KickAssAnime", "kickassanime", true },
};

static constexpr std::size_t kApiSourceCount = sizeof(kApiSources) / sizeof(kApiSources[0]);

inline const ApiSourceInfo* find_api_source(int id)
{
    for (std::size_t i = 0; i < kApiSourceCount; ++i)
        if (static_cast<int>(kApiSources[i].id) == id)
            return &kApiSources[i];
    return nullptr;
}

inline const char* api_source_name(int id)
{
    const ApiSourceInfo* source = find_api_source(id);
    return source ? source->name : "KickAssAnime";
}

inline bool api_source_is_valid(int id)
{
    return id == static_cast<int>(ApiSourceId::KickAssAnime);
}
