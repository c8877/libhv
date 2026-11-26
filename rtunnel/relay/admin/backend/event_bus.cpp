#include "event_bus.hpp"

#include <chrono>

namespace {

long long nowMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

long long EventBus::publish(const std::string& type, const std::string& data_json) {
    std::lock_guard<std::mutex> lk(mu_);
    EventEntry e;
    e.seq = ++seq_;
    e.ts_ms = nowMs();
    e.type = type;
    e.data = data_json;
    buf_.push_back(std::move(e));
    while (buf_.size() > kMax) buf_.pop_front();
    return seq_;
}

long long EventBus::drain(long long cursor, std::vector<EventEntry>& out, bool& lost) {
    std::lock_guard<std::mutex> lk(mu_);
    lost = false;
    if (buf_.empty()) return cursor;
    long long oldest = buf_.front().seq;
    long long latest = buf_.back().seq;
    if (cursor > latest) return latest;   // 时钟/游标异常：直接对齐，别回放
    if (cursor < oldest - 1) {
        // 游标被挤掉了：从头给，并标记，客户端知道自己漏了事件
        lost = true;
        cursor = oldest - 1;
    }
    for (const auto& e : buf_) {
        if (e.seq > cursor) out.push_back(e);
    }
    return latest;
}

long long EventBus::head() {
    std::lock_guard<std::mutex> lk(mu_);
    return seq_;
}
