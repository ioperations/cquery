#include <doctest/doctest.h>

#include <loguru.hpp>
#include <regex>

#include "clang_complete.h"
#include "code_complete_cache.h"
#include "fuzzy_match.h"
#include "include_complete.h"
#include "lex_utils.h"
#include "message_handler.h"
#include "queue_manager.h"
#include "timer.h"
#include "working_files.h"

namespace {
MethodType k_method_type = "textDocument/completion";

// How a completion was triggered
enum class ls_completion_trigger_kind {
    // Completion was triggered by typing an identifier (24x7 code
    // complete), manual invocation (e.g Ctrl+Space) or via API.
    Invoked = 1,

    // Completion was triggered by a trigger character specified by
    // the `triggerCharacters` properties of the
    // `CompletionRegistrationOptions`.
    TriggerCharacter = 2
};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
MAKE_REFLECT_TYPE_PROXY(ls_completion_trigger_kind);
#pragma clang diagnostic pop

// Contains additional information about the context in which a completion
// request is triggered.
struct LsCompletionContext {
    // How the completion was triggered.
    ls_completion_trigger_kind trigger_kind;

    // The trigger character (a single character) that has trigger code
    // complete. Is undefined if `triggerKind !==
    // CompletionTriggerKind.TriggerCharacter`
    optional<std::string> trigger_character;
};
MAKE_REFLECT_STRUCT(LsCompletionContext, trigger_kind, trigger_character);

struct LsCompletionParams : LsTextDocumentPositionParams {
    // The completion context. This is only available it the client specifies to
    // send this using
    // `ClientCapabilities.textDocument.completion.contextSupport === true`
    optional<LsCompletionContext> context;
};
MAKE_REFLECT_STRUCT(LsCompletionParams, text_document, position, context);

struct InTextDocumentComplete : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsCompletionParams params;
};
MAKE_REFLECT_STRUCT(InTextDocumentComplete, id, params);
REGISTER_IN_MESSAGE(InTextDocumentComplete);

struct LsTextDocumentCompleteResult {
    // This list it not complete. Further typing should result in recomputing
    // this list.
    bool is_incomplete = false;
    // The completion items.
    std::vector<lsCompletionItem> items;
};
MAKE_REFLECT_STRUCT(LsTextDocumentCompleteResult, is_incomplete, items);

struct OutTextDocumentComplete : public LsOutMessage<OutTextDocumentComplete> {
    LsRequestId id;
    LsTextDocumentCompleteResult result;
};
MAKE_REFLECT_STRUCT(OutTextDocumentComplete, jsonrpc, id, result);

void DecorateIncludePaths(const std::smatch& match,
                          std::vector<lsCompletionItem>* items) {
    std::string spaces_after_include = " ";
    if (match[3].compare("include") == 0 && match[5].length())
        spaces_after_include = match[4].str();

    std::string prefix = match[1].str() + '#' + match[2].str() + "include" +
                         spaces_after_include;
    std::string suffix = match[7].str();

    for (lsCompletionItem& item : *items) {
        char quote0, quote1;
        if (match[5].compare("<") == 0 ||
            (match[5].length() == 0 && item.use_angle_brackets_))
            quote0 = '<', quote1 = '>';
        else
            quote0 = quote1 = '"';

        item.textEdit->new_text =
            prefix + quote0 + item.textEdit->new_text + quote1 + suffix;
        item.label = prefix + quote0 + item.label + quote1 + suffix;
        item.filterText = nullopt;
    }
}

struct ParseIncludeLineResult {
    bool ok;
    std::string keyword;
    std::string quote;
    std::string pattern;
    std::smatch match;
};

ParseIncludeLineResult ParseIncludeLine(const std::string& line) {
    static const std::regex pattern(
        "(\\s*)"        // [1]: spaces before '#'
        "#"             //
        "(\\s*)"        // [2]: spaces after '#'
        "([^\\s\"<]*)"  // [3]: "include"
        "(\\s*)"        // [4]: spaces before quote
        "([\"<])?"      // [5]: the first quote char
        "([^\\s\">]*)"  // [6]: path of file
        "[\">]?"        //
        "(.*)");        // [7]: suffix after quote char
    std::smatch match;
    bool ok = std::regex_match(line, match, pattern);
    return {ok, match[3], match[5], match[6], match};
}

static const std::vector<std::string> preprocessor_keywords = {
    "define", "undef", "include", "if",   "ifdef", "ifndef",
    "else",   "elif",  "endif",   "line", "error", "pragma"};

std::vector<lsCompletionItem> PreprocessorKeywordCompletionItems(
    const std::smatch& match) {
    std::vector<lsCompletionItem> items;
    for (auto& keyword : preprocessor_keywords) {
        lsCompletionItem item;
        item.label = keyword;
        item.priority_ = (keyword == "include" ? 2 : 1);
        item.textEdit = LsTextEdit();
        std::string space =
            (keyword == "else" || keyword == "endif") ? "" : " ";
        item.textEdit->new_text = match[1].str() + "#" + match[2].str() +
                                  keyword + space + match[6].str();
        item.insertTextFormat = lsInsertTextFormat::PlainText;
        items.push_back(item);
    }
    return items;
}

// Returns a string that sorts in the same order as rank.
std::string ToSortText(size_t rank) {
    // 32 digits, could be more though. Lowercase should be excluded so that
    // case insensitive comparisons do not reorder our results.
    static constexpr char digits[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUV";
    constexpr int n = sizeof(digits) - 1;
    // Four digits is plenty, it can support 32^4 = 1048576 ranks.
    return {digits[rank / (n * n * n) % n], digits[rank / (n * n) % n],
            digits[rank / n % n], digits[rank % n]};
}

// Pre-filters completion responses before sending to vscode. This results in a
// significantly snappier completion experience as vscode is easily overloaded
// when given 1000+ completion items.
void FilterAndSortCompletionResponse(OutTextDocumentComplete* complete_response,
                                     const std::string& complete_text,
                                     bool has_open_paren, bool enable) {
    if (!enable) return;

    ScopedPerfTimer timer("FilterAndSortCompletionResponse");

// Used to inject more completions.
#if false
  const size_t kNumIterations = 250;
  size_t size = complete_response->result.items.size();
  complete_response->result.items.reserve(size * (kNumIterations + 1));
  for (size_t iteration = 0; iteration < kNumIterations; ++iteration) {
    for (size_t i = 0; i < size; ++i) {
      auto item = complete_response->result.items[i];
      item.label += "#" + std::to_string(iteration);
      complete_response->result.items.push_back(item);
    }
  }
#endif

    auto& items = complete_response->result.items;

    auto finalize = [&]() {
        const size_t k_max_result_size = 100u;
        if (items.size() > k_max_result_size) {
            items.resize(k_max_result_size);
            complete_response->result.is_incomplete = true;
        }

        if (has_open_paren) {
            for (auto& item : items) {
                item.insertText = item.label;
            }
        }

        // Set sortText. Note that this happens after resizing - we could do it
        // before, but then we should also sort by priority.
        for (size_t i = 0; i < items.size(); ++i)
            items[i].sortText = ToSortText(i);
    };

    // No complete text; don't run any filtering logic except to trim the items.
    if (complete_text.empty()) {
        finalize();
        return;
    }

    // Make sure all items have |filterText| set, code that follow needs it.
    for (auto& item : items) {
        if (!item.filterText) item.filterText = item.label;
    }

    // Fuzzy match and remove awful candidates.
    FuzzyMatcher fuzzy(complete_text);
    for (auto& item : items) {
        item.score_ =
            CaseFoldingSubsequenceMatch(complete_text, *item.filterText).first
                ? fuzzy.Match(*item.filterText)
                : FuzzyMatcher::k_min_score;
    }
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const lsCompletionItem& item) {
                                   return item.score_ <=
                                          FuzzyMatcher::k_min_score;
                               }),
                items.end());
    std::sort(items.begin(), items.end(),
              [](const lsCompletionItem& lhs, const lsCompletionItem& rhs) {
                  if (lhs.score_ != rhs.score_) return lhs.score_ > rhs.score_;
                  if (lhs.priority_ != rhs.priority_)
                      return lhs.priority_ < rhs.priority_;
                  if (lhs.filterText->size() != rhs.filterText->size())
                      return lhs.filterText->size() < rhs.filterText->size();
                  return *lhs.filterText < *rhs.filterText;
              });

    // Trim result.
    finalize();
}

// Returns true if position is an points to a '(' character in |lines|. Skips
// whitespace.
bool IsOpenParenOrBracket(const std::vector<std::string>& lines,
                          const LsPosition& position) {
    // TODO: refactor this logic to be in the style of `optional<char>
    // GetNextNonWhitespaceToken(lines, position)`
    int c = position.character;
    int l = position.line;
    while (l < lines.size()) {
        std::string_view line = lines[l];
        if (line[c] == '(' || line[c] == '<') return true;
        if (isspace(line[c])) {
            c++;
            if (c >= line.size()) {
                c = 0;
                l += 1;
            }
            continue;
        }
        break;
    }
    return false;
}

struct HandlerTextDocumentCompletion : MessageHandler {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(std::unique_ptr<InMessage> message) override {
        auto request = std::shared_ptr<InTextDocumentComplete>(
            static_cast<InTextDocumentComplete*>(message.release()));

        auto write_empty_result = [request]() {
            OutTextDocumentComplete out;
            out.id = request->id;
            QueueManager::WriteStdout(k_method_type, out);
        };

        AbsolutePath path = request->params.text_document.uri.GetAbsolutePath();
        WorkingFile* file = working_files->GetFileByFilename(path);
        if (!file) {
            write_empty_result();
            return;
        }

        // It shouldn't be possible, but sometimes vscode will send queries out
        // of order, ie, we get completion request before buffer content update.
        std::string buffer_line;
        if (request->params.position.line >= 0 &&
            request->params.position.line < file->buffer_lines.size()) {
            buffer_line = file->buffer_lines[request->params.position.line];
        }

        // Check for - and : before completing -> or ::, since vscode does not
        // support multi-character trigger characters.
        if (request->params.context &&
            request->params.context->trigger_kind ==
                ls_completion_trigger_kind::TriggerCharacter &&
            request->params.context->trigger_character) {
            bool did_fail_check = false;

            std::string character = *request->params.context->trigger_character;
            int preceding_index = request->params.position.character - 2;

            // If the character is '"', '<' or '/', make sure that the line
            // starts with '#'.
            if (character == "\"" || character == "<" || character == "/") {
                size_t i = 0;
                while (i < buffer_line.size() && isspace(buffer_line[i])) ++i;
                if (i >= buffer_line.size() || buffer_line[i] != '#')
                    did_fail_check = true;
            }
            // If the character is > or : and we are at the start of the line,
            // do not show completion results.
            else if ((character == ">" || character == ":") &&
                     preceding_index < 0) {
                did_fail_check = true;
            }
            // If the character is > but - does not preced it, or if it is : and
            // : does not preced it, do not show completion results.
            else if (preceding_index >= 0 &&
                     preceding_index < (int)buffer_line.size()) {
                char preceding = buffer_line[preceding_index];
                did_fail_check = (preceding != '-' && character == ">") ||
                                 (preceding != ':' && character == ":");
            }

            if (did_fail_check) {
                write_empty_result();
                return;
            }
        }

        bool is_global_completion = false;
        std::string existing_completion;
        LsPosition end_pos = request->params.position;
        if (file) {
            request->params.position = file->FindStableCompletionSource(
                request->params.position, &is_global_completion,
                &existing_completion, &end_pos);
        }

        ParseIncludeLineResult result = ParseIncludeLine(buffer_line);
        bool has_open_paren = IsOpenParenOrBracket(file->buffer_lines, end_pos);

        if (result.ok) {
            OutTextDocumentComplete out;
            out.id = request->id;

            if (result.quote.empty() && result.pattern.empty()) {
                // no quote or path of file, do preprocessor keyword completion
                if (!std::any_of(preprocessor_keywords.begin(),
                                 preprocessor_keywords.end(),
                                 [&result](std::string_view k) {
                                     return k == result.keyword;
                                 })) {
                    out.result.items =
                        PreprocessorKeywordCompletionItems(result.match);
                    FilterAndSortCompletionResponse(
                        &out, result.keyword, has_open_paren,
                        g_config->completion.filterAndSort);
                }
            } else if (result.keyword.compare("include") == 0) {
                {
                    // do include completion
                    std::unique_lock<std::mutex> lock(
                        include_complete->completion_items_mutex,
                        std::defer_lock);
                    if (include_complete->is_scanning) lock.lock();
                    out.result.items = include_complete->completion_items;
                }
                FilterAndSortCompletionResponse(
                    &out, result.pattern, has_open_paren,
                    g_config->completion.filterAndSort);
                DecorateIncludePaths(result.match, &out.result.items);
            }

            for (lsCompletionItem& item : out.result.items) {
                item.textEdit->range.start.line = request->params.position.line;
                item.textEdit->range.start.character = 0;
                item.textEdit->range.end.line = request->params.position.line;
                item.textEdit->range.end.character = (int)buffer_line.size();
            }

            QueueManager::WriteStdout(k_method_type, out);
        } else {
            ClangCompleteManager::OnComplete callback =
                [this, request, existing_completion, end_pos,
                 is_global_completion,
                 has_open_paren](const LsRequestId& id,
                                 std::vector<lsCompletionItem> results,
                                 bool is_cached_result) {
                    OutTextDocumentComplete out;
                    out.id = request->id;
                    out.result.items = results;

                    // Emit completion results.
                    FilterAndSortCompletionResponse(
                        &out, existing_completion, has_open_paren,
                        g_config->completion.filterAndSort);
                    // Add text edits with the same text, but whose ranges
                    // include the whole token from start to end.
                    for (auto& item : out.result.items) {
                        item.textEdit = LsTextEdit{
                            LsRange(request->params.position, end_pos),
                            item.insertText};
                    }

                    QueueManager::WriteStdout(k_method_type, out);

                    // Cache completion results.
                    if (!is_cached_result) {
                        AbsolutePath path =
                            request->params.text_document.uri.GetAbsolutePath();
                        if (is_global_completion) {
                            global_code_complete_cache->WithLock([&]() {
                                global_code_complete_cache->m_cached_path =
                                    path;
                                global_code_complete_cache->m_cached_results =
                                    results;
                            });
                        } else {
                            non_global_code_complete_cache->WithLock([&]() {
                                non_global_code_complete_cache->m_cached_path =
                                    path;
                                non_global_code_complete_cache
                                    ->m_cached_completion_position =
                                    request->params.position;
                                non_global_code_complete_cache
                                    ->m_cached_results = results;
                            });
                        }
                    }
                };

            bool is_cache_match = false;
            global_code_complete_cache->WithLock([&]() {
                is_cache_match =
                    is_global_completion &&
                    global_code_complete_cache->m_cached_path == path &&
                    !global_code_complete_cache->m_cached_results.empty();
            });
            if (is_cache_match) {
                ClangCompleteManager::OnComplete freshen_global =
                    [this](const LsRequestId& id,
                           std::vector<lsCompletionItem> results,
                           bool is_cached_result) {
                        assert(!is_cached_result);

                        // note: path is updated in the normal completion
                        // handler.
                        global_code_complete_cache->WithLock([&]() {
                            global_code_complete_cache->m_cached_results =
                                results;
                        });
                    };

                // Reply immediately with the cache, and then send a new
                // completion request in the background that will be freshen the
                // global index.
                global_code_complete_cache->WithLock([&]() {
                    callback(request->id,
                             global_code_complete_cache->m_cached_results,
                             true /*is_cached_result*/);
                });
                // Do not pass the request id, since we've already sent a
                // response for the id.
                clang_complete->CodeComplete(LsRequestId(), request->params,
                                             freshen_global);
            } else if (non_global_code_complete_cache->IsCacheValid(
                           request->params)) {
                // Don't bother updating a non-global completion request, since
                // cache hits are much less likely and the cache is much more
                // likely to be up to date.
                non_global_code_complete_cache->WithLock([&]() {
                    callback(request->id,
                             non_global_code_complete_cache->m_cached_results,
                             true /*is_cached_result*/);
                });
            } else {
                // No cache hit.
                clang_complete->CodeComplete(request->id, request->params,
                                             callback);
            }
        }
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentCompletion);

TEST_SUITE("Completion lexing") {
    TEST_CASE("NextCharIsOpenParen") {
        auto check = [](std::vector<std::string> lines, int line,
                        int character) {
            return IsOpenParenOrBracket(lines, LsPosition(line, character));
        };
        REQUIRE(!check(std::vector<std::string>{"abc"}, 0, 0));
        REQUIRE(!check(std::vector<std::string>{"abc"}, 0, 0));
        REQUIRE(!check(std::vector<std::string>{"    "}, 0, 0));
        REQUIRE(!check(std::vector<std::string>{"    ", "   "}, 0, 0));
        REQUIRE(!check(std::vector<std::string>{}, 0, 0));
        REQUIRE(!check(std::vector<std::string>{"abc"}, 1, 0));
        REQUIRE(!check(std::vector<std::string>{"a("}, 1, 1));
        REQUIRE(!check(std::vector<std::string>{"a("}, 0, 0));
        REQUIRE(check(std::vector<std::string>{"a("}, 0, 1));
        REQUIRE(check(std::vector<std::string>{"a    ("}, 0, 1));
        REQUIRE(check(std::vector<std::string>{"    ("}, 0, 0));
        REQUIRE(check(std::vector<std::string>{"    ", "   ("}, 0, 0));
        REQUIRE(!check(std::vector<std::string>{"    ", " a  ("}, 0, 0));
        REQUIRE(check(std::vector<std::string>{"    ", "   <  "}, 0, 0));
    }
}
}  // namespace
