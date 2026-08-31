// csrc/json.hpp — 极简 JSON 解析/序列化（自研，零第三方依赖）
//
// 覆盖本项目所需的全部 JSON 能力：
//   - 解析: 字符串 → Json（object/array/string/number/bool/null，嵌套）
//   - 序列化: Json → 紧凑字符串 / 2 空格缩进 pretty 字符串
//   - 访问: 类型判断 + get(key)/geti(key)/getd(key)/gets(key)/getb(key) 便捷读取
//
// 数值统一存 double（与 JSON 语义一致），整数读写走 int64 转换。
// 字符串按 UTF-8 原样透传（不转义非 ASCII，与 Python ensure_ascii=False 一致）。

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace csrc {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() : type_(Type::Null), num_(0), bool_(false) {}
    Json(Type t) : type_(t), num_(0), bool_(false) {}
    Json(bool b) : type_(Type::Bool), num_(0), bool_(b) {}
    Json(double n) : type_(Type::Number), num_(n), bool_(false) {}
    Json(int n) : type_(Type::Number), num_((double)n), bool_(false) {}
    Json(int64_t n) : type_(Type::Number), num_((double)n), bool_(false) {}
    Json(const char* s) : type_(Type::String), num_(0), bool_(false), str_(s) {}
    Json(const std::string& s) : type_(Type::String), num_(0), bool_(false), str_(s) {}

    // ── 类型判断 ──
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // ── 便捷读取（失败返回默认值）──
    bool getb(const std::string& k, bool def = false) const {
        auto* v = find(k);
        return v && v->is_bool() ? v->bool_ : def;
    }
    int64_t geti(const std::string& k, int64_t def = 0) const {
        auto* v = find(k);
        return v && v->is_number() ? (int64_t)v->num_ : def;
    }
    double getd(const std::string& k, double def = 0.0) const {
        auto* v = find(k);
        return v && v->is_number() ? v->num_ : def;
    }
    std::string gets(const std::string& k, const std::string& def = "") const {
        auto* v = find(k);
        return v && v->is_string() ? v->str_ : def;
    }
    const Json* get(const std::string& k) const { return find(k); }
    Json* get(const std::string& k) { return find(k); }

    /// 数字 → int（数组元素/字符串转数字场景）
    int64_t as_int(int64_t def = 0) const {
        if (is_number()) return (int64_t)num_;
        if (is_bool()) return bool_ ? 1 : 0;
        if (is_string()) {
            char* end = nullptr;
            long long v = strtoll(str_.c_str(), &end, 10);
            return end && *end == '\0' ? (int64_t)v : def;
        }
        return def;
    }
    double as_double(double def = 0.0) const {
        if (is_number()) return num_;
        if (is_bool()) return bool_ ? 1.0 : 0.0;
        if (is_string()) {
            char* end = nullptr;
            double v = strtod(str_.c_str(), &end);
            return end && *end == '\0' ? v : def;
        }
        return def;
    }
    std::string as_string() const {
        if (is_string()) return str_;
        return dump();
    }

    // ── 修改 ──
    Json& operator[](const std::string& k) {
        ensure_object();
        auto it = obj_.find(k);
        if (it == obj_.end()) {
            obj_.emplace(k, Json());
            return obj_[k];
        }
        return it->second;
    }
    Json& operator[](size_t i) {
        ensure_array();
        if (arr_.size() <= i) arr_.resize(i + 1);
        return arr_[i];
    }
    void push_back(const Json& v) {
        ensure_array();
        arr_.push_back(v);
    }
    void set(const std::string& k, Json v) {
        ensure_object();
        obj_[k] = std::move(v);
    }
    void erase(const std::string& k) {
        if (is_object()) obj_.erase(k);
    }
    bool has(const std::string& k) const { return find(k) != nullptr; }

    const std::map<std::string, Json>& object() const { return obj_; }
    std::map<std::string, Json>& object() { return obj_; }
    const std::vector<Json>& array() const { return arr_; }
    std::vector<Json>& array() { return arr_; }
    size_t size() const {
        if (is_object()) return obj_.size();
        if (is_array()) return arr_.size();
        return 0;
    }

    /// 迭代 object：for (auto& [k, v] : j.object())
    std::map<std::string, Json>::const_iterator begin() const { return obj_.begin(); }
    std::map<std::string, Json>::const_iterator end() const { return obj_.end(); }

    // ── 序列化 ──
    std::string dump(bool pretty = false, int indent = 0) const;

    // ── 解析 ──
    /// 解析 JSON 文本。成功返回 true 并把结果写入 out；失败返回 false。
    static bool parse(const std::string& text, Json& out, std::string* err = nullptr);
    static Json parse_or(const std::string& text, Json fallback = Json());

private:
    const Json* find(const std::string& k) const {
        if (!is_object()) return nullptr;
        auto it = obj_.find(k);
        return it == obj_.end() ? nullptr : &it->second;
    }
    Json* find(const std::string& k) {
        if (!is_object()) return nullptr;
        auto it = obj_.find(k);
        return it == obj_.end() ? nullptr : &it->second;
    }
    void ensure_object() {
        if (type_ != Type::Object) { *this = Json(Type::Object); }
    }
    void ensure_array() {
        if (type_ != Type::Array) { *this = Json(Type::Array); }
    }

    Type type_;
    double num_;
    bool bool_;
    std::string str_;
    std::map<std::string, Json> obj_;
    std::vector<Json> arr_;
};

// ── 内部解析器 ──

namespace json_detail {

struct Parser {
    const char* p;
    const char* end;
    std::string err;

    explicit Parser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool fail(const std::string& msg) {
        if (err.empty()) err = msg;
        return false;
    }
    bool parse_value(Json& out) {
        skip_ws();
        if (p >= end) return fail("unexpected end of input");
        char c = *p;
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            out = Json(s);
            return true;
        }
        if (c == 't' || c == 'f') {
            if (end - p >= 4 && strncmp(p, "true", 4) == 0) { p += 4; out = Json(true); return true; }
            if (end - p >= 5 && strncmp(p, "false", 5) == 0) { p += 5; out = Json(false); return true; }
            return fail("invalid literal");
        }
        if (c == 'n') {
            if (end - p >= 4 && strncmp(p, "null", 4) == 0) { p += 4; out = Json(); return true; }
            return fail("invalid literal");
        }
        return parse_number(out);
    }

    bool parse_object(Json& out) {
        p++;  // {
        out = Json(Json::Type::Object);
        skip_ws();
        if (p < end && *p == '}') { p++; return true; }
        while (true) {
            skip_ws();
            if (p >= end || *p != '"') return fail("expected string key");
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            p++;
            Json val;
            if (!parse_value(val)) return false;
            out.set(key, std::move(val));
            skip_ws();
            if (p >= end) return fail("unterminated object");
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(Json& out) {
        p++;  // [
        out = Json(Json::Type::Array);
        skip_ws();
        if (p < end && *p == ']') { p++; return true; }
        while (true) {
            Json val;
            if (!parse_value(val)) return false;
            out.push_back(std::move(val));
            skip_ws();
            if (p >= end) return fail("unterminated array");
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_string(std::string& out) {
        p++;  // "
        out.clear();
        while (p < end) {
            unsigned char c = (unsigned char)*p;
            if (c == '"') { p++; return true; }
            if (c == '\\') {
                p++;
                if (p >= end) return fail("bad escape");
                char e = *p++;
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
                        if (end - p < 4) return fail("bad \\u escape");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else return fail("bad \\u hex");
                        }
                        // 简单 UTF-8 编码（不处理 surrogate pair，本项目无此需求）
                        if (cp < 0x80) out.push_back((char)cp);
                        else if (cp < 0x800) {
                            out.push_back((char)(0xC0 | (cp >> 6)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back((char)(0xE0 | (cp >> 12)));
                            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: return fail("bad escape char");
                }
                continue;
            }
            if (c < 0x20) return fail("control char in string");
            out.push_back((char)c);
            p++;
        }
        return fail("unterminated string");
    }

    bool parse_number(Json& out) {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) p++;
        bool any_digit = false;
        while (p < end && *p >= '0' && *p <= '9') { p++; any_digit = true; }
        if (p < end && *p == '.') {
            p++;
            while (p < end && *p >= '0' && *p <= '9') { p++; any_digit = true; }
        }
        if (any_digit && p < end && (*p == 'e' || *p == 'E')) {
            p++;
            if (p < end && (*p == '+' || *p == '-')) p++;
            bool exp_digit = false;
            while (p < end && *p >= '0' && *p <= '9') { p++; exp_digit = true; }
            if (!exp_digit) return fail("bad exponent");
        }
        if (!any_digit) return fail("invalid number");
        std::string num_str(start, (size_t)(p - start));
        out = Json(strtod(num_str.c_str(), nullptr));
        return true;
    }
};

}  // namespace json_detail

inline std::string Json::dump(bool pretty, int indent) const {
    std::string pad;
    if (pretty) pad.assign((size_t)indent * 2, ' ');

    switch (type_) {
        case Type::Null: return "null";
        case Type::Bool: return bool_ ? "true" : "false";
        case Type::Number: {
            if (num_ == (double)(int64_t)num_) {
                char buf[32];
                snprintf(buf, sizeof buf, "%lld", (long long)(int64_t)num_);
                return buf;
            }
            char buf[64];
            snprintf(buf, sizeof buf, "%.10g", num_);
            return buf;
        }
        case Type::String: {
            std::string out = "\"";
            for (unsigned char c : str_) {
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
                            char buf[8];
                            snprintf(buf, sizeof buf, "\\u%04x", c);
                            out += buf;
                        } else {
                            out.push_back((char)c);  // UTF-8 原样
                        }
                }
            }
            out += "\"";
            return out;
        }
        case Type::Array: {
            if (arr_.empty()) return "[]";
            std::string out = "[";
            for (size_t i = 0; i < arr_.size(); i++) {
                if (i) out += ",";
                if (pretty) out += "\n" + pad + "  ";
                out += arr_[i].dump(pretty, indent + 1);
            }
            if (pretty) out += "\n" + pad;
            out += "]";
            return out;
        }
        case Type::Object: {
            if (obj_.empty()) return "{}";
            std::string out = "{";
            size_t i = 0;
            for (auto& kv : obj_) {
                if (i++) out += ",";
                if (pretty) out += "\n" + pad + "  ";
                out += Json(kv.first).dump(pretty, indent + 1);
                out += pretty ? ": " : ":";
                out += kv.second.dump(pretty, indent + 1);
            }
            if (pretty) out += "\n" + pad;
            out += "}";
            return out;
        }
    }
    return "null";
}

inline bool Json::parse(const std::string& text, Json& out, std::string* err) {
    json_detail::Parser parser(text);
    if (!parser.parse_value(out)) {
        if (err) *err = parser.err;
        return false;
    }
    parser.skip_ws();
    if (parser.p != parser.end) {
        if (err) *err = "trailing data after JSON value";
        return false;
    }
    return true;
}

inline Json Json::parse_or(const std::string& text, Json fallback) {
    Json out;
    if (parse(text, out)) return out;
    return fallback;
}

}  // namespace csrc
