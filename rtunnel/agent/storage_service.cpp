#include "storage_service.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>

namespace rtstorage {

namespace {

struct NodeAcc {
    std::string path;
    uint64_t bytes = 0;
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t small_bytes = 0; // 本层未单独成行的小文件
    uint64_t small_files = 0;
    long long mtime = 0;
    int depth = 0;
};

// 单独回传的大文件；小文件按目录汇总进 NodeAcc::small_*
struct FileNode {
    std::string path;
    uint64_t bytes = 0;
    long long mtime = 0;
};

struct Walker {
    const Options& opt;
    const std::atomic<bool>& cancel;
    dev_t root_dev = 0;
    std::vector<NodeAcc> nodes; // nodes[0] 是挂载点
    std::vector<FileNode> files;
    std::set<std::pair<uint64_t, uint64_t>> seen_inodes;
    uint64_t entries = 0;
    bool truncated = false;
    bool cancelled = false;
    std::string reason;
    std::vector<std::string> skipped;
    std::chrono::steady_clock::time_point t0;

    explicit Walker(const Options& o, const std::atomic<bool>& c) : opt(o), cancel(c) {}

    bool outOfBudget() {
        if (cancel.load(std::memory_order_relaxed)) {
            cancelled = true;
            reason = "cancelled";
            return true;
        }
        if (entries >= opt.max_entries) {
            truncated = true;
            reason = "entries";
            return true;
        }
        if ((entries & 0x3FF) == 0) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
            if (ms > (long long)opt.max_seconds * 1000) {
                truncated = true;
                reason = "time";
                return true;
            }
        }
        return false;
    }

    void skip(const std::string& path) {
        if (skipped.size() < 8) skipped.push_back(path);
    }

    // 返回 false 表示遇到无法继续的错误（权限/IO），调用方计入 skipped。
    bool walk(int dfd, size_t idx, int depth) {
        DIR* d = ::fdopendir(dfd);
        if (d == nullptr) {
            ::close(dfd);
            skip(nodes[idx].path);
            return false;
        }
        struct dirent* de;
        while ((de = ::readdir(d)) != nullptr) {
            std::string name = de->d_name;
            if (name == "." || name == "..") continue;
            if (outOfBudget()) break;
            ++entries;

            struct stat st;
            if (::fstatat(::dirfd(d), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
            if (S_ISREG(st.st_mode)) {
                // 硬链接只算一次，和 du 口径一致
                if (st.st_nlink > 1) {
                    auto key = std::make_pair((uint64_t)st.st_dev, (uint64_t)st.st_ino);
                    if (!seen_inodes.insert(key).second) continue;
                }
                uint64_t sz = (uint64_t)st.st_size;
                nodes[idx].bytes += sz;
                ++nodes[idx].files;
                // 大文件单独成行；小文件按目录聚合，保证子项之和等于父项
                if (sz >= opt.min_file_bytes && !nodes[idx].path.empty() &&
                    files.size() < opt.max_file_nodes) {
                    std::string child = nodes[idx].path;
                    if (child.back() != '/') child += '/';
                    child += name;
                    FileNode fn;
                    fn.path = std::move(child);
                    fn.bytes = sz;
                    fn.mtime = (long long)st.st_mtime;
                    files.push_back(std::move(fn));
                }
                else {
                    nodes[idx].small_bytes += sz;
                    ++nodes[idx].small_files;
                }
                continue;
            }
            if (!S_ISDIR(st.st_mode)) continue;
            if (st.st_dev != root_dev) continue; // 不跨分区
            if (depth + 1 > opt.max_depth) {
                truncated = true;
                if (reason.empty()) reason = "depth";
                continue;
            }

            std::string child = nodes[idx].path;
            if (child.empty() || child.back() != '/') child += '/';
            child += name;

            int cfd = ::openat(::dirfd(d), name.c_str(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd < 0) {
                skip(child);
                continue;
            }

            // 节点数达到上限后只做累加、不保留节点，避免内存随目录数增长
            bool keep = nodes.size() < opt.max_nodes;
            if (!keep) {
                truncated = true;
                if (reason.empty()) reason = "nodes";
            }
            size_t cidx = nodes.size();
            NodeAcc n;
            n.path = keep ? child : std::string();
            n.mtime = (long long)st.st_mtime;
            n.depth = depth + 1;
            nodes.push_back(std::move(n));

            walk(cfd, cidx, depth + 1);

            // 子目录统计汇总到父节点；未保留的节点折叠后立即丢弃
            nodes[idx].bytes += nodes[cidx].bytes;
            nodes[idx].files += nodes[cidx].files;
            nodes[idx].dirs += nodes[cidx].dirs + 1;
            if (!keep) nodes.pop_back();
        }
        ::closedir(d);
        return true;
    }
};

std::string normalizeMount(const std::string& in) {
    if (in.empty()) return "/";
    std::string s = in;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    if (s.empty() || s[0] != '/') s = "/" + s;
    return s;
}

} // namespace

bool scan(const Options& opt_in, const std::atomic<bool>& cancel, Result& out) {
    Options opt = opt_in;
    opt.mount = normalizeMount(opt.mount);
    out = Result();
    out.mount = opt.mount;

    struct statvfs vfs;
    if (::statvfs(opt.mount.c_str(), &vfs) != 0) {
        out.error = "statvfs failed: " + std::string(strerror(errno));
        return false;
    }
    uint64_t frsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    out.total_kb = (uint64_t)vfs.f_blocks * frsize / 1024;
    out.used_kb = (uint64_t)(vfs.f_blocks - vfs.f_bfree) * frsize / 1024;
    out.free_kb = (uint64_t)vfs.f_bavail * frsize / 1024;
    uint64_t reserved = vfs.f_bfree > vfs.f_bavail ? vfs.f_bfree - vfs.f_bavail : 0;
    out.reserved_kb = reserved * frsize / 1024;

    int fd = ::open(opt.mount.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        out.error = "open failed: " + std::string(strerror(errno));
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        int e = errno;
        ::close(fd);
        out.error = "stat failed: " + std::string(strerror(e));
        return false;
    }
    Walker w(opt, cancel);
    w.root_dev = st.st_dev;
    w.t0 = std::chrono::steady_clock::now();
    {
        NodeAcc root;
        root.path = opt.mount;
        root.mtime = (long long)st.st_mtime;
        root.depth = 0;
        w.nodes.push_back(std::move(root));
    }
    w.walk(fd, 0, 0);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - w.t0)
                  .count();
    out.scan_ms = (int)(ms > 0 ? ms : 0);
    out.entries = w.entries;
    out.truncated = w.truncated;
    out.truncated_reason = w.reason;
    out.skipped = w.skipped;
    if (w.cancelled) {
        out.error = "cancelled";
        return false;
    }
    if (w.nodes.empty()) {
        out.error = "empty";
        return false;
    }
    out.bytes = w.nodes[0].bytes;
    out.files = w.nodes[0].files;
    out.dirs = w.nodes[0].dirs;

    nlohmann::json arr = nlohmann::json::array();
    auto addDir = [&arr](const NodeAcc& n) {
        arr.push_back({{"p", n.path}, {"b", n.bytes}, {"n", n.files},
                       {"d", n.dirs}, {"m", n.mtime}, {"t", "d"}});
    };
    addDir(w.nodes[0]);
    for (size_t i = 1; i < w.nodes.size(); ++i) {
        if (w.nodes[i].path.empty()) continue; // sink 节点
        addDir(w.nodes[i]);
    }
    // 大文件作为叶子节点，前端按路径挂到所属目录下
    for (const FileNode& f : w.files) {
        arr.push_back({{"p", f.path}, {"b", f.bytes}, {"n", 1},
                       {"d", 0}, {"m", f.mtime}, {"t", "f"}});
    }
    // 小于阈值的文件按目录聚合成一条，路径用一个不会与实际文件重名的后缀
    for (const NodeAcc& n : w.nodes) {
        if (n.path.empty() || n.small_files == 0) continue;
        std::string ap = n.path;
        if (ap.back() != '/') ap += '/';
        ap += std::string("\x01") + "files";
        arr.push_back({{"p", ap}, {"b", n.small_bytes}, {"n", n.small_files},
                       {"d", 0}, {"m", n.mtime}, {"t", "o"}});
    }
    out.nodes = std::move(arr);
    out.ok = true;
    return true;
}

std::string toJson(const Result& r) {
    nlohmann::json j;
    j["ok"] = r.ok;
    if (!r.error.empty()) j["error"] = r.error;
    j["mount"] = r.mount;
    j["total_kb"] = r.total_kb;
    j["used_kb"] = r.used_kb;
    j["free_kb"] = r.free_kb;
    j["reserved_kb"] = r.reserved_kb;
    j["bytes"] = r.bytes;
    j["files"] = r.files;
    j["dirs"] = r.dirs;
    j["entries"] = r.entries;
    j["scan_ms"] = r.scan_ms;
    j["truncated"] = r.truncated;
    j["truncated_reason"] = r.truncated_reason;
    j["skipped"] = r.skipped;
    j["nodes"] = r.nodes;
    return j.dump();
}

} // namespace rtstorage
