#pragma once

#include <cstddef>

// Stable provider registry. Keep IDs stable once released; add/remove providers
// by changing the enabled flag rather than renumbering existing providers.
enum class ApiSourceId : int
{
    Miruro = 0,
    AnimePahe = 1,
    Gogoanime = 2,
    Aniwatch = 3,
    HiAnime = 4,
    KickAssAnime = 5,
    Anichi = 6,
    Anikoto = 7,
    AniWave = 8,
    AnimeSogo = 9,
    AnimeKai = 10,
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
    { ApiSourceId::Miruro,    "Miruro",    "miruro",    true },
    { ApiSourceId::AnimePahe, "AnimePahe", "animepahe", true },
    { ApiSourceId::Gogoanime, "Gogoanime", "gogoanime", true },
    { ApiSourceId::Aniwatch,  "Aniwatch",  "aniwatch",  true },
    { ApiSourceId::HiAnime,   "HiAnime",   "hianime",   true },
    { ApiSourceId::KickAssAnime, "KickAssAnime", "kickassanime", true },
    { ApiSourceId::Anichi,      "Anichi",      "anichi",      true },
    { ApiSourceId::Anikoto,     "Anikoto",     "anikoto",     true },
    { ApiSourceId::AniWave,     "AniWave (Unoriginal)", "aniwave", true },
    { ApiSourceId::AnimeSogo,   "AnimeSogo",   "animesogo",   true },
    { ApiSourceId::AnimeKai,    "AnimeKai (Unoriginal)", "animekai", true },
};

static constexpr std::size_t kApiSourceCount = sizeof(kApiSources) / sizeof(kApiSources[0]);

inline const ApiSourceInfo* find_api_source(int id)
{
    for (std::size_t i = 0; i < kApiSourceCount; ++i)
    {
        if (static_cast<int>(kApiSources[i].id) == id)
            return &kApiSources[i];
    }
    return nullptr;
}

inline const char* api_source_name(int id)
{
    const ApiSourceInfo* source = find_api_source(id);
    return source ? source->name : kApiSources[0].name;
}

inline bool api_source_is_valid(int id)
{
    const ApiSourceInfo* source = find_api_source(id);
    return source && source->enabled;
}
