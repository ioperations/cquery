#include "query.h"

#include <doctest/doctest.h>
#include <optional.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <loguru.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "indexer.h"
#include "serializer.h"
#include "serializers/json.h"

// TODO: Make all copy constructors explicit.

namespace {

template <typename T>
void VerifyUnique(const std::vector<T>& values0) {
// FIXME: Run on a big code-base for a while and verify no assertions are
// triggered.
#if false
  auto values = values0;
  std::sort(values.begin(), values.end());
  assert(std::unique(values.begin(), values.end()) == values.end());
#endif
}

template <typename T>
void RemoveRange(std::vector<T>* dest, const std::vector<T>& to_remove) {
    std::unordered_set<T> to_remove_lookup(to_remove.begin(), to_remove.end());
    RemoveIf(dest, [&](const T& t) { return to_remove_lookup.count(t) > 0; });
}

optional<QueryType::Def> ToQuery(const IdMap& id_map,
                                 const IndexType::Def& type) {
    if (type.detailed_name.empty()) return nullopt;

    QueryType::Def result;
    result.detailed_name = type.detailed_name;
    result.short_name_offset = type.short_name_offset;
    result.short_name_size = type.short_name_size;
    result.kind = type.kind;
    if (!type.hover.empty()) result.hover = type.hover;
    if (!type.comments.empty()) result.comments = type.comments;
    result.file = id_map.primary_file;
    result.spell = id_map.ToQuery(type.spell);
    result.extent = id_map.ToQuery(type.extent);
    result.alias_of = id_map.ToQuery(type.alias_of);
    result.bases = id_map.ToQuery(type.bases);
    result.types = id_map.ToQuery(type.types);
    result.funcs = id_map.ToQuery(type.funcs);
    result.vars = id_map.ToQuery(type.vars);
    return result;
}

optional<QueryFunc::Def> ToQuery(const IdMap& id_map,
                                 const IndexFunc::Def& func) {
    if (func.detailed_name.empty()) return nullopt;

    QueryFunc::Def result;
    result.detailed_name = func.detailed_name;
    result.short_name_offset = func.short_name_offset;
    result.short_name_size = func.short_name_size;
    result.kind = func.kind;
    result.storage = func.storage;
    if (!func.hover.empty()) result.hover = func.hover;
    if (!func.comments.empty()) result.comments = func.comments;
    result.file = id_map.primary_file;
    result.spell = id_map.ToQuery(func.spell);
    result.extent = id_map.ToQuery(func.extent);
    result.declaring_type = id_map.ToQuery(func.declaring_type);
    result.bases = id_map.ToQuery(func.bases);
    result.vars = id_map.ToQuery(func.vars);
    result.callees = id_map.ToQuery(func.callees);
    return result;
}

optional<QueryVar::Def> ToQuery(const IdMap& id_map, const IndexVar::Def& var) {
    if (var.detailed_name.empty()) return nullopt;

    QueryVar::Def result;
    result.detailed_name = var.detailed_name;
    result.short_name_offset = var.short_name_offset;
    result.short_name_size = var.short_name_size;
    if (!var.hover.empty()) result.hover = var.hover;
    if (!var.comments.empty()) result.comments = var.comments;
    result.file = id_map.primary_file;
    result.spell = id_map.ToQuery(var.spell);
    result.extent = id_map.ToQuery(var.extent);
    result.type = id_map.ToQuery(var.type);
    result.kind = var.kind;
    result.storage = var.storage;
    return result;
}

// Adds the mergeable updates in |source| to |dest|. If a mergeable update for
// the destination type already exists, it will be combined. This makes merging
// updates take longer but reduces import time on the querydb thread.
template <typename TId, typename TValue>
void AddMergeableRange(std::vector<MergeableUpdate<TId, TValue>>* dest,
                       std::vector<MergeableUpdate<TId, TValue>>&& source) {
    // TODO: Consider caching the lookup table. It can probably save even more
    // time at the cost of some additional memory.

    // Build lookup table.
    spp::sparse_hash_map<TId, size_t> id_to_index;
    id_to_index.resize(dest->size());
    for (size_t i = 0; i < dest->size(); ++i) id_to_index[(*dest)[i].id] = i;

    // Add entries. Try to add them to an existing entry.
    for (auto& entry : source) {
        auto it = id_to_index.find(entry.id);
        if (it != id_to_index.end()) {
            AddRange(&(*dest)[it->second].to_add, std::move(entry.to_add));
            AddRange(&(*dest)[it->second].to_remove,
                     std::move(entry.to_remove));
        } else {
            dest->push_back(std::move(entry));
        }
    }
}

// Compares |previous| and |current|, adding all elements that are in |previous|
// but not |current| to |removed|, and all elements that are in |current| but
// not |previous| to |added|.
//
// Returns true iff |removed| or |added| are non-empty.
template <typename T>
bool ComputeDifferenceForUpdate(std::vector<T>&& previous,
                                std::vector<T>&& current,
                                std::vector<T>* removed,
                                std::vector<T>* added) {
    // We need to sort to use std::set_difference.
    std::sort(previous.begin(), previous.end());
    std::sort(current.begin(), current.end());

    auto it0 = previous.begin(), it1 = current.begin();
    while (it0 != previous.end() && it1 != current.end()) {
        // Elements in |previous| that are not in |current|.
        if (*it0 < *it1) removed->push_back(std::move(*it0++));
        // Elements in |current| that are not in |previous|.
        else if (*it1 < *it0)
            added->push_back(std::move(*it1++));
        else
            ++it0, ++it1;
    }
    while (it0 != previous.end()) removed->push_back(std::move(*it0++));
    while (it1 != current.end()) added->push_back(std::move(*it1++));

    return !removed->empty() || !added->empty();
}

template <typename T>
void CompareGroups(std::vector<T>& previous_data, std::vector<T>& current_data,
                   std::function<void(T*)> on_removed,
                   std::function<void(T*)> on_added,
                   std::function<void(T*, T*)> on_found) {
    auto compare_by_usr = [](const T& a, const T& b) { return a.usr < b.usr; };
    std::sort(previous_data.begin(), previous_data.end(), compare_by_usr);
    std::sort(current_data.begin(), current_data.end(), compare_by_usr);

    auto prev_it = previous_data.begin();
    auto curr_it = current_data.begin();
    while (prev_it != previous_data.end() && curr_it != current_data.end()) {
        // same id
        if (prev_it->usr == curr_it->usr) {
            on_found(&*prev_it, &*curr_it);
            ++prev_it;
            ++curr_it;
        }

        // prev_id is smaller - prev_it has data curr_it does not have.
        else if (prev_it->usr < curr_it->usr) {
            on_removed(&*prev_it);
            ++prev_it;
        }

        // prev_id is bigger - curr_it has data prev_it does not have.
        else {
            on_added(&*curr_it);
            ++curr_it;
        }
    }

    // if prev_it still has data, that means it is not in curr_it and was
    // removed.
    while (prev_it != previous_data.end()) {
        on_removed(&*prev_it);
        ++prev_it;
    }

    // if curr_it still has data, that means it is not in prev_it and was added.
    while (curr_it != current_data.end()) {
        on_added(&*curr_it);
        ++curr_it;
    }
}

QueryFile::DefUpdate BuildFileDefUpdate(const IdMap& id_map,
                                        const IndexFile& indexed) {
    QueryFile::Def def;
    def.file = id_map.primary_file;
    def.path = indexed.path;
    def.includes = indexed.includes;
    def.inactive_regions = indexed.skipped_by_preprocessor;
    def.dependencies = indexed.dependencies;

    // Convert enum to markdown compatible strings
    def.language = [&indexed]() {
        switch (indexed.language) {
            case LanguageId::C:
                return "c";
            case LanguageId::Cpp:
                return "cpp";
            case LanguageId::ObjC:
                return "objective-c";
            case LanguageId::ObjCpp:
                return "objective-cpp";
            default:
                return "";
        }
    }();

    auto add_all_symbols = [&](Reference ref, AnyId id, SymbolKind kind) {
        def.all_symbols.push_back(
            QueryId::SymbolRef(ref.range, id, kind, ref.role));
    };
    auto add_outline = [&](Reference ref, AnyId id, SymbolKind kind) {
        def.outline.push_back(
            QueryId::SymbolRef(ref.range, id, kind, ref.role));
    };

    for (const IndexType& type : indexed.types) {
        QueryId::Type id = id_map.ToQuery(type.id);
        if (type.def.spell)
            add_all_symbols(*type.def.spell, id, SymbolKind::Type);
        if (type.def.extent)
            add_outline(*type.def.extent, id, SymbolKind::Type);
        for (auto decl : type.declarations) {
            add_all_symbols(decl, id, SymbolKind::Type);
            // Constructor positions have references to the class,
            // which we do not want to show in textDocument/documentSymbol
            if (!(decl.role & role::Reference))
                add_outline(decl, id, SymbolKind::Type);
        }
        for (Reference use : type.uses)
            add_all_symbols(use, id, SymbolKind::Type);
    }
    for (const IndexFunc& func : indexed.funcs) {
        QueryId::Func id = id_map.ToQuery(func.id);
        if (func.def.spell)
            add_all_symbols(*func.def.spell, id, SymbolKind::Func);
        if (func.def.extent)
            add_outline(*func.def.extent, id, SymbolKind::Func);
        for (const IndexFunc::Declaration& decl : func.declarations) {
            add_all_symbols(decl.spell, id, SymbolKind::Func);
            add_outline(decl.spell, id, SymbolKind::Func);
        }
        for (auto use : func.uses) {
            // Make ranges of implicit function calls larger (spanning one more
            // column to the left/right). This is hacky but useful. e.g.
            // textDocument/definition on the space/semicolon in `A a;` or
            // `return 42;` will take you to the constructor.
            if (use.role & role::Implicit) {
                if (use.range.start.column > 0) use.range.start.column--;
                use.range.end.column++;
            }
            add_all_symbols(use, id, SymbolKind::Func);
        }
    }
    for (const IndexVar& var : indexed.vars) {
        QueryId::Var id = id_map.ToQuery(var.id);
        if (var.def.spell) add_all_symbols(*var.def.spell, id, SymbolKind::Var);
        if (var.def.extent) add_outline(*var.def.extent, id, SymbolKind::Var);
        for (auto decl : var.declarations) {
            add_all_symbols(decl, id, SymbolKind::Var);
            add_outline(decl, id, SymbolKind::Var);
        }
        for (auto use : var.uses) add_all_symbols(use, id, SymbolKind::Var);
    }

    std::sort(def.outline.begin(), def.outline.end(),
              [](const QueryId::SymbolRef& a, const QueryId::SymbolRef& b) {
                  return a.range.start < b.range.start;
              });
    std::sort(def.all_symbols.begin(), def.all_symbols.end(),
              [](const QueryId::SymbolRef& a, const QueryId::SymbolRef& b) {
                  return a.range.start < b.range.start;
              });

    return QueryFile::DefUpdate{id_map.primary_file, indexed.file_contents,
                                def};
}

Maybe<QueryId::File> GetQueryFileIdFromPath(QueryDatabase* query_db,
                                            const AbsolutePath& path) {
    auto it = query_db->usr_to_file.find(path);
    if (it != query_db->usr_to_file.end()) return QueryId::File(it->second.id);

    RawId idx = query_db->files.size();
    query_db->usr_to_file[path] = QueryId::File(idx);
    query_db->files.push_back(QueryFile(path));
    return QueryId::File(idx);
}

Maybe<QueryId::Type> GetQueryTypeIdFromUsr(QueryDatabase* query_db, Usr usr) {
    auto it = query_db->usr_to_type.find(usr);
    if (it != query_db->usr_to_type.end()) return QueryId::Type(it->second.id);

    RawId idx = query_db->types.size();
    query_db->usr_to_type[usr] = QueryId::Type(idx);
    query_db->types.push_back(QueryType(usr));
    return QueryId::Type(idx);
}

Maybe<QueryId::Func> GetQueryFuncIdFromUsr(QueryDatabase* query_db, Usr usr) {
    auto it = query_db->usr_to_func.find(usr);
    if (it != query_db->usr_to_func.end()) return QueryId::Func(it->second.id);

    RawId idx = query_db->funcs.size();
    query_db->usr_to_func[usr] = QueryId::Func(idx);
    query_db->funcs.push_back(QueryFunc(usr));
    return QueryId::Func(idx);
}

Maybe<QueryId::Var> GetQueryVarIdFromUsr(QueryDatabase* query_db, Usr usr) {
    auto it = query_db->usr_to_var.find(usr);
    if (it != query_db->usr_to_var.end()) return QueryId::Var(it->second.id);

    RawId idx = query_db->vars.size();
    query_db->usr_to_var[usr] = QueryId::Var(idx);
    query_db->vars.push_back(QueryVar(usr));
    return QueryId::Var(idx);
}

// Returns true if an element with the same file is found.
template <typename Q>
bool TryReplaceDef(std::vector<Q>& def_list, Q&& def) {
    for (auto& def1 : def_list)
        if (def1.file == def.file) {
            if (!def1.spell || def.spell) def1 = std::move(def);
            return true;
        }
    return false;
}

// Adds an element to the front of the vector, potentially swapping the current
// front element to the back.
template <typename T>
void PushFront(std::vector<T>& v, T&& value) {
    if (v.empty()) {
        v.push_back(value);
        return;
    }

    v.push_back(v.front());
    v[0] = value;
}

}  // namespace

IdMap::IdMap(QueryDatabase* query_db, const IdCache& local_ids)
    : local_ids(local_ids) {
    primary_file = *GetQueryFileIdFromPath(query_db, local_ids.primary_file);

    m_cached_type_ids.resize(local_ids.type_id_to_usr.size());
    for (const auto& entry : local_ids.type_id_to_usr)
        m_cached_type_ids[entry.first] =
            *GetQueryTypeIdFromUsr(query_db, entry.second);

    m_cached_func_ids.resize(local_ids.func_id_to_usr.size());
    for (const auto& entry : local_ids.func_id_to_usr)
        m_cached_func_ids[entry.first] =
            *GetQueryFuncIdFromUsr(query_db, entry.second);

    m_cached_var_ids.resize(local_ids.var_id_to_usr.size());
    for (const auto& entry : local_ids.var_id_to_usr)
        m_cached_var_ids[entry.first] =
            *GetQueryVarIdFromUsr(query_db, entry.second);
}

Id<void> IdMap::ToQuery(SymbolKind kind, Id<void> id) const {
    switch (kind) {
        case SymbolKind::File:
            return primary_file;
        case SymbolKind::Type:
            return ToQuery(IndexId::Type(id.id));
        case SymbolKind::Func:
            return ToQuery(IndexId::Func(id.id));
        case SymbolKind::Var:
            return ToQuery(IndexId::Var(id.id));
        case SymbolKind::Invalid:
            break;
    }
    assert(false);
    return Id<void>(-1);
}

QueryId::Type IdMap::ToQuery(IndexId::Type id) const {
    assert(m_cached_type_ids.find(id) != m_cached_type_ids.end());
    return QueryId::Type(m_cached_type_ids.find(id)->second);
}
QueryId::Func IdMap::ToQuery(IndexId::Func id) const {
    assert(m_cached_func_ids.find(id) != m_cached_func_ids.end());
    return QueryId::Func(m_cached_func_ids.find(id)->second);
}
QueryId::Var IdMap::ToQuery(IndexId::Var id) const {
    assert(m_cached_var_ids.find(id) != m_cached_var_ids.end());
    return QueryId::Var(m_cached_var_ids.find(id)->second);
}

QueryId::SymbolRef IdMap::ToQuery(IndexId::SymbolRef ref) const {
    QueryId::SymbolRef result;
    result.range = ref.range;
    result.id = ToQuery(ref.kind, ref.id);
    result.kind = ref.kind;
    result.role = ref.role;
    return result;
}
QueryId::LexicalRef IdMap::ToQuery(IndexId::LexicalRef ref) const {
    QueryId::LexicalRef result;
    result.file = primary_file;
    result.range = ref.range;
    result.id = ToQuery(ref.kind, ref.id);
    result.kind = ref.kind;
    result.role = ref.role;
    return result;
}
QueryId::LexicalRef IdMap::ToQuery(IndexFunc::Declaration decl) const {
    return ToQuery(decl.spell);
}

// ----------------------
// INDEX THREAD FUNCTIONS
// ----------------------

// static
IndexUpdate IndexUpdate::CreateDelta(const IdMap* previous_id_map,
                                     const IdMap* current_id_map,
                                     IndexFile* previous, IndexFile* current) {
    // This function runs on an indexer thread.

    if (!previous_id_map) {
        assert(!previous);
        IndexFile empty(current->path);
        return IndexUpdate(*current_id_map, *current_id_map, empty, *current);
    }
    return IndexUpdate(*previous_id_map, *current_id_map, *previous, *current);
}

IndexUpdate::IndexUpdate(const IdMap& previous_id_map,
                         const IdMap& current_id_map, IndexFile& previous_file,
                         IndexFile& current_file) {
// This function runs on an indexer thread.

// |query_name| is the name of the variable on the query type.
// |index_name| is the name of the variable on the index type.
// |type| is the type of the variable.
#define PROCESS_UPDATE_DIFF(type_id, query_name, index_name, type)           \
    {                                                                        \
        /* Check for changes. */                                             \
        std::vector<type> removed, added;                                    \
        auto query_previous = previous_id_map.ToQuery(previous->index_name); \
        auto query_current = current_id_map.ToQuery(current->index_name);    \
        bool did_add = ComputeDifferenceForUpdate(std::move(query_previous), \
                                                  std::move(query_current),  \
                                                  &removed, &added);         \
        if (did_add) {                                                       \
            query_name.push_back(MergeableUpdate<type_id, type>(             \
                current_id_map.ToQuery(current->id), std::move(added),       \
                std::move(removed)));                                        \
        }                                                                    \
    }
    // File
    files_def_update.push_back(
        BuildFileDefUpdate(current_id_map, current_file));

    // **NOTE** We only remove entries if they were defined in the previous
    // index. For example, if a type is included from another file it will be
    // defined simply so we can attribute the usage/reference to it. If the
    // reference goes away we don't want to remove the type/func/var usage.

    // Types
    CompareGroups<IndexType>(
        previous_file.types, current_file.types,
        /*onRemoved:*/
        [this, &previous_id_map](IndexType* type) {
            if (type->def.spell)
                types_removed.push_back(WithId<QueryId::File, QueryId::Type>(
                    previous_id_map.primary_file,
                    previous_id_map.ToQuery(type->id)));
            if (!type->declarations.empty())
                types_declarations.push_back(QueryType::DeclarationsUpdate(
                    previous_id_map.ToQuery(type->id), {},
                    previous_id_map.ToQuery(type->declarations)));
            if (!type->derived.empty())
                types_derived.push_back(QueryType::DerivedUpdate(
                    previous_id_map.ToQuery(type->id), {},
                    previous_id_map.ToQuery(type->derived)));
            if (!type->instances.empty())
                types_instances.push_back(QueryType::InstancesUpdate(
                    previous_id_map.ToQuery(type->id), {},
                    previous_id_map.ToQuery(type->instances)));
            if (!type->uses.empty())
                types_uses.push_back(
                    QueryType::UsesUpdate(previous_id_map.ToQuery(type->id), {},
                                          previous_id_map.ToQuery(type->uses)));
        },
        /*onAdded:*/
        [this, &current_id_map](IndexType* type) {
            optional<QueryType::Def> def_update =
                ToQuery(current_id_map, type->def);
            if (def_update)
                types_def_update.push_back(QueryType::DefUpdate(
                    current_id_map.ToQuery(type->id), std::move(*def_update)));
            if (!type->declarations.empty())
                types_declarations.push_back(QueryType::DeclarationsUpdate(
                    current_id_map.ToQuery(type->id),
                    current_id_map.ToQuery(type->declarations)));
            if (!type->derived.empty())
                types_derived.push_back(QueryType::DerivedUpdate(
                    current_id_map.ToQuery(type->id),
                    current_id_map.ToQuery(type->derived)));
            if (!type->instances.empty())
                types_instances.push_back(QueryType::InstancesUpdate(
                    current_id_map.ToQuery(type->id),
                    current_id_map.ToQuery(type->instances)));
            if (!type->uses.empty())
                types_uses.push_back(
                    QueryType::UsesUpdate(current_id_map.ToQuery(type->id),
                                          current_id_map.ToQuery(type->uses)));
        },
        /*onFound:*/
        [this, &previous_id_map, &current_id_map](IndexType* previous,
                                                  IndexType* current) {
            optional<QueryType::Def> previous_remapped_def =
                ToQuery(previous_id_map, previous->def);
            optional<QueryType::Def> current_remapped_def =
                ToQuery(current_id_map, current->def);
            if (current_remapped_def &&
                previous_remapped_def != current_remapped_def &&
                !current_remapped_def->detailed_name.empty()) {
                types_def_update.push_back(
                    QueryType::DefUpdate(current_id_map.ToQuery(current->id),
                                         std::move(*current_remapped_def)));
            }

            PROCESS_UPDATE_DIFF(QueryId::Type, types_declarations, declarations,
                                QueryId::LexicalRef);
            PROCESS_UPDATE_DIFF(QueryId::Type, types_derived, derived,
                                QueryId::Type);
            PROCESS_UPDATE_DIFF(QueryId::Type, types_instances, instances,
                                QueryId::Var);
            PROCESS_UPDATE_DIFF(QueryId::Type, types_uses, uses,
                                QueryId::LexicalRef);
        });

    // Functions
    CompareGroups<IndexFunc>(
        previous_file.funcs, current_file.funcs,
        /*onRemoved:*/
        [this, &previous_id_map](IndexFunc* func) {
            if (func->def.spell)
                funcs_removed.push_back(WithId<QueryId::File, QueryId::Func>(
                    previous_id_map.primary_file,
                    previous_id_map.ToQuery(func->id)));
            if (!func->declarations.empty())
                funcs_declarations.push_back(QueryFunc::DeclarationsUpdate(
                    previous_id_map.ToQuery(func->id), {},
                    previous_id_map.ToQuery(func->declarations)));
            if (!func->derived.empty())
                funcs_derived.push_back(QueryFunc::DerivedUpdate(
                    previous_id_map.ToQuery(func->id), {},
                    previous_id_map.ToQuery(func->derived)));
            if (!func->uses.empty())
                funcs_uses.push_back(
                    QueryFunc::UsesUpdate(previous_id_map.ToQuery(func->id), {},
                                          previous_id_map.ToQuery(func->uses)));
        },
        /*onAdded:*/
        [this, &current_id_map](IndexFunc* func) {
            optional<QueryFunc::Def> def_update =
                ToQuery(current_id_map, func->def);
            if (def_update)
                funcs_def_update.push_back(QueryFunc::DefUpdate(
                    current_id_map.ToQuery(func->id), std::move(*def_update)));
            if (!func->declarations.empty())
                funcs_declarations.push_back(QueryFunc::DeclarationsUpdate(
                    current_id_map.ToQuery(func->id),
                    current_id_map.ToQuery(func->declarations)));
            if (!func->derived.empty())
                funcs_derived.push_back(QueryFunc::DerivedUpdate(
                    current_id_map.ToQuery(func->id),
                    current_id_map.ToQuery(func->derived)));
            if (!func->uses.empty())
                funcs_uses.push_back(
                    QueryFunc::UsesUpdate(current_id_map.ToQuery(func->id),
                                          current_id_map.ToQuery(func->uses)));
        },
        /*onFound:*/
        [this, &previous_id_map, &current_id_map](IndexFunc* previous,
                                                  IndexFunc* current) {
            optional<QueryFunc::Def> previous_remapped_def =
                ToQuery(previous_id_map, previous->def);
            optional<QueryFunc::Def> current_remapped_def =
                ToQuery(current_id_map, current->def);
            if (current_remapped_def &&
                previous_remapped_def != current_remapped_def &&
                !current_remapped_def->detailed_name.empty()) {
                funcs_def_update.push_back(
                    QueryFunc::DefUpdate(current_id_map.ToQuery(current->id),
                                         std::move(*current_remapped_def)));
            }

            PROCESS_UPDATE_DIFF(QueryId::Func, funcs_declarations, declarations,
                                QueryId::LexicalRef);
            PROCESS_UPDATE_DIFF(QueryId::Func, funcs_derived, derived,
                                QueryId::Func);
            PROCESS_UPDATE_DIFF(QueryId::Func, funcs_uses, uses,
                                QueryId::LexicalRef);
        });

    // Variables
    CompareGroups<IndexVar>(
        previous_file.vars, current_file.vars,
        /*onRemoved:*/
        [this, &previous_id_map](IndexVar* var) {
            if (var->def.spell)
                vars_removed.push_back(WithId<QueryId::File, QueryId::Var>(
                    previous_id_map.primary_file,
                    previous_id_map.ToQuery(var->id)));
            if (!var->declarations.empty())
                vars_declarations.push_back(QueryVar::DeclarationsUpdate(
                    previous_id_map.ToQuery(var->id), {},
                    previous_id_map.ToQuery(var->declarations)));
            if (!var->uses.empty())
                vars_uses.push_back(
                    QueryVar::UsesUpdate(previous_id_map.ToQuery(var->id), {},
                                         previous_id_map.ToQuery(var->uses)));
        },
        /*onAdded:*/
        [this, &current_id_map](IndexVar* var) {
            optional<QueryVar::Def> def_update =
                ToQuery(current_id_map, var->def);
            if (def_update)
                vars_def_update.push_back(QueryVar::DefUpdate(
                    current_id_map.ToQuery(var->id), std::move(*def_update)));
            if (!var->declarations.empty())
                vars_declarations.push_back(QueryVar::DeclarationsUpdate(
                    current_id_map.ToQuery(var->id),
                    current_id_map.ToQuery(var->declarations)));
            if (!var->uses.empty())
                vars_uses.push_back(
                    QueryVar::UsesUpdate(current_id_map.ToQuery(var->id),
                                         current_id_map.ToQuery(var->uses)));
        },
        /*onFound:*/
        [this, &previous_id_map, &current_id_map](IndexVar* previous,
                                                  IndexVar* current) {
            optional<QueryVar::Def> previous_remapped_def =
                ToQuery(previous_id_map, previous->def);
            optional<QueryVar::Def> current_remapped_def =
                ToQuery(current_id_map, current->def);
            if (current_remapped_def &&
                previous_remapped_def != current_remapped_def &&
                !current_remapped_def->detailed_name.empty())
                vars_def_update.push_back(
                    QueryVar::DefUpdate(current_id_map.ToQuery(current->id),
                                        std::move(*current_remapped_def)));

            PROCESS_UPDATE_DIFF(QueryId::Var, vars_declarations, declarations,
                                QueryId::LexicalRef);
            PROCESS_UPDATE_DIFF(QueryId::Var, vars_uses, uses,
                                QueryId::LexicalRef);
        });

#undef PROCESS_UPDATE_DIFF
}

// This function runs on an indexer thread.
void IndexUpdate::Merge(IndexUpdate&& update) {
#define INDEX_UPDATE_APPEND(name) AddRange(&name, std::move(update.name));
#define INDEX_UPDATE_MERGE(name) \
    AddMergeableRange(&name, std::move(update.name));

    INDEX_UPDATE_APPEND(files_removed);
    INDEX_UPDATE_APPEND(files_def_update);

    INDEX_UPDATE_APPEND(types_removed);
    INDEX_UPDATE_APPEND(types_def_update);
    INDEX_UPDATE_MERGE(types_derived);
    INDEX_UPDATE_MERGE(types_instances);
    INDEX_UPDATE_MERGE(types_uses);

    INDEX_UPDATE_APPEND(funcs_removed);
    INDEX_UPDATE_APPEND(funcs_def_update);
    INDEX_UPDATE_MERGE(funcs_declarations);
    INDEX_UPDATE_MERGE(funcs_derived);
    INDEX_UPDATE_MERGE(funcs_uses);

    INDEX_UPDATE_APPEND(vars_removed);
    INDEX_UPDATE_APPEND(vars_def_update);
    INDEX_UPDATE_MERGE(vars_declarations);
    INDEX_UPDATE_MERGE(vars_uses);

#undef INDEX_UPDATE_APPEND
#undef INDEX_UPDATE_MERGE
}

// ------------------------
// QUERYDB THREAD FUNCTIONS
// ------------------------

// When we remove an element, we just erase the state from the storage. We do
// not update array indices because that would take a huge amount of time for a
// very large index.
//
// There means that there is some memory growth that will never be reclaimed,
// but it should be pretty minimal and is solved by simply restarting the
// indexer and loading from cache, which is a fast operation.
//
// TODO: Add "cquery: Reload Index" command which unloads all querydb state and
// fully reloads from cache. This will address the memory leak above.
void QueryDatabase::Remove(
    const std::vector<WithId<QueryId::File, QueryId::Type>>& to_remove) {
    for (const auto& entry : to_remove) {
        QueryId::File file_id = entry.id;
        QueryId::Type type_id = entry.value;

        QueryType& type = types[type_id.id];
        RemoveIf(&type.def, [&](const QueryType::Def& def) {
            return def.file == file_id;
        });
        if (type.symbol_idx != size_t(-1) && type.def.empty())
            symbols[type.symbol_idx].kind = SymbolKind::Invalid;
    }
}

void QueryDatabase::Remove(
    const std::vector<WithId<QueryId::File, QueryId::Func>>& to_remove) {
    for (const auto& entry : to_remove) {
        QueryId::File file_id = entry.id;
        QueryId::Func func_id = entry.value;

        QueryFunc& func = funcs[func_id.id];
        RemoveIf(&func.def, [&](const QueryFunc::Def& def) {
            return def.file == file_id;
        });
        if (func.symbol_idx != size_t(-1) && func.def.empty())
            symbols[func.symbol_idx].kind = SymbolKind::Invalid;
    }
}
void QueryDatabase::Remove(
    const std::vector<WithId<QueryId::File, QueryId::Var>>& to_remove) {
    for (const auto& entry : to_remove) {
        QueryId::File file_id = entry.id;
        QueryId::Var var_id = entry.value;

        QueryVar& var = vars[var_id.id];
        RemoveIf(&var.def,
                 [&](const QueryVar::Def& def) { return def.file == file_id; });
        if (var.symbol_idx != size_t(-1) && var.def.empty())
            symbols[var.symbol_idx].kind = SymbolKind::Invalid;
    }
}

void QueryDatabase::ApplyIndexUpdate(IndexUpdate* update) {
// This function runs on the querydb thread.

// Example types:
//  storage_name       =>  std::vector<optional<QueryType>>
//  merge_update       =>  QueryType::DerivedUpdate =>
//  MergeableUpdate<QueryId::Type, QueryId::Type> def                =>
//  QueryType def->def_var_name  =>  std::vector<QueryId::Type>
#define HANDLE_MERGEABLE(update_var_name, def_var_name, storage_name) \
    for (auto merge_update : update->update_var_name) {               \
        auto& def = storage_name[merge_update.id.id];                 \
        AddRange(&def.def_var_name, merge_update.to_add);             \
        RemoveRange(&def.def_var_name, merge_update.to_remove);       \
        VerifyUnique(def.def_var_name);                               \
    }

    for (const AbsolutePath& filename : update->files_removed)
        files[usr_to_file[filename].id].def = nullopt;
    ImportOrUpdate(update->files_def_update);

    Remove(update->types_removed);
    ImportOrUpdate(std::move(update->types_def_update));
    HANDLE_MERGEABLE(types_declarations, declarations, types);
    HANDLE_MERGEABLE(types_derived, derived, types);
    HANDLE_MERGEABLE(types_instances, instances, types);
    HANDLE_MERGEABLE(types_uses, uses, types);

    Remove(update->funcs_removed);
    ImportOrUpdate(std::move(update->funcs_def_update));
    HANDLE_MERGEABLE(funcs_declarations, declarations, funcs);
    HANDLE_MERGEABLE(funcs_derived, derived, funcs);
    HANDLE_MERGEABLE(funcs_uses, uses, funcs);

    Remove(update->vars_removed);
    ImportOrUpdate(std::move(update->vars_def_update));
    HANDLE_MERGEABLE(vars_declarations, declarations, vars);
    HANDLE_MERGEABLE(vars_uses, uses, vars);

#undef HANDLE_MERGEABLE
}

void QueryDatabase::ImportOrUpdate(
    const std::vector<QueryFile::DefUpdate>& updates) {
    // This function runs on the querydb thread.

    for (auto& def : updates) {
        assert(def.id.id >= 0 && def.id.id < files.size());
        QueryFile& existing = files[def.id.id];

        existing.def = def.value;
        UpdateSymbols(&existing.symbol_idx, SymbolKind::File, def.id);
    }
}

void QueryDatabase::ImportOrUpdate(
    std::vector<QueryType::DefUpdate>&& updates) {
    // This function runs on the querydb thread.

    for (auto& def : updates) {
        assert(!def.value.detailed_name.empty());
        assert(def.id.id >= 0 && def.id.id < types.size());
        QueryType& existing = types[def.id.id];
        if (!TryReplaceDef(existing.def, std::move(def.value))) {
            PushFront(existing.def, std::move(def.value));
            UpdateSymbols(&existing.symbol_idx, SymbolKind::Type, def.id);
        }
    }
}

void QueryDatabase::ImportOrUpdate(
    std::vector<QueryFunc::DefUpdate>&& updates) {
    // This function runs on the querydb thread.

    for (auto& def : updates) {
        assert(!def.value.detailed_name.empty());
        assert(def.id.id >= 0 && def.id.id < funcs.size());
        QueryFunc& existing = funcs[def.id.id];
        if (!TryReplaceDef(existing.def, std::move(def.value))) {
            PushFront(existing.def, std::move(def.value));
            UpdateSymbols(&existing.symbol_idx, SymbolKind::Func, def.id);
        }
    }
}

void QueryDatabase::ImportOrUpdate(std::vector<QueryVar::DefUpdate>&& updates) {
    // This function runs on the querydb thread.

    for (auto& def : updates) {
        assert(!def.value.detailed_name.empty());
        assert(def.id.id >= 0 && def.id.id < vars.size());
        QueryVar& existing = vars[def.id.id];
        if (!TryReplaceDef(existing.def, std::move(def.value))) {
            PushFront(existing.def, std::move(def.value));
            if (!existing.def.front().IsLocal())
                UpdateSymbols(&existing.symbol_idx, SymbolKind::Var, def.id);
        }
    }
}

void QueryDatabase::UpdateSymbols(size_t* symbol_idx, SymbolKind kind,
                                  AnyId idx) {
    if (*symbol_idx == -1) {
        *symbol_idx = symbols.size();
        symbols.push_back(SymbolIdx{idx, kind});
    }
}

// For Func, the returned name does not include parameters.
std::string_view QueryDatabase::GetSymbolDetailedName(RawId symbol_idx) const {
    RawId idx = symbols[symbol_idx].id.id;
    switch (symbols[symbol_idx].kind) {
        default:
            break;
        case SymbolKind::File:
            if (files[idx].def) return files[idx].def->path.path;
            break;
        case SymbolKind::Func:
            if (const auto* def = funcs[idx].AnyDef())
                return def->DetailedName(false);
            break;
        case SymbolKind::Type:
            if (const auto* def = types[idx].AnyDef())
                return def->detailed_name;
            break;
        case SymbolKind::Var:
            if (const auto* def = vars[idx].AnyDef()) return def->detailed_name;
            break;
    }
    return "";
}

std::string_view QueryDatabase::GetSymbolShortName(RawId symbol_idx) const {
    RawId idx = symbols[symbol_idx].id.id;
    switch (symbols[symbol_idx].kind) {
        default:
            break;
        case SymbolKind::File:
            if (files[idx].def) return files[idx].def->path.path;
            break;
        case SymbolKind::Func:
            if (const auto* def = funcs[idx].AnyDef()) return def->ShortName();
            break;
        case SymbolKind::Type:
            if (const auto* def = types[idx].AnyDef()) return def->ShortName();
            break;
        case SymbolKind::Var:
            if (const auto* def = vars[idx].AnyDef()) return def->ShortName();
            break;
    }
    return "";
}

QueryFile& QueryDatabase::GetFile(QueryId::File id) { return files[id.id]; }
QueryFunc& QueryDatabase::GetFunc(QueryId::Func id) { return funcs[id.id]; }
QueryType& QueryDatabase::GetType(QueryId::Type id) { return types[id.id]; }
QueryVar& QueryDatabase::GetVar(QueryId::Var id) { return vars[id.id]; }

QueryFile& QueryDatabase::GetFile(SymbolIdx id) {
    assert(id.kind == SymbolKind::File);
    return files[id.id.id];
}
QueryType& QueryDatabase::GetType(SymbolIdx id) {
    assert(id.kind == SymbolKind::Type);
    return types[id.id.id];
}
QueryFunc& QueryDatabase::GetFunc(SymbolIdx id) {
    assert(id.kind == SymbolKind::Func);
    return funcs[id.id.id];
}
QueryVar& QueryDatabase::GetVar(SymbolIdx id) {
    assert(id.kind == SymbolKind::Var);
    return vars[id.id.id];
}

TEST_SUITE("query") {
    IndexUpdate GetDelta(IndexFile previous, IndexFile current) {
        QueryDatabase db;
        IdMap previous_map(&db, previous.id_cache);
        IdMap current_map(&db, current.id_cache);
        return IndexUpdate::CreateDelta(&previous_map, &current_map, &previous,
                                        &current);
    }

    TEST_CASE("remove defs") {
        IndexFile previous(AbsolutePath("foo.cc"));
        IndexFile current(AbsolutePath("foo.cc"));

        previous.Resolve(previous.ToTypeId(HashUsr("usr1")))->def.spell =
            IndexId::LexicalRef(Range(Position(1, 0)), {}, {}, {});
        previous.Resolve(previous.ToFuncId(HashUsr("usr2")))->def.spell =
            IndexId::LexicalRef(Range(Position(2, 0)), {}, {}, {});
        previous.Resolve(previous.ToVarId(HashUsr("usr3")))->def.spell =
            IndexId::LexicalRef(Range(Position(3, 0)), {}, {}, {});

        IndexUpdate update = GetDelta(previous, current);

        REQUIRE(update.types_removed.size() == 1);
        REQUIRE(update.types_removed[0].id.id == 0);
        REQUIRE(update.funcs_removed.size() == 1);
        REQUIRE(update.funcs_removed[0].id.id == 0);
        REQUIRE(update.vars_removed.size() == 1);
        REQUIRE(update.vars_removed[0].id.id == 0);
    }

    TEST_CASE("do not remove ref-only defs") {
        IndexFile previous(AbsolutePath("foo.cc"));
        IndexFile current(AbsolutePath("foo.cc"));

        previous.Resolve(previous.ToTypeId(HashUsr("usr1")))
            ->uses.push_back(IndexId::LexicalRef(
                Range(Position(1, 0)), AnyId(0), SymbolKind::Func, {}));
        previous.Resolve(previous.ToFuncId(HashUsr("usr2")))
            ->uses.push_back(IndexId::LexicalRef(
                Range(Position(2, 0)), AnyId(0), SymbolKind::Func, {}));
        previous.Resolve(previous.ToVarId(HashUsr("usr3")))
            ->uses.push_back(IndexId::LexicalRef(
                Range(Position(3, 0)), AnyId(0), SymbolKind::Func, {}));

        IndexUpdate update = GetDelta(previous, current);

        REQUIRE(update.types_removed.empty());
        REQUIRE(update.funcs_removed.empty());
        REQUIRE(update.vars_removed.empty());
    }

    TEST_CASE("func callers") {
        IndexFile previous(AbsolutePath("foo.cc"));
        IndexFile current(AbsolutePath("foo.cc"));

        IndexFunc* pf = previous.Resolve(previous.ToFuncId(HashUsr("usr")));
        IndexFunc* cf = current.Resolve(current.ToFuncId(HashUsr("usr")));

        pf->uses.push_back(IndexId::LexicalRef(Range(Position(1, 0)), AnyId(0),
                                               SymbolKind::Func, {}));
        cf->uses.push_back(IndexId::LexicalRef(Range(Position(2, 0)), AnyId(0),
                                               SymbolKind::Func, {}));

        IndexUpdate update = GetDelta(previous, current);

        REQUIRE(update.funcs_removed.empty());
        REQUIRE(update.funcs_uses.size() == 1);
        REQUIRE(update.funcs_uses[0].id == QueryId::Func(0));
        REQUIRE(update.funcs_uses[0].to_remove.size() == 1);
        REQUIRE(update.funcs_uses[0].to_remove[0].range ==
                Range(Position(1, 0)));
        REQUIRE(update.funcs_uses[0].to_add.size() == 1);
        REQUIRE(update.funcs_uses[0].to_add[0].range == Range(Position(2, 0)));
    }

    TEST_CASE("type usages") {
        IndexFile previous(AbsolutePath("foo.cc"));
        IndexFile current(AbsolutePath("foo.cc"));

        IndexType* pt = previous.Resolve(previous.ToTypeId(HashUsr("usr")));
        IndexType* ct = current.Resolve(current.ToTypeId(HashUsr("usr")));

        pt->uses.push_back(IndexId::LexicalRef(Range(Position(1, 0)), AnyId(0),
                                               SymbolKind::Type, {}));
        ct->uses.push_back(IndexId::LexicalRef(Range(Position(2, 0)), AnyId(0),
                                               SymbolKind::Type, {}));

        IndexUpdate update = GetDelta(previous, current);

        REQUIRE(update.types_removed.empty());
        REQUIRE(update.types_def_update.empty());
        REQUIRE(update.types_uses.size() == 1);
        REQUIRE(update.types_uses[0].to_remove.size() == 1);
        REQUIRE(update.types_uses[0].to_remove[0].range ==
                Range(Position(1, 0)));
        REQUIRE(update.types_uses[0].to_add.size() == 1);
        REQUIRE(update.types_uses[0].to_add[0].range == Range(Position(2, 0)));
    }

    TEST_CASE("apply delta") {
        IndexFile previous(AbsolutePath("foo.cc"));
        IndexFile current(AbsolutePath("foo.cc"));

        IndexFunc* pf = previous.Resolve(previous.ToFuncId(HashUsr("usr")));
        IndexFunc* cf = current.Resolve(current.ToFuncId(HashUsr("usr")));
        pf->uses.push_back(IndexId::LexicalRef(Range(Position(1, 0)), AnyId(0),
                                               SymbolKind::Func, {}));
        pf->uses.push_back(IndexId::LexicalRef(Range(Position(2, 0)), AnyId(0),
                                               SymbolKind::Func, {}));
        cf->uses.push_back(IndexId::LexicalRef(Range(Position(4, 0)), AnyId(0),
                                               SymbolKind::Func, {}));
        cf->uses.push_back(IndexId::LexicalRef(Range(Position(5, 0)), AnyId(0),
                                               SymbolKind::Func, {}));

        QueryDatabase db;
        IdMap previous_map(&db, previous.id_cache);
        IdMap current_map(&db, current.id_cache);
        REQUIRE(db.funcs.size() == 1);

        IndexUpdate import_update = IndexUpdate::CreateDelta(
            nullptr, &previous_map, nullptr, &previous);
        IndexUpdate delta_update = IndexUpdate::CreateDelta(
            &previous_map, &current_map, &previous, &current);

        db.ApplyIndexUpdate(&import_update);
        REQUIRE(db.funcs[0].uses.size() == 2);
        REQUIRE(db.funcs[0].uses[0].range == Range(Position(1, 0)));
        REQUIRE(db.funcs[0].uses[1].range == Range(Position(2, 0)));

        db.ApplyIndexUpdate(&delta_update);
        REQUIRE(db.funcs[0].uses.size() == 2);
        REQUIRE(db.funcs[0].uses[0].range == Range(Position(4, 0)));
        REQUIRE(db.funcs[0].uses[1].range == Range(Position(5, 0)));
    }

    TEST_CASE("Remove variable with usage") {
        auto load_index_from_json = [](const char* json) {
            return Deserialize(serialize_format::Json,
                               AbsolutePath::BuildDoNotUse("foo.cc"), json,
                               "<empty>", nullopt);
        };

        auto previous = load_index_from_json(R"RAW(
{
  "types": [
    {
      "id": 0,
      "usr": 17,
      "detailed_name": "",
      "short_name_offset": 0,
      "short_name_size": 0,
      "kind": 0,
      "hover": "",
      "comments": "",
      "parents": [],
      "derived": [],
      "types": [],
      "funcs": [],
      "vars": [],
      "instances": [
        0
      ],
      "uses": []
    }
  ],
  "funcs": [
    {
      "id": 0,
      "usr": 4259594751088586730,
      "detailed_name": "void foo()",
      "short_name_offset": 5,
      "short_name_size": 3,
      "kind": 12,
      "storage": 1,
      "hover": "",
      "comments": "",
      "declarations": [],
      "spell": "1:6-1:9|-1|1|2",
      "extent": "1:1-4:2|-1|1|0",
      "base": [],
      "derived": [],
      "locals": [],
      "uses": [],
      "callees": []
    }
  ],
  "vars": [
    {
      "id": 0,
      "usr": 16837348799350457167,
      "detailed_name": "int a",
      "short_name_offset": 4,
      "short_name_size": 1,
      "hover": "",
      "comments": "",
      "declarations": [],
      "spell": "2:7-2:8|0|3|2",
      "extent": "2:3-2:8|0|3|2",
      "type": 0,
      "uses": [
        "3:3-3:4|0|3|4"
      ],
      "kind": 13,
      "storage": 1
    }
  ]
}
    )RAW");

        auto current = load_index_from_json(R"RAW(
{
  "types": [],
  "funcs": [
    {
      "id": 0,
      "usr": 4259594751088586730,
      "detailed_name": "void foo()",
      "short_name_offset": 5,
      "short_name_size": 3,
      "kind": 12,
      "storage": 1,
      "hover": "",
      "comments": "",
      "declarations": [],
      "spell": "1:6-1:9|-1|1|2",
      "extent": "1:1-5:2|-1|1|0",
      "base": [],
      "derived": [],
      "locals": [],
      "uses": [],
      "callees": []
    }
  ],
  "vars": []
}
    )RAW");

        // Validate previous/current were parsed.
        REQUIRE(previous->vars.size() == 1);
        REQUIRE(current->vars.size() == 0);

        QueryDatabase db;

        // Apply initial file.
        {
            IdMap previous_map(&db, previous->id_cache);
            IndexUpdate import_update = IndexUpdate::CreateDelta(
                nullptr, &previous_map, nullptr, previous.get());
            db.ApplyIndexUpdate(&import_update);
        }

        REQUIRE(db.vars.size() == 1);
        REQUIRE(db.vars[0].uses.size() == 1);

        // Apply change.
        {
            IdMap previous_map(&db, previous->id_cache);
            IdMap current_map(&db, current->id_cache);
            IndexUpdate delta_update = IndexUpdate::CreateDelta(
                &previous_map, &current_map, previous.get(), current.get());
            db.ApplyIndexUpdate(&delta_update);
        }
        REQUIRE(db.vars.size() == 1);
        REQUIRE(db.vars[0].uses.size() == 0);
    }
}
