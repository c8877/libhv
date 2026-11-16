#pragma once
// FS WebSocket 协议状态机。本地 Web 和 relay 隧道共用同一份实现：
// 输入是 WebSocket 的 text/binary 消息，输出通过 Sink 原样发出。
//
// text  : JSON 控制消息（op/id/...）
// binary: 16 字节头 + 数据
//   id(u32 BE) | kind(u8) | flags(u8) | rsv(u16) | offset(u64 BE) | data
//   kind: 1=download, 2=upload

#include "fs_archive.hpp"
#include "fs_service.hpp"
#include "cpputil/json.hpp"

#include <deque>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

namespace rtfs {

class Sink {
public:
    virtual ~Sink() {}
    virtual void send(bool binary, const std::string& msg) = 0;
};

class Session {
public:
    Session(Service* svc, Sink* sink, const std::string& label, bool trusted, bool allow_rw = true);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void onText(const std::string& msg);
    void onBinary(const std::string& msg);
    void onClose();

    // 隧道流量控制；本地 Web 用 setUnlimited(true)
    void setUnlimited(bool v);
    void setCredit(uint32_t c);
    void addCredit(uint32_t n);
    void pump();

    bool authed() const { return authed_; }
    bool writable() const { return rw_; }

    static const size_t MAX_MESSAGE = 30000;
    static const size_t BIN_HEADER = 16;

private:
    void sendJson(const nlohmann::json& j);
    void sendError(uint32_t id, int code, const std::string& text);
    void handleHello(const nlohmann::json& j, uint32_t id);
    void handleList(const nlohmann::json& j, uint32_t id);
    void handleStat(const nlohmann::json& j, uint32_t id);
    void handleSize(const nlohmann::json& j, uint32_t id);
    void handleMkdir(const nlohmann::json& j, uint32_t id);
    void handleTouch(const nlohmann::json& j, uint32_t id);
    void handleRename(const nlohmann::json& j, uint32_t id);
    void handleMove(const nlohmann::json& j, uint32_t id);
    void handleDelete(const nlohmann::json& j, uint32_t id);
    void handleDownloadOpen(const nlohmann::json& j, uint32_t id);
    void handleArchiveOpen(const nlohmann::json& j, uint32_t id);
    void handleDownloadAck(const nlohmann::json& j, uint32_t id);
    void handleDownloadCancel(const nlohmann::json& j, uint32_t id);
    void handleUploadOpen(const nlohmann::json& j, uint32_t id);
    void handleUploadFinish(const nlohmann::json& j, uint32_t id);
    void handleUploadCancel(const nlohmann::json& j, uint32_t id);
    void handleUploadStatus(const nlohmann::json& j, uint32_t id);
    void pumpDownload();
    void cancelDownload();

    Service* svc_ = nullptr;
    Sink* sink_ = nullptr;
    std::string label_;
    bool trusted_ = false;
    bool allow_rw_ = true;

    bool authed_ = false;
    bool rw_ = false;

    bool unlimited_ = false;
    uint32_t credit_ = 0;
    struct Out {
        bool binary;
        std::string data;
    };
    std::deque<Out> out_;

    int dl_fd_ = -1;
    uint32_t dl_id_ = 0;
    uint64_t dl_off_ = 0;
    uint64_t dl_size_ = 0;
    bool dl_active_ = false;
    int  dl_inflight_ = 0;   // 已入队未确认的下载块
    std::unique_ptr<ArchiveStream> dl_archive_;

    std::string up_id_;
    uint32_t up_req_ = 0;
    uint64_t up_off_ = 0;
    uint64_t up_size_ = 0;
};

} // namespace rtfs
