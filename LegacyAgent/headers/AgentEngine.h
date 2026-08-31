//
//  AgentEngine.h
//  LegacyAgent
//
//  Created by SimonMihoyo on 2026/8/22.
//

#ifndef AgentEngine_h
#define AgentEngine_h

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "LLMClient.h"
#include "Tool.h"
#include "ChatMessage.h"

class AgentEngine {
public:
    AgentEngine(LLMClient& client, std::vector<std::shared_ptr<Tool>> tools, int maxIterations = 10);

    void start(const std::string& systemPrompt);
    void ask(const std::string& text, std::function<void(const AgentResult&)> completion);

    // 持久化支持
    const std::vector<ChatMessage>& messages() const { return messages_; }
    // 恢复历史：载入已有消息后继续对话（需要消息里已含 system 消息）
    void restore(const std::vector<ChatMessage>& history) { messages_ = history; }

    // 进度通知（工作线程回调，使用者需自行保证线程安全）
    void setProgress(std::function<void(const std::string&)> cb) { progress_ = std::move(cb); }

    // 工具调用即将执行时回调（参数：本次所有 tool calls）
    void setOnToolCalls(std::function<void(const std::vector<ChatMessage::ToolCall>&)> cb) { onToolCalls_ = std::move(cb); }

    // 单个工具执行完毕后回调（参数：工具名, 结果摘要）
    void setOnToolResult(std::function<void(const std::string& name, const std::string& summary)> cb) { onToolResult_ = std::move(cb); }

    // LLM 思考过程回调（DeepSeek-R1 reasoning_content）
    void setOnReasoning(std::function<void(const std::string& text)> cb) { onReasoning_ = std::move(cb); }

    // 流式增量回调
    void setOnContentDelta(std::function<void(const std::string& delta)> cb) { onContentDelta_ = std::move(cb); }
    void setOnReasoningDelta(std::function<void(const std::string& delta)> cb) { onReasoningDelta_ = std::move(cb); }
    void setOnStreamEnd(std::function<void()> cb) { onStreamEnd_ = std::move(cb); }

    // 斜杠命令支持
    void clearMessages();  // 清空对话（保留 system prompt）
    std::vector<std::string> getToolNames() const;
    size_t messageCount() const { return messages_.size(); }
    void compact(std::function<void(bool ok, const std::string& msg)> completion);  // 压缩上下文

private:
    LLMClient& client_;
    std::vector<std::shared_ptr<Tool>> tools_;
    int maxIterations_;
    std::vector<ChatMessage> messages_;
    std::vector<ToolDefinition> toolDefs_;
    std::function<void(const std::string&)> progress_;
    std::function<void(const std::vector<ChatMessage::ToolCall>&)> onToolCalls_;
    std::function<void(const std::string&, const std::string&)> onToolResult_;
    std::function<void(const std::string&)> onReasoning_;
    std::function<void(const std::string&)> onContentDelta_;
    std::function<void(const std::string&)> onReasoningDelta_;
    std::function<void()> onStreamEnd_;

    void iterate(int round, std::function<void(const AgentResult&)> completion);
    void executeCalls(const std::vector<ChatMessage::ToolCall>& calls,
                      std::function<void(std::vector<ChatMessage>)> completion);
};

#endif /* AgentEngine_h */
