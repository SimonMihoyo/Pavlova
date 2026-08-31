//
//  LLMClient.h
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//

#ifndef LLMClient_h
#define LLMClient_h

#pragma once

#include <string>
#include <vector>
#include <functional>
#include "ChatMessage.h"
#include "ToolDefinition.h"

struct AgentConfig {
    std::string baseURL;
    std::string apiKey;
    std::string model;
    std::string systemPrompt;
};

enum class AgentError {
    Config,
    HTTP,
    Decoding,
    EmptyResponse,
    UnknownTool,
    MaxIterations,
    Network
};

struct AgentResult {
    bool ok = false;
    AgentError error = AgentError::Config;
    std::string message;   // 成功时：回答；失败时：错误描述

    static AgentResult success(const std::string& msg) {
        return AgentResult{true, AgentError::Config, msg};
    }
    static AgentResult failure(AgentError e, const std::string& msg) {
        return AgentResult{false, e, msg};
    }
};

class LLMClient {
public:
    explicit LLMClient(const AgentConfig& config);

    struct ChatResponse {
        struct Choice {
            ChatMessage message;
            std::string finishReason;
        };
        std::vector<Choice> choices;
        std::string apiError; // 非空表示 API 层错误
    };

    // 回调风格：成功时 ok=true 且 chat 填充
    void chat(const std::vector<ChatMessage>& messages,
              const std::vector<ToolDefinition>& tools,
              const std::string& toolChoice,
              std::function<void(bool ok, ChatResponse response, std::string error)> completion);

    // SSE 流式请求。delta 回调在数据到达时立即触发（工作线程），
    // completion 在流结束后触发一次，携带完整累积的 ChatMessage。
    void chatStream(const std::vector<ChatMessage>& messages,
                    const std::vector<ToolDefinition>& tools,
                    const std::string& toolChoice,
                    std::function<void(const std::string& contentDelta)> onContentDelta,
                    std::function<void(const std::string& reasoningDelta)> onReasoningDelta,
                    std::function<void(bool ok, ChatResponse response, std::string error)> completion);

    // 运行时切换模型（/model 命令用）
    void setModel(const std::string& model) { config_.model = model; }
    const std::string& getModel() const { return config_.model; }

private:
    AgentConfig config_;
    std::string endpoint_;
    std::string post(const std::string& body,
                     const std::vector<std::pair<std::string, std::string>>& headers,
                     std::string& responseBody,
                     std::string& error) const; // 返回 HTTP 状态码字符串
    std::string buildRequestBody(const std::vector<ChatMessage>& messages,
                                 const std::vector<ToolDefinition>& tools,
                                 const std::string& toolChoice,
                                 bool stream = false) const;

    // 通用 HTTP 请求：chunk 回调在每次读取时触发，返回 false 可提前中止
    std::string httpRequestStream(
        const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::function<void(int statusCode, const std::string& contentType)>& onHeaders,
        const std::function<bool(const char* data, size_t len)>& onChunk,
        std::string& error,
        std::string* retryAfter = nullptr) const;

    // 解析完整（非流式）chat 响应 JSON
    bool parseFullChatResponse(const std::string& responseBody, const std::string& status,
                               ChatResponse& out, std::string& error) const;
};

#endif /* LLMClient_h */
