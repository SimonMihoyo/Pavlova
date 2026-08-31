//
//  TuiApp.h
//  pavlova
//
//  Created by SimonMihoyo on 2026/8/22.
//

#ifndef TuiApp_h
#define TuiApp_h

#pragma once

#include <string>
#include <functional>

// 基于 ncurses 的极简 TUI：
//   上方：可滚动对话日志区（PgUp/PgDn 翻页）
//   倒数第二行：状态栏（含 spinner）
//   底部：输入行
// 线程模型：log()/setBusy()/setStatus() 可从任意线程调用（内部加锁投递），
//           渲染只在 run() 主循环内进行，符合 ncurses 单线程约束。
class TuiApp {
public:
    TuiApp();
    ~TuiApp();

    // 用户在输入行按下回车时回调（主循环线程内调用）
    void setOnSubmit(std::function<void(const std::string&)> handler);

    // 以下均可线程安全调用
    void log(const std::string& role, const std::string& text); // role: user/agent/tool/error/info
    void appendStream(const std::string& role, const std::string& text); // 增量流式文本
    void endStream();                                                   // 结束当前流
    void setStatus(const std::string& text);
    void setBusy(bool busy);
    void requestExit();

    bool init();     // 初始化 ncurses；失败（非 tty 等）返回 false
    void run();      // 阻塞主循环，直到 requestExit 或用户 exit/quit
    void shutdown(); // 恢复终端

private:
    struct Impl;
    Impl* impl_;

    std::function<void(const std::string&)> onSubmit_;
};

#endif /* TuiApp_h */
