// Minimal single-header JSON parser (UTF-8 in, UTF-8 std::string values).
// Only what HyoExam needs to read/write schedule + settings files — no external deps,
// keeps the binary small instead of vendoring a full JSON library.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <cctype>

namespace hyo::json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Type type = Type::Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    static Value makeObject() { Value v; v.type = Type::Object; return v; }
    static Value makeArray() { Value v; v.type = Type::Array; return v; }
    static Value makeString(std::string s) { Value v; v.type = Type::String; v.strVal = std::move(s); return v; }
    static Value makeNumber(double d) { Value v; v.type = Type::Number; v.numVal = d; return v; }
    static Value makeBool(bool b) { Value v; v.type = Type::Bool; v.boolVal = b; return v; }

    bool has(const std::string& key) const {
        for (auto& [k, v] : obj) if (k == key) return true;
        return false;
    }
    const Value& operator[](const std::string& key) const {
        static Value null_;
        for (auto& [k, v] : obj) if (k == key) return v;
        return null_;
    }
    void set(const std::string& key, Value v) {
        for (auto& kv : obj) { if (kv.first == key) { kv.second = std::move(v); return; } }
        obj.emplace_back(key, std::move(v));
    }
    std::string asString(const std::string& def = "") const { return type == Type::String ? strVal : def; }
    double asNumber(double def = 0.0) const { return type == Type::Number ? numVal : def; }
    bool asBool(bool def = false) const { return type == Type::Bool ? boolVal : def; }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s(text), i(0) {}

    Value parse() {
        skipWs();
        Value v = parseValue();
        return v;
    }

private:
    const std::string& s;
    size_t i;

    void skipWs() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
    char peek() { return i < s.size() ? s[i] : '\0'; }
    char next() { return i < s.size() ? s[i++] : '\0'; }

    Value parseValue() {
        skipWs();
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value::makeString(parseString());
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { i += 4; return Value(); }
        return parseNumber();
    }

    Value parseObject() {
        Value v = Value::makeObject();
        next(); // {
        skipWs();
        if (peek() == '}') { next(); return v; }
        while (true) {
            skipWs();
            std::string key = parseString();
            skipWs();
            next(); // :
            Value val = parseValue();
            v.obj.emplace_back(key, std::move(val));
            skipWs();
            if (peek() == ',') { next(); continue; }
            break;
        }
        skipWs();
        next(); // }
        return v;
    }

    Value parseArray() {
        Value v = Value::makeArray();
        next(); // [
        skipWs();
        if (peek() == ']') { next(); return v; }
        while (true) {
            Value val = parseValue();
            v.arr.push_back(std::move(val));
            skipWs();
            if (peek() == ',') { next(); continue; }
            break;
        }
        skipWs();
        next(); // ]
        return v;
    }

    std::string parseString() {
        std::string out;
        next(); // opening quote
        while (true) {
            char c = next();
            if (c == '"' || c == '\0') break;
            if (c == '\\') {
                char e = next();
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'u': {
                        // Passed through as literal for our simple ASCII/Korean UTF-8 use case.
                        std::string hex = s.substr(i, 4);
                        i += 4;
                        int code = std::stoi(hex, nullptr, 16);
                        if (code < 0x80) out += (char)code;
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    Value parseBool() {
        if (s.compare(i, 4, "true") == 0) { i += 4; return Value::makeBool(true); }
        i += 5;
        return Value::makeBool(false);
    }

    Value parseNumber() {
        size_t start = i;
        if (peek() == '-') next();
        while (std::isdigit((unsigned char)peek()) || peek() == '.' || peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-') next();
        return Value::makeNumber(std::stod(s.substr(start, i - start)));
    }
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

inline void escapeInto(std::ostringstream& os, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n"; break;
            case '\t': os << "\\t"; break;
            case '\r': os << "\\r"; break;
            default: os << c;
        }
    }
}

inline void dump(const Value& v, std::ostringstream& os) {
    switch (v.type) {
        case Type::Null: os << "null"; break;
        case Type::Bool: os << (v.boolVal ? "true" : "false"); break;
        case Type::Number: {
            if (v.numVal == (long long)v.numVal) os << (long long)v.numVal;
            else os << v.numVal;
            break;
        }
        case Type::String: os << '"'; escapeInto(os, v.strVal); os << '"'; break;
        case Type::Array: {
            os << '[';
            for (size_t k = 0; k < v.arr.size(); k++) { if (k) os << ','; dump(v.arr[k], os); }
            os << ']';
            break;
        }
        case Type::Object: {
            os << '{';
            for (size_t k = 0; k < v.obj.size(); k++) {
                if (k) os << ',';
                os << '"'; escapeInto(os, v.obj[k].first); os << "\":";
                dump(v.obj[k].second, os);
            }
            os << '}';
            break;
        }
    }
}

inline std::string dump(const Value& v) {
    std::ostringstream os;
    dump(v, os);
    return os.str();
}

} // namespace hyo::json
