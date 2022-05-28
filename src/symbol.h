#pragma once

#include "lsp.h"
#include "serializer.h"

// The order matters. In FindSymbolsAtLocation, we want Var/Func ordered in
// front of others.
enum class SymbolKind : uint8_t { Invalid, File, Type, Func, Var };
MAKE_REFLECT_TYPE_PROXY(SymbolKind);
MAKE_ENUM_HASHABLE(SymbolKind);

// clang/Basic/Specifiers.h clang::StorageClass
enum class storage_class : uint8_t {
    // In |CX_StorageClass| but not in |clang::StorageClass|
    // e.g. non-type template parameters
    Invalid,

    // These are legal on both functions and variables.
    // e.g. global functions/variables, local variables
    None,
    Extern,
    Static,
    // e.g. |__private_extern__ int a;|
    PrivateExtern,

    // These are only legal on variables.
    // e.g. explicit |auto int a;|
    Auto,
    Register
};
MAKE_REFLECT_TYPE_PROXY(storage_class);

enum class role : uint16_t {
    None = 0,
    Declaration = 1 << 0,
    Definition = 1 << 1,
    Reference = 1 << 2,
    Read = 1 << 3,
    Write = 1 << 4,
    Call = 1 << 5,
    Dynamic = 1 << 6,
    Address = 1 << 7,
    Implicit = 1 << 8,
    All = (1 << 9) - 1,
};
MAKE_REFLECT_TYPE_PROXY(role);
MAKE_ENUM_HASHABLE(role);

inline uint16_t operator&(role lhs, role rhs) {
    return uint16_t(lhs) & uint16_t(rhs);
}

inline role operator|(role lhs, role rhs) {
    return role(uint16_t(lhs) | uint16_t(rhs));
}

// A document highlight kind.
enum class ls_document_highlight_kind {
    // A textual occurrence.
    Text = 1,
    // Read-access of a symbol, like reading a variable.
    Read = 2,
    // Write-access of a symbol, like writing to a variable.
    Write = 3
};
MAKE_REFLECT_TYPE_PROXY(ls_document_highlight_kind);

// A document highlight is a range inside a text document which deserves
// special attention. Usually a document highlight is visualized by changing
// the background color of its range.
struct LsDocumentHighlight {
    // The range this highlight applies to.
    LsRange range;

    // The highlight kind, default is DocumentHighlightKind.Text.
    ls_document_highlight_kind kind = ls_document_highlight_kind::Text;
};
MAKE_REFLECT_STRUCT(LsDocumentHighlight, range, kind);

struct LsSymbolInformation {
    std::string_view name;
    ls_symbol_kind kind;
    LsLocation location;
    std::string_view container_name;
};
MAKE_REFLECT_STRUCT(LsSymbolInformation, name, kind, location, container_name);
