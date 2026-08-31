//  JSONValue.cpp
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//

#include "headers/JSONValue.h"
#include <cstdio>
#include <cctype>
#include <sstream>
#include <cmath>

namespace {

// ---- 序列化 ----

void appendEscaped(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void serializeValue(std::string& out, const JSONValue& v) {
    std::visit([&](const auto& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            out += "null";
        } else if constexpr (std::is_same_v<T, bool>) {
            out += arg ? "true" : "false";
        } else if constexpr (std::is_same_v<T, double>) {
            if (std::isfinite(arg)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%g", arg);
                out += buf;
            } else {
                out += "null";
            }
        } else if constexpr (std::is_same_v<T, std::string>) {
            appendEscaped(out, arg);
        } else if constexpr (std::is_same_v<T, JSONArray>) {
            out += '[';
            bool first = true;
            for (const auto& item : arg) {
                if (!first) out += ',';
                first = false;
                serializeValue(out, item);
            }
            out += ']';
        } else if constexpr (std::is_same_v<T, JSONObject>) {
            out += '{';
            bool first = true;
            for (const auto& kv : arg) {
                if (!first) out += ',';
                first = false;
                appendEscaped(out, kv.first);
                out += ':';
                serializeValue(out, kv.second);
            }
            out += '}';
        }
    }, v.value);
}

// ---- 解析 ----

struct Parser {
    const std::string& s;
    size_t pos = 0;
    std::string* error;

    bool fail(const std::string& msg) {
        if (error) *error = msg + " (位置 " + std::to_string(pos) + ")";
        return false;
    }

    void skipWs() {
        while (pos < s.size() && isspace((unsigned char)s[pos])) pos++;
    }

    bool parseValue(JSONValue& out) {
        skipWs();
        if (pos >= s.size()) return fail("意外的结尾");
        char c = s[pos];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') {
            std::string str;
            if (!parseString(str)) return false;
            out = JSONValue(str);
            return true;
        }
        if (c == 't' || c == 'f') {
            bool b;
            if (!parseBool(b)) return false;
            out = JSONValue(b);
            return true;
        }
        if (c == 'n') {
            if (s.compare(pos, 4, "null") != 0) return fail("无效 token");
            pos += 4;
            out = JSONValue();
            return true;
        }
        // number
        double d;
        if (!parseNumber(d)) return false;
        out = JSONValue(d);
        return true;
    }

    bool parseObject(JSONValue& out) {
        pos++; // '{'
        JSONObject obj;
        skipWs();
        if (pos < s.size() && s[pos] == '}') { pos++; out = JSONValue(obj); return true; }
        while (true) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos >= s.size() || s[pos] != ':') return fail("缺少 ':'");
            pos++;
            JSONValue value;
            if (!parseValue(value)) return false;
            obj[key] = value;
            skipWs();
            if (pos >= s.size()) return fail("对象未闭合");
            if (s[pos] == ',') { pos++; continue; }
            if (s[pos] == '}') { pos++; out = JSONValue(obj); return true; }
            return fail("对象中预期 ',' 或 '}'");
        }
    }

    bool parseArray(JSONValue& out) {
        pos++; // '['
        JSONArray arr;
        skipWs();
        if (pos < s.size() && s[pos] == ']') { pos++; out = JSONValue(arr); return true; }
        while (true) {
            JSONValue value;
            if (!parseValue(value)) return false;
            arr.push_back(value);
            skipWs();
            if (pos >= s.size()) return fail("数组未闭合");
            if (s[pos] == ',') { pos++; continue; }
            if (s[pos] == ']') { pos++; out = JSONValue(arr); return true; }
            return fail("数组中预期 ',' 或 ']'");
        }
    }

    bool parseString(std::string& out) {
        if (pos >= s.size() || s[pos] != '"') return fail("预期字符串");
        pos++;
        std::string result;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') { out = result; return true; }
            if (c == '\\') {
                if (pos >= s.size()) return fail("转义符后缺少字符");
                char e = s[pos++];
                switch (e) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'u': {
                        if (pos + 4 > s.size()) return fail("非法 \\u 转义");
                        unsigned code = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = s[pos++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else return fail("非法 \\u 转义");
                        }
                        // 只处理 BMP 内的字符（简化）
                        if (code < 0x80) result += static_cast<char>(code);
                        else if (code < 0x800) {
                            result += static_cast<char>(0xC0 | (code >> 6));
                            result += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (code >> 12));
                            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: return fail("非法转义");
                }
            } else {
                result += c;
            }
        }
        return fail("字符串未闭合");
    }

    bool parseBool(bool& out) {
        if (s.compare(pos, 4, "true") == 0) { pos += 4; out = true; return true; }
        if (s.compare(pos, 5, "false") == 0) { pos += 5; out = false; return true; }
        return fail("无效布尔值");
    }

    bool parseNumber(double& out) {
        size_t start = pos;
        if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) pos++;
        bool hasDigits = false;
        while (pos < s.size() && isdigit((unsigned char)s[pos])) { pos++; hasDigits = true; }
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            while (pos < s.size() && isdigit((unsigned char)s[pos])) pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            while (pos < s.size() && isdigit((unsigned char)s[pos])) pos++;
        }
        if (!hasDigits) return fail("无效数字");
        std::string token = s.substr(start, pos - start);
        try {
            out = std::stod(token);
        } catch (...) {
            return fail("无效数字");
        }
        return true;
    }
};

} // namespace

bool JSON::parse(const std::string& text, JSONValue& out, std::string& error) {
    Parser parser{text, 0, &error};
    if (!parser.parseValue(out)) return false;
    parser.skipWs();
    if (parser.pos != text.size()) {
        error = "JSON 末尾有多余内容 (位置 " + std::to_string(parser.pos) + ")";
        return false;
    }
    return true;
}

std::string JSON::serialize(const JSONValue& value) {
    std::string out;
    serializeValue(out, value);
    return out;
}
