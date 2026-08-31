//
//  SessionStore.cpp
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//

#include "headers/SessionStore.h"

#include <fstream>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>

// ---- 消息编解码 ----

JSONValue chatMessageToJSON(const ChatMessage& m) {
    JSONObject obj;
    obj["role"] = JSONValue(m.role);
    if (m.hasContent) {
        obj["content"] = JSONValue(m.content);
    } else if (!m.toolCalls.empty()) {
        obj["content"] = JSONValue();
    }
    if (!m.reasoningContent.empty()) {
        obj["reasoning_content"] = JSONValue(m.reasoningContent);
    }
    if (!m.toolCalls.empty()) {
        JSONArray calls;
        for (const auto& tc : m.toolCalls) {
            JSONObject call;
            call["id"] = JSONValue(tc.id);
            call["type"] = JSONValue(tc.type);
            JSONObject func;
            func["name"] = JSONValue(tc.function.name);
            func["arguments"] = JSONValue(tc.function.arguments);
            call["function"] = JSONValue(func);
            calls.push_back(JSONValue(call));
        }
        obj["tool_calls"] = JSONValue(calls);
    }
    if (!m.toolCallId.empty()) obj["tool_call_id"] = JSONValue(m.toolCallId);
    if (!m.name.empty()) obj["name"] = JSONValue(m.name);
    return JSONValue(obj);
}

ChatMessage chatMessageFromJSON(const JSONValue& v) {
    ChatMessage m;
    const JSONObject* obj = v.asObject();
    if (!obj) return m;
    auto it = obj->find("role");
    if (it != obj->end() && it->second.asString()) m.role = *it->second.asString();
    it = obj->find("content");
    if (it != obj->end() && it->second.asString()) {
        m.content = *it->second.asString();
        m.hasContent = true;
    }
    it = obj->find("reasoning_content");
    if (it != obj->end() && it->second.asString()) {
        m.reasoningContent = *it->second.asString();
    }
    it = obj->find("tool_call_id");
    if (it != obj->end() && it->second.asString()) m.toolCallId = *it->second.asString();
    it = obj->find("name");
    if (it != obj->end() && it->second.asString()) m.name = *it->second.asString();

    it = obj->find("tool_calls");
    if (it != obj->end()) {
        const JSONArray* arr = it->second.asArray();
        if (arr) {
            for (const auto& cv : *arr) {
                const JSONObject* callObj = cv.asObject();
                if (!callObj) continue;
                ChatMessage::ToolCall tc;
                auto cit = callObj->find("id");
                if (cit != callObj->end() && cit->second.asString()) tc.id = *cit->second.asString();
                cit = callObj->find("type");
                if (cit != callObj->end() && cit->second.asString()) tc.type = *cit->second.asString();
                cit = callObj->find("function");
                if (cit != callObj->end()) {
                    const JSONObject* fobj = cit->second.asObject();
                    if (fobj) {
                        auto fit = fobj->find("name");
                        if (fit != fobj->end() && fit->second.asString()) tc.function.name = *fit->second.asString();
                        fit = fobj->find("arguments");
                        if (fit != fobj->end() && fit->second.asString()) tc.function.arguments = *fit->second.asString();
                    }
                }
                m.toolCalls.push_back(tc);
            }
        }
    }
    return m;
}

// ---- 会话存取 ----

std::string SessionStore::directory() {
    const char* home = std::getenv("HOME");
    std::string base = home ? home : "/tmp";
    return base + "/.agentapp/sessions/";
}

std::string SessionStore::pathFor(const std::string& sessionId) {
    return directory() + sessionId + ".json";
}

std::string SessionStore::newSessionId() {
    std::time_t now = std::time(nullptr);
    std::tm tmv;
    localtime_r(&now, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
    std::string id(buf);
    // 同秒冲突时追加序号
    int suffix = 1;
    std::string candidate = id;
    FILE* f = fopen(pathFor(candidate).c_str(), "r");
    while (f) {
        fclose(f);
        candidate = id + "-" + std::to_string(suffix++);
        f = fopen(pathFor(candidate).c_str(), "r");
    }
    return candidate;
}

bool SessionStore::save(const std::string& sessionId,
                        const std::vector<ChatMessage>& messages,
                        std::string& error) {
    std::string dir = directory();
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        if (mkdir((dir.substr(0, dir.size() - 1)).c_str(), 0700) != 0 && errno != EEXIST) {
            // 父目录也可能不存在，逐级尝试
            error = "无法创建会话目录 " + dir;
            return false;
        }
    }

    JSONObject root;
    JSONArray arr;
    for (const auto& m : messages) {
        arr.push_back(chatMessageToJSON(m));
    }
    root["messages"] = JSONValue(arr);
    std::string content = JSON::serialize(JSONValue(root));

    std::string tmpPath = pathFor(sessionId) + ".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary);
        if (!f) {
            error = "无法写入临时会话文件 " + tmpPath;
            return false;
        }
        f << content;
        f.flush();
        f.close();
    }
    if (std::rename(tmpPath.c_str(), pathFor(sessionId).c_str()) != 0) {
        error = "无法重命名会话文件";
        std::remove(tmpPath.c_str());
        return false;
    }
    return true;
}

bool SessionStore::load(const std::string& sessionId,
                        std::vector<ChatMessage>& messages,
                        std::string& error) {
    std::ifstream file(pathFor(sessionId), std::ios::binary);
    if (!file) {
        error = "找不到会话 " + sessionId;
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();

    JSONValue parsed;
    if (!JSON::parse(ss.str(), parsed, error)) {
        error = "解析会话 " + sessionId + " 失败：" + error;
        return false;
    }
    const JSONObject* root = parsed.asObject();
    if (!root) {
        error = "会话 " + sessionId + " 格式错误";
        return false;
    }
    auto it = root->find("messages");
    const JSONArray* arr = it == root->end() ? nullptr : it->second.asArray();
    if (!arr) {
        error = "会话 " + sessionId + " 缺少 messages";
        return false;
    }
    messages.clear();
    for (const auto& mv : *arr) {
        messages.push_back(chatMessageFromJSON(mv));
    }
    return true;
}

std::vector<std::string> SessionStore::list() {
    std::vector<std::string> ids;
    DIR* dir = opendir(directory().c_str());
    if (!dir) return ids;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
            ids.push_back(name.substr(0, name.size() - 5));
        }
    }
    closedir(dir);
    std::sort(ids.begin(), ids.end(), std::greater<std::string>());
    return ids;
}
