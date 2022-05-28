#include <loguru.hpp>

#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {

MethodType k_method_type = "$cquery/callHierarchy";

enum class call_type : uint8_t {
    Direct = 0,
    Base = 1,
    Derived = 2,
    All = 1 | 2
};
MAKE_REFLECT_TYPE_PROXY(call_type);

bool operator&(call_type lhs, call_type rhs) {
    return uint8_t(lhs) & uint8_t(rhs);
}

struct InCqueryCallHierarchy : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }

    struct Params {
        // If id is specified, expand a node; otherwise textDocument+position
        // should be specified for building the root and |levels| of nodes
        // below.
        LsTextDocumentIdentifier text_document;
        LsPosition position;

        Maybe<QueryId::Func> id;

        // true: callee tree (functions called by this function); false: caller
        // tree (where this function is called)
        bool callee = false;
        // Base: include base functions; All: include both base and derived
        // functions.
        call_type call_type = call_type::All;
        bool detailed_name = false;
        int levels = 1;
    };
    Params params;
};
MAKE_REFLECT_STRUCT(InCqueryCallHierarchy::Params, text_document, position, id,
                    callee, call_type, detailed_name, levels);
MAKE_REFLECT_STRUCT(InCqueryCallHierarchy, id, params);
REGISTER_IN_MESSAGE(InCqueryCallHierarchy);

struct OutCqueryCallHierarchy : public LsOutMessage<OutCqueryCallHierarchy> {
    struct Entry {
        QueryId::Func id;
        std::string_view name;
        LsLocation location;
        call_type call_type = call_type::Direct;
        int num_children;
        // Empty if the |levels| limit is reached.
        std::vector<Entry> children;
    };

    LsRequestId id;
    optional<Entry> result;
};
MAKE_REFLECT_STRUCT(OutCqueryCallHierarchy::Entry, id, name, location,
                    call_type, num_children, children);
MAKE_REFLECT_STRUCT_OPTIONALS_MANDATORY(OutCqueryCallHierarchy, jsonrpc, id,
                                        result);

bool Expand(MessageHandler* m, OutCqueryCallHierarchy::Entry* entry,
            bool callee, call_type call_type, bool detailed_name, int levels) {
    const QueryFunc& func = m->db->GetFunc(entry->id);
    const QueryFunc::Def* def = func.AnyDef();
    entry->num_children = 0;
    if (!def) return false;
    auto handle = [&](QueryId::LexicalRef ref, enum call_type call_type) {
        entry->num_children++;
        if (levels > 0) {
            OutCqueryCallHierarchy::Entry entry1;
            entry1.id = QueryId::Func(ref.id);
            if (auto loc = GetLsLocation(m->db, m->working_files, ref))
                entry1.location = *loc;
            entry1.call_type = call_type;
            if (Expand(m, &entry1, callee, call_type, detailed_name,
                       levels - 1))
                entry->children.push_back(std::move(entry1));
        }
    };
    auto handle_uses = [&](const QueryFunc& func, enum call_type call_type) {
        if (callee) {
            if (const auto* def = func.AnyDef())
                for (const QueryId::SymbolRef& ref : def->callees) {
                    if (ref.kind == SymbolKind::Func)
                        handle(QueryId::LexicalRef(ref.range, ref.id, ref.kind,
                                                   ref.role, def->file),
                               call_type);
                }
        } else {
            for (QueryId::LexicalRef ref : func.uses)
                if (ref.kind == SymbolKind::Func) handle(ref, call_type);
        }
    };

    std::unordered_set<Usr> seen;
    seen.insert(func.usr);
    std::vector<const QueryFunc*> stack;
    if (detailed_name)
        entry->name = def->detailed_name;
    else
        entry->name = def->ShortName();
    handle_uses(func, call_type::Direct);

    // Callers/callees of base functions.
    if (call_type & call_type::Base) {
        stack.push_back(&func);
        while (stack.size()) {
            const QueryFunc& func1 = *stack.back();
            stack.pop_back();
            if (auto* def1 = func1.AnyDef()) {
                EachDefinedFunc(m->db, def1->bases, [&](QueryFunc& func2) {
                    if (!seen.count(func2.usr)) {
                        seen.insert(func2.usr);
                        stack.push_back(&func2);
                        handle_uses(func2, call_type::Base);
                    }
                });
            }
        }
    }

    // Callers/callees of derived functions.
    if (call_type & call_type::Derived) {
        stack.push_back(&func);
        while (stack.size()) {
            const QueryFunc& func1 = *stack.back();
            stack.pop_back();
            EachDefinedFunc(m->db, func1.derived, [&](QueryFunc& func2) {
                if (!seen.count(func2.usr)) {
                    seen.insert(func2.usr);
                    stack.push_back(&func2);
                    handle_uses(func2, call_type::Derived);
                }
            });
        }
    }
    return true;
}

struct HandlerCqueryCallHierarchy : BaseMessageHandler<InCqueryCallHierarchy> {
    MethodType GetMethodType() const override { return k_method_type; }

    optional<OutCqueryCallHierarchy::Entry> BuildInitial(QueryId::Func root_id,
                                                         bool callee,
                                                         call_type call_type,
                                                         bool detailed_name,
                                                         int levels) {
        const auto* def = db->GetFunc(root_id).AnyDef();
        if (!def) return {};

        OutCqueryCallHierarchy::Entry entry;
        entry.id = root_id;
        entry.call_type = call_type::Direct;
        if (def->spell) {
            if (optional<LsLocation> loc =
                    GetLsLocation(db, working_files, *def->spell))
                entry.location = *loc;
        }
        Expand(this, &entry, callee, call_type, detailed_name, levels);
        return entry;
    }

    void Run(InCqueryCallHierarchy* request) override {
        const auto& params = request->params;
        OutCqueryCallHierarchy out;
        out.id = request->id;

        if (params.id) {
            OutCqueryCallHierarchy::Entry entry;
            entry.id = *params.id;
            entry.call_type = call_type::Direct;
            if (entry.id.id < db->funcs.size())
                Expand(this, &entry, params.callee, params.call_type,
                       params.detailed_name, params.levels);
            out.result = std::move(entry);
        } else {
            QueryFile* file;
            if (!FindFileOrFail(db, project, request->id,
                                params.text_document.uri.GetAbsolutePath(),
                                &file))
                return;
            WorkingFile* working_file =
                working_files->GetFileByFilename(file->def->path);
            for (QueryId::SymbolRef sym :
                 FindSymbolsAtLocation(working_file, file, params.position)) {
                if (sym.kind == SymbolKind::Func) {
                    out.result = BuildInitial(
                        QueryId::Func(sym.id), params.callee, params.call_type,
                        params.detailed_name, params.levels);
                    break;
                }
            }
        }

        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerCqueryCallHierarchy);

}  // namespace
