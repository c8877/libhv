// 事件队列与分钟级指标采样。
//
// 这两个都在 relay 线程里写（hloop 回调、帧处理），管理面 worker 通过 takeEvents /
// takeMetricSamples 取走——和 takeUsageEvents 一样的模式，避免跨线程共享容器。
#include "relay_server.hpp"

#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <stdio.h>
#include <string.h>
#include <time.h>

using nlohmann::json;

namespace {
const size_t kEventMax = 1000;    // 管理面长期不在时兜底，丢最旧
const size_t kMetricMax = 2000;
const long long kSampleIntervalMs = 60000;   // 一台设备一分钟一条
}  // namespace

void RelayServer::pushEvent(const char* type, const std::string& extra_json) {
    json e;
    e["type"] = type;
    e["ts"] = (long long)time(NULL) * 1000;
    if (!extra_json.empty()) {
        json x = json::parse(extra_json, nullptr, false);
        if (!x.is_discarded() && x.is_object()) {
            for (auto it = x.begin(); it != x.end(); ++it) e[it.key()] = it.value();
        }
    }
    if (event_pending.size() >= kEventMax) {
        event_pending.erase(event_pending.begin());
        ++event_dropped;
    }
    event_pending.push_back(e.dump());
}

std::string RelayServer::takeEvents() {
    json arr = json::array();
    for (auto& line : event_pending) {
        json x = json::parse(line, nullptr, false);
        if (!x.is_discarded()) arr.push_back(std::move(x));
    }
    event_pending.clear();
    if (event_dropped > 0) {
        json d;
        d["dropped"] = (long long)event_dropped;
        event_dropped = 0;
        // 丢弃量单独一条，管理面据此记录告警计数器
        json e;
        e["type"] = "bus.dropped";
        e["ts"] = (long long)time(NULL) * 1000;
        e["count"] = d["dropped"];
        arr.push_back(e);
    }
    return arr.dump();
}

std::string RelayServer::takeMetricSamples() {
    json arr = json::array();
    for (auto& line : metric_pending) {
        json x = json::parse(line, nullptr, false);
        if (!x.is_discarded()) arr.push_back(std::move(x));
    }
    metric_pending.clear();
    metric_dropped = 0;
    return arr.dump();
}

// 从缓存的 sysinfo 里抽一行时序。数据都是 agent 报上来的原始值：
//   cpu 百分比、内存 KB、1 分钟负载、全部网卡的收/发字节合计。
void RelayServer::sampleMetrics(const char* devid) {
    if (devid == NULL || devid[0] == 0) return;
    if (strncmp(devid, "a:", 2) == 0) return;   // 管理台按需拉起的临时 agent 不计
    // 只采集 CLI 白名单里登记过的设备？不：默认全采，靠 retention 控制体积。
    uint64_t now = hloop_now_ms(loop);
    auto sit = metric_sampled_ms.find(devid);
    if (sit != metric_sampled_ms.end() && now - sit->second < (uint64_t)kSampleIntervalMs * 9 / 10) {
        return;   // 60s 内已经采过（留 10% 余量给 timer 抖动）
    }
    auto it = sysinfo_cache.find(devid);
    if (it == sysinfo_cache.end()) return;
    json s = json::parse(it->second.json, nullptr, false);
    if (s.is_discarded() || !s.is_object()) return;

    double cpu = s.value("cpu_usage", -1.0);
    if (cpu < 0) return;   // 还没有有效 CPU 数据，不落半条

    long long mem_total = s.value("mem_total_kb", (long long)0);
    long long mem_avail = s.value("mem_available_kb", (long long)0);
    if (mem_avail <= 0) mem_avail = s.value("mem_free_kb", (long long)0);
    long long mem_used = mem_total > 0 ? mem_total - mem_avail : 0;

    double load1 = 0;
    if (s.contains("loadavg") && s["loadavg"].is_array() && !s["loadavg"].empty() &&
        s["loadavg"][0].is_number()) {
        load1 = s["loadavg"][0].get<double>();
    }

    long long rx = 0, tx = 0;
    if (s.contains("nics") && s["nics"].is_array()) {
        for (auto& n : s["nics"]) {
            if (!n.is_object()) continue;
            rx += n.value("rx_bytes", (long long)0);
            tx += n.value("tx_bytes", (long long)0);
        }
    }

    json row;
    row["ts"] = (long long)time(NULL) * 1000;
    row["devid"] = devid;
    row["cpu"] = cpu;
    row["mem"] = mem_used;
    row["mem_total"] = mem_total;
    row["load1"] = load1;
    row["rx"] = rx;
    row["tx"] = tx;

    if (metric_pending.size() >= kMetricMax) {
        metric_pending.erase(metric_pending.begin());
        ++metric_dropped;
    }
    metric_pending.push_back(row.dump());
    metric_sampled_ms[devid] = now;
}

// 30s 一次：给「距上次采样已满 60s」的在线设备要一份增量 sysinfo。
// 用增量模式（mode=1）：只带 CPU/负载/内存/网卡，比全量省一半以上。
// 设备不回也没关系，下一轮会再要一次。
void RelayServer::metricsTick() {
    char payload = 1;
    for (RelayConn* c = agents; c; c = c->reg_next) {
        if (!c->authed || c->io == NULL || c->devid[0] == 0) continue;
        if (strncmp(c->devid, "a:", 2) == 0) continue;
        uint64_t now = hloop_now_ms(loop);
        auto it = metric_sampled_ms.find(c->devid);
        if (it != metric_sampled_ms.end() &&
            now - it->second < (uint64_t)kSampleIntervalMs) {
            continue;
        }
        rtnp::send(c->io, rtnp::SYSINFO_REQ, 0, &payload, 1);
    }
}
