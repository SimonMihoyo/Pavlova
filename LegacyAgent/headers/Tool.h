//
//  Tool.h
//  LegacyAgent
//
//  Created by SimonMihoyo on 2026/8/22.
//  Copyright © 2026 SimonMihoyo. All rights reserved.
//

#ifndef Tool_h
#define Tool_h

#pragma once

#include <string>
#include <map>
#include <functional>

struct ToolDefinition;
struct JSONValue;

class Tool {
public:
    virtual ~Tool() = default;

    virtual const ToolDefinition& getDefinition() const = 0;

    virtual void call(
        const std::map<std::string, JSONValue>& arguments,
        std::function<void(std::string)> completion
    ) = 0;
};


#endif /* Tool_h */
