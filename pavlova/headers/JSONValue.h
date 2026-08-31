//
//  JSONValue.h
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//  Copyright © 2026 SimonMihoyo. All rights reserved.
//

#ifndef JSONValue_h
#define JSONValue_h

#pragma once

#include <string>
#include <vector>
#include <map>
#include <variant>

struct JSONValue;

using JSONObject = std::map<std::string, JSONValue>;
using JSONArray  = std::vector<JSONValue>;

struct JSONValue {
    std::variant<
        std::monostate,   // 对应 .null
        bool,             // 对应 .bool
        double,           // 对应 .number
        std::string,      // 对应 .string
        JSONArray,        // 对应 .array
        JSONObject        // 对应 .object
    > value;

    // 便捷访问
    const JSONObject* asObject() const { return std::get_if<JSONObject>(&value); }
    const JSONArray*  asArray()  const { return std::get_if<JSONArray>(&value); }
    const std::string* asString() const { return std::get_if<std::string>(&value); }
    const bool*       asBool()   const { return std::get_if<bool>(&value); }
    const double*     asNumber() const { return std::get_if<double>(&value); }

    // 便捷构造
    JSONValue() : value(std::monostate{}) {}
    JSONValue(std::nullptr_t) : value(std::monostate{}) {}
    JSONValue(bool b) : value(b) {}
    JSONValue(double d) : value(d) {}
    JSONValue(const std::string& s) : value(s) {}
    JSONValue(const char* s) : value(std::string(s ? s : "")) {}
    JSONValue(const JSONArray& a) : value(a) {}
    JSONValue(const JSONObject& o) : value(o) {}

    // C++17 下需要手动实现 operator==
    bool operator==(const JSONValue& other) const {
        return value == other.value;
    }

    bool operator!=(const JSONValue& other) const {
        return !(*this == other);
    }
};

namespace JSON {
    // 解析 JSON 字符串，失败返回 false 并填充 error
    bool parse(const std::string& text, JSONValue& out, std::string& error);
    // 序列化 JSONValue 为 JSON 字符串
    std::string serialize(const JSONValue& value);
}

#endif /* JSONValue_h */
