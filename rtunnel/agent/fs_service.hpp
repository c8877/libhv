#pragma once
// agent 文件服务：路径 jail + 目录/文件操作 + 分片上传会话。
//
// 所有路径都以 --file-root 为根，内部只持有 root fd，通过 openat/openat2
// 逐级解析，拒绝 ".."、空组件和逃出根目录的符号链接。
// 同一进程里可能同时被隧道线程和本地 Web 线程调用，因此对外接口全部加锁。

#include <stdint.h>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rtfs {

enum ErrCode {
    OK = 0,
    BAD_REQUEST = 1,
    UNAUTHORIZED = 2,
    FORBIDDEN = 3,
    NOT_FOUND = 4,
    CONFLICT = 5,
    TOO_LARGE = 6,
    NO_SPACE = 7,
    IO_ERROR = 8,
    BUSY = 9,
    CANCELLED = 10,
};

struct Config {
    std::string root = "/opt/data/files";
    bool rw = false;
    uint64_t max_upload = 512ull * 1024 * 1024;
    uint32_t chunk = 30 * 1024;
    std::string token;
};

struct Entry {
    std::string name;
    int type = 0; // 0=file 1=dir 2=symlink 3=other
    uint64_t size = 0;
    int64_t mtime = 0;
    std::string mode;
    bool is_link = false;
    std::string target;
};

struct Space {
    uint64_t free_kb = 0;
    uint64_t total_kb = 0;
    bool ok = false;
};

class Service {
public:
    Service() = default;
    ~Service() { close(); }
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    int open(const Config& cfg, std::string& err);
    void close();

    bool ready() const { return root_fd_ >= 0; }
    bool writable() const { return cfg_.rw; }
    bool tokenRequired() const { return !cfg_.token.empty(); }
    bool checkToken(const std::string& token) const;
    const Config& config() const { return cfg_; }
    const std::string& root() const { return cfg_.root; }

    int list(const std::string& path, bool hidden, uint32_t offset, uint32_t limit, std::vector<Entry>& out, bool& has_more, std::string& err);
    int stat(const std::string& path, Entry& out, std::string& err);
    // 递归统计目录占用（不跟随符号链接；超限时 truncated=true）
    int dirSize(const std::string& path, uint64_t& bytes, uint64_t& files, uint64_t& dirs, bool& truncated, std::string& err);
    Space space();

    int mkdir(const std::string& path, std::string& err);
    int touch(const std::string& path, std::string& err);
    int renameEntry(const std::string& path, const std::string& new_name, std::string& err);
    int move(const std::string& path, const std::string& dest_dir, std::string& err);
    int remove(const std::string& path, bool recursive, std::string& err);

    int openRead(const std::string& path, int& fd, uint64_t& size, int64_t& mtime, const std::string& name, std::string& err);
    // 打开目录用于 tar.gz 打包下载；fd 交给 ArchiveStream 持有
    int openDir(const std::string& path, int& fd, std::string& name, std::string& err);
    static int preadAt(int fd, uint64_t offset, char* buf, size_t n, size_t& got, std::string& err);
    static void closeFd(int& fd);

    // 分片上传：服务端生成 upload_id，页面凭 id 续传。
    int uploadBegin(const std::string& dir, const std::string& name, uint64_t size, bool overwrite, const std::string& resume_id, std::string& upload_id,
                    uint64_t& offset, std::string& err);
    int uploadWrite(const std::string& upload_id, uint64_t offset, const char* data, size_t n, uint64_t& received, std::string& err);
    int uploadStatus(const std::string& upload_id, uint64_t& offset, std::string& err);
    int uploadFinish(const std::string& upload_id, uint64_t& size, int64_t& mtime, std::string& err);
    int uploadCancel(const std::string& upload_id);

private:
    struct Upload {
        int dir_fd = -1;
        int fd = -1;
        std::string name;
        std::string tmp;
        uint64_t size = 0;
        uint64_t offset = 0;
        time_t started = 0;
    };

    void closeLocked();
    void cleanupUploadsLocked();
    int openAt(const std::string& path, int flags, int& fd, std::string& err);
    int parentAt(const std::string& path, int& pfd, std::string& name, std::string& err);
    int removeDir(int dfd, std::string& err);
    void cleanupParts(int dfd, const std::string& dirname);
    std::string realPath(const std::string& path);
    bool beneathRoot(const std::string& real) const;

    mutable std::mutex mu_;
    Config cfg_;
    int root_fd_ = -1;
    std::string root_real_;
    std::map<std::string, Upload> uploads_;
};

} // namespace rtfs
