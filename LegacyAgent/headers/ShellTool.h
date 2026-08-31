//
//  ShellTool.h
//  LegacyAgent
//
//  Created by SimonMihoyo on 2026/8/22.
//  Copyright © 2026 SimonMihoyo. All rights reserved.
//

#ifndef ShellTool_h
#define ShellTool_h

#pragma once

#include "Tool.h"
#include "ToolDefinition.h"
#include <string>

class ShellTool : public Tool {
public:
    explicit ShellTool(double timeout = 30.0);
    const ToolDefinition& getDefinition() const override;
    void call(const std::map<std::string, JSONValue>& arguments, std::function<void(std::string)> completion) override;

private:
    ToolDefinition definition;
    double timeout;

    void run(const std::string& command, std::function<void(std::string)> completion);
};

#endif /* ShellTool_h */
