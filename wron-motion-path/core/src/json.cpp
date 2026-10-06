#include "wmp/json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>

namespace wmp::json {

const Value* Value::find(std::string_view key) const {
    for (const auto& m : object_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value* Value::find(std::string_view key) {
    for (auto& m : object_)
        if (m.first == key) return &m.second;
    return nullptr;
}

void Value::set(std::string_view key, Value v) {
    if (Value* existing = find(key)) {
        *existing = std::move(v);
        return;
    }
    object_.emplace_back(std::string(key), std::move(v));
}

bool Value::erase(std::string_view key) {
    for (auto it = object_.begin(); it != object_.end(); ++it) {
        if (it->first == key) {
            object_.erase(it);
            return true;
        }
    }
    return false;
}

namespace {

class Parser {
public:
    Parser(std::string_view text, const ParseLimits& limits) : s_(text), limits_(limits) {}

    bool run(Value& out, ParseError* err) {
        if (s_.size() > limits_.maxBytes) return fail(err, 0, "input exceeds size limit");
        skipWs();
        if (!parseValue(out, 0)) return fail(err, pos_, msg_);
        skipWs();
        if (pos_ != s_.size()) return fail(err, pos_, "trailing characters after JSON value");
        return true;
    }

private:
    std::string_view s_;
    ParseLimits limits_;
    std::size_t pos_ = 0;
    const char* msg_ = "syntax error";

    static bool fail(ParseError* err, std::size_t at, const char* m) {
        if (err) {
            err->offset = at;
            err->message = m;
        }
        return false;
    }

    bool error(const char* m) {
        msg_ = m;
        return false;
    }

    void skipWs() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return error("invalid literal");
        pos_ += word.size();
        return true;
    }

    bool parseValue(Value& out, std::size_t depth) {
        if (depth > limits_.maxDepth) return error("nesting too deep");
        if (pos_ >= s_.size()) return error("unexpected end of input");
        const char c = s_[pos_];
        switch (c) {
            case 'n': if (!literal("null")) return false; out = Value::null(); return true;
            case 't': if (!literal("true")) return false; out = Value::boolean(true); return true;
            case 'f': if (!literal("false")) return false; out = Value::boolean(false); return true;
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out = Value::string(std::move(str));
                return true;
            }
            case '[': return parseArray(out, depth);
            case '{': return parseObject(out, depth);
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
                return error("unexpected character");
        }
    }

    bool parseNumber(Value& out) {
        const std::size_t start = pos_;
        auto digit = [&](std::size_t i) { return i < s_.size() && s_[i] >= '0' && s_[i] <= '9'; };
        std::size_t i = pos_;
        if (i < s_.size() && s_[i] == '-') ++i;
        if (!digit(i)) return error("invalid number");
        if (s_[i] == '0') {
            ++i;
        } else {
            while (digit(i)) ++i;
        }
        if (i < s_.size() && s_[i] == '.') {
            ++i;
            if (!digit(i)) return error("invalid number fraction");
            while (digit(i)) ++i;
        }
        if (i < s_.size() && (s_[i] == 'e' || s_[i] == 'E')) {
            ++i;
            if (i < s_.size() && (s_[i] == '+' || s_[i] == '-')) ++i;
            if (!digit(i)) return error("invalid number exponent");
            while (digit(i)) ++i;
        }
        double v = 0.0;
        const char* first = s_.data() + start;
        const char* last = s_.data() + i;
        const auto res = std::from_chars(first, last, v);
        if (res.ec != std::errc() || res.ptr != last || !std::isfinite(v))
            return error("number out of range");
        pos_ = i;
        out = Value::number(v);
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(std::uint32_t& v) {
        if (pos_ + 4 > s_.size()) return error("truncated unicode escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return error("invalid unicode escape");
        }
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= s_.size()) return error("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return error("control character in string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) return error("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                            return error("unpaired surrogate");
                        pos_ += 2;
                        std::uint32_t lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return error("invalid surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return error("unpaired surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return error("invalid escape");
            }
        }
    }

    bool parseArray(Value& out, std::size_t depth) {
        ++pos_;
        out = Value::array();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            Value item;
            skipWs();
            if (!parseValue(item, depth + 1)) return false;
            out.push(std::move(item));
            skipWs();
            if (pos_ >= s_.size()) return error("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return error("expected ',' or ']'");
        }
    }

    bool parseObject(Value& out, std::size_t depth) {
        ++pos_;
        out = Value::object();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return error("expected object key");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return error("expected ':'");
            ++pos_;
            skipWs();
            Value v;
            if (!parseValue(v, depth + 1)) return false;
            // Duplicate keys: last one wins (common JSON practice), position kept.
            out.set(key, std::move(v));
            skipWs();
            if (pos_ >= s_.size()) return error("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return error("expected ',' or '}'");
        }
    }
};

void writeString(std::string& out, const std::string& s) {
    static const char* hex = "0123456789abcdef";
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void writeValue(std::string& out, const Value& v) {
    switch (v.type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Type::Number: {
            const double n = v.asNumber();
            if (!std::isfinite(n)) {
                out += "null";
                break;
            }
            char buf[64];
            const auto res = std::to_chars(buf, buf + sizeof(buf), n == 0.0 ? 0.0 : n);
            out.append(buf, res.ptr);
            break;
        }
        case Type::String: writeString(out, v.asString()); break;
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& item : v.items()) {
                if (!first) out.push_back(',');
                first = false;
                writeValue(out, item);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& m : v.members()) {
                if (!first) out.push_back(',');
                first = false;
                writeString(out, m.first);
                out.push_back(':');
                writeValue(out, m.second);
            }
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

bool parse(std::string_view text, Value& out, ParseError* err, const ParseLimits& limits) {
    Parser p(text, limits);
    Value tmp;
    if (!p.run(tmp, err)) return false;
    out = std::move(tmp);
    return true;
}

std::string serialize(const Value& v) {
    std::string out;
    writeValue(out, v);
    return out;
}

}  // namespace wmp::json
