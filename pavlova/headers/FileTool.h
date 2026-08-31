//
//  FileTool.h
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//  Copyright © 2026 SimonMihoyo. All rights reserved.
//

#ifndef FileTool_h
#define FileTool_h

#pragma once

#include "Tool.h"
#include "ToolDefinition.h"
#include <string>

class FileTool : public Tool {
public:
    FileTool();
    const ToolDefinition& getDefinition() const override;
    void call(const std::map<std::string, JSONValue>& arguments, std::function<void(std::string)> completion) override;

private:
    ToolDefinition definition;
};

#endif /* FileTool_h */
