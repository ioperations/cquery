#include "message_handler.h"

#include <algorithm>
#include <loguru.hpp>

#include "lex_utils.h"
#include "project.h"
#include "query_utils.h"
#include "queue_manager.h"
#include "semantic_highlight_symbol_cache.h"

namespace {

struct Out_CquerySetInactiveRegion
    : public LsOutMessage<Out_CquerySetInactiveRegion> {
    struct Params {
        LsDocumentUri uri;
        std::vector<LsRange> inactiveRegions;
    };
    std::string method = "$cquery/setInactiveRegions";
    Params params;
};
MAKE_REFLECT_STRUCT(Out_CquerySetInactiveRegion::Params, uri, inactiveRegions);
MAKE_REFLECT_STRUCT(Out_CquerySetInactiveRegion, jsonrpc, method, params);

struct ScanLineEvent {
    LsPosition pos;
    LsPosition end_pos;  // Second key when there is a tie for insertion events.
    int id;
    Out_CqueryPublishSemanticHighlighting::Symbol* symbol;
    bool operator<(const ScanLineEvent& other) const {
        // See the comments below when insertion/deletion events are inserted.
        if (!(pos == other.pos)) return pos < other.pos;
        if (!(other.end_pos == end_pos)) return other.end_pos < end_pos;
        // This comparison essentially order Macro after non-Macro,
        // So that macros will not be rendered as Var/Type/...
        return symbol->kind < other.symbol->kind;
    }
};
}  // namespace

MessageHandler::MessageHandler() {
    // Dynamically allocate |message_handlers|, otherwise there will be static
    // initialization order races.
    if (!message_handlers)
        message_handlers = new std::vector<MessageHandler*>();
    message_handlers->push_back(this);
}

// static
std::vector<MessageHandler*>* MessageHandler::message_handlers = nullptr;

bool FindFileOrFail(QueryDatabase* db, const Project* project,
                    optional<LsRequestId> id, const AbsolutePath& absolute_path,
                    QueryFile** out_query_file, QueryId::File* out_file_id) {
    *out_query_file = nullptr;

    auto it = db->usr_to_file.find(absolute_path);
    if (it != db->usr_to_file.end()) {
        QueryFile& file = db->files[it->second.id];
        if (file.def) {
            *out_query_file = &file;
            if (out_file_id) *out_file_id = QueryId::File(it->second.id);
            return true;
        }
    }

    if (out_file_id) *out_file_id = QueryId::File();

    bool indexing =
        project->absolute_path_to_entry_index_.find(absolute_path) !=
        project->absolute_path_to_entry_index_.end();
    if (indexing)
        LOG_S(INFO) << "\"" << absolute_path << "\" is being indexed.";
    else
        LOG_S(INFO) << "Unable to find file \"" << absolute_path << "\"";

    if (id) {
        OutError out;
        out.id = *id;
        if (indexing) {
            out.error.code = lsErrorCodes::ServerNotInitialized;
            out.error.message = absolute_path.path + " is being indexed.";
        } else {
            out.error.code = lsErrorCodes::InternalError;
            out.error.message = "Unable to find file " + absolute_path.path;
        }
        QueueManager::WriteStdout(kMethodType_Unknown, out);
    }

    return false;
}

void EmitInactiveLines(WorkingFile* working_file,
                       const std::vector<Range>& inactive_regions) {
    if (!g_config->emitInactiveRegions) return;

    Out_CquerySetInactiveRegion out;
    out.params.uri = LsDocumentUri::FromPath(working_file->filename);
    for (Range skipped : inactive_regions) {
        optional<LsRange> ls_skipped = GetLsRange(working_file, skipped);
        if (ls_skipped) out.params.inactiveRegions.push_back(*ls_skipped);
    }
    QueueManager::WriteStdout(kMethodType_CqueryPublishInactiveRegions, out);
}

struct SymbolAndRole {
    SymbolIdx symbol;
    role role;

    bool operator==(const SymbolAndRole& o) const {
        return symbol == o.symbol && role == o.role;
    }
};
MAKE_HASHABLE(SymbolAndRole, t.symbol, t.role);

void EmitSemanticHighlighting(QueryDatabase* db,
                              SemanticHighlightSymbolCache* semantic_cache,
                              WorkingFile* working_file, QueryFile* file) {
    if (!g_config->highlight.enabled) return;

    assert(file->def);
    if (!semantic_cache->m_match->IsMatch(file->def->path)) return;
    auto semantic_cache_for_file =
        semantic_cache->GetCacheForFile(file->def->path);

    // Group symbols together.
    std::unordered_map<SymbolAndRole,
                       Out_CqueryPublishSemanticHighlighting::Symbol>
        grouped_symbols;
    for (QueryId::SymbolRef sym : file->def->all_symbols) {
        std::string_view detailed_name;
        ls_symbol_kind parent_kind = ls_symbol_kind::Unknown;
        ls_symbol_kind kind = ls_symbol_kind::Unknown;
        storage_class storage = storage_class::Invalid;
        role role = sym.role;
        // This switch statement also filters out symbols that are not
        // highlighted.
        switch (sym.kind) {
            case SymbolKind::Func: {
                const QueryFunc& func = db->GetFunc(sym);
                const QueryFunc::Def* def = func.AnyDef();
                if (!def) continue;  // applies to for loop
                if (def->spell) parent_kind = GetSymbolKind(db, *def->spell);
                if (parent_kind == ls_symbol_kind::Unknown) {
                    for (QueryId::LexicalRef ref : func.declarations) {
                        parent_kind = GetSymbolKind(db, ref);
                        break;
                    }
                }
                // Don't highlight overloadable operators or implicit lambda ->
                // std::function constructor.
                std::string_view short_name = def->ShortName();
                if (short_name.compare(0, 8, "operator") == 0 ||
                    short_name.compare(0, 27, "function<type-parameter-0-0") ==
                        0)
                    continue;  // applies to for loop
                if (def->spell) parent_kind = GetSymbolKind(db, *def->spell);
                kind = def->kind;
                storage = def->storage;
                detailed_name = short_name;

                // Check whether the function name is actually there.
                // If not, do not publish the semantic highlight.
                // E.g. copy-initialization of constructors should not be
                // highlighted but we still want to keep the range for jumping
                // to definition.
                std::string_view concise_name =
                    detailed_name.substr(0, detailed_name.find('<'));
                int16_t start_line = sym.range.start.line;
                int16_t start_col = sym.range.start.column;
                // The function is not there if this isn't at least zero.
                if (start_line < 0) continue;
                if (start_line >= 0 &&
                    start_line < working_file->index_lines.size()) {
                    std::string_view line =
                        working_file->index_lines[start_line];
                    sym.range.end.line = start_line;
                    if (start_col + concise_name.size() <= line.size() &&
                        line.compare(start_col, concise_name.size(),
                                     concise_name) == 0)
                        sym.range.end.column = start_col + concise_name.size();
                    else
                        continue;  // applies to for loop
                }
                break;
            }
            case SymbolKind::Type:
                for (auto& def : db->GetType(sym).def) {
                    kind = def.kind;
                    detailed_name = def.detailed_name;
                    if (def.spell) {
                        parent_kind = GetSymbolKind(db, *def.spell);
                        break;
                    }
                }
                break;
            case SymbolKind::Var: {
                const QueryVar& var = db->GetVar(sym);
                for (auto& def : var.def) {
                    kind = def.kind;
                    storage = def.storage;
                    detailed_name = def.detailed_name;
                    if (def.spell) {
                        parent_kind = GetSymbolKind(db, *def.spell);
                        break;
                    }
                }
                if (parent_kind == ls_symbol_kind::Unknown) {
                    for (QueryId::LexicalRef ref : var.declarations) {
                        parent_kind = GetSymbolKind(db, ref);
                        break;
                    }
                }
                break;
            }
            default:
                continue;  // applies to for loop
        }

        optional<LsRange> loc = GetLsRange(working_file, sym.range);
        SymbolAndRole key{sym, role};
        if (loc) {
            auto it = grouped_symbols.find(key);
            if (it != grouped_symbols.end()) {
                it->second.ranges.push_back(*loc);
            } else {
                Out_CqueryPublishSemanticHighlighting::Symbol symbol;
                symbol.stableId = semantic_cache_for_file->GetStableId(
                    sym.kind, std::string(detailed_name));
                symbol.parentKind = parent_kind;
                symbol.kind = kind;
                symbol.storage = storage;
                symbol.role = role;
                symbol.ranges.push_back(*loc);
                grouped_symbols[key] = symbol;
            }
        }
    }

    // Make ranges non-overlapping using a scan line algorithm.
    std::vector<ScanLineEvent> events;
    int id = 0;
    for (auto& entry : grouped_symbols) {
        Out_CqueryPublishSemanticHighlighting::Symbol& symbol = entry.second;
        for (auto& loc : symbol.ranges) {
            // For ranges sharing the same start point, the one with leftmost
            // end point comes first.
            events.push_back({loc.start, loc.end, id, &symbol});
            // For ranges sharing the same end point, their relative order does
            // not matter, therefore we arbitrarily assign loc.end to them. We
            // use negative id to indicate a deletion event.
            events.push_back({loc.end, loc.end, ~id, &symbol});
            id++;
        }
        symbol.ranges.clear();
    }
    std::sort(events.begin(), events.end());

    std::vector<uint8_t> deleted(id, 0);
    int top = 0;
    for (size_t i = 0; i < events.size(); i++) {
        while (top && deleted[events[top - 1].id]) top--;
        // Order [a, b0) after [a, b1) if b0 < b1. The range comes later
        // overrides the ealier. The order of [a0, b) [a1, b) does not matter.
        // The order of [a, b) [b, c) does not as long as we do not emit empty
        // ranges.
        // Attribute range [events[i-1].pos, events[i].pos) to
        // events[top-1].symbol
        // .
        if (top && !(events[i - 1].pos == events[i].pos))
            events[top - 1].symbol->ranges.emplace_back(events[i - 1].pos,
                                                        events[i].pos);
        if (events[i].id >= 0)
            events[top++] = events[i];
        else
            deleted[~events[i].id] = 1;
    }

    // Publish.
    Out_CqueryPublishSemanticHighlighting out;
    out.params.uri = LsDocumentUri::FromPath(working_file->filename);
    for (auto& entry : grouped_symbols)
        if (entry.second.ranges.size())
            out.params.symbols.push_back(entry.second);
    QueueManager::WriteStdout(kMethodType_CqueryPublishSemanticHighlighting,
                              out);
}

bool ShouldIgnoreFileForIndexing(const std::string& path) {
    return StartsWith(path, "git:");
}
