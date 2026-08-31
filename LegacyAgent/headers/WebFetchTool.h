//
//  WebFetchTool.hpp
//  LegacyAgent
//
//  Created by SimonMihoyo on 2026/8/22.
//  Copyright © 2026 SimonMihoyo. All rights reserved.
//

#ifndef WebFetchTool_hpp
#define WebFetchTool_hpp

#pragma once

#include "Tool.h"
#include "ToolDefinition.h"

#include <stdio.h>
#include <string>

class WebFetchTool : public Tool {
public:
    WebFetchTool();
    const ToolDefinition& getDefinition() const override;
    void call(const std::map<std::string, JSONValue>& arguments, std::function<void(std::string)> completion) override;

private:
    ToolDefinition definition;
};

#endif /* WebFetchTool_hpp */
