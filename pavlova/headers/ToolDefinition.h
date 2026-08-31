//
//  ToolDefinition.h
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//  Copyright © 2026 SimonMihoyo. All rights reserved.
//

#ifndef ToolDefinition_h
#define ToolDefinition_h

#pragma once

#include <string>
#include <optional>
#include "JSONValue.h"

struct ToolDefinition {
    struct Function {
        std::string name;
        std::string description;
        std::optional<JSONValue> parameters;

        // C++17 下需要手动实现 operator==
        bool operator==(const Function& other) const {
            return name == other.name
                && description == other.description
                && parameters == other.parameters;
        }

        bool operator!=(const Function& other) const {
            return !(*this == other);
        }
    };

    std::string type;
    Function function;

    // C++17 下需要手动实现 operator==
    bool operator==(const ToolDefinition& other) const {
        return type == other.type
            && function == other.function;
    }

    bool operator!=(const ToolDefinition& other) const {
        return !(*this == other);
    }

    // 对应 Swift 的静态工厂方法
    static ToolDefinition makeFunction(
        const std::string& name,
        const std::string& description,
        const std::optional<JSONValue>& parameters = std::nullopt
    ) {
        return ToolDefinition{
            "function",
            Function{name, description, parameters}
        };
    }
};

#endif /* ToolDefinition_h */
