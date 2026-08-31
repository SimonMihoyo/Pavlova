//
//  AgentEngine.cpp
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//

#include "headers/AgentEngine.h"

#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <condition_variable>

AgentEngine::AgentEngine(LLMClient& client,
                         std::vector<std::shared_ptr<Tool>> tools,
                         int maxIterations)
    : client_(client), tools_(std::move(tools)), maxIterations_(maxIterations) {
    for (const auto& t : tools_) {
        toolDefs_.push_back(t->getDefinition());
    }
}

void AgentEngine::start(const std::string& systemPrompt) {
    messages_.clear();
    messages_.push_back(ChatMessage::system(systemPrompt));
}

void AgentEngine::ask(const std::string& text, std::function<void(const AgentResult&)> completion) {
    messages_.push_back(ChatMessage::user(text));
    iterate(0, completion);
}

void AgentEngine::iterate(int round, std::function<void(const AgentResult&)> completion) {
    if (round > maxIterations_) {
        completion(AgentResult::failure(AgentError::MaxIterations, "达到最大迭代次数，已停止"));
        return;
    }

    if (progress_) progress_("正在请求模型…");

    client_.chatStream(messages_, toolDefs_, "",
        // onContentDelta
        [this](const std::string& delta) {
            if (onContentDelta_) onContentDelta_(delta);
        },
        // onReasoningDelta
        [this](const std::string& delta) {
            if (onReasoningDelta_) onReasoningDelta_(delta);
        },
        // completion
        [this, round, completion](bool ok, LLMClient::ChatResponse response, std::string error) {
            if (onStreamEnd_) onStreamEnd_();

            if (!ok) {
                completion(AgentResult::failure(AgentError::HTTP, error));
                return;
            }
            if (response.choices.empty()) {
                completion(AgentResult::failure(AgentError::EmptyResponse, "模型没有返回内容"));
                return;
            }

            ChatMessage message = response.choices[0].message;
            messages_.push_back(message);

            if (message.toolCalls.empty()) {
                completion(AgentResult::success(message.content));
                return;
            }

            if (onToolCalls_) onToolCalls_(message.toolCalls);

            executeCalls(message.toolCalls, [this, round, completion](std::vector<ChatMessage> results) {
                messages_.insert(messages_.end(), results.begin(), results.end());
                iterate(round + 1, completion);
            });
        });
}

void AgentEngine::executeCalls(const std::vector<ChatMessage::ToolCall>& calls,
                               std::function<void(std::vector<ChatMessage>)> completion) {
    std::vector<ChatMessage> results(calls.size());
    std::atomic<int> pending{ static_cast<int>(calls.size()) };
    std::mutex mtx;
    std::condition_variable cv;

    for (size_t i = 0; i < calls.size(); i++) {
        const auto& call = calls[i];
        std::string toolName = call.function.name;

        auto toolIt = std::find_if(tools_.begin(), tools_.end(),
                                   [&](const std::shared_ptr<Tool>& t) {
                                       return t->getDefinition().function.name == toolName;
                                   });

        if (toolIt == tools_.end()) {
            results[i] = ChatMessage::toolResult(call.id, toolName, "错误：未知工具 " + toolName);
            if (--pending == 0) cv.notify_all();
            continue;
        }

        if (progress_) progress_("执行工具: " + toolName);

        std::map<std::string, JSONValue> args;
        std::string parseErr;
        JSONValue parsed;
        if (JSON::parse(call.function.arguments, parsed, parseErr)) {
            const JSONObject* obj = parsed.asObject();
            if (obj) args = *obj;
        }

        auto tool = *toolIt;
        tool->call(args, [i, &results, &pending, &cv, &mtx, call, toolName, this](std::string output) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                results[i] = ChatMessage::toolResult(call.id, toolName, output);
            }
            if (onToolResult_) {
                std::string summary = output.size() > 200 ? output.substr(0, 200) + "…" : output;
                onToolResult_(toolName, summary);
            }
            if (--pending == 0) cv.notify_all();
        });
    }

    // 等待所有工具完成
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&] { return pending == 0; });
    lock.unlock();

    completion(std::move(results));
}

void AgentEngine::clearMessages() {
    // 保留 system prompt（第一条消息），清空其余
    if (!messages_.empty() && messages_[0].role == "system") {
        ChatMessage system = messages_[0];
        messages_.clear();
        messages_.push_back(system);
    } else {
        messages_.clear();
    }
}

std::vector<std::string> AgentEngine::getToolNames() const {
    std::vector<std::string> names;
    for (const auto& def : toolDefs_) {
        names.push_back(def.function.name);
    }
    return names;
}

void AgentEngine::compact(std::function<void(bool ok, const std::string& msg)> completion) {
    // 简单实现：保留 system prompt + 最近 10 条消息
    // TODO: 未来可以用 LLM 生成摘要替代旧消息
    if (messages_.size() <= 11) {
        completion(false, "消息数量较少，无需压缩");
        return;
    }

    // 保留 system prompt
    ChatMessage system;
    bool hasSystem = false;
    if (!messages_.empty() && messages_[0].role == "system") {
        system = messages_[0];
        hasSystem = true;
    }

    // 保留最后 10 条
    std::vector<ChatMessage> recent(messages_.end() - 10, messages_.end());
    messages_.clear();

    if (hasSystem) {
        messages_.push_back(system);
    }
    for (auto& m : recent) {
        messages_.push_back(std::move(m));
    }

    completion(true, "已压缩对话，保留最近 10 条消息");
}
