// 告警：设备离线/恢复、证书到期、分发任务失败 → webhook。
// 采集与发送都在 alertLoop 这一个受管 worker 里，不阻塞 relay 与 HTTP 线程。
#include "admin_server.hpp"

#include "admin_tls.hpp"
#include "common/logging.hpp"
#include "common/sha256.hpp"
#include "cpputil/json.hpp"

#include "util/base64.h"
#include "http/client/HttpClient.h"

#include <ctype.h>
#include <set>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>

using nlohmann::json;

namespace {

// ---------------------------------------------------------------- 加密/编码

// HMAC-SHA256（复用 rtunnel 自带的 sha256 实现，无 OpenSSL 依赖）
void hmacSha256(const std::string& key, const std::string& msg, uint8_t out[32]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key.size() > sizeof(k)) {
        rtutil::sha256(key.data(), key.size(), k);
    } else if (!key.empty()) {
        memcpy(k, key.data(), key.size());
    }
    uint8_t ipad[64], opad[64];
    for (size_t i = 0; i < sizeof(k); ++i) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    uint8_t inner[32];
    std::string in((const char*)ipad, sizeof(ipad));
    in += msg;
    rtutil::sha256(in.data(), in.size(), inner);

    std::string outer((const char*)opad, sizeof(opad));
    outer.append((const char*)inner, sizeof(inner));
    rtutil::sha256(outer.data(), outer.size(), out);
}

std::string hmacHex(const std::string& key, const std::string& msg) {
    uint8_t mac[32];
    hmacSha256(key, msg, mac);
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (size_t i = 0; i < sizeof(mac); ++i) {
        s += hex[mac[i] >> 4];
        s += hex[mac[i] & 0x0f];
    }
    return s;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        }
    }
    return out;
}

std::string isoTime(long long ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

long long wallMs() { return (long long)time(NULL) * 1000; }

std::string relayName() {
    char host[128] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0 || !host[0]) return "rtunnel-relay";
    return host;
}

// 与 api_settings.cpp 的脱敏一致：只露 scheme+主机+末 4 位
std::string maskUrlForStatus(const std::string& url) {
    if (url.empty()) return "";
    size_t scheme = url.find("://");
    size_t host_start = scheme == std::string::npos ? 0 : scheme + 3;
    size_t slash = url.find('/', host_start);
    std::string head = url.substr(0, slash == std::string::npos ? url.size() : slash);
    std::string tail = url.size() > 4 ? url.substr(url.size() - 4) : url;
    return head + "/…" + tail;
}

} // namespace

// ---------------------------------------------------------------- 采集

void AdminServer::alertEnqueue(const AlertEvent& ev) {
    std::lock_guard<std::mutex> lk(alert_mu_);
    if (alert_q_.size() >= 256) alert_q_.pop_front();   // 队列有界，避免内存涨
    alert_q_.push_back(ev);
}

void AdminServer::notifyTaskFailed(long long id, const std::string& devid,
                                   const std::string& status, const std::string& err) {
    if (!settingInt("alert.task_fail", 1)) return;
    AlertEvent ev;
    ev.key = "task:" + std::to_string(id) + ":" + status;
    ev.event = "task.failed";
    ev.level = (status == "timeout") ? "warning" : "critical";
    ev.title = "分发任务" + status;
    ev.text = "任务 #" + std::to_string(id) + " → " + devid + " " + status;
    if (!err.empty()) ev.text += "：" + err;
    json d;
    d["task_id"] = id;
    d["devid"] = devid;
    d["status"] = status;
    d["error"] = err;
    ev.details = d.dump();
    alertEnqueue(ev);
}

// 设备在线表：每次快照更新；消失超过阈值发一次离线，恢复时发一次上线
void AdminServer::alertCheckDevices(long long now_ms) {
    if (!settingInt("alert.device_offline", 1)) return;
    int after_s = settingInt("alert.offline_after_s", 120);
    if (after_s < 1) after_s = 1;

    std::string snap = callRelay([this]() {
        json arr = json::array();
        for (RelayConn* c = relay_->agents; c; c = c->reg_next) {
            if (!c->authed) continue;
            arr.push_back({{"devid", c->devid}, {"last_seen_ms", c->last_seen_ms}});
        }
        return arr.dump();
    });
    auto arr = json::parse(snap, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return;

    std::set<std::string> online;
    for (auto& j : arr) {
        std::string devid = j.value("devid", "");
        if (!devid.empty()) online.insert(devid);
    }

    std::vector<AlertEvent> out;
    {
        std::lock_guard<std::mutex> lk(alert_mu_);
        for (auto& d : online) {
            auto it = alert_agent_online_.find(d);
            bool was_offline = (it != alert_agent_online_.end() && it->second == 0);
            alert_agent_seen_[d] = now_ms;
            alert_agent_online_[d] = 1;
            if (was_offline) {
                AlertEvent ev;
                ev.key = "device.online:" + d;
                ev.event = "device.online";
                ev.level = "info";
                ev.title = "设备恢复在线";
                ev.text = "设备 " + d + " 已重新连接 relay";
                json det;
                det["devid"] = d;
                ev.details = det.dump();
                out.push_back(ev);
            }
        }
        for (auto& kv : alert_agent_online_) {
            if (kv.second != 1) continue;
            if (online.count(kv.first)) continue;
            long long seen = alert_agent_seen_.count(kv.first) ? alert_agent_seen_[kv.first] : 0;
            if (now_ms - seen < (long long)after_s * 1000) continue;
            kv.second = 0;
            AlertEvent ev;
            ev.key = "device.offline:" + kv.first;
            ev.event = "device.offline";
            ev.level = "warning";
            ev.title = "设备离线";
            ev.text = "设备 " + kv.first + " 已离线超过 " + std::to_string(after_s) + " 秒";
            json det;
            det["devid"] = kv.first;
            det["offline_ms"] = now_ms - seen;
            ev.details = det.dump();
            out.push_back(ev);
        }
        // 首次采集只建基线，不对“启动前就离线”的设备刷屏
        bool baseline = !alert_baseline_done_;
        alert_baseline_done_ = true;
        if (baseline) out.clear();
    }
    for (auto& e : out) alertEnqueue(e);
}

void AdminServer::alertCheckCerts(long long now_ms) {
    int warn = settingInt("alert.cert_warn_days", 30);
    int crit = settingInt("alert.cert_crit_days", 7);
    if (warn < 1) warn = 1;
    if (crit < 1) crit = 1;
    if (crit > warn) crit = warn;

    std::vector<AlertEvent> out;

    struct Item {
        std::string san;
        std::string role;
        long long expire_ms = 0;
    };
    std::vector<Item> items;

    if (!cfg_.cert_dir.empty()) {
        FILE* fp = fopen((cfg_.cert_dir + "/index.txt").c_str(), "r");
        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                char san[128] = {0}, role[32] = {0};
                unsigned long long serial = 0, issue = 0, expire = 0;
                if (sscanf(line, "%127[^|]|%31[^|]|%llu|%llu|%llu",
                           san, role, &serial, &issue, &expire) != 5) {
                    continue;
                }
                Item it;
                it.san = san;
                it.role = role;
                it.expire_ms = (long long)expire;
                items.push_back(it);
            }
            fclose(fp);
        }
    }
    if (!admin_cert_path_.empty()) {
        int days = 0;
        if (admin_tls_cert_days_left(admin_cert_path_, days)) {
            Item it;
            it.san = "admin-console";
            it.role = "admin";
            it.expire_ms = now_ms + (long long)days * 86400 * 1000;
            items.push_back(it);
        }
    }

    {
        std::lock_guard<std::mutex> lk(alert_mu_);
        for (auto& it : items) {
            if (it.expire_ms <= 0) continue;
            long long left_ms = it.expire_ms - now_ms;
            long long left_days = left_ms / 86400000;
            int level = left_days <= crit ? 2 : (left_days <= warn ? 1 : 0);
            if (level == 0) continue;
            std::string key = "cert:" + it.san + ":" + std::to_string(it.expire_ms) + ":" +
                              std::to_string(level);
            if (alert_cert_level_[key]) continue;
            alert_cert_level_[key] = 1;
            AlertEvent ev;
            ev.key = key;
            ev.event = "cert.expiring";
            ev.level = level == 2 ? "critical" : "warning";
            ev.title = level == 2 ? "证书即将过期（严重）" : "证书即将过期";
            ev.text = "证书 " + it.san + "（" + it.role + "）剩余 " +
                      std::to_string(left_days) + " 天";
            json det;
            det["san"] = it.san;
            det["role"] = it.role;
            det["expire_ms"] = it.expire_ms;
            det["days_left"] = left_days;
            ev.details = det.dump();
            ev.cooldown_s = 86400;
            out.push_back(ev);
        }
        // 保留最近 4096 条去重记录，防止无限增长
        while (alert_cert_level_.size() > 4096) alert_cert_level_.erase(alert_cert_level_.begin());
    }
    for (auto& e : out) alertEnqueue(e);
}

// ---------------------------------------------------------------- 发送

bool AdminServer::alertSend(const AlertEvent& ev) {
    if (!settingInt("alert.enabled", 0)) return false;
    std::string url = settingStr("alert.webhook_url", "");
    if (url.empty()) return false;

    int cooldown = ev.cooldown_s > 0 ? ev.cooldown_s : settingInt("alert.cooldown_s", 300);
    long long now = wallMs();
    {
        std::lock_guard<std::mutex> lk(alert_mu_);
        auto it = alert_last_sent_.find(ev.key);
        if (it != alert_last_sent_.end() && now - it->second < (long long)cooldown * 1000) return false;
        alert_last_sent_[ev.key] = now;
        alert_last_try_ms_ = now;
    }

    std::string kind = settingStr("alert.kind", "generic");
    std::string secret = settingStr("alert.webhook_secret", "");
    std::string relay = relayName();
    std::string body;
    std::string sign_header, sign_value;

    std::string content = "### " + ev.title + "\n> 级别: " + ev.level + " · 时间: " +
                          isoTime(now) + " · relay: " + relay + "\n\n" + ev.text;

    if (kind == "wecom") {
        json j;
        j["msgtype"] = "markdown";
        j["markdown"]["content"] = content;
        body = j.dump();
    } else if (kind == "dingtalk") {
        json j;
        j["msgtype"] = "markdown";
        j["markdown"]["title"] = ev.title;
        j["markdown"]["text"] = content;
        body = j.dump();
        if (!secret.empty()) {
            std::string ts = std::to_string(now);
            uint8_t mac[32];
            std::string msg = ts + "\n" + secret;
            hmacSha256(secret, msg, mac);
            std::string sign = hv::Base64Encode(mac, sizeof(mac));
            url += (url.find('?') == std::string::npos ? "?" : "&");
            url += "timestamp=" + ts + "&sign=" + urlEncode(sign);
        }
    } else if (kind == "feishu") {
        json j;
        j["msg_type"] = "text";
        j["content"]["text"] = ev.title + "\n" + ev.text + "\n(" + isoTime(now) + ")";
        body = j.dump();
    } else {
        json j;
        j["event"] = ev.event;
        j["level"] = ev.level;
        j["ts"] = now;
        j["relay"] = relay;
        j["title"] = ev.title;
        j["text"] = ev.text;
        auto det = json::parse(ev.details, nullptr, false);
        j["details"] = det.is_discarded() ? json::object() : det;
        body = j.dump();
        if (!secret.empty()) {
            sign_header = "X-Rtunnel-Signature";
            sign_value = "sha256=" + hmacHex(secret, body);
        }
    }

    hv::HttpClient cli;
    cli.setTimeout(3);
    HttpRequest req;
    req.method = HTTP_POST;
    req.url = url;
    req.headers["Content-Type"] = "application/json";
    if (!sign_header.empty()) req.headers[sign_header] = sign_value;
    req.body = body;
    HttpResponse resp;
    int rc = cli.send(&req, &resp);
    bool ok = (rc == 0 && resp.status_code >= 200 && resp.status_code < 300);

    std::lock_guard<std::mutex> lk(alert_mu_);
    if (ok) {
        alert_last_ok_ms_ = now;
        alert_sent_total_++;
        alert_last_error_.clear();
        logf_("[alert] sent %s (%s) -> %d", ev.event.c_str(), ev.title.c_str(), resp.status_code);
    } else {
        alert_last_error_ = rc != 0 ? ("http rc=" + std::to_string(rc))
                                    : ("http status " + std::to_string(resp.status_code));
        logf_("[alert] send failed: %s (%s)", ev.event.c_str(), alert_last_error_.c_str());
    }
    return ok;
}

void AdminServer::alertLoop() {
    long long last_check = 0;
    long long last_cert_check = 0;
    while (!waitStop(500)) {
        long long now = wallMs();
        bool enabled = settingInt("alert.enabled", 0) != 0;
        if (enabled && now - last_check >= 5000) {
            last_check = now;
            alertCheckDevices(now);
            if (now - last_cert_check >= 60000) {
                last_cert_check = now;
                alertCheckCerts(now);
            }
        }
        std::vector<AlertEvent> batch;
        {
            std::lock_guard<std::mutex> lk(alert_mu_);
            while (!alert_q_.empty() && batch.size() < 10) {
                batch.push_back(alert_q_.front());
                alert_q_.pop_front();
            }
        }
        for (auto& ev : batch) {
            if (waitStop(0)) return;
            alertSend(ev);
        }
    }
}

// ---------------------------------------------------------------- API

std::string AdminServer::alertStatusJson() {
    std::string url = settingStr("alert.webhook_url", "");
    json j;
    j["enabled"] = settingInt("alert.enabled", 0);
    j["kind"] = settingStr("alert.kind", "generic");
    j["url"] = url.empty() ? "" : maskUrlForStatus(url);
    {
        std::lock_guard<std::mutex> lk(alert_mu_);
        j["last_try_ms"] = alert_last_try_ms_;
        j["last_ok_ms"] = alert_last_ok_ms_;
        j["last_error"] = alert_last_error_;
        j["sent_total"] = alert_sent_total_;
        j["queue"] = (long long)alert_q_.size();
        int online = 0, offline = 0;
        for (auto& kv : alert_agent_online_) (kv.second ? online : offline)++;
        j["devices_online"] = online;
        j["devices_offline"] = offline;
    }
    return j.dump();
}

void AdminServer::handleAlertStatus(const HttpContextPtr& ctx) {
    ctx->send(alertStatusJson());
}

void AdminServer::handleAlertTest(const HttpContextPtr& ctx) {
    if (settingInt("alert.enabled", 0) == 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"alert disabled\"}");
        return;
    }
    AlertEvent ev;
    ev.key = "test:" + std::to_string(wallMs());
    ev.event = "test";
    ev.level = "info";
    ev.title = "rtunnel 告警测试";
    ev.text = "收到这条消息说明 webhook 配置可用";
    ev.details = "{\"source\":\"admin\"}";
    ev.cooldown_s = 1;
    alertEnqueue(ev);
    audit(currentActor().username, "alerts.test", "");
    ctx->send("{\"ok\":true,\"queued\":true}");
}
