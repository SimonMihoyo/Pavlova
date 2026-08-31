//
//  main.cpp
//  LegacyAgent
//
//  Created by SimonMihoyo on 2026/8/22.
//

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "headers/LLMClient.h"
#include "headers/AgentEngine.h"
#include "headers/ShellTool.h"
#include "headers/FileTool.h"
#include "headers/WebFetchTool.h"
#include "headers/SessionStore.h"
#include "headers/TuiApp.h"

namespace {

const std::string kDefaultConfigPath = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.agentapp/config.json";

AgentConfig loadConfig(const std::string& path, std::string& error) {
    std::ifstream file(path);
    AgentConfig config;
    if (!file) {
        error = "找不到配置文件 " + path + "，请先创建：\n{\n  \"baseURL\": \"https://api.openai.com/v1\",\n  \"apiKey\": \"sk-...\",\n  \"model\": \"gpt-4o\"\n}";
        return config;
    }
    std::stringstream ss;
    ss << file.rdbuf();

    JSONValue parsed;
    if (!JSON::parse(ss.str(), parsed, error)) {
        error = "解析 " + path + " 失败：" + error;
        return config;
    }
    const JSONObject* obj = parsed.asObject();
    if (!obj) {
        error = "解析 " + path + " 失败：配置不是 JSON 对象";
        return config;
    }
    auto get = [&](const std::string& key) -> std::string {
        auto it = obj->find(key);
        if (it != obj->end() && it->second.asString()) return *it->second.asString();
        return "";
    };
    config.baseURL = get("baseURL");
    config.apiKey = get("apiKey");
    config.model = get("model");
    config.systemPrompt = get("systemPrompt");
    return config;
}

void printHelp() {
    std::cout <<
        "用法: agentapp [选项]\n"
        "\n"
        "选项:\n"
        "  -q <问题>            单次问答，输出结果后退出\n"
        "  -i, --interactive    交互模式（默认）\n"
        "  -m <模型>            覆盖配置中的模型\n"
        "  -c <路径>            指定配置文件（默认 ~/.agentapp/config.json）\n"
        "  --no-tools           禁用工具调用\n"
        "  -t, --tui            TUI 模式（ncurses 全屏界面）\n"
        "  -r                   列出已保存会话\n"
        "  -r <会话ID>          恢复指定会话继续对话\n"
        "  -h, --help           显示帮助\n";
}

std::string defaultSystemPrompt() {
    return "你是一个运行在 macOS 终端里的命令行 Agent。你可以使用工具完成用户的任务。\n"
           "需要执行命令、读写文件或抓取网页时，先调用对应工具，再根据结果回答。\n"
           "回答保持简洁、直接。";
}

} // namespace

int main(int argc, const char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);

    std::string query;
    std::string configPath;
    std::string modelOverride;
    std::string resumeArg;   // 非空表示 -r 且带会话ID
    bool resumeListOnly = false; // -r 无值：仅列出会话
    bool interactive = false;
    bool noTools = false;
    bool useTui = false;
    bool help = false;

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 < args.size()) return args[++i];
            return "";
        };
        auto isFlag = [&](const std::string& s) {
            return !s.empty() && s.front() == '-';
        };
        if (arg == "-q") {
            query = next();
        } else if (arg == "-i" || arg == "--interactive") {
            interactive = true;
        } else if (arg == "-m") {
            modelOverride = next();
        } else if (arg == "-c") {
            configPath = next();
        } else if (arg == "--no-tools") {
            noTools = true;
        } else if (arg == "-t" || arg == "--tui") {
            useTui = true;
        } else if (arg == "-r" || arg == "--resume") {
            if (i + 1 < args.size() && !isFlag(args[i + 1])) {
                resumeArg = args[++i];
            } else {
                resumeListOnly = true;
            }
        } else if (arg == "-h" || arg == "--help") {
            help = true;
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "未知参数: " << arg << std::endl;
            help = true;
        } else {
            if (query.empty()) query = arg;
            else { std::cerr << "多余参数: " << arg << std::endl; help = true; }
        }
    }

    if (help) {
        printHelp();
        return 0;
    }

    // 仅列出会话
    if (resumeListOnly) {
        auto sessions = SessionStore::list();
        if (sessions.empty()) {
            std::cout << "没有已保存的会话。" << std::endl;
        } else {
            std::cout << "已保存会话：" << std::endl;
            for (const auto& s : sessions) {
                std::cout << "  " << s << std::endl;
            }
        }
        return 0;
    }

    std::string error;
    AgentConfig config = loadConfig(configPath.empty() ? kDefaultConfigPath : configPath, error);
    if (!error.empty()) {
        std::cerr << "错误: " << error << std::endl;
        return 1;
    }
    if (!modelOverride.empty()) {
        config.model = modelOverride;
    }

    LLMClient client(config);

    std::vector<std::shared_ptr<Tool>> tools;
    if (!noTools) {
        tools.push_back(std::make_shared<ShellTool>());
        tools.push_back(std::make_shared<FileTool>());
        tools.push_back(std::make_shared<WebFetchTool>());
    }

    AgentEngine engine(client, tools);
    engine.setProgress([&](const std::string& msg) {
        // TUI 模式下由 tui 显示进度；行模式忽略
        (void)msg;
    });

    // 会话ID与初始化：恢复或新建
    std::string sessionId;
    if (resumeArg.empty()) {
        engine.start(config.systemPrompt.empty() ? defaultSystemPrompt() : config.systemPrompt);
        sessionId = SessionStore::newSessionId();
    } else {
        std::vector<ChatMessage> history;
        if (!SessionStore::load(resumeArg, history, error)) {
            std::cerr << "错误: " << error << std::endl;
            return 1;
        }
        engine.restore(history);
        sessionId = resumeArg;
        std::cout << "已恢复会话 " << sessionId << "（" << history.size() << " 条消息），可继续对话。" << std::endl;
    }

    auto saveSession = [&]() {
        std::string serr;
        if (!SessionStore::save(sessionId, engine.messages(), serr)) {
            std::cerr << "警告: 保存会话失败: " << serr << std::endl;
        }
    };

    // TUI 模式
    if (useTui) {
        TuiApp tui;
        if (!tui.init()) {
            std::cerr << "错误: TUI 初始化失败（需要真实终端）" << std::endl;
            return 1;
        }
        if (!resumeArg.empty()) {
            // 把历史消息渲染进对话区
            for (const auto& m : engine.messages()) {
                if (m.role == "user") {
                    tui.log("user", m.content);
                } else if (m.role == "assistant") {
                    if (m.hasContent && !m.content.empty()) {
                        tui.log("agent", m.content);
                    } else {
                        for (const auto& tc : m.toolCalls) {
                            tui.log("tool", "调用 " + tc.function.name + " " + tc.function.arguments);
                        }
                    }
                } else if (m.role == "tool") {
                    std::string c = m.content;
                    if (c.size() > 200) c = c.substr(0, 200) + "…";
                    tui.log("info", "工具结果: " + c);
                }
            }
            size_t n = engine.messages().size();
            tui.log("info", "已恢复会话 " + sessionId + "（" + std::to_string(n) + " 条消息）");
        } else {
            tui.log("info", "新会话 " + sessionId);
        }
        tui.log("info", "输入问题开始对话，/help 查看命令，exit/quit 退出。滚动：↑/↓ 行滚、PgUp/PgDn 翻页、Home/End 跳顶底、鼠标滚轮。");

        engine.setOnToolCalls([&](const std::vector<ChatMessage::ToolCall>& calls) {
            for (const auto& tc : calls) {
                std::string args = tc.function.arguments;
                if (args.size() > 120) args = args.substr(0, 120) + "…";
                tui.log("tool", "调用 " + tc.function.name + " " + args);
            }
        });
        engine.setOnToolResult([&](const std::string& name, const std::string& summary) {
            tui.log("info", "✓ " + name + " 完成");
        });
        engine.setOnContentDelta([&](const std::string& delta) {
            tui.appendStream("agent", delta);
        });
        engine.setOnReasoningDelta([&](const std::string& delta) {
            tui.appendStream("thinking", delta);
        });
        engine.setOnStreamEnd([&]() {
            tui.endStream();
        });

        tui.setOnSubmit([&](const std::string& text) {
            // 斜杠命令处理
            if (!text.empty() && text[0] == '/') {
                std::string cmd = text;
                std::string arg;
                size_t sp = cmd.find(' ');
                if (sp != std::string::npos) {
                    arg = cmd.substr(sp + 1);
                    cmd = cmd.substr(0, sp);
                }

                if (cmd == "/help") {
                    tui.log("info", "可用命令：");
                    tui.log("info", "  /help              显示此帮助");
                    tui.log("info", "  /clear             清空对话历史");
                    tui.log("info", "  /tools             列出可用工具");
                    tui.log("info", "  /session           显示当前会话信息");
                    tui.log("info", "  /model [名称]      查看或切换模型");
                    tui.log("info", "  /compact           压缩对话（保留最近消息）");
                    tui.log("info", "  /exit              退出程序");
                } else if (cmd == "/clear") {
                    engine.clearMessages();
                    tui.log("info", "对话已清空");
                } else if (cmd == "/tools") {
                    auto names = engine.getToolNames();
                    if (names.empty()) {
                        tui.log("info", "当前没有可用工具");
                    } else {
                        std::string list = "可用工具：" + names[0];
                        for (size_t i = 1; i < names.size(); i++) {
                            list += ", " + names[i];
                        }
                        tui.log("info", list);
                    }
                } else if (cmd == "/session") {
                    tui.log("info", "会话 ID: " + sessionId);
                    tui.log("info", "消息数量: " + std::to_string(engine.messageCount()));
                } else if (cmd == "/model") {
                    if (arg.empty()) {
                        tui.log("info", "当前模型: " + client.getModel());
                    } else {
                        client.setModel(arg);
                        tui.log("info", "已切换模型: " + arg);
                    }
                } else if (cmd == "/compact") {
                    engine.compact([&](bool ok, const std::string& msg) {
                        tui.log(ok ? "info" : "error", msg);
                    });
                } else if (cmd == "/exit" || cmd == "/quit") {
                    tui.requestExit();
                } else {
                    tui.log("error", "未知命令: " + cmd + "（输入 /help 查看帮助）");
                }
                return;
            }

            // 普通提问
            tui.setBusy(true);
            tui.setStatus("思考中");
            engine.ask(text, [&](const AgentResult& result) {
                if (!result.ok) {
                    tui.log("error", result.message);
                }
                saveSession();
                tui.setBusy(false);
                tui.setStatus("");
            });
        });

        tui.run();
        saveSession();
        tui.shutdown();
        return 0;
    }

    // 单次问答
    if (!query.empty()) {
        std::atomic<bool> done(false);
        engine.ask(query, [&](const AgentResult& result) {
            if (result.ok) {
                std::cout << "Agent: " << result.message << std::endl;
            } else {
                std::cerr << "错误: " << result.message << std::endl;
            }
            saveSession();
            done = true;
        });
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::cout << "会话已保存: " << sessionId << std::endl;
        return 0;
    }

    // 交互模式
    std::cout << "进入交互模式，输入问题，Ctrl-D 退出。" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::string trimmed = line;
        size_t b = trimmed.find_first_not_of(" \t\r\n");
        size_t e = trimmed.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        trimmed = trimmed.substr(b, e - b + 1);

        if (trimmed == "exit" || trimmed == "quit") break;

        std::atomic<bool> done(false);
        engine.ask(trimmed, [&](const AgentResult& result) {
            if (result.ok) {
                std::cout << "Agent: " << result.message << std::endl;
            } else {
                std::cerr << "错误: " << result.message << std::endl;
            }
            saveSession();
            done = true;
        });
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    std::cout << "会话已保存: " << sessionId << std::endl;

    return 0;
}
