# Pavlova

## Introduction

Pavlova is an AI agent app for macOS 10.14+.  
For some reason, I have to use older systems, so I wrote this — a native, lightweight terminal agent with zero Electron/Node dependencies.

## Features

- **TUI Mode** — Full-screen ncurses interface with streaming display
- **Interactive Mode** — Simple REPL for quick conversations
- **One-shot Mode** — Ask a single question and exit
- **Tool Calling** — Shell commands, file read/write, web fetching
- **SSE Streaming** — Token-by-token display, including DeepSeek-R1 reasoning
- **Session Persistence** — Save and resume conversations
- **API Retry** — Automatic retry on 429/5xx with exponential backoff
- **Slash Commands** — `/help`, `/clear`, `/model`, `/compact`, etc.

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j
```

Binary: `build/agentapp`

### Requirements

- macOS 10.14+
- CMake 3.14+
- C++17 compiler
- ncurses (system-provided)

## Usage

```bash
# TUI mode (recommended)
./build/agentapp -t

# Interactive mode
./build/agentapp -i

# One-shot question
./build/agentapp -q "What time is it?"

# Resume a session
./build/agentapp -t -r <session-id>

# List saved sessions
./build/agentapp -r
```

## Configuration

Create `~/.agentapp/config.json`:

```json
{
  "baseURL": "https://api.openai.com/v1",
  "apiKey": "sk-...",
  "model": "gpt-4o",
  "systemPrompt": "You are a helpful assistant."
}
```

Compatible with any OpenAI-style API (OpenAI, DeepSeek, Ollama, etc.).

## TUI Controls

| Key | Action |
|-----|--------|
| `↑` / `↓` | Scroll line by line |
| `PgUp` / `PgDn` | Page up/down |
| `Home` / `End` | Jump to top/bottom |
| Mouse wheel | Scroll |
| `Ctrl-B` / `Ctrl-F` | Page up/down (less style) |

## Slash Commands (TUI only)

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/clear` | Clear conversation history |
| `/tools` | List available tools |
| `/session` | Show session ID and message count |
| `/model` | Show current model |
| `/model <name>` | Switch to a different model |
| `/compact` | Compress conversation (keep last 10 messages) |
| `/exit` | Quit |

## Architecture

```
pavlova/
├── main.cpp              Entry point, CLI parsing, TUI/line mode wiring
├── AgentEngine.cpp       Agent loop: LLM call → tool execution → iterate
├── LLMClient.cpp         HTTP client (CFNetwork), SSE streaming, auto-retry
├── TuiApp.cpp            ncurses TUI: pad scrolling, incremental rendering
├── SessionStore.cpp      Session persistence (JSON files)
├── ShellTool.cpp         Tool: execute shell commands
├── FileTool.cpp          Tool: read/write files
├── WebFetchTool.cpp      Tool: fetch web content
├── JSONValue.cpp         Lightweight JSON parser/serializer
└── headers/              Header files
```

### Threading Model

- **Main thread**: TUI rendering (ncurses requires single-threaded access)
- **Worker thread**: HTTP requests run in `std::thread::detach()`
- **Callback direction**: Worker → `TuiApp::log()`/`appendStream()` → mutex-protected queue → main loop renders

## License

MIT