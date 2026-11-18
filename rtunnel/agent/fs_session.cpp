#include "fs_session.hpp"

#include <algorithm>
#include <cstring>

using nlohmann::json;

namespace rtfs {

const size_t Session::MAX_MESSAGE;
const size_t Session::BIN_HEADER;

namespace {

// 下载/上传在途块数。传输信用 DEFAULT_CREDIT=128KB，30KB 一块正好 4 块；
// 再大就可能把 relay 的写缓冲（CONN_INFLIGHT+MAX_PAYLOAD）顶满导致断链。
constexpr int kPipelineWindow = 4;

uint32_t rd32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint64_t rd64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

std::string baseName(const std::string& path) {
    if (path == "/" || path.empty()) return "/";
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string parentPath(const std::string& path) {
    if (path == "/" || path.empty()) return "/";
    size_t p = path.find_last_of('/');
    if (p == 0) return "/";
    return path.substr(0, p);
}

json entryJson(const Entry& e) {
    json o;
    o["name"] = e.name;
    o["type"] = e.type == 1 ? "dir" : e.type == 2 ? "link" : e.type == 0 ? "file" : "other";
    o["size"] = e.size;
    o["mtime"] = e.mtime;
    o["mode"] = e.mode;
    o["is_link"] = e.is_link;
    if (!e.target.empty()) o["target"] = e.target;
    return o;
}

} // namespace

Session::Session(Service* svc, Sink* sink, const std::string& label, bool trusted, bool allow_rw)
    : svc_(svc), sink_(sink), label_(label), trusted_(trusted), allow_rw_(allow_rw) {}

Session::~Session() {
    onClose();
}

void Session::setUnlimited(bool v) {
    unlimited_ = v;
}

void Session::setCredit(uint32_t c) {
    credit_ = c;
    pump();
}

void Session::addCredit(uint32_t n) {
    if (unlimited_) return;
    const uint32_t kMaxCredit = 1024 * 1024;
    if (credit_ >= kMaxCredit) return;
    credit_ += n;
    if (credit_ > kMaxCredit) credit_ = kMaxCredit;
    pump();
}

void Session::pump() {
    while (!out_.empty()) {
        Out& o = out_.front();
        size_t need = o.data.size() + 1; // 隧道 DATA 里带 1 字节 kind
        if (!unlimited_) {
            if (credit_ < need) return;
            credit_ -= (uint32_t)need;
        }
        sink_->send(o.binary, o.data);
        out_.pop_front();
    }
}

void Session::sendJson(const nlohmann::json& j) {
    Out o;
    o.binary = false;
    o.data = j.dump();
    if (o.data.size() > MAX_MESSAGE) {
        // 控制消息不该这么大；截断会破坏 JSON，直接丢弃并回错误。
        json e;
        e["op"] = "error";
        e["id"] = 0;
        e["code"] = IO_ERROR;
        e["message"] = "response too large";
        o.data = e.dump();
    }
    out_.push_back(std::move(o));
    pump();
}

void Session::sendError(uint32_t id, int code, const std::string& text) {
    json e;
    e["op"] = "error";
    e["id"] = id;
    e["code"] = code;
    e["message"] = text;
    sendJson(e);
}

void Session::cancelDownload() {
    if (dl_fd_ >= 0) Service::closeFd(dl_fd_);
    dl_archive_.reset();
    dl_active_ = false;
    dl_inflight_ = 0;
    dl_id_ = 0;
    dl_off_ = dl_size_ = 0;
}

void Session::onClose() {
    cancelDownload();
    // 上传会话保留在 Service 里，断线/刷新后可用 upload_id 续传；
    // 超过 24 小时的残留由 Service::uploadBegin 清理。
    up_id_.clear();
    up_off_ = 0;
}

// ---------------------------------------------------------------- 入口

void Session::onText(const std::string& msg) {
    json j = json::parse(msg, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        sendError(0, BAD_REQUEST, "bad json");
        return;
    }
    uint32_t id = j.value("id", (uint32_t)0);
    std::string op = j.value("op", "");
    if (op.empty()) {
        sendError(id, BAD_REQUEST, "missing op");
        return;
    }
    if (op == "hello") {
        handleHello(j, id);
        return;
    }
    if (op == "ping") {
        json p;
        p["op"] = "pong";
        p["id"] = id;
        sendJson(p);
        return;
    }
    if (!authed_) {
        sendError(id, UNAUTHORIZED, "hello required");
        return;
    }
    if (op == "list")
        handleList(j, id);
    else if (op == "stat")
        handleStat(j, id);
    else if (op == "size")
        handleSize(j, id);
    else if (op == "mkdir")
        handleMkdir(j, id);
    else if (op == "touch")
        handleTouch(j, id);
    else if (op == "rename")
        handleRename(j, id);
    else if (op == "move")
        handleMove(j, id);
    else if (op == "delete")
        handleDelete(j, id);
    else if (op == "download_open")
        handleDownloadOpen(j, id);
    else if (op == "archive_open")
        handleArchiveOpen(j, id);
    else if (op == "download_ack")
        handleDownloadAck(j, id);
    else if (op == "download_cancel")
        handleDownloadCancel(j, id);
    else if (op == "upload_open")
        handleUploadOpen(j, id);
    else if (op == "upload_finish")
        handleUploadFinish(j, id);
    else if (op == "upload_cancel")
        handleUploadCancel(j, id);
    else if (op == "upload_status")
        handleUploadStatus(j, id);
    else
        sendError(id, BAD_REQUEST, "unknown op: " + op);
}

void Session::onBinary(const std::string& msg) {
    if (!authed_) return;
    if (msg.size() < BIN_HEADER) return;
    const unsigned char* p = (const unsigned char*)msg.data();
    uint32_t id = rd32(p);
    uint8_t kind = p[4];
    uint64_t off = rd64(p + 8);
    const char* data = msg.data() + BIN_HEADER;
    size_t n = msg.size() - BIN_HEADER;
    if (kind != 2) return;
    if (!rw_ || up_id_.empty() || id != up_req_ || off != up_off_) {
        json e;
        e["op"] = "error";
        e["id"] = id;
        e["code"] = CONFLICT;
        e["message"] = up_id_.empty() ? "no upload session" : "offset mismatch";
        e["expected"] = up_off_;
        sendJson(e);
        return;
    }
    uint64_t received = 0;
    std::string err;
    int rc = svc_->uploadWrite(up_id_, off, data, n, received, err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    up_off_ = received;
    json a;
    a["op"] = "upload_ack";
    a["id"] = id;
    a["received"] = up_off_;
    sendJson(a);
}

// ---------------------------------------------------------------- 控制协议

void Session::handleHello(const nlohmann::json& j, uint32_t id) {
    bool token_ok = true;
    if (svc_->tokenRequired() && !trusted_) {
        token_ok = svc_->checkToken(j.value("token", std::string()));
    }
    if (!token_ok) {
        authed_ = false;
        sendError(id, UNAUTHORIZED, "invalid file token");
        return;
    }
    bool want_rw = j.value("rw", true);
    if (want_rw && (!svc_->writable() || !allow_rw_)) want_rw = false;
    rw_ = want_rw;
    authed_ = true;

    const Config& cfg = svc_->config();
    Space sp = svc_->space();
    json w;
    w["op"] = "welcome";
    w["id"] = id;
    w["v"] = 1;
    w["label"] = label_;
    w["root"] = cfg.root;
    w["rw"] = rw_;
    w["readonly"] = !rw_;
    w["chunk"] = cfg.chunk;
    w["max_upload"] = cfg.max_upload;
    w["token_required"] = svc_->tokenRequired() && !trusted_;
    w["free_kb"] = sp.free_kb;
    w["total_kb"] = sp.total_kb;
    sendJson(w);
}

void Session::handleList(const nlohmann::json& j, uint32_t id) {
    std::string path = j.value("path", "/");
    bool hidden = j.value("hidden", false);
    uint32_t offset = j.value("offset", (uint32_t)0);
    uint32_t limit = j.value("limit", (uint32_t)500);
    std::vector<Entry> entries;
    bool has_more = false;
    std::string err;
    int rc = svc_->list(path, hidden, offset, limit, entries, has_more, err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    Space sp = svc_->space();
    json arr = json::array();
    for (auto& e : entries) arr.push_back(entryJson(e));

    json o;
    o["op"] = "list";
    o["id"] = id;
    o["path"] = path;
    o["parent"] = parentPath(path);
    o["root"] = svc_->root();
    o["rw"] = rw_;
    o["free_kb"] = sp.free_kb;
    o["total_kb"] = sp.total_kb;
    o["offset"] = offset;
    // 单条消息控制在 30 KiB 内：超了就少返回几条，下次翻页再取。
    size_t n = entries.size();
    for (;;) {
        json page = json::array();
        for (size_t i = 0; i < n; ++i) page.push_back(arr[i]);
        o["entries"] = page;
        o["has_more"] = has_more || n < entries.size();
        std::string s = o.dump();
        if (s.size() <= MAX_MESSAGE || n == 0) {
            out_.push_back(Out{false, s});
            pump();
            return;
        }
        n = n * 3 / 4;
        if (n == 0) n = 0;
    }
}

void Session::handleStat(const nlohmann::json& j, uint32_t id) {
    std::string path = j.value("path", "/");
    Entry e;
    std::string err;
    int rc = svc_->stat(path, e, err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    json o;
    o["op"] = "stat";
    o["id"] = id;
    o["path"] = path;
    o["entry"] = entryJson(e);
    sendJson(o);
}

void Session::handleSize(const nlohmann::json& j, uint32_t id) {
    std::string path = j.value("path", "/");
    uint64_t bytes = 0, files = 0, dirs = 0;
    bool truncated = false;
    std::string err;
    int rc = svc_->dirSize(path, bytes, files, dirs, truncated, err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    json o;
    o["op"] = "size";
    o["id"] = id;
    o["path"] = path;
    o["bytes"] = bytes;
    o["files"] = files;
    o["dirs"] = dirs;
    o["truncated"] = truncated;
    sendJson(o);
}

void Session::handleMkdir(const nlohmann::json& j, uint32_t id) {
    if (!rw_) {
        sendError(id, FORBIDDEN, "read-only");
        return;
    }
    std::string err;
    int rc = svc_->mkdir(j.value("path", std::string()), err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    json o;
    o["op"] = "mkdir";
    o["id"] = id;
    o["ok"] = true;
    sendJson(o);
}

void Session::handleTouch(const nlohmann::json& j, uint32_t id) {
    if (!rw_) {
        sendError(id, FORBIDDEN, "read-only");
        return;
    }
    std::string err;
    int rc = svc_->touch(j.value("path", std::string()), err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    json o;
    o["op"] = "touch";
    o["id"] = id;
    o["ok"] = true;
    sendJson(o);
}

void Session::handleRename(const nlohmann::json& j, uint32_t id) {
    if (!rw_) {
        sendError(id, FORBIDDEN, "read-only");
        return;
    }
    std::string err;
    int rc = svc_->renameEntry(j.value("path", std::string()), j.value("name", std::string()), err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    json o;
    o["op"] = "rename";
    o["id"] = id;
    o["ok"] = true;
    sendJson(o);
}

void Session::handleMove(const nlohmann::json& j, uint32_t id) {
    if (!rw_) {
        sendError(id, FORBIDDEN, "read-only");
        return;
    }
    std::string err;
    int rc = svc_->move(j.value("path", std::string()), j.value("dir", std::string()), err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    json o;
    o["op"] = "move";
    o["id"] = id;
    o["ok"] = true;
    sendJson(o);
}

void Session::handleDelete(const nlohmann::json& j, uint32_t id) {
    if (!rw_) {
        sendError(id, FORBIDDEN, "read-only");
        return;
    }
    std::string path = j.value("path", std::string());
    bool recursive = j.value("recursive", false);
    std::string err;
    int rc = svc_->remove(path, recursive, err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    json o;
    o["op"] = "delete";
    o["id"] = id;
    o["ok"] = true;
    sendJson(o);
}

// ---------------------------------------------------------------- 下载

void Session::handleDownloadOpen(const nlohmann::json& j, uint32_t id) {
    std::string path = j.value("path", std::string());
    // 范围读取：Hex/尾部查看用，不传 offset/limit 时仍是整个文件
    uint64_t offset = j.value("offset", (uint64_t)0);
    uint64_t limit = j.value("limit", (uint64_t)0);
    uint64_t file_size = 0;
    if (dl_active_ || dl_fd_ >= 0) cancelDownload();
    {
        int fd = -1;
        uint64_t size = 0;
        int64_t mtime = 0;
        std::string err;
        int rc = svc_->openRead(path, fd, size, mtime, baseName(path), err);
        if (rc != OK) {
            sendError(id, rc, err);
            return;
        }
        file_size = size;
        if (offset > size) offset = size;
        uint64_t remain = size - offset;
        uint64_t len = (limit > 0 && limit < remain) ? limit : remain;
        dl_fd_ = fd;
        dl_id_ = id;
        dl_off_ = offset;
        dl_size_ = offset + len;   // 绝对结束位置，pumpDownload 用 dl_off_ >= dl_size_ 判断结束
        dl_active_ = true;
        dl_inflight_ = 0;
    }
    json m;
    m["op"] = "download_meta";
    m["id"] = id;
    m["path"] = path;
    m["name"] = baseName(path);
    m["size"] = dl_size_ - dl_off_;
    m["offset"] = dl_off_;
    m["file_size"] = file_size;
    m["chunk"] = svc_->config().chunk;
    sendJson(m);
    pumpDownload();
}

void Session::handleArchiveOpen(const nlohmann::json& j, uint32_t id) {
    std::string path = j.value("path", std::string());
    if (dl_active_ || dl_fd_ >= 0) cancelDownload();
    int fd = -1;
    std::string name, err;
    int rc = svc_->openDir(path, fd, name, err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    dl_archive_.reset(new ArchiveStream(fd, name));
    dl_fd_ = -1;
    dl_id_ = id;
    dl_off_ = 0;
    dl_size_ = 0;
    dl_active_ = true;
    dl_inflight_ = 0;
    json m;
    m["op"] = "archive_meta";
    m["id"] = id;
    m["path"] = path;
    m["name"] = name + ".tar.gz";
    m["size"] = 0; // 边打包边发送，总大小未知
    m["chunk"] = svc_->config().chunk;
    sendJson(m);
    pumpDownload();
}

void Session::pumpDownload() {
    // 允许 4 块在途：每块不再等一个 RTT，吞吐从 30KB/RTT 提升到 4*30KB/RTT
    while (dl_active_ && dl_inflight_ < kPipelineWindow) {
        if (dl_archive_) {
            size_t want = svc_->config().chunk;
            std::string chunk;
            chunk.resize(BIN_HEADER + want);
            size_t got = 0;
            std::string err;
            int rc = dl_archive_->read(&chunk[BIN_HEADER], want, got, err);
            if (rc != OK) {
                sendError(dl_id_, rc, err);
                cancelDownload();
                return;
            }
            uint64_t off = dl_off_;
            unsigned char* h = (unsigned char*)&chunk[0];
            h[0] = (unsigned char)(dl_id_ >> 24);
            h[1] = (unsigned char)(dl_id_ >> 16);
            h[2] = (unsigned char)(dl_id_ >> 8);
            h[3] = (unsigned char)dl_id_;
            h[4] = 1; // download
            if (got == 0) {
                h[5] = 1; // archive end
                chunk.resize(BIN_HEADER);
                dl_active_ = false;
            } else {
                h[5] = 0;
                chunk.resize(BIN_HEADER + got);
                dl_off_ += got;
                ++dl_inflight_;
            }
            for (int i = 0; i < 8; ++i) h[8 + i] = (unsigned char)(off >> (8 * (7 - i)));
            out_.push_back(Out{true, std::move(chunk)});
            pump();
            if (!dl_active_) {
                cancelDownload();
                return;
            }
            continue;
        }
        if (dl_fd_ < 0) {
            dl_active_ = false;
            return;
        }
        if (dl_off_ >= dl_size_) {
            json e;
            e["op"] = "download_end";
            e["id"] = dl_id_;
            e["size"] = dl_size_;
            sendJson(e);
            cancelDownload();
            return;
        }
        size_t want = svc_->config().chunk;
        if (want > dl_size_ - dl_off_) want = (size_t)(dl_size_ - dl_off_);
        std::string chunk;
        chunk.resize(BIN_HEADER + want);
        std::string err;
        size_t got = 0;
        int rc = Service::preadAt(dl_fd_, dl_off_, &chunk[BIN_HEADER], want, got, err);
        if (rc != OK) {
            sendError(dl_id_, rc, err);
            cancelDownload();
            return;
        }
        if (got == 0) {
            json e;
            e["op"] = "download_end";
            e["id"] = dl_id_;
            e["size"] = dl_off_;
            sendJson(e);
            cancelDownload();
            return;
        }
        chunk.resize(BIN_HEADER + got);
        unsigned char* h = (unsigned char*)&chunk[0];
        h[0] = (unsigned char)(dl_id_ >> 24);
        h[1] = (unsigned char)(dl_id_ >> 16);
        h[2] = (unsigned char)(dl_id_ >> 8);
        h[3] = (unsigned char)dl_id_;
        h[4] = 1; // download
        h[5] = 0;
        h[6] = 0;
        h[7] = 0;
        for (int i = 0; i < 8; ++i) h[8 + i] = (unsigned char)(dl_off_ >> (8 * (7 - i)));
        dl_off_ += got;
        ++dl_inflight_;
        out_.push_back(Out{true, std::move(chunk)});
        pump();
    }
}

void Session::handleDownloadAck(const nlohmann::json& j, uint32_t id) {
    if (!dl_active_ || id != dl_id_) return;
    (void)j;
    if (dl_inflight_ > 0) --dl_inflight_;
    pumpDownload();
}

void Session::handleDownloadCancel(const nlohmann::json& j, uint32_t id) {
    if (dl_active_ && id == dl_id_) {
        cancelDownload();
        json o;
        o["op"] = "download_cancelled";
        o["id"] = id;
        sendJson(o);
    }
}

// ---------------------------------------------------------------- 上传

void Session::handleUploadOpen(const nlohmann::json& j, uint32_t id) {
    if (!rw_) {
        sendError(id, FORBIDDEN, "read-only");
        return;
    }
    std::string name = j.value("name", std::string());
    std::string dir = j.value("dir", std::string("/"));
    uint64_t size = j.value("size", (uint64_t)0);
    bool overwrite = j.value("overwrite", false);
    std::string resume = j.value("upload_id", std::string());
    std::string upid;
    uint64_t offset = 0;
    std::string err;
    int rc = svc_->uploadBegin(dir, name, size, overwrite, resume, upid, offset, err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    if (!up_id_.empty() && up_id_ != upid) svc_->uploadCancel(up_id_);
    up_id_ = upid;
    up_req_ = id;
    up_off_ = offset;
    up_size_ = size;
    json o;
    o["op"] = "upload_ready";
    o["id"] = id;
    o["upload_id"] = upid;
    o["offset"] = offset;
    o["chunk"] = svc_->config().chunk;
    o["inflight"] = kPipelineWindow;
    sendJson(o);
}

void Session::handleUploadFinish(const nlohmann::json& j, uint32_t id) {
    std::string upid = j.value("upload_id", up_id_);
    if (upid.empty() || upid != up_id_) {
        sendError(id, NOT_FOUND, "upload session not found");
        return;
    }
    uint64_t size = 0;
    int64_t mtime = 0;
    std::string err;
    int rc = svc_->uploadFinish(upid, size, mtime, err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    std::string name = baseName(j.value("path", std::string()));
    up_id_.clear();
    up_off_ = 0;
    json o;
    o["op"] = "upload_done";
    o["id"] = id;
    o["size"] = size;
    o["mtime"] = mtime;
    if (!name.empty()) o["name"] = name;
    sendJson(o);
}

void Session::handleUploadCancel(const nlohmann::json& j, uint32_t id) {
    std::string upid = j.value("upload_id", up_id_);
    if (!upid.empty()) svc_->uploadCancel(upid);
    if (upid == up_id_) {
        up_id_.clear();
        up_off_ = 0;
    }
    json o;
    o["op"] = "upload_cancelled";
    o["id"] = id;
    sendJson(o);
}

void Session::handleUploadStatus(const nlohmann::json& j, uint32_t id) {
    std::string upid = j.value("upload_id", up_id_);
    uint64_t offset = 0;
    std::string err;
    int rc = svc_->uploadStatus(upid, offset, err);
    if (rc != OK) {
        sendError(id, rc, err);
        return;
    }
    json o;
    o["op"] = "upload_status";
    o["id"] = id;
    o["upload_id"] = upid;
    o["received"] = offset;
    sendJson(o);
}

} // namespace rtfs
