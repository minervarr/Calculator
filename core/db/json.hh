// json.hh — minimal JSON parse + escape for workspace export/import.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// Header-only, dependency-free. Parses standard JSON into a Value tree (enough
// for our workspace files and tolerant of hand/edited input) and escapes strings
// for output. Numbers are parsed as double; \uXXXX is decoded to UTF-8 (BMP).
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace db {
namespace json {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool        b   = false;
    double      num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    const Value* find(const std::string& k) const {
        if (type != Obj) return nullptr;
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    double      numOr(double d)              const { return type == Num  ? num : d; }
    bool        boolOr(bool d)               const { return type == Bool ? b   : d; }
    std::string strOr(const std::string& d)  const { return type == Str  ? str : d; }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    bool parse(Value& out) { return value(out); }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skip() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') i_++;
            else break;
        }
    }
    bool value(Value& v) {
        skip();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.type = Value::Str; return string(v.str); }
        if (c == 't' || c == 'f') return boolean(v);
        if (c == 'n') {
            if (s_.compare(i_, 4, "null") == 0) { i_ += 4; v.type = Value::Null; return true; }
            return false;
        }
        return number(v);
    }
    bool string(std::string& out) {
        if (s_[i_] != '"') return false;
        i_++;
        out.clear();
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            char e = s_[i_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    if (i_ + 4 > s_.size()) return false;
                    int cp = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = s_[i_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp += h - '0';
                        else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
                        else return false;
                    }
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool number(Value& v) {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) i_++;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '-' || c == '+') i_++;
            else break;
        }
        if (i_ == start) return false;
        v.type = Value::Num;
        v.num  = std::atof(s_.substr(start, i_ - start).c_str());
        return true;
    }
    bool boolean(Value& v) {
        if (s_.compare(i_, 4, "true") == 0)  { i_ += 4; v.type = Value::Bool; v.b = true;  return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; v.type = Value::Bool; v.b = false; return true; }
        return false;
    }
    bool array(Value& v) {
        v.type = Value::Arr;
        i_++; skip();
        if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
        for (;;) {
            Value e;
            if (!value(e)) return false;
            v.arr.push_back(std::move(e));
            skip();
            if (i_ >= s_.size()) return false;
            char c = s_[i_++];
            if (c == ']') return true;
            if (c != ',') return false;
        }
    }
    bool object(Value& v) {
        v.type = Value::Obj;
        i_++; skip();
        if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
        for (;;) {
            skip();
            if (i_ >= s_.size() || s_[i_] != '"') return false;
            std::string key;
            if (!string(key)) return false;
            skip();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            i_++;
            Value val;
            if (!value(val)) return false;
            v.obj.emplace_back(std::move(key), std::move(val));
            skip();
            if (i_ >= s_.size()) return false;
            char c = s_[i_++];
            if (c == '}') return true;
            if (c != ',') return false;
        }
    }
};

inline bool parse(const std::string& s, Value& out) { Parser p(s); return p.parse(out); }

inline std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\t': o += "\\t";  break;
            case '\r': o += "\\r";  break;
            default:   o += c;      break;
        }
    }
    return o;
}

}  // namespace json
}  // namespace db
