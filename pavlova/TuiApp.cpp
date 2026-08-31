//
//  TuiApp.cpp
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//

#include "headers/TuiApp.h"

#include <ncurses.h>
#include <signal.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <locale.h>

#include <deque>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>

namespace {

constexpr int COLOR_USER    = 1; // 青
constexpr int COLOR_AGENT   = 2; // 绿
constexpr int COLOR_TOOL    = 3; // 黄
constexpr int COLOR_ERROR   = 4; // 红
constexpr int COLOR_INFO    = 5; // 灰
constexpr int COLOR_THINKING = 6; // 品红（思考过程）

short colorForRole(const std::string& role, std::string& prefix) {
    if (role == "user")    { prefix = "\xe4\xbd\xa0> "; return COLOR_USER; }   // 你>
    if (role == "agent")   { prefix = "Agent> "; return COLOR_AGENT; }
    if (role == "thinking") { prefix = "\xf0\x9f\x92\xad "; return COLOR_THINKING; } // 💭
    if (role == "tool")    { prefix = "[\xe5\xb7\xa5\xe5\x85\xb7] "; return COLOR_TOOL; } // [工具]
    if (role == "error")   { prefix = "[\xe9\x94\x99\xe8\xaf\xaf] "; return COLOR_ERROR; } // [错误]
    prefix = "- "; return COLOR_INFO;
}

// 从字符串尾部删除一个 UTF-8 codepoint
void eraseLastCodepoint(std::string& s) {
    while (!s.empty()) {
        unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) break; // 删到 lead byte 为止
    }
}

void splitLines(const std::string& text, std::vector<std::string>& out) {
    std::string cur;
    for (char c : text) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
}

// 估算 UTF-8 字符串的终端显示宽度（列数）：
// 1 字节序列 = 窄字符(1 列)；2 字节 = 拉丁扩展等(1 列)；3/4 字节 = CJK、emoji 等(2 列)
int displayWidth(const std::string& s) {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { w += 1; i += 1; }
        else if ((c & 0xE0) == 0xC0) { w += 1; i += 2; }
        else if ((c & 0xF0) == 0xE0) { w += 2; i += 3; }
        else if ((c & 0xF8) == 0xF0) { w += 2; i += 4; }
        else { i += 1; }
    }
    return w;
}

TuiApp* g_activeTui = nullptr;

void sigintHandler(int) {
    if (g_activeTui) {
        endwin(); // 尽力恢复终端
    }
    _exit(130);
}

} // namespace

struct TuiApp::Impl {
    struct Line {
        short color;
        std::string text;
    };

    struct StreamDelta {
        std::string role;
        std::string text;
    };

    // 待渲染队列（多线程 -> 主循环）
    std::mutex mtx;
    std::deque<Line> pending;

    // 流式增量队列（多线程 -> 主循环）
    std::deque<StreamDelta> streamQueue;
    bool streamEndPending = false;

    // 流处理状态（仅主循环线程访问）
    bool streamActive = false;
    std::string streamRole;
    short streamColor = 0;
    std::string streamPrefix;
    std::string streamBuffer;   // 当前流累积的未完成文本
    Line openLine;              // 当前正在流式写入的行（尚未 finalize 到 lines[]）
    bool hasOpenLine = false;

    std::vector<Line> lines;   // 全部逻辑行
    WINDOW* pad = nullptr;
    int padRows = 0;
    int lastCursorY = 0;       // pad 内最后写入位置
    bool padDirty = false;

    std::string input;
    std::string status;
    std::atomic<bool> busy{false};
    std::atomic<bool> quit{false};

    bool follow = true;        // 贴底跟随
    int scrollPos = 0;
    bool needFullRedraw = false;

    ~Impl() {
        if (pad) delwin(pad);
    }

    // ---- 滚动操作（调用方需持有 mtx）----
    int viewportHeight() const { return LINES - 2; }

    void enterScrollModeIfNeeded() {
        if (follow) {
            follow = false;
            scrollPos = lastCursorY - viewportHeight();
            if (scrollPos < 0) scrollPos = 0;
        }
    }

    void scrollBy(int lines) {
        if (lines < 0) {
            enterScrollModeIfNeeded();
        } else if (follow) {
            return; // 贴底状态下向下滚无意义
        }
        scrollPos += lines;
        if (scrollPos < 0) scrollPos = 0;
    }

    void pageUp()   { scrollBy(-(viewportHeight() - 1)); }
    void pageDown() { scrollBy(viewportHeight() - 1); }
    void scrollToTop()  { follow = false; scrollPos = 0; }
    void jumpToBottom() { follow = true; }

    void appendToPad(WINDOW* p) {
        for (const auto& ln : lines) {
            wattron(p, COLOR_PAIR(ln.color));
            waddstr(p, ln.text.c_str());
            wattroff(p, COLOR_PAIR(ln.color));
            waddch(p, '\n');
        }
        if (hasOpenLine) {
            wattron(p, COLOR_PAIR(openLine.color));
            waddstr(p, openLine.text.c_str());
            wattroff(p, COLOR_PAIR(openLine.color));
        }
        lastCursorY = getcury(p);
    }

    void rebuildPad(int width) {
        int w = width > 0 ? width : 80;
        int need = 64;
        for (const auto& ln : lines) {
            need += static_cast<int>(ln.text.size()) + 1;
        }
        if (hasOpenLine) {
            need += static_cast<int>(openLine.text.size()) + 1;
        }
        int exact = 1;
        for (const auto& ln : lines) {
            int dw = displayWidth(ln.text);
            exact += (dw / w) + 1;
        }
        if (hasOpenLine) {
            exact += (displayWidth(openLine.text) / w) + 1;
        }
        need = std::max(need, exact);
        if (pad) delwin(pad);
        pad = newpad(need, w);
        padRows = need;
        lastCursorY = 0;
        werase(pad);
        scrollok(pad, FALSE);
        appendToPad(pad);
    }

    // 处理流式队列（主循环线程内调用，调用方持有 mtx）
    void processStreamQueue() {
        bool changed = false;

        while (!streamQueue.empty()) {
            auto delta = std::move(streamQueue.front());
            streamQueue.pop_front();
            changed = true;

            std::string prefix;
            short color = colorForRole(delta.role, prefix);

            // role 切换：finalize 当前 open line
            if (!streamActive || delta.role != streamRole) {
                finalizeOpenLine();
                streamActive = true;
                streamRole = delta.role;
                streamColor = color;
                streamPrefix = prefix;
                streamBuffer.clear();
            }

            streamBuffer += delta.text;

            // 按 \n 分割：完成行 → lines[]，残余 → openLine
            size_t pos;
            while ((pos = streamBuffer.find('\n')) != std::string::npos) {
                std::string segment = streamBuffer.substr(0, pos);
                streamBuffer.erase(0, pos + 1);

                if (!segment.empty() || !hasOpenLine) {
                    // 如果当前有 openLine，先合并 segment
                    if (hasOpenLine) {
                        openLine.text += segment;
                        finalizeOpenLine();
                    } else {
                        Line ln;
                        ln.color = streamColor;
                        ln.text = streamPrefix + segment;
                        lines.push_back(ln);
                    }
                } else {
                    finalizeOpenLine();
                }
                // 下一行需要新前缀
                streamPrefix = std::string(prefix.size() > 2 ? 2 : prefix.size(), ' ');
            }

            // 残余文本 → openLine
            if (!streamBuffer.empty()) {
                if (!hasOpenLine) {
                    openLine.color = streamColor;
                    openLine.text = streamPrefix + streamBuffer;
                    hasOpenLine = true;
                } else {
                    openLine.text += streamBuffer;
                }
                streamBuffer.clear();
            }
        }

        if (streamEndPending) {
            finalizeOpenLine();
            streamActive = false;
            streamRole.clear();
            streamBuffer.clear();
            streamEndPending = false;
            changed = true;
        }

        if (changed) padDirty = true;
    }

    void finalizeOpenLine() {
        if (hasOpenLine) {
            lines.push_back(openLine);
            hasOpenLine = false;
            openLine.text.clear();
        }
    }
};

TuiApp::TuiApp() : impl_(new Impl()) {}
TuiApp::~TuiApp() { delete impl_; }

void TuiApp::setOnSubmit(std::function<void(const std::string&)> handler) {
    onSubmit_ = std::move(handler);
}

void TuiApp::log(const std::string& role, const std::string& text) {
    std::string prefix;
    short color = colorForRole(role, prefix);
    std::vector<std::string> parts;
    splitLines(text, parts);

    std::lock_guard<std::mutex> lock(impl_->mtx);
    bool first = true;
    for (auto& p : parts) {
        Impl::Line ln;
        ln.color = color;
        ln.text = first ? (prefix + p) : "  " + p; // 续行缩进
        first = false;
        impl_->pending.push_back(ln);
    }
    impl_->padDirty = true;
}

void TuiApp::appendStream(const std::string& role, const std::string& text) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->streamQueue.push_back({role, text});
    impl_->padDirty = true;
}

void TuiApp::endStream() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->streamEndPending = true;
    impl_->padDirty = true;
}

void TuiApp::setStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->status = text;
    impl_->padDirty = true; // 借用 dirty 标记触发重绘
}

void TuiApp::setBusy(bool busy) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->busy = busy;
    impl_->padDirty = true;
}

void TuiApp::requestExit() {
    impl_->quit = true;
}

bool TuiApp::init() {
    const char* term = std::getenv("TERM");
    if (!term || term[0] == '\0' || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return false;
    }
    // 关键：让 ncurses 按 UTF-8 透传字节。C locale 下高位字节会被 unctrl
    // 替换成 "ç~T¨" 之类的乱码（macOS 自带 ncurses 5.4 的行为）。
    const char* loc = setlocale(LC_ALL, "");
    if (!loc || std::strcmp(loc, "C") == 0 || std::strcmp(loc, "POSIX") == 0) {
        // 环境没有 LANG 时依次尝试常见 UTF-8 locale
        if (!setlocale(LC_ALL, "en_US.UTF-8")) {
            setlocale(LC_ALL, "C.UTF-8");
        }
    }
    if (!initscr()) return false;

    g_activeTui = this;
    signal(SIGINT, sigintHandler);

    start_color();
    use_default_colors();
    init_pair(COLOR_USER, COLOR_CYAN, -1);
    init_pair(COLOR_AGENT, COLOR_GREEN, -1);
    init_pair(COLOR_TOOL, COLOR_YELLOW, -1);
    init_pair(COLOR_ERROR, COLOR_RED, -1);
    init_pair(COLOR_INFO, COLOR_WHITE, -1);
    init_pair(COLOR_THINKING, COLOR_MAGENTA, -1);

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    wtimeout(stdscr, 80); // getch 非阻塞轮询周期

    // 启用滚轮上报（Terminal 收到 1000 模式序列后会把滚轮作为事件发给程序）
#ifdef BUTTON5_PRESSED
    mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
#else
    mousemask(BUTTON4_PRESSED, nullptr);
#endif

    impl_->rebuildPad(COLS);
    return true;
}

void TuiApp::run() {
    static const char spinner[] = "|/-\\";
    int spinIdx = 0;
    int lastSpinTick = -1;

    while (!impl_->quit) {
        bool redraw = false;
        bool fullClear = false;

        // 1. 消费日志队列 + 流式队列（增量写入 pad）
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            if (impl_->needFullRedraw) {
                impl_->rebuildPad(COLS);
                impl_->needFullRedraw = false;
                redraw = true;
                fullClear = true; // resize 后强制全屏重绘
            }
            // 流式增量处理
            bool hadStream = !impl_->streamQueue.empty() || impl_->streamEndPending;
            if (hadStream) {
                impl_->processStreamQueue();
            }
            if (!impl_->pending.empty() || hadStream) {
                // 流式更新或 pending 行需要重建 pad
                if (hadStream || impl_->pending.empty()) {
                    // 纯流式更新：直接重建
                    impl_->rebuildPad(COLS);
                    fullClear = true;
                } else {
                    // 有 pending 行：精确计算需要的物理行数
                    int w = COLS > 0 ? COLS : 80;
                    int pendingRows = 0;
                    for (const auto& ln : impl_->pending) {
                        int dw = displayWidth(ln.text);
                        pendingRows += (dw / w) + 2;
                    }
                    if (impl_->lastCursorY + pendingRows >= impl_->padRows) {
                        impl_->rebuildPad(COLS);
                        fullClear = true;
                    }
                    for (const auto& ln : impl_->pending) {
                        wattron(impl_->pad, COLOR_PAIR(ln.color));
                        waddstr(impl_->pad, ln.text.c_str());
                        wattroff(impl_->pad, COLOR_PAIR(ln.color));
                        waddch(impl_->pad, '\n');
                        impl_->lines.push_back(ln);
                    }
                    impl_->lastCursorY = getcury(impl_->pad);
                }
                impl_->pending.clear();
                redraw = true;
            }
        }

        // 2. 键盘输入
        int ch = getch();
        if (ch != ERR) {
            switch (ch) {
                case '\n':
                case '\r':
                case KEY_ENTER: {
                    std::string text;
                    {
                        std::lock_guard<std::mutex> lock(impl_->mtx);
                        if (impl_->busy || impl_->input.empty()) break;
                        text = impl_->input;
                        impl_->input.clear();
                    }
                    // 斜杠命令不作为用户消息显示
                    if (text.empty() || text[0] != '/') {
                        log("user", text);
                    }
                    if (text == "exit" || text == "quit") {
                        impl_->quit = true;
                    } else if (onSubmit_) {
                        onSubmit_(text);
                    }
                    redraw = true;
                    break;
                }
                case KEY_BACKSPACE:
                case 127:
                case 8: {
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    eraseLastCodepoint(impl_->input);
                    redraw = true;
                    break;
                }
                case KEY_PPAGE: {
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    impl_->pageUp();
                    redraw = true;
                    break;
                }
                case KEY_NPAGE: {
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    impl_->pageDown();
                    redraw = true;
                    break;
                }
                case KEY_HOME: {
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    impl_->scrollToTop();
                    redraw = true;
                    break;
                }
                case KEY_END: {
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    impl_->jumpToBottom();
                    redraw = true;
                    break;
                }
                // less 风格整页翻页
                case 2: { // Ctrl-B
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    impl_->pageUp();
                    redraw = true;
                    break;
                }
                case 6: { // Ctrl-F
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    impl_->pageDown();
                    redraw = true;
                    break;
                }
                // 方向键逐行滚动（不用 Ctrl-Y：它是 macOS termios 的 DSUSP
                // 字符，内核会直接 SIGSTOP 冻结进程，根本到不了 getch）
                case KEY_UP: {
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    impl_->scrollBy(-1);
                    redraw = true;
                    break;
                }
                case KEY_DOWN: {
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    impl_->scrollBy(1);
                    redraw = true;
                    break;
                }
                // 鼠标滚轮（一格滚 3 行）
                case KEY_MOUSE: {
                    MEVENT ev;
                    if (getmouse(&ev) == OK) {
                        std::lock_guard<std::mutex> lock(impl_->mtx);
                        bool handled = false;
#ifdef BUTTON5_PRESSED
                        if (ev.bstate & BUTTON4_PRESSED) { impl_->scrollBy(-3); handled = true; }
                        else if (ev.bstate & BUTTON5_PRESSED) { impl_->scrollBy(3); handled = true; }
#else
                        if (ev.bstate & BUTTON4_PRESSED) { impl_->scrollBy(-3); handled = true; }
#endif
                        if (handled) redraw = true;
                    }
                    break;
                }
                case KEY_RESIZE: {
                    std::lock_guard<std::mutex> lock(impl_->mtx);
                    impl_->needFullRedraw = true;
                    redraw = true;
                    break;
                }
                default: {
                    if (ch >= 32 && ch <= 255) {
                        std::lock_guard<std::mutex> lock(impl_->mtx);
                        impl_->input += static_cast<char>(ch);
                        redraw = true;
                    }
                    break;
                }
            }
        }

        // 3. 到底判定
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            int viewH = LINES - 2;
            if (!impl_->follow && impl_->scrollPos + viewH >= impl_->lastCursorY + 1) {
                impl_->follow = true;
                redraw = true;
            }
        }

        // 4. spinner 跳动
        if (impl_->busy) {
            int tick = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() / 120);
            if (tick != lastSpinTick) {
                lastSpinTick = tick;
                spinIdx = (spinIdx + 1) % 4;
                redraw = true;
            }
        }

        if (!redraw) continue;

        if (fullClear) {
            clear(); // touch 全屏，下面的 pad 随后入虚拟屏覆盖对话区
            refresh();
        }

        // 渲染。注意：不能用 erase() 清 stdscr——refresh() 时 stdscr 的全屏空白
        // 会覆盖掉 pnoutrefresh 写入虚拟屏的 pad 内容。这里 pad 先入虚拟屏，
        // 底部两行用 stdscr 做局部更新（mvhline 触碰的行不与 pad 视口重叠）。
        int viewH = LINES - 2;
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            int top = impl_->follow ? (impl_->lastCursorY - viewH + 1) : impl_->scrollPos;
            if (top < 0) top = 0;

            // macOS 自带 ncurses 5.4 为单字节 cell，diff 式局部刷新会从行中间
            // 开始写字节，截断 UTF-8 序列产生乱码。这里强制视口内所有行整行
            // 重画（从行首写完整字节流），规避截断。
            wredrawln(impl_->pad, top, viewH);
            pnoutrefresh(impl_->pad, top, 0, 0, 0, viewH - 1, COLS - 1);

            // 状态栏
            attron(A_REVERSE);
            mvhline(LINES - 2, 0, ' ', COLS);
            if (impl_->busy) {
                mvprintw(LINES - 2, 0, " %c %s", spinner[spinIdx],
                         impl_->status.empty() ? "\xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad" : impl_->status.c_str()); // 思考中
            } else if (!impl_->status.empty()) {
                mvprintw(LINES - 2, 0, " %s", impl_->status.c_str());
            }
            if (!impl_->follow) {
                // 上翻时右侧显示位置指示（纯 ASCII，避免宽度问题）
                char pos[32];
                snprintf(pos, sizeof(pos), "L%d/%d ", top + 1, impl_->lastCursorY);
                mvprintw(LINES - 2, COLS - static_cast<int>(strlen(pos)) - 1, "%s", pos);
            } else if (top > 0) {
                // follow 模式下，内容超出视口上方时提示可上翻
                char hint[32];
                snprintf(hint, sizeof(hint), " ^%d ", top);
                mvprintw(LINES - 2, COLS - static_cast<int>(strlen(hint)) - 1, "%s", hint);
            }
            attroff(A_REVERSE);

            // 输入行
            mvhline(LINES - 1, 0, ' ', COLS);
            mvprintw(LINES - 1, 0, "> %s", impl_->input.c_str());
            move(LINES - 1, 2 + displayWidth(impl_->input));
        }
        refresh();
    }
}

void TuiApp::shutdown() {
    g_activeTui = nullptr;
    signal(SIGINT, SIG_DFL);
    endwin();
}
