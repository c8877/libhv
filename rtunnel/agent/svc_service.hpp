#pragma once
// systemd 服务管理：列表 / 日志 / 启停等动作。
// 所有 systemctl、journalctl 调用都在后台线程执行，结果以 JSON 回调返回，
// 回调线程 = 工作线程，调用方负责切回事件循环。

#include "cpputil/json.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace rtvs {

struct UnitStat {
    int total = 0;
    int running = 0;
    int failed = 0;
};

class Service {
public:
    using Callback = std::function<void(const nlohmann::json&)>;

    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    static bool systemdAvailable();

    void setEnabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    void setWritable(bool v) { writable_.store(v, std::memory_order_relaxed); }
    bool writable() const { return writable_.load(std::memory_order_relaxed); }
    void setToken(const std::string& t) { token_ = t; }
    bool tokenRequired() const { return !token_.empty(); }
    bool checkToken(const std::string& t) const { return t == token_; }
    bool ready() const { return enabled_.load(std::memory_order_relaxed) && systemd_; }
    bool systemd() const { return systemd_; }

    // 在后台线程执行，cb 也在后台线程调用
    void listAsync(Callback cb);
    void logsAsync(const std::string& unit, int lines, Callback cb);
    void actionAsync(const std::string& unit, const std::string& verb, Callback cb);

private:
    void post(std::function<void()> fn);
    void workerLoop();

    std::atomic<bool> enabled_{true};
    std::atomic<bool> writable_{false};
    bool systemd_ = false;
    std::string token_;

    // 列表缓存：systemctl show 在单核设备上约 1s，5s 轮询不能每次都跑
    nlohmann::json list_cache_;
    long long list_cache_ms_ = 0;
    std::map<std::string, std::string> file_states_;
    long long file_states_ms_ = 0;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::thread th_;
    bool stop_ = false;
};

// 服务名是否合法（只允许 systemd 单元名字符，且必须是 .service）
bool validUnitName(const std::string& unit);
bool validVerb(const std::string& verb);

} // namespace rtvs
