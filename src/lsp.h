#pragma once

#include <iosfwd>
#include <unordered_map>

#include "config.h"
#include "method.h"
#include "serializer.h"
#include "utils.h"

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
///////////////////////////// OUTGOING MESSAGES /////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
///////////////////////////// INCOMING MESSAGES /////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

#define REGISTER_IN_MESSAGE(type) \
    static MessageRegistryRegister<type> type##message_handler_instance_;

struct MessageRegistry {
    static MessageRegistry* m_instance;
    static MessageRegistry* Instance();

    using Allocator =
        std::function<void(Reader& visitor, std::unique_ptr<InMessage>*)>;
    std::unordered_map<std::string, Allocator> allocators;

    optional<std::string> ReadMessageFromStdin(
        std::unique_ptr<InMessage>* message);
    optional<std::string> Parse(Reader& visitor,
                                std::unique_ptr<InMessage>* message);
};

template <typename T>
struct MessageRegistryRegister {
    MessageRegistryRegister() {
        T dummy;
        std::string method_name = dummy.GetMethodType();
        MessageRegistry::Instance()->allocators[method_name] =
            [](Reader& visitor, std::unique_ptr<InMessage>* message) {
                *message = std::make_unique<T>();
                // Reflect may throw and *message will be partially
                // deserialized.
                Reflect(visitor, static_cast<T&>(**message));
            };
    }
};

struct LsBaseOutMessage {
    virtual ~LsBaseOutMessage();
    virtual void ReflectWriter(Writer&) = 0;

    // Send the message to the language client by writing it to stdout.
    void Write(std::ostream& out);
};

template <typename TDerived>
struct LsOutMessage : LsBaseOutMessage {
    // All derived types need to reflect on the |jsonrpc| member.
    std::string jsonrpc = "2.0";

    void ReflectWriter(Writer& writer) override {
        Reflect(writer, static_cast<TDerived&>(*this));
    }
};

struct LsResponseError {
    enum class ls_error_codes : int {
        ParseError = -32700,
        InvalidRequest = -32600,
        MethodNotFound = -32601,
        InvalidParams = -32602,
        InternalError = -32603,
        serverErrorStart = -32099,
        serverErrorEnd = -32000,
        ServerNotInitialized = -32002,
        UnknownErrorCode = -32001,
        RequestCancelled = -32800,
    };

    ls_error_codes code;
    // Short description.
    std::string message;

    void Write(Writer& visitor);
};

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
////////////////////////////// PRIMITIVE TYPES //////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

struct LsDocumentUri {
    static LsDocumentUri FromPath(const AbsolutePath& path);

    LsDocumentUri();
    bool operator==(const LsDocumentUri& other) const;

    void SetPath(const AbsolutePath& path);
    std::string GetRawPath() const;
    AbsolutePath GetAbsolutePath() const;

    std::string raw_uri;
};
MAKE_HASHABLE(LsDocumentUri, t.raw_uri);

void Reflect(Writer& visitor, LsDocumentUri& value);
void Reflect(Reader& visitor, LsDocumentUri& value);

struct LsPosition {
    LsPosition();
    LsPosition(int line, int character);

    bool operator==(const LsPosition& other) const;
    bool operator<(const LsPosition& other) const;

    std::string ToString() const;

    // Note: these are 0-based.
    int line = 0;
    int character = 0;
    static const LsPosition k_zero_position;
};
MAKE_HASHABLE(LsPosition, t.line, t.character);
MAKE_REFLECT_STRUCT(LsPosition, line, character);

struct LsRange {
    LsRange();
    LsRange(LsPosition start, LsPosition end);

    bool operator==(const LsRange& other) const;
    bool operator<(const LsRange& other) const;

    LsPosition start;
    LsPosition end;
};
MAKE_HASHABLE(LsRange, t.start, t.end);
MAKE_REFLECT_STRUCT(LsRange, start, end);

struct LsLocation {
    LsLocation();
    LsLocation(LsDocumentUri uri, LsRange range);

    bool operator==(const LsLocation& other) const;
    bool operator<(const LsLocation& o) const;

    LsDocumentUri uri;
    LsRange range;
};
MAKE_HASHABLE(LsLocation, t.uri, t.range);
MAKE_REFLECT_STRUCT(LsLocation, uri, range);

enum class ls_symbol_kind : uint8_t {
    Unknown = 0,

    File = 1,
    Module = 2,
    Namespace = 3,
    Package = 4,
    Class = 5,
    Method = 6,
    Property = 7,
    Field = 8,
    Constructor = 9,
    Enum = 10,
    Interface = 11,
    Function = 12,
    Variable = 13,
    Constant = 14,
    String = 15,
    Number = 16,
    Boolean = 17,
    Array = 18,
    Object = 19,
    Key = 20,
    Null = 21,
    EnumMember = 22,
    Struct = 23,
    Event = 24,
    Operator = 25,

    // For C++, this is interpreted as "template parameter" (including
    // non-type template parameters).
    TypeParameter = 26,

    // cquery extensions
    // See also https://github.com/Microsoft/language-server-protocol/issues/344
    // for new SymbolKind clang/Index/IndexSymbol.h clang::index::SymbolKind
    TypeAlias = 252,
    Parameter = 253,
    StaticMethod = 254,
    Macro = 255,
};
MAKE_REFLECT_TYPE_PROXY(ls_symbol_kind);

template <typename T>
struct LsCommand {
    // Title of the command (ie, 'save')
    std::string title;
    // Actual command identifier.
    std::string command;
    // Arguments to run the command with.
    // **NOTE** This must be serialized as an array. Use
    // MAKE_REFLECT_STRUCT_WRITER_AS_ARRAY.
    T arguments;
};
template <typename TVisitor, typename T>
void Reflect(TVisitor& visitor, LsCommand<T>& value) {
    REFLECT_MEMBER_START();
    REFLECT_MEMBER(title);
    REFLECT_MEMBER(command);
    REFLECT_MEMBER(arguments);
    REFLECT_MEMBER_END();
}

template <typename TData, typename TCommandArguments>
struct LsCodeLens {
    // The range in which this code lens is valid. Should only span a single
    // line.
    LsRange range;
    // The command this code lens represents.
    optional<LsCommand<TCommandArguments>> command;
    // A data entry field that is preserved on a code lens item between
    // a code lens and a code lens resolve request.
    TData data;
};
template <typename TVisitor, typename TData, typename TCommandArguments>
void Reflect(TVisitor& visitor, LsCodeLens<TData, TCommandArguments>& value) {
    REFLECT_MEMBER_START();
    REFLECT_MEMBER(range);
    REFLECT_MEMBER(command);
    REFLECT_MEMBER(data);
    REFLECT_MEMBER_END();
}

struct LsTextDocumentIdentifier {
    LsDocumentUri uri;
};
MAKE_REFLECT_STRUCT(LsTextDocumentIdentifier, uri);

struct LsVersionedTextDocumentIdentifier {
    LsDocumentUri uri;
    // The version number of this document.  number | null
    optional<int> version;

    LsTextDocumentIdentifier AsTextDocumentIdentifier() const;
};
MAKE_REFLECT_STRUCT(LsVersionedTextDocumentIdentifier, uri, version);

struct LsTextDocumentPositionParams {
    // The text document.
    LsTextDocumentIdentifier text_document;

    // The position inside the text document.
    LsPosition position;
};
MAKE_REFLECT_STRUCT(LsTextDocumentPositionParams, text_document, position);

struct LsTextEdit {
    // The range of the text document to be manipulated. To insert
    // text into a document create a range where start === end.
    LsRange range;

    // The string to be inserted. For delete operations use an
    // empty string.
    std::string new_text;

    bool operator==(const LsTextEdit& that);
};
MAKE_REFLECT_STRUCT(LsTextEdit, range, new_text);

struct LsTextDocumentItem {
    // The text document's URI.
    LsDocumentUri uri;

    // The text document's language identifier.
    std::string language_id;

    // The version number of this document (it will strictly increase after each
    // change, including undo/redo).
    int version;

    // The content of the opened text document.
    std::string text;
};
MAKE_REFLECT_STRUCT(LsTextDocumentItem, uri, language_id, version, text);

struct LsTextDocumentEdit {
    // The text document to change.
    LsVersionedTextDocumentIdentifier text_document;

    // The edits to be applied.
    std::vector<LsTextEdit> edits;
};
MAKE_REFLECT_STRUCT(LsTextDocumentEdit, text_document, edits);

struct LsWorkspaceEdit {
    // Holds changes to existing resources.
    // changes ? : { [uri:string]: TextEdit[]; };
    // std::unordered_map<lsDocumentUri, std::vector<lsTextEdit>> changes;

    // An array of `TextDocumentEdit`s to express changes to specific a specific
    // version of a text document. Whether a client supports versioned document
    // edits is expressed via
    // `WorkspaceClientCapabilites.versionedWorkspaceEdit`.
    std::vector<LsTextDocumentEdit> document_changes;
};
MAKE_REFLECT_STRUCT(LsWorkspaceEdit, document_changes);

struct LsFormattingOptions {
    // Size of a tab in spaces.
    int tab_size;
    // Prefer spaces over tabs.
    bool insert_spaces;
};
MAKE_REFLECT_STRUCT(LsFormattingOptions, tab_size, insert_spaces);

// MarkedString can be used to render human readable text. It is either a
// markdown string or a code-block that provides a language and a code snippet.
// The language identifier is sematically equal to the optional language
// identifier in fenced code blocks in GitHub issues. See
// https://help.github.com/articles/creating-and-highlighting-code-blocks/#syntax-highlighting
//
// The pair of a language and a value is an equivalent to markdown:
// ```${language}
// ${value}
// ```
//
// Note that markdown strings will be sanitized - that means html will be
// escaped.
struct LsMarkedString {
    optional<std::string> language;
    std::string value;
};
void Reflect(Writer& visitor, LsMarkedString& value);

struct LsTextDocumentContentChangeEvent {
    // The range of the document that changed.
    optional<LsRange> range;
    // The length of the range that got replaced.
    optional<int> range_length;
    // The new text of the range/document.
    std::string text;
};
MAKE_REFLECT_STRUCT(LsTextDocumentContentChangeEvent, range, range_length,
                    text);

struct LsTextDocumentDidChangeParams {
    LsVersionedTextDocumentIdentifier text_document;
    std::vector<LsTextDocumentContentChangeEvent> content_changes;
};
MAKE_REFLECT_STRUCT(LsTextDocumentDidChangeParams, text_document,
                    content_changes);

// Show a message to the user.
enum class ls_message_type : int { Error = 1, Warning = 2, Info = 3, Log = 4 };
MAKE_REFLECT_TYPE_PROXY(ls_message_type)
struct OutShowLogMessageParams {
    ls_message_type type = ls_message_type::Error;
    std::string message;
};
MAKE_REFLECT_STRUCT(OutShowLogMessageParams, type, message);
struct OutShowLogMessage : public LsOutMessage<OutShowLogMessage> {
    enum class display_type { Show, Log };
    display_type display_type = display_type::Show;

    std::string Method();
    OutShowLogMessageParams params;
};
template <typename TVisitor>
void Reflect(TVisitor& visitor, OutShowLogMessage& value) {
    REFLECT_MEMBER_START();
    REFLECT_MEMBER(jsonrpc);
    std::string method = value.Method();
    REFLECT_MEMBER2("method", method);
    REFLECT_MEMBER(params);
    REFLECT_MEMBER_END();
}

struct OutLocationList : public LsOutMessage<OutLocationList> {
    LsRequestId id;
    std::vector<LsLocation> result;
};
MAKE_REFLECT_STRUCT(OutLocationList, jsonrpc, id, result);
