// 进程内事件总线：relay 与管理面把「发生了什么」发布进来，SSE 连接按 seq 游标增量读取。
//
// 为什么不用推送：SSE 连接分散在多个 hv worker 线程上，跨线程写 hio 不安全；
// 每个连接在自己的 loop 上起一个 1s 定时器拉取增量，写 hio 只发生在该 loop 线程。
// 1s 粒度对 UI 和 AI 都够用，也天然支持 Last-Event-ID 断点续传（seq 就是事件 id）。
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct EventEntry {
    long long   seq = 0;
    long long   ts_ms = 0;
    std::string type;   // device.online / device.offline / stream.open / audit / task.failed ...
    std::string data;   // JSON 对象文本
};

class EventBus {
public:
    // 只保留最近 kMax 条：SSE 客户端断太久就从头开始，并标记 lost
    static const size_t kMax = 2000;

    // 返回新事件的 seq
    long long publish(const std::string& type, const std::string& data_json);

    // 取 seq > cursor 的事件；lost=true 表示 cursor 已过期（被环形缓冲挤掉）
    long long drain(long long cursor, std::vector<EventEntry>& out, bool& lost);

    long long head();

private:
    std::mutex            mu_;
    std::deque<EventEntry> buf_;
    long long             seq_ = 0;
};
