// Wron Motion Path — minimal JSON DOM.
//
// Scope: exactly what the path document needs. Strict RFC 8259 grammar,
// locale-independent number parsing/printing (std::from_chars / to_chars),
// object key order preserved so unknown sections round-trip unchanged,
// hard limits on input size and nesting depth so corrupt project data cannot
// exhaust memory or the stack.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wmp::json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    static Value null() { return Value(); }
    static Value boolean(bool b) { Value v; v.type_ = Type::Bool; v.bool_ = b; return v; }
    static Value number(double n) { Value v; v.type_ = Type::Number; v.number_ = n; return v; }
    static Value string(std::string s) { Value v; v.type_ = Type::String; v.string_ = std::move(s); return v; }
    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const { return bool_; }
    double asNumber() const { return number_; }
    const std::string& asString() const { return string_; }

    // Array access.
    const std::vector<Value>& items() const { return array_; }
    std::vector<Value>& items() { return array_; }
    void push(Value v) { array_.push_back(std::move(v)); }

    // Object access (ordered).
    using Member = std::pair<std::string, Value>;
    const std::vector<Member>& members() const { return object_; }
    std::vector<Member>& members() { return object_; }
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    // Inserts or replaces, keeping the position of an existing key.
    void set(std::string_view key, Value v);
    bool erase(std::string_view key);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<Member> object_;
};

struct ParseError {
    std::size_t offset = 0;
    std::string message;
};

struct ParseLimits {
    std::size_t maxBytes = 8u * 1024u * 1024u;
    std::size_t maxDepth = 64;
};

// Returns false (and fills err) on any syntax error, trailing garbage,
// non-finite number, invalid UTF-16 escape or limit violation.
bool parse(std::string_view text, Value& out, ParseError* err = nullptr,
           const ParseLimits& limits = ParseLimits());

// Compact serialization. Non-finite numbers are written as null.
std::string serialize(const Value& v);

}  // namespace wmp::json
