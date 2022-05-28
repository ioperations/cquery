#pragma once

#include "lsp.h"

// codeAction
struct CommandArgs {
    LsDocumentUri text_document_uri;
    std::vector<LsTextEdit> edits;
};
MAKE_REFLECT_STRUCT_WRITER_AS_ARRAY(CommandArgs, text_document_uri, edits);

// codeLens
struct LsCodeLensUserData {};
MAKE_REFLECT_EMPTY_STRUCT(LsCodeLensUserData);

struct LsCodeLensCommandArguments {
    LsDocumentUri uri;
    LsPosition position;
    std::vector<LsLocation> locations;
};

// FIXME Don't use array in vscode-cquery
inline void Reflect(Writer& visitor, LsCodeLensCommandArguments& value) {
    visitor.StartArray(3);
    Reflect(visitor, value.uri);
    Reflect(visitor, value.position);
    Reflect(visitor, value.locations);
    visitor.EndArray();
}

inline void Reflect(Reader& visitor, LsCodeLensCommandArguments& value) {
    int i = 0;
    visitor.IterArray([&](Reader& visitor) {
        switch (i++) {
            case 0:
                Reflect(visitor, value.uri);
                break;
            case 1:
                Reflect(visitor, value.position);
                break;
            case 2:
                Reflect(visitor, value.locations);
                break;
        }
    });
}
