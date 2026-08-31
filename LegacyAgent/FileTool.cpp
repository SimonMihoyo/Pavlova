//
//  FileTool.cpp
//  LegacyAgent
//
//  Created by SimonMihoyo on 2026/8/22.
//  Copyright © 2026 SimonMihoyo. All rights reserved.
//

#include <stdio.h>
#include "headers/FileTool.h"
#include "headers/ToolDefinition.h"
#include <fstream>
#include <dirent.h>
#include <sys/stat.h>

FileTool::FileTool() {
    JSONArray ops;
    ops.push_back(JSONValue("read"));
    ops.push_back(JSONValue("write"));
    ops.push_back(JSONValue("append"));
    ops.push_back(JSONValue("list"));

    JSONObject properties;
    properties["operation"] = JSONValue(JSONObject{
        {"type", JSONValue("string")},
        {"enum", JSONValue(ops)},
        {"description", JSONValue("操作类型：read 读取、write 写入、append 追加、list 列目录")}
    });
    properties["path"] = JSONValue(JSONObject{
        {"type", JSONValue("string")},
        {"description", JSONValue("文件或目录路径")}
    });
    properties["content"] = JSONValue(JSONObject{
        {"type", JSONValue("string")},
        {"description", JSONValue("写入或追加的内容")}
    });

    definition = ToolDefinition::makeFunction(
        "file",
        "读写文件：读取文件内容、写入文件、追加内容、列出目录。",
        JSONValue(JSONObject{
            {"type", JSONValue("object")},
            {"properties", JSONValue(properties)},
            {"required", JSONValue(JSONArray{JSONValue("operation"), JSONValue("path")})}
        })
    );
}

const ToolDefinition& FileTool::getDefinition() const {
    return definition;
}

void FileTool::call(const std::map<std::string, JSONValue>& arguments, std::function<void(std::string)> completion) {
    // 获取参数
    auto get_str = [&](const std::string& key) -> std::string {
        auto it = arguments.find(key);
        if (it != arguments.end() && std::holds_alternative<std::string>(it->second.value)) {
            return std::get<std::string>(it->second.value);
        }
        return "";
    };

    std::string op = get_str("operation");
    std::string path = get_str("path");
    std::string content = get_str("content");

    if (op.empty()) { completion("错误：缺少 operation 参数"); return; }
    if (path.empty()) { completion("错误：缺少 path 参数"); return; }

    if (op == "read") {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            completion("错误：无法读取文件 " + path);
            return;
        }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        completion(content);
    }
    else if (op == "write") {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            completion("错误：无法写入文件 " + path);
            return;
        }
        file << content;
        completion("已写入 " + path);
    }
    else if (op == "append") {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        if (!file) {
            completion("错误：无法追加到文件 " + path);
            return;
        }
        file << content;
        completion("已追加到 " + path);
    }
    else if (op == "list") {
        DIR *dir;
        struct dirent *ent;
        std::string result;
        if ((dir = opendir(path.c_str())) != nullptr) {
            while ((ent = readdir(dir)) != nullptr) {
                std::string name = ent->d_name;
                if (name != "." && name != "..") {
                    result += name + "\n";
                }
            }
            closedir(dir);
            if (!result.empty()) result.pop_back(); // 移除最后一个换行符
            completion(result);
        } else {
            completion("错误：无法列出目录 " + path);
        }
    }
    else {
        completion("错误：未知操作 " + op);
    }
}
