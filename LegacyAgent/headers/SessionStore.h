//
//  SessionStore.h
//  LegacyAgent
//
//  Created by SimonMihoyo on 2026/8/22.
//

#ifndef SessionStore_h
#define SessionStore_h

#pragma once

#include <string>
#include <vector>
#include "ChatMessage.h"
#include "JSONValue.h"

// 消息 <-> JSON 编解码（LLMClient 与 SessionStore 共用）
JSONValue chatMessageToJSON(const ChatMessage& m);
ChatMessage chatMessageFromJSON(const JSONValue& v);

class SessionStore {
public:
    // ~/.agentapp/sessions/，会话按文件 <时间戳>.json 存储
    static std::string directory();
    static std::string newSessionId();

    // 把 messages 写入会话文件（先写临时文件再 rename，避免中途崩溃留半文件）
    static bool save(const std::string& sessionId,
                     const std::vector<ChatMessage>& messages,
                     std::string& error);
    static bool load(const std::string& sessionId,
                     std::vector<ChatMessage>& messages,
                     std::string& error);

    // 列出已保存会话 id，按时间倒序（新在前）
    static std::vector<std::string> list();

private:
    static std::string pathFor(const std::string& sessionId);
};

#endif /* SessionStore_h */
