# pavlova

macOS 终端 AI Agent，C++17 编写。支持 TUI（ncurses 全屏）和行模式交互。

## 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j
```

产物：`build/agentapp`

## 运行

```bash
# TUI 模式
./build/agentapp -t

# 单次问答
./build/agentapp -q "你好"

# 交互模式
./build/agentapp -i

# 恢复会话
./build/agentapp -t -r <session-id>
```

配置文件：`~/.agentapp/config.json`

```json
{
  "baseURL": "https://api.openai.com/v1",
  "apiKey": "sk-...",
  "model": "gpt-4o"
}
```

## 架构

```
pavlova/
├── main.cpp              入口，CLI 解析，TUI/行模式接线
├── AgentEngine.cpp       Agent 循环：LLM 调用 → 工具执行 → 迭代
├── LLMClient.cpp         HTTP 客户端（CFNetwork），SSE 流式解析，自动重试
├── TuiApp.cpp            ncurses TUI：pad 滚动，流式增量渲染
├── SessionStore.cpp      会话持久化（JSON 文件）
├── ShellTool.cpp         工具：执行 shell 命令
├── FileTool.cpp          工具：读写文件
├── WebFetchTool.cpp      工具：抓取网页
├── JSONValue.cpp         轻量 JSON 解析/序列化
└── headers/              对应头文件
```

### 线程模型

- **主线程**：TUI 渲染（ncurses 要求单线程）
- **工作线程**：`LLMClient` 的 HTTP 请求在 `std::thread::detach()` 中执行
- **回调方向**：工作线程 → `TuiApp::log()`/`appendStream()` 投递到 mutex 保护的队列 → 主循环消费渲染

### 关键约定

- 所有 ncurses 操作只在主循环线程内执行
- `TuiApp::log()`、`appendStream()`、`setStatus()`、`setBusy()` 可线程安全调用
- SSE 流式回调（`onContentDelta`/`onReasoningDelta`）在工作线程触发，通过 TuiApp 队列转到主线程
- `chatStream()` 的 `deltaSent` 标记：一旦 delta 回调触发，不再重试（避免 TUI 重复显示）
- 斜杠命令（`/help`、`/clear` 等）仅在 TUI 模式有效，在 `main.cpp` 的 `onSubmit` 中解析

### API 重试

- 429、500、502、503、504 自动重试，最多 3 次
- 指数退避 + 随机抖动，尊重 Retry-After 头
- 400、401、403 等客户端错误不重试
- 流式请求在已发送 delta 后不再重试

## TUI 斜杠命令

| 命令 | 说明 |
|------|------|
| `/help` | 显示帮助 |
| `/clear` | 清空对话历史 |
| `/tools` | 列出可用工具 |
| `/session` | 显示会话 ID 和消息数 |
| `/model [名称]` | 查看或切换模型 |
| `/compact` | 压缩对话（保留最近 10 条） |
| `/exit` | 退出 |

## 依赖

- macOS（使用 CFNetwork / CoreFoundation）
- ncurses（系统自带）
- CMake 3.14+
- C++17
