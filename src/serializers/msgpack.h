#pragma once

#include <msgpack.hpp>

#include "serializer.h"

class MessagePackReader : public Reader {
    msgpack::unpacker* pk;
    msgpack::object_handle oh;

    template <typename T>
    T Get() {
        T ret = oh.get().as<T>();
        pk->next(oh);
        return ret;
    }

   public:
    MessagePackReader(msgpack::unpacker* pk) : pk(pk) { pk->next(oh); }
    serialize_format Format() const override {
        return serialize_format::MessagePack;
    }

    bool IsBool() override { return oh.get().type == msgpack::type::BOOLEAN; }
    bool IsNull() override { return oh.get().is_nil(); }
    bool IsArray() override { return oh.get().type == msgpack::type::ARRAY; }
    bool IsInt() override {
        return oh.get().type == msgpack::type::POSITIVE_INTEGER ||
               oh.get().type == msgpack::type::NEGATIVE_INTEGER;
    }
    bool IsInt64() override { return IsInt(); }
    bool IsUint64() override { return IsInt(); }
    bool IsDouble() override {
        return oh.get().type == msgpack::type::FLOAT64;
    };
    bool IsString() override { return oh.get().type == msgpack::type::STR; }

    void GetNull() override { pk->next(oh); }
    bool GetBool() override { return Get<bool>(); }
    int GetInt() override { return Get<int>(); }
    uint32_t GetUint32() override { return Get<uint32_t>(); }
    int64_t GetInt64() override { return Get<int64_t>(); }
    uint64_t GetUint64() override { return Get<uint64_t>(); }
    double GetDouble() override { return Get<double>(); }
    std::string GetString() override { return Get<std::string>(); }

    bool HasMember(const char* x) override { return true; }
    std::unique_ptr<Reader> operator[](const char* x) override { return {}; }

    void IterArray(std::function<void(Reader&)> fn) override {
        size_t n = Get<size_t>();
        for (size_t i = 0; i < n; i++) fn(*this);
    }

    void DoMember(const char*, std::function<void(Reader&)> fn) override {
        fn(*this);
    }
};

class MessagePackWriter : public Writer {
    msgpack::packer<msgpack::sbuffer>* m;

   public:
    MessagePackWriter(msgpack::packer<msgpack::sbuffer>* m) : m(m) {}
    serialize_format Format() const override {
        return serialize_format::MessagePack;
    }

    void Null() override { m->pack_nil(); }
    void Bool(bool x) override { m->pack(x); }
    void Int(int x) override { m->pack(x); }
    void Uint32(uint32_t x) override { m->pack(x); }
    void Int64(int64_t x) override { m->pack(x); }
    void Uint64(uint64_t x) override { m->pack(x); }
    void Double(double x) override { m->pack(x); }
    void String(const char* x) override { m->pack(x); }
    // TODO Remove std::string
    void String(const char* x, size_t len) override {
        m->pack(std::string(x, len));
    }
    void StartArray(size_t n) override { m->pack(n); }
    void EndArray() override {}
    void StartObject() override {}
    void EndObject() override {}
    void Key(const char* name) override {}
};
