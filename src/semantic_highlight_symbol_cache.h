#pragma once

#include <optional.h>

#include <string>
#include <unordered_map>

#include "lru_cache.h"
#include "match.h"
#include "query.h"

// Caches symbols for a single file for semantic highlighting to provide
// relatively stable ids. Only supports xxx files at a time.
struct SemanticHighlightSymbolCache {
    struct Entry {
        SemanticHighlightSymbolCache* m_all_caches = nullptr;

        // The path this cache belongs to.
        std::string path;
        // Detailed symbol name to stable id.
        using TNameToId = std::unordered_map<std::string, int>;
        TNameToId detailed_type_name_to_stable_id;
        TNameToId detailed_func_name_to_stable_id;
        TNameToId detailed_var_name_to_stable_id;

        Entry(SemanticHighlightSymbolCache* all_caches,
              const std::string& path);

        optional<int> TryGetStableId(SymbolKind kind,
                                     const std::string& detailed_name);
        int GetStableId(SymbolKind kind, const std::string& detailed_name);

        TNameToId* GetMapForSymbol(SymbolKind kind);
    };

    constexpr static int m_k_cache_size = 10;
    LruCache<std::string, std::shared_ptr<Entry>> m_cache;
    uint32_t m_next_stable_id = 0;
    std::unique_ptr<GroupMatch> m_match;

    SemanticHighlightSymbolCache();
    void Init();
    std::shared_ptr<Entry> GetCacheForFile(const std::string& path);
};
