#pragma once

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>

#include "serializer.h"

class JsonReader : public Reader {
    rapidjson::GenericValue<rapidjson::UTF8<>>* m;
    std::vector<const char*> path;

   public:
    JsonReader(rapidjson::GenericValue<rapidjson::UTF8<>>* m) : m(m) {}
    serialize_format Format() const override { return serialize_format::Json; }

    bool IsBool() override { return m->IsBool(); }
    bool IsNull() override { return m->IsNull(); }
    bool IsArray() override { return m->IsArray(); }
    bool IsInt() override { return m->IsInt(); }
    bool IsInt64() override { return m->IsInt64(); }
    bool IsUint64() override { return m->IsUint64(); }
    bool IsDouble() override { return m->IsDouble(); }
    bool IsString() override { return m->IsString(); }

    void GetNull() override {}
    bool GetBool() override { return m->GetBool(); }
    int GetInt() override { return m->GetInt(); }
    uint32_t GetUint32() override { return uint32_t(m->GetUint64()); }
    int64_t GetInt64() override { return m->GetInt64(); }
    uint64_t GetUint64() override { return m->GetUint64(); }
    double GetDouble() override { return m->GetDouble(); }
    std::string GetString() override { return m->GetString(); }

    bool HasMember(const char* x) override { return m->HasMember(x); }
    std::unique_ptr<Reader> operator[](const char* x) override {
        auto& sub = (*m)[x];
        return std::unique_ptr<JsonReader>(new JsonReader(&sub));
    }

    void IterArray(std::function<void(Reader&)> fn) override {
        if (!m->IsArray()) throw std::invalid_argument("array");
        // Use "0" to indicate any element for now.
        path.push_back("0");
        for (auto& entry : m->GetArray()) {
            auto saved = m;
            m = &entry;
            fn(*this);
            m = saved;
        }
        path.pop_back();
    }

    void DoMember(const char* name, std::function<void(Reader&)> fn) override {
        path.push_back(name);
        auto it = m->FindMember(name);
        if (it != m->MemberEnd()) {
            auto saved = m;
            m = &it->value;
            fn(*this);
            m = saved;
        }
        path.pop_back();
    }

    std::string GetPath() const {
        std::string ret;
        for (auto& t : path) {
            ret += '/';
            ret += t;
        }
        ret.pop_back();
        return ret;
    }
};

class JsonWriter : public Writer {
    rapidjson::Writer<rapidjson::StringBuffer>* m_;

   public:
    JsonWriter(rapidjson::Writer<rapidjson::StringBuffer>* m) : m_(m) {}
    serialize_format Format() const override { return serialize_format::Json; }

    void Null() override { m_->Null(); }
    void Bool(bool x) override { m_->Bool(x); }
    void Int(int x) override { m_->Int(x); }
    void Uint32(uint32_t x) override { m_->Uint64(x); }
    void Int64(int64_t x) override { m_->Int64(x); }
    void Uint64(uint64_t x) override { m_->Uint64(x); }
    void Double(double x) override { m_->Double(x); }
    void String(const char* x) override { m_->String(x); }
    void String(const char* x, size_t len) override { m_->String(x, len); }
    void StartArray(size_t) override { m_->StartArray(); }
    void EndArray() override { m_->EndArray(); }
    void StartObject() override { m_->StartObject(); }
    void EndObject() override { m_->EndObject(); }
    void Key(const char* name) override { m_->Key(name); }
};
