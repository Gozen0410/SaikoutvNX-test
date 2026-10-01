#pragma once
#include <cstddef>
enum class ApiSourceId : int { Miruro=0, AnimePahe=1, Gogoanime=2, Aniwatch=3, HiAnime=4, KickAssAnime=5, AnimeKai=6 };
struct ApiSourceInfo { ApiSourceId id; const char* name; const char* slug; bool enabled; };
static constexpr ApiSourceInfo kApiSources[] = {
    { ApiSourceId::KickAssAnime, "KickAssAnime", "kickassanime", true },
    { ApiSourceId::AnimeKai, "AnimeKai", "animekai", true }
};
static constexpr std::size_t kApiSourceCount = 2;
inline const ApiSourceInfo* find_api_source(int id){ for(std::size_t i=0;i<kApiSourceCount;++i) if(static_cast<int>(kApiSources[i].id)==id) return &kApiSources[i]; return nullptr; }
inline const char* api_source_name(int id){ const auto* s=find_api_source(id); return s?s->name:"Unknown"; }
inline bool api_source_is_valid(int id){ return id==static_cast<int>(ApiSourceId::KickAssAnime)||id==static_cast<int>(ApiSourceId::AnimeKai); }