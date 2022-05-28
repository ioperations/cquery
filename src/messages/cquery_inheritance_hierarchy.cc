#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "$cquery/inheritanceHierarchy";

struct InCqueryInheritanceHierarchy : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    struct Params {
        // If id+kind are specified, expand a node; otherwise
        // textDocument+position should be specified for building the root and
        // |levels| of nodes below.
        LsTextDocumentIdentifier text_document;
        LsPosition position;

        Maybe<AnyId> id;
        SymbolKind kind = SymbolKind::Invalid;

        // true: derived classes/functions; false: base classes/functions
        bool derived = false;
        bool detailed_name = false;
        int levels = 1;
    };
    Params params;
};

MAKE_REFLECT_STRUCT(InCqueryInheritanceHierarchy::Params, text_document,
                    position, id, kind, derived, detailed_name, levels);
MAKE_REFLECT_STRUCT(InCqueryInheritanceHierarchy, id, params);
REGISTER_IN_MESSAGE(InCqueryInheritanceHierarchy);

struct OutCqueryInheritanceHierarchy
    : public LsOutMessage<OutCqueryInheritanceHierarchy> {
    struct Entry {
        AnyId id;
        SymbolKind kind;
        std::string_view name;
        LsLocation location;
        // For unexpanded nodes, this is an upper bound because some entities
        // may be undefined. If it is 0, there are no members.
        int num_children;
        // Empty if the |levels| limit is reached.
        std::vector<Entry> children;
    };
    LsRequestId id;
    optional<Entry> result;
};
MAKE_REFLECT_STRUCT(OutCqueryInheritanceHierarchy::Entry, id, kind, name,
                    location, num_children, children);
MAKE_REFLECT_STRUCT_OPTIONALS_MANDATORY(OutCqueryInheritanceHierarchy, jsonrpc,
                                        id, result);

bool Expand(MessageHandler* m, OutCqueryInheritanceHierarchy::Entry* entry,
            bool derived, bool detailed_name, int levels);

template <typename Q>
bool ExpandHelper(MessageHandler* m,
                  OutCqueryInheritanceHierarchy::Entry* entry, bool derived,
                  bool detailed_name, int levels, Q& entity) {
    const auto* def = entity.AnyDef();
    if (!def) {
        entry->num_children = 0;
        return false;
    }
    if (detailed_name)
        entry->name = def->DetailedName(false);
    else
        entry->name = def->ShortName();
    if (def->spell) {
        if (optional<LsLocation> loc =
                GetLsLocation(m->db, m->working_files, *def->spell))
            entry->location = *loc;
    }
    if (derived) {
        if (levels > 0) {
            for (auto id : entity.derived) {
                OutCqueryInheritanceHierarchy::Entry entry1;
                entry1.id = id;
                entry1.kind = entry->kind;
                if (Expand(m, &entry1, derived, detailed_name, levels - 1))
                    entry->children.push_back(std::move(entry1));
            }
            entry->num_children = int(entry->children.size());
        } else
            entry->num_children = int(entity.derived.size());
    } else {
        if (levels > 0) {
            for (auto id : def->bases) {
                OutCqueryInheritanceHierarchy::Entry entry1;
                entry1.id = id;
                entry1.kind = entry->kind;
                if (Expand(m, &entry1, derived, detailed_name, levels - 1))
                    entry->children.push_back(std::move(entry1));
            }
            entry->num_children = int(entry->children.size());
        } else
            entry->num_children = int(def->bases.size());
    }
    return true;
}

bool Expand(MessageHandler* m, OutCqueryInheritanceHierarchy::Entry* entry,
            bool derived, bool detailed_name, int levels) {
    if (entry->kind == SymbolKind::Func)
        return ExpandHelper(m, entry, derived, detailed_name, levels,
                            m->db->GetFunc({entry->id, SymbolKind::Func}));
    return ExpandHelper(m, entry, derived, detailed_name, levels,
                        m->db->GetType({entry->id, SymbolKind::Type}));
}

struct HandlerCqueryInheritanceHierarchy
    : BaseMessageHandler<InCqueryInheritanceHierarchy> {
    MethodType GetMethodType() const override { return k_method_type; }

    optional<OutCqueryInheritanceHierarchy::Entry> BuildInitial(
        QueryId::SymbolRef sym, bool derived, bool detailed_name, int levels) {
        OutCqueryInheritanceHierarchy::Entry entry;
        entry.id = sym.id;
        entry.kind = sym.kind;
        Expand(this, &entry, derived, detailed_name, levels);
        return entry;
    }

    void Run(InCqueryInheritanceHierarchy* request) override {
        const auto& params = request->params;
        OutCqueryInheritanceHierarchy out;
        out.id = request->id;

        if (params.id) {
            OutCqueryInheritanceHierarchy::Entry entry;
            entry.id = *params.id;
            entry.kind = params.kind;
            if (((entry.kind == SymbolKind::Func &&
                  entry.id.id < db->funcs.size()) ||
                 (entry.kind == SymbolKind::Type &&
                  entry.id.id < db->types.size())) &&
                Expand(this, &entry, params.derived, params.detailed_name,
                       params.levels))
                out.result = std::move(entry);
        } else {
            QueryFile* file;
            if (!FindFileOrFail(db, project, request->id,
                                params.text_document.uri.GetAbsolutePath(),
                                &file))
                return;
            WorkingFile* working_file =
                working_files->GetFileByFilename(file->def->path);

            for (QueryId::SymbolRef sym : FindSymbolsAtLocation(
                     working_file, file, request->params.position)) {
                if (sym.kind == SymbolKind::Func ||
                    sym.kind == SymbolKind::Type) {
                    out.result =
                        BuildInitial(sym, params.derived, params.detailed_name,
                                     params.levels);
                    break;
                }
            }
        }

        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerCqueryInheritanceHierarchy);

}  // namespace
