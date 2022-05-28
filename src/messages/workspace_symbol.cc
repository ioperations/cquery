#include <ctype.h>
#include <limits.h>

#include <algorithm>
#include <functional>
#include <loguru.hpp>

#include "fuzzy_match.h"
#include "lex_utils.h"
#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "workspace/symbol";

// Lookup |symbol| in |db| and insert the value into |result|.
bool InsertSymbolIntoResult(QueryDatabase* db, WorkingFiles* working_files,
                            SymbolIdx symbol,
                            std::vector<LsSymbolInformation>* result) {
    optional<LsSymbolInformation> info =
        GetSymbolInfo(db, working_files, symbol, false /*use_short_name*/);
    if (!info) return false;

    optional<QueryId::LexicalRef> location = GetDefinitionExtent(db, symbol);
    QueryId::LexicalRef loc;
    if (location)
        loc = *location;
    else {
        auto decls = GetNonDefDeclarations(db, symbol);
        if (decls.empty()) return false;
        loc = decls[0];
    }

    optional<LsLocation> ls_location = GetLsLocation(db, working_files, loc);
    if (!ls_location) return false;
    info->location = *ls_location;
    result->push_back(*info);
    return true;
}

struct InWorkspaceSymbol : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    struct Params {
        std::string query;
    };
    Params params;
};
MAKE_REFLECT_STRUCT(InWorkspaceSymbol::Params, query);
MAKE_REFLECT_STRUCT(InWorkspaceSymbol, id, params);
REGISTER_IN_MESSAGE(InWorkspaceSymbol);

struct OutWorkspaceSymbol : public LsOutMessage<OutWorkspaceSymbol> {
    LsRequestId id;
    std::vector<LsSymbolInformation> result;
};
MAKE_REFLECT_STRUCT(OutWorkspaceSymbol, jsonrpc, id, result);

///// Fuzzy matching

struct HandlerWorkspaceSymbol : BaseMessageHandler<InWorkspaceSymbol> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InWorkspaceSymbol* request) override {
        OutWorkspaceSymbol out;
        out.id = request->id;

        LOG_S(INFO) << "[querydb] Considering " << db->symbols.size()
                    << " candidates for query " << request->params.query;

        std::string query = request->params.query;

        std::unordered_set<std::string> inserted_results;
        // db->detailed_names indices of each lsSymbolInformation in out.result
        std::vector<int> result_indices;
        std::vector<LsSymbolInformation> unsorted_results;
        inserted_results.reserve(g_config->workspaceSymbol.maxNum);
        result_indices.reserve(g_config->workspaceSymbol.maxNum);

        // We use detailed_names without parameters for matching.

        // Find exact substring matches.
        for (int i = 0; i < db->symbols.size(); ++i) {
            std::string_view detailed_name = db->GetSymbolDetailedName(i);
            if (detailed_name.find(query) != std::string::npos) {
                // Do not show the same entry twice.
                if (!inserted_results.insert(std::string(detailed_name)).second)
                    continue;

                if (InsertSymbolIntoResult(db, working_files, db->symbols[i],
                                           &unsorted_results)) {
                    result_indices.push_back(i);
                    if (unsorted_results.size() >=
                        g_config->workspaceSymbol.maxNum)
                        break;
                }
            }
        }

        // Find subsequence matches.
        if (unsorted_results.size() < g_config->workspaceSymbol.maxNum) {
            std::string query_without_space;
            query_without_space.reserve(query.size());
            for (char c : query)
                if (!isspace(c)) query_without_space += c;

            for (int i = 0; i < (int)db->symbols.size(); ++i) {
                std::string_view detailed_name = db->GetSymbolDetailedName(i);
                if (CaseFoldingSubsequenceMatch(query_without_space,
                                                detailed_name)
                        .first) {
                    // Do not show the same entry twice.
                    if (!inserted_results.insert(std::string(detailed_name))
                             .second)
                        continue;

                    if (InsertSymbolIntoResult(db, working_files,
                                               db->symbols[i],
                                               &unsorted_results)) {
                        result_indices.push_back(i);
                        if (unsorted_results.size() >=
                            g_config->workspaceSymbol.maxNum)
                            break;
                    }
                }
            }
        }

        if (g_config->workspaceSymbol.sort &&
            query.size() <= FuzzyMatcher::k_max_pat) {
            // Sort results with a fuzzy matching algorithm.
            int longest = 0;
            for (int i : result_indices)
                longest =
                    std::max(longest, int(db->GetSymbolDetailedName(i).size()));
            FuzzyMatcher fuzzy(query);
            std::vector<std::pair<int, int>> permutation(result_indices.size());
            for (int i = 0; i < int(result_indices.size()); i++) {
                permutation[i] = {
                    fuzzy.Match(db->GetSymbolDetailedName(result_indices[i])),
                    i};
            }
            std::sort(permutation.begin(), permutation.end(),
                      std::greater<std::pair<int, int>>());
            out.result.reserve(result_indices.size());
            // Discard awful candidates.
            for (int i = 0; i < int(result_indices.size()) &&
                            permutation[i].first > FuzzyMatcher::k_min_score;
                 i++)
                out.result.push_back(
                    std::move(unsorted_results[permutation[i].second]));
        } else {
            out.result.reserve(unsorted_results.size());
            for (const auto& entry : unsorted_results)
                out.result.push_back(std::move(entry));
        }

        LOG_S(INFO) << "[querydb] Found " << out.result.size()
                    << " results for query " << query;
        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerWorkspaceSymbol);
}  // namespace
