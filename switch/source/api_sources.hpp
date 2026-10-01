#pragma once
#include <cstddef>
enum class ApiSourceId : int { KickAssAnime = 0 };
struct ApiSourceInfo { ApiSourceId id; const char* name; const char* slug; bool enabled; };
static constexpr ApiSourceInfo kApiSources[] = {{ ApiSourceId::KickAssAnime, "KickAssAnime", "kickassanime", true }};
static constexpr std::size_t kApiSourceCount = 1;
inline const ApiSourceInfo* find_api_source(int id){ for(std::size_t i=0;i<kApiSourceCount;++i) if(static_cast<int>(kApiSources[i].id)==id) return &kApiSources[i]; return nullptr; }
inline const char* api_source_name(int id){ const auto* s=find_api_source(id); return s?s->name:kApiSources[0].name; }
inline bool api_source_is_valid(int id){ const auto* s=find_api_source(id); return s&&s->enabled; }