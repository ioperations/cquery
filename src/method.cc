#include "method.h"

#include <doctest/doctest.h>

#include <iostream>

#include "serializers/json.h"

MethodType kMethodType_Unknown = "$unknown";
MethodType kMethodType_Exit = "exit";
MethodType kMethodType_TextDocumentPublishDiagnostics =
    "textDocument/publishDiagnostics";
MethodType kMethodType_CqueryPublishInactiveRegions =
    "$cquery/publishInactiveRegions";
MethodType kMethodType_CqueryQueryDbStatus = "$cquery/queryDbStatus";
MethodType kMethodType_CqueryPublishSemanticHighlighting =
    "$cquery/publishSemanticHighlighting";

void Reflect(Reader& visitor, LsRequestId& value) {
    if (visitor.IsInt()) {
        value.type = LsRequestId::kInt;
        value.value = visitor.GetInt();
    } else if (visitor.IsInt64()) {
        value.type = LsRequestId::kInt;
        // `lsRequestId.value` is an `int`, so we're forced to truncate.
        value.value = static_cast<int>(visitor.GetInt64());
    } else if (visitor.IsString()) {
        value.type = LsRequestId::kString;
        std::string s = visitor.GetString();
        value.value = atoi(s.c_str());
    } else {
        value.type = LsRequestId::kNone;
        value.value = -1;
    }
}

void Reflect(Writer& visitor, LsRequestId& value) {
    switch (value.type) {
        case LsRequestId::kNone:
            visitor.Null();
            break;
        case LsRequestId::kInt:
            visitor.Int(value.value);
            break;
        case LsRequestId::kString:
            std::string str = std::to_string(value.value);
            visitor.String(str.c_str(), str.length());
            break;
    }
}

std::string ToString(const LsRequestId& id) {
    if (id.type != LsRequestId::kNone) return std::to_string(id.value);
    return "";
}

InMessage::~InMessage() = default;

LsRequestId RequestInMessage::GetRequestId() const { return id; }

LsRequestId NotificationInMessage::GetRequestId() const {
    return LsRequestId();
}

TEST_SUITE("lsRequestId") {
    TEST_CASE("To string") {
        // FIXME: Add test reflection helpers; make it easier to use.
        rapidjson::StringBuffer output;
        rapidjson::Writer<rapidjson::StringBuffer> writer(output);
        JsonWriter json_writer(&writer);

        json_writer.StartArray(0);

        LsRequestId id;
        id.value = 3;

        id.type = LsRequestId::kNone;
        Reflect(json_writer, id);

        id.type = LsRequestId::kInt;
        Reflect(json_writer, id);

        id.type = LsRequestId::kString;
        Reflect(json_writer, id);

        json_writer.EndArray();

        REQUIRE(std::string(output.GetString()) == "[null,3,\"3\"]");
    }
}
