//
//  ChatMessage.h
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//

#ifndef ChatMessage_h
#define ChatMessage_h

#pragma once

#include <string>
#include <vector>
#include "JSONValue.h"

struct ChatMessage {
    struct ToolCall {
        struct FunctionCall {
            std::string name;
            std::string arguments; // JSON 字符串
        };
        std::string id;
        std::string type;
        FunctionCall function;
    };

    std::string role;
    std::string content;
    std::string reasoningContent; // DeepSeek-R1 reasoning / thinking
    bool hasContent = false;
    std::vector<ToolCall> toolCalls;
    std::string toolCallId;
    std::string name;

    static ChatMessage system(const std::string& c) {
        ChatMessage m;
        m.role = "system";
        m.content = c;
        m.hasContent = true;
        return m;
    }

    static ChatMessage user(const std::string& c) {
        ChatMessage m;
        m.role = "user";
        m.content = c;
        m.hasContent = true;
        return m;
    }

    static ChatMessage assistant(const std::string& c) {
        ChatMessage m;
        m.role = "assistant";
        m.content = c;
        m.hasContent = true;
        return m;
    }

    static ChatMessage assistantToolCalls(const std::vector<ToolCall>& calls) {
        ChatMessage m;
        m.role = "assistant";
        m.hasContent = false;
        m.toolCalls = calls;
        return m;
    }

    static ChatMessage toolResult(const std::string& callId, const std::string& name, const std::string& c) {
        ChatMessage m;
        m.role = "tool";
        m.content = c;
        m.hasContent = true;
        m.toolCallId = callId;
        m.name = name;
        return m;
    }
};

#endif /* ChatMessage_h */
