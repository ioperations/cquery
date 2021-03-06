#include "semantic_highlight_symbol_cache.h"

SemanticHighlightSymbolCache::Entry::Entry(
    SemanticHighlightSymbolCache* all_caches, const std::string& path)
    : m_all_caches(all_caches), path(path) {}

optional<int> SemanticHighlightSymbolCache::Entry::TryGetStableId(
    SymbolKind kind, const std::string& detailed_name) {
    TNameToId* map = GetMapForSymbol(kind);
    auto it = map->find(detailed_name);
    if (it != map->end()) return it->second;

    return nullopt;
}

int SemanticHighlightSymbolCache::Entry::GetStableId(
    SymbolKind kind, const std::string& detailed_name) {
    optional<int> id = TryGetStableId(kind, detailed_name);
    if (id) return *id;

    // Create a new id. First try to find a key in another map.
    m_all_caches->m_cache.IterateValues(
        [&](const std::shared_ptr<Entry>& entry) {
            optional<int> other_id = entry->TryGetStableId(kind, detailed_name);
            if (other_id) {
                id = other_id;
                return false;
            }
            return true;
        });

    // Create a new id.
    TNameToId* map = GetMapForSymbol(kind);
    if (!id) id = m_all_caches->m_next_stable_id++;
    return (*map)[detailed_name] = *id;
}

SemanticHighlightSymbolCache::Entry::TNameToId*
SemanticHighlightSymbolCache::Entry::GetMapForSymbol(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Type:
            return &detailed_type_name_to_stable_id;
        case SymbolKind::Func:
            return &detailed_func_name_to_stable_id;
        case SymbolKind::Var:
            return &detailed_var_name_to_stable_id;
        case SymbolKind::File:
        case SymbolKind::Invalid:
            break;
    }
    assert(false);
    return nullptr;
}

SemanticHighlightSymbolCache::SemanticHighlightSymbolCache()
    : m_cache(m_k_cache_size) {}

void SemanticHighlightSymbolCache::Init() {
    m_match = std::make_unique<GroupMatch>(g_config->highlight.whitelist,
                                           g_config->highlight.blacklist);
}

std::shared_ptr<SemanticHighlightSymbolCache::Entry>
SemanticHighlightSymbolCache::GetCacheForFile(const std::string& path) {
    return m_cache.Get(
        path, [&, this]() { return std::make_shared<Entry>(this, path); });
}
