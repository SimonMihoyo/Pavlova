//
//  WebFetchTool.cpp
//  LegacyAgent
//
//  Created by SimonMihoyo on 2026/8/22.
//  Copyright © 2026 SimonMihoyo. All rights reserved.
//

#include "headers/WebFetchTool.h"
#include "headers/ToolDefinition.h"

#include <thread>
#include <array>
#include <cstdio>
#include <cstring>

// 简单的 CURL 封装，用于在子线程中执行
static std::string fetch_url(const std::string& url_str) {
    // 使用 curl 命令行工具作为后端，避免直接链接 libcurl 的复杂性
    // 在实际生产环境中，建议直接使用 libcurl C API
    std::string cmd = "curl -s --max-time 30 \"" + url_str + "\"";
    std::array<char, 4096> buffer;
    std::string result;
    
    // 使用 popen 执行命令并读取输出
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "错误：无法启动 curl";

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    
    int exitCode = pclose(pipe);
    if (exitCode != 0) {
        return "错误：请求失败 (curl exit code " + std::to_string(exitCode) + ")";
    }
    return result;
}

WebFetchTool::WebFetchTool() {
    // 构建 parameters JSON
    JSONObject properties;
    properties["url"] = JSONValue(JSONObject{
        {"type", JSONValue("string")},
        {"description", JSONValue("要抓取的完整 URL")}
    });

    definition = ToolDefinition::makeFunction(
        "web_fetch",
        "抓取指定 URL 的网页内容并返回文本。",
        JSONValue(JSONObject{
            {"type", JSONValue("object")},
            {"properties", JSONValue(properties)},
            {"required", JSONValue(JSONArray{JSONValue("url")})}
        })
    );
}

const ToolDefinition& WebFetchTool::getDefinition() const {
    return definition;
}

void WebFetchTool::call(const std::map<std::string, JSONValue>& arguments, std::function<void(std::string)> completion) {
    // 1. 参数校验
    auto it = arguments.find("url");
    if (it == arguments.end() || !std::holds_alternative<std::string>(it->second.value)) {
        completion("错误：缺少有效的 url 参数");
        return;
    }
    std::string url = std::get<std::string>(it->second.value);

    // 2. 异步执行 (对应 Swift 的 URLSession)
    std::thread([url, completion]() {
        std::string result = fetch_url(url);
        // 注意：在实际 App 中，如果 completion 需要更新 UI，需要 dispatch 回主线程
        // 这里假设 completion 是纯逻辑处理
        completion(result);
    }).detach(); // detach 类似于后台任务，不需要 join
}
