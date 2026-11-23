#pragma once
// systemd 服务管理：列表 / 日志 / 启停等动作。
// 所有 systemctl、journalctl 调用都在后台线程执行，结果以 JSON 回调返回，
// 回调线程 = 工作线程，调用方负责切回事件循环。

#include "cpputil/json.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rtvs {

struct UnitStat {
    int total = 0;
    int running = 0;
    int failed = 0;
};

struct ListOptions {
    int page = 1;
    int page_size = 10;
    std::string sort;   // name|status|start|mem|uptime，空=按状态
    int dir = 1;        // 1 升序，-1 降序
    std::string q;      // 服务名/描述
    std::string tab;    // all|running|failed|stopped
};

class Service {
public:
    using Callback = std::function<void(const nlohmann::json&)>;

    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    static bool systemdAvailable();

    // 阻塞任务交给 AgentApp 的统一 worker：high=true 用于列表/启停，
    // false 用于 unit-file 清单这类不着急的后台补充。
    using Executor = std::function<bool(std::function<void()>, bool)>;
    void setExecutor(Executor ex) { executor_ = std::move(ex); }

    void setEnabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    void setWritable(bool v) { writable_.store(v, std::memory_order_relaxed); }
    bool writable() const { return writable_.load(std::memory_order_relaxed); }
    void setToken(const std::string& t) { token_ = t; }
    bool tokenRequired() const { return !token_.empty(); }
    bool checkToken(const std::string& t) const { return t == token_; }
    bool ready() const { return enabled_.load(std::memory_order_relaxed) && systemd_; }
    bool systemd() const { return systemd_; }

    // 在后台线程执行，cb 也在后台线程调用
    void listAsync(const ListOptions& opt, Callback cb);
    void logsAsync(const std::string& unit, int lines, Callback cb);
    void actionAsync(const std::string& unit, const std::string& verb, Callback cb);

private:
    void post(std::function<void()> fn);
    void postBg(std::function<void()> fn);

    std::atomic<bool> enabled_{true};
    std::atomic<bool> writable_{false};
    bool systemd_ = false;
    std::string token_;
    Executor executor_;

    // 列表缓存：systemctl show 在单核设备上约 1s，60s 内不重复扫
    nlohmann::json list_cache_;
    long long list_cache_ms_ = 0;
    uint64_t list_files_ver_ = 0;   // 建缓存时用的 unit-file 版本

    // unit-file 清单（disabled/masked 等未加载单元）约 2.8s，放低优先级后台任务刷新。
    // 任务只持有 shared_ptr，不引用 Service，进程退出时也不会悬空。
    struct UnitFiles {
        std::mutex mu;
        std::map<std::string, std::string> files;
        long long ms = 0;
        uint64_t gen = 0;      // 启停/启禁用后递增，丢弃旧代刷新结果
        uint64_t ver = 0;      // 数据版本，每次写入清单递增
        bool refreshing = false;
    };
    std::shared_ptr<UnitFiles> unit_files_ = std::make_shared<UnitFiles>();
    void invalidateUnitFiles();
    static void unitFilesRefresh(std::shared_ptr<UnitFiles> uf, uint64_t gen);
};

// 服务名是否合法（只允许 systemd 单元名字符，且必须是 .service）
bool validUnitName(const std::string& unit);
bool validVerb(const std::string& verb);

} // namespace rtvs
