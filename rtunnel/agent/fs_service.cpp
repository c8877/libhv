#include "fs_service.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

// openat2 的内核 ABI；即使头文件里没有也可以手写。
#if defined(__linux__) && (defined(SYS_openat2) || defined(__NR_openat2))
#define RTFS_HAVE_OPENAT2 1
#ifndef SYS_openat2
#define SYS_openat2 __NR_openat2
#endif
#ifndef RESOLVE_BENEATH
#define RESOLVE_BENEATH 0x08
#endif
#ifndef RESOLVE_NO_MAGICLINKS
#define RESOLVE_NO_MAGICLINKS 0x02
#endif
struct rtfs_open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};
#endif

namespace rtfs {

namespace {

int errCode(int e) {
    switch (e) {
    case 0: return OK;
    case ENOENT: return NOT_FOUND;
    case EACCES:
    case EPERM:
    case ELOOP:
    case EXDEV: return FORBIDDEN;
    case EEXIST:
    case ENOTEMPTY:
    case EISDIR: return CONFLICT;
    case ENOSPC: return NO_SPACE;
    case EFBIG: return TOO_LARGE;
    case ENOTDIR:
    case ENAMETOOLONG:
    case EINVAL: return BAD_REQUEST;
    default: return IO_ERROR;
    }
}

std::string msg(const std::string& what, int e) {
    return what + ": " + strerror(e);
}

// 统一路径格式：必须以 / 开头，拒绝 "." / ".." / 空组件。
bool normalize(const std::string& in, std::string& out, std::string& err) {
    if (in.empty() || in[0] != '/') {
        err = "path must be absolute";
        return false;
    }
    if (in.size() > 4096) {
        err = "path too long";
        return false;
    }
    std::string s;
    size_t i = 1;
    while (i <= in.size()) {
        size_t j = in.find('/', i);
        std::string part = in.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!part.empty()) {
            if (part == "." || part == "..") {
                err = "path may not contain . or ..";
                return false;
            }
            if (part.find('\0') != std::string::npos || part.size() > 255) {
                err = "invalid path component";
                return false;
            }
            s += '/';
            s += part;
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    out = s.empty() ? "/" : s;
    return true;
}

bool validName(const std::string& name, std::string& err) {
    if (name.empty() || name == "." || name == "..") {
        err = "invalid name";
        return false;
    }
    if (name.size() > 255 || name.find('/') != std::string::npos || name.find('\0') != std::string::npos) {
        err = "invalid name";
        return false;
    }
    return true;
}

std::string modeString(mode_t m) {
    char s[11];
    s[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) s[1 + i] = (m & (1 << (8 - i))) ? rwx[i] : '-';
    s[10] = '\0';
    return std::string(s);
}

std::string randomId() {
    unsigned char b[12];
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    if (fd >= 0) {
        got = (size_t)::read(fd, b, sizeof(b));
        ::close(fd);
    }
    if (got != sizeof(b)) {
        uint64_t seed = (uint64_t)time(nullptr) ^ ((uint64_t)getpid() << 32);
        for (size_t i = 0; i < sizeof(b); ++i) {
            seed = seed * 6364136223846793005ull + 1;
            b[i] = (unsigned char)(seed >> 33);
        }
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(b) * 2);
    for (size_t i = 0; i < sizeof(b); ++i) {
        out.push_back(hex[b[i] >> 4]);
        out.push_back(hex[b[i] & 15]);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------- 生命周期

int Service::open(const Config& cfg, std::string& err) {
    Config nc = cfg;
    if (nc.root.empty() || nc.root[0] != '/') {
        err = "file root must be absolute";
        return BAD_REQUEST;
    }
    if (nc.chunk < 4096) nc.chunk = 4096;
    if (nc.chunk > 30 * 1024) nc.chunk = 30 * 1024;

    // 先打开新根目录再切换，失败时保留原配置（运行期改配置不中断现有服务）。
    int fd = ::open(nc.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && nc.rw) {
        // 默认根目录不存在时按需创建；父目录不存在则仍失败
        if (::mkdir(nc.root.c_str(), 0755) == 0) {
            fd = ::open(nc.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        }
    }
    if (fd < 0) {
        err = msg("open root " + nc.root, errno);
        return errCode(errno);
    }
    std::string real = nc.root;
    char buf[4096];
    if (::realpath(nc.root.c_str(), buf) != nullptr) real = buf;
    while (real.size() > 1 && real.back() == '/') real.pop_back();

    std::lock_guard<std::mutex> lk(mu_);
    closeLocked();
    cfg_ = nc;
    root_fd_ = fd;
    root_real_ = real;
    return OK;
}

void Service::close() {
    std::lock_guard<std::mutex> lk(mu_);
    closeLocked();
}

void Service::closeLocked() {
    for (auto& kv : uploads_) {
        if (kv.second.fd >= 0) ::close(kv.second.fd);
        if (kv.second.dir_fd >= 0) {
            if (!kv.second.tmp.empty()) ::unlinkat(kv.second.dir_fd, kv.second.tmp.c_str(), 0);
            ::close(kv.second.dir_fd);
        }
    }
    uploads_.clear();
    if (root_fd_ >= 0) {
        ::close(root_fd_);
        root_fd_ = -1;
    }
    root_real_.clear();
}

bool Service::checkToken(const std::string& token) const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_.token.empty() || token == cfg_.token;
}

std::string Service::realPath(const std::string& path) {
    std::string full = root_real_;
    if (path != "/" && !path.empty()) full += path;
    char buf[4096];
    if (::realpath(full.c_str(), buf) == nullptr) return std::string();
    return std::string(buf);
}

bool Service::beneathRoot(const std::string& real) const {
    if (real.empty()) return false;
    if (real == root_real_) return true;
    if (root_real_ == "/") return real.size() > 1 && real[0] == '/';
    return real.size() > root_real_.size() && real.compare(0, root_real_.size(), root_real_) == 0 && real[root_real_.size()] == '/';
}

// ---------------------------------------------------------------- 路径解析

int Service::openAt(const std::string& path, int flags, int& fd, std::string& err) {
    std::string rel;
    if (!normalize(path, rel, err)) return BAD_REQUEST;
    if (root_fd_ < 0) {
        err = "file service not ready";
        return IO_ERROR;
    }
    // 根目录挂到 / 时，拒绝伪文件系统和设备节点。
    if (root_real_ == "/" && rel.size() > 1) {
        static const char* deny[] = {"/proc", "/sys", "/dev"};
        for (auto d : deny) {
            size_t n = strlen(d);
            if (rel.compare(0, n, d) == 0 && (rel.size() == n || rel[n] == '/')) {
                err = "path is not allowed";
                return FORBIDDEN;
            }
        }
    }
    const char* r = rel == "/" ? "." : rel.c_str() + 1;

#ifdef RTFS_HAVE_OPENAT2
    {
        rtfs_open_how how;
        how.flags = (uint64_t)(flags | O_CLOEXEC);
        how.mode = 0;
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
        long nfd = ::syscall(SYS_openat2, root_fd_, r, &how, sizeof(how));
        if (nfd >= 0) {
            fd = (int)nfd;
            return OK;
        }
        if (errno != ENOSYS && errno != EINVAL) {
            err = msg("open " + path, errno);
            return errCode(errno);
        }
    }
#endif
    // 回退：realpath 前缀校验。只在没有 openat2 的内核上使用。
    std::string real = realPath(rel);
    if (real.empty()) {
        err = msg("open " + path, errno);
        return errCode(errno);
    }
    if (!beneathRoot(real)) {
        err = "path escapes file root";
        return FORBIDDEN;
    }
    int nfd = ::open(real.c_str(), flags | O_CLOEXEC);
    if (nfd < 0) {
        err = msg("open " + path, errno);
        return errCode(errno);
    }
    fd = nfd;
    return OK;
}

int Service::parentAt(const std::string& path, int& pfd, std::string& name, std::string& err) {
    std::string rel;
    if (!normalize(path, rel, err)) return BAD_REQUEST;
    if (rel == "/") {
        err = "root directory cannot be modified";
        return FORBIDDEN;
    }
    size_t slash = rel.find_last_of('/');
    std::string parent = slash == 0 ? "/" : rel.substr(0, slash);
    name = rel.substr(slash + 1);
    if (!validName(name, err)) return BAD_REQUEST;
    int fd = -1;
    int rc = openAt(parent, O_RDONLY | O_DIRECTORY, fd, err);
    if (rc != OK) return rc;
    pfd = fd;
    return OK;
}

// ---------------------------------------------------------------- 浏览

int Service::list(const std::string& path, bool hidden, uint32_t offset, uint32_t limit, std::vector<Entry>& out, bool& has_more, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    has_more = false;
    if (limit == 0 || limit > 500) limit = 500;
    int fd = -1;
    int rc = openAt(path, O_RDONLY | O_DIRECTORY, fd, err);
    if (rc != OK) return rc;
    DIR* d = ::fdopendir(fd);
    if (d == nullptr) {
        ::close(fd);
        err = msg("opendir", errno);
        return errCode(errno);
    }
    std::vector<Entry> all;
    struct dirent* de;
    while ((de = ::readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        if (!hidden && !name.empty() && name[0] == '.') continue;
        struct stat st;
        if (::fstatat(::dirfd(d), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        Entry e;
        e.name = name;
        e.type = S_ISDIR(st.st_mode) ? 1 : S_ISLNK(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 0 : 3;
        e.size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
        e.mtime = (int64_t)st.st_mtime;
        e.mode = modeString(st.st_mode);
        e.is_link = S_ISLNK(st.st_mode);
        if (e.is_link) {
            char buf[1024];
            ssize_t n = ::readlinkat(::dirfd(d), name.c_str(), buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                e.target = buf;
            }
        }
        all.push_back(e);
    }
    ::closedir(d);

    std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
        if ((a.type == 1) != (b.type == 1)) return a.type == 1;
        return a.name < b.name;
    });
    size_t begin = offset < all.size() ? offset : all.size();
    size_t end = begin + limit;
    if (end > all.size()) end = all.size();
    out.assign(all.begin() + begin, all.begin() + end);
    has_more = end < all.size();
    return OK;
}

int Service::stat(const std::string& path, Entry& out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string rel;
    if (!normalize(path, rel, err)) return BAD_REQUEST;
    if (rel == "/") {
        struct stat st;
        if (::fstat(root_fd_, &st) != 0) {
            err = msg("stat /", errno);
            return errCode(errno);
        }
        out = Entry{};
        out.name = "/";
        out.type = 1;
        out.mode = modeString(st.st_mode);
        out.mtime = (int64_t)st.st_mtime;
        return OK;
    }
    int pfd = -1;
    std::string name;
    int rc = parentAt(path, pfd, name, err);
    if (rc != OK) return rc;
    struct stat st;
    if (::fstatat(pfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        int e = errno;
        ::close(pfd);
        err = msg("stat " + path, e);
        return errCode(e);
    }
    out = Entry{};
    out.name = name;
    out.type = S_ISDIR(st.st_mode) ? 1 : S_ISLNK(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 0 : 3;
    out.size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
    out.mtime = (int64_t)st.st_mtime;
    out.mode = modeString(st.st_mode);
    out.is_link = S_ISLNK(st.st_mode);
    if (out.is_link) {
        char buf[1024];
        ssize_t n = ::readlinkat(pfd, name.c_str(), buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            out.target = buf;
        }
    }
    ::close(pfd);
    return OK;
}

namespace {

struct SizeWalk {
    uint64_t bytes = 0;
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t budget = 200000; // 限制单次统计的目录项数，避免超大目录长时间占用 loop
    bool truncated = false;
};

void walkDirSize(int dfd, SizeWalk& w, int depth) {
    if (depth > 64 || w.budget == 0) {
        w.truncated = true;
        ::close(dfd);
        return;
    }
    DIR* d = ::fdopendir(dfd);
    if (d == nullptr) {
        ::close(dfd);
        return;
    }
    struct dirent* de;
    while ((de = ::readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        if (w.budget == 0) {
            w.truncated = true;
            break;
        }
        --w.budget;
        struct stat st;
        if (::fstatat(::dirfd(d), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            w.bytes += (uint64_t)st.st_size;
            ++w.files;
        }
        else if (S_ISDIR(st.st_mode)) {
            int cfd = ::openat(::dirfd(d), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd < 0) continue;
            ++w.dirs;
            walkDirSize(cfd, w, depth + 1);
        }
    }
    ::closedir(d);
}

} // namespace

int Service::dirSize(const std::string& path, uint64_t& bytes, uint64_t& files, uint64_t& dirs, bool& truncated, std::string& err) {
    bytes = 0;
    files = 0;
    dirs = 0;
    truncated = false;
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        int rc = openAt(path, O_RDONLY | O_CLOEXEC, fd, err);
        if (rc != OK) return rc;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        int e = errno;
        ::close(fd);
        err = msg("stat " + path, e);
        return errCode(e);
    }
    if (S_ISREG(st.st_mode)) {
        bytes = (uint64_t)st.st_size;
        files = 1;
        ::close(fd);
        return OK;
    }
    if (!S_ISDIR(st.st_mode)) {
        ::close(fd);
        err = "not a file or directory";
        return BAD_REQUEST;
    }
    SizeWalk w;
    walkDirSize(fd, w, 0); // fd 交给 fdopendir 关闭
    bytes = w.bytes;
    files = w.files;
    dirs = w.dirs;
    truncated = w.truncated;
    return OK;
}

Space Service::space() {
    std::lock_guard<std::mutex> lk(mu_);
    Space sp;
    if (root_fd_ < 0) return sp;
    struct statvfs vfs;
    if (::fstatvfs(root_fd_, &vfs) != 0) return sp;
    sp.free_kb = (uint64_t)vfs.f_bavail * vfs.f_frsize / 1024;
    sp.total_kb = (uint64_t)vfs.f_blocks * vfs.f_frsize / 1024;
    sp.ok = true;
    return sp;
}

// ---------------------------------------------------------------- 变更

int Service::mkdir(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!cfg_.rw) {
        err = "read-only";
        return FORBIDDEN;
    }
    int pfd = -1;
    std::string name;
    int rc = parentAt(path, pfd, name, err);
    if (rc != OK) return rc;
    if (::mkdirat(pfd, name.c_str(), 0755) != 0) {
        int e = errno;
        ::close(pfd);
        err = msg("mkdir " + path, e);
        return errCode(e);
    }
    ::close(pfd);
    return OK;
}

int Service::touch(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!cfg_.rw) {
        err = "read-only";
        return FORBIDDEN;
    }
    int pfd = -1;
    std::string name;
    int rc = parentAt(path, pfd, name, err);
    if (rc != OK) return rc;
    int fd = ::openat(pfd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        int e = errno;
        ::close(pfd);
        err = msg("touch " + path, e);
        return errCode(e);
    }
    ::close(fd);
    ::close(pfd);
    return OK;
}

int Service::renameEntry(const std::string& path, const std::string& new_name, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!cfg_.rw) {
        err = "read-only";
        return FORBIDDEN;
    }
    if (!validName(new_name, err)) return BAD_REQUEST;
    int pfd = -1;
    std::string name;
    int rc = parentAt(path, pfd, name, err);
    if (rc != OK) return rc;
    struct stat st;
    if (::fstatat(pfd, new_name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        ::close(pfd);
        err = "target already exists";
        return CONFLICT;
    }
    if (::renameat(pfd, name.c_str(), pfd, new_name.c_str()) != 0) {
        int e = errno;
        ::close(pfd);
        err = msg("rename", e);
        return errCode(e);
    }
    ::close(pfd);
    return OK;
}

int Service::move(const std::string& path, const std::string& dest_dir, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!cfg_.rw) {
        err = "read-only";
        return FORBIDDEN;
    }
    int spfd = -1;
    std::string name;
    int rc = parentAt(path, spfd, name, err);
    if (rc != OK) return rc;
    int dfd = -1;
    rc = openAt(dest_dir, O_RDONLY | O_DIRECTORY, dfd, err);
    if (rc != OK) {
        ::close(spfd);
        return rc;
    }
    struct stat st;
    if (::fstatat(dfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        ::close(spfd);
        ::close(dfd);
        err = "target already exists";
        return CONFLICT;
    }
    if (::renameat(spfd, name.c_str(), dfd, name.c_str()) != 0) {
        int e = errno;
        ::close(spfd);
        ::close(dfd);
        err = msg("move", e);
        return errCode(e);
    }
    ::close(spfd);
    ::close(dfd);
    return OK;
}

int Service::removeDir(int dfd, std::string& err) {
    DIR* d = ::fdopendir(::dup(dfd));
    if (d == nullptr) {
        err = msg("opendir", errno);
        return errCode(errno);
    }
    struct dirent* de;
    int rc = OK;
    while ((de = ::readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        struct stat st;
        if (::fstatat(::dirfd(d), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            int sub = ::openat(::dirfd(d), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub < 0) {
                rc = errCode(errno);
                err = msg("open " + name, errno);
                break;
            }
            rc = removeDir(sub, err);
            ::close(sub);
            if (rc != OK) break;
            if (::unlinkat(::dirfd(d), name.c_str(), AT_REMOVEDIR) != 0) {
                rc = errCode(errno);
                err = msg("rmdir " + name, errno);
                break;
            }
        }
        else {
            if (::unlinkat(::dirfd(d), name.c_str(), 0) != 0) {
                rc = errCode(errno);
                err = msg("unlink " + name, errno);
                break;
            }
        }
    }
    ::closedir(d);
    return rc;
}

int Service::remove(const std::string& path, bool recursive, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!cfg_.rw) {
        err = "read-only";
        return FORBIDDEN;
    }
    int pfd = -1;
    std::string name;
    int rc = parentAt(path, pfd, name, err);
    if (rc != OK) return rc;
    struct stat st;
    if (::fstatat(pfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        int e = errno;
        ::close(pfd);
        err = msg("stat " + path, e);
        return errCode(e);
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            int rc2 = ::unlinkat(pfd, name.c_str(), AT_REMOVEDIR);
            int e = errno;
            ::close(pfd);
            if (rc2 == 0) return OK;
            err = msg("rmdir " + path, e);
            return errCode(e);
        }
        int dfd = ::openat(pfd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dfd < 0) {
            int e = errno;
            ::close(pfd);
            err = msg("open dir " + path, e);
            return errCode(e);
        }
        rc = removeDir(dfd, err);
        ::close(dfd);
        if (rc != OK) {
            ::close(pfd);
            return rc;
        }
        if (::unlinkat(pfd, name.c_str(), AT_REMOVEDIR) != 0) {
            int e = errno;
            ::close(pfd);
            err = msg("rmdir " + path, e);
            return errCode(e);
        }
    }
    else {
        if (::unlinkat(pfd, name.c_str(), 0) != 0) {
            int e = errno;
            ::close(pfd);
            err = msg("unlink " + path, e);
            return errCode(e);
        }
    }
    ::close(pfd);
    return OK;
}

// ---------------------------------------------------------------- 下载

int Service::openRead(const std::string& path, int& fd, uint64_t& size, int64_t& mtime, const std::string& name, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    int nfd = -1;
    int rc = openAt(path, O_RDONLY, nfd, err);
    if (rc != OK) return rc;
    struct stat st;
    if (::fstat(nfd, &st) != 0) {
        int e = errno;
        ::close(nfd);
        err = msg("stat " + path, e);
        return errCode(e);
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(nfd);
        err = "not a regular file";
        return BAD_REQUEST;
    }
    fd = nfd;
    size = (uint64_t)st.st_size;
    mtime = (int64_t)st.st_mtime;
    (void)name;
    return OK;
}

int Service::openDir(const std::string& path, int& fd, std::string& name, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    int nfd = -1;
    int rc = openAt(path, O_RDONLY | O_DIRECTORY, nfd, err);
    if (rc != OK) return rc;
    struct stat st;
    if (::fstat(nfd, &st) != 0) {
        int e = errno;
        ::close(nfd);
        err = msg("stat " + path, e);
        return errCode(e);
    }
    if (!S_ISDIR(st.st_mode)) {
        ::close(nfd);
        err = "not a directory";
        return BAD_REQUEST;
    }
    std::string rel, nerr;
    if (!normalize(path, rel, nerr)) {
        ::close(nfd);
        err = nerr;
        return BAD_REQUEST;
    }
    std::string full = rel == "/" ? cfg_.root : rel;
    while (full.size() > 1 && full.back() == '/') full.pop_back();
    size_t p = full.find_last_of('/');
    name = p == std::string::npos ? full : full.substr(p + 1);
    if (name.empty() || name == "/") name = "root";
    fd = nfd;
    return OK;
}

int Service::preadAt(int fd, uint64_t offset, char* buf, size_t n, size_t& got, std::string& err) {
    got = 0;
    while (got < n) {
        ssize_t r = ::pread(fd, buf + got, n - got, (off_t)(offset + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            err = msg("read", errno);
            return errCode(errno);
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return OK;
}

void Service::closeFd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// ---------------------------------------------------------------- 上传

void Service::cleanupParts(int dfd, const std::string& dirname) {
    DIR* d = ::fdopendir(::dup(dfd));
    if (d == nullptr) return;
    time_t cutoff = time(nullptr) - 24 * 3600;
    struct dirent* de;
    while ((de = ::readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        struct stat st;
        if (::fstatat(::dirfd(d), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (st.st_mtime > cutoff) continue;
        ::unlinkat(::dirfd(d), (dirname + "/" + name).c_str(), 0);
    }
    ::closedir(d);
}

void Service::cleanupUploadsLocked() {
    time_t cutoff = time(nullptr) - 24 * 3600;
    for (auto it = uploads_.begin(); it != uploads_.end();) {
        if (it->second.started > cutoff) {
            ++it;
            continue;
        }
        if (it->second.fd >= 0) ::close(it->second.fd);
        if (it->second.dir_fd >= 0) {
            ::unlinkat(it->second.dir_fd, it->second.tmp.c_str(), 0);
            ::close(it->second.dir_fd);
        }
        it = uploads_.erase(it);
    }
}

int Service::uploadBegin(const std::string& dir, const std::string& name, uint64_t size, bool overwrite, const std::string& resume_id, std::string& upload_id,
                         uint64_t& offset, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    cleanupUploadsLocked();
    offset = 0;
    if (!cfg_.rw) {
        err = "read-only";
        return FORBIDDEN;
    }
    if (!validName(name, err)) return BAD_REQUEST;
    if (size > cfg_.max_upload) {
        err = "file too large";
        return TOO_LARGE;
    }
    int dfd = -1;
    int rc = openAt(dir.empty() ? "/" : dir, O_RDONLY | O_DIRECTORY, dfd, err);
    if (rc != OK) return rc;

    if (!resume_id.empty()) {
        auto it = uploads_.find(resume_id);
        if (it != uploads_.end() && it->second.name == name && it->second.size == size) {
            upload_id = resume_id;
            offset = it->second.offset;
            ::close(dfd);
            return OK;
        }
    }

    struct stat st;
    if (!overwrite && ::fstatat(dfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        ::close(dfd);
        err = "target already exists";
        return CONFLICT;
    }
    struct statvfs vfs;
    if (::fstatvfs(dfd, &vfs) == 0) {
        uint64_t free_b = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        if (free_b < size) {
            ::close(dfd);
            err = "no space left";
            return NO_SPACE;
        }
    }
    if (::mkdirat(dfd, ".rtunnel-upload", 0700) != 0 && errno != EEXIST) {
        int e = errno;
        ::close(dfd);
        err = msg("mkdir .rtunnel-upload", e);
        return errCode(e);
    }
    cleanupParts(dfd, ".rtunnel-upload");

    upload_id = randomId();
    Upload up;
    up.dir_fd = dfd;
    up.name = name;
    up.tmp = ".rtunnel-upload/" + upload_id + ".part";
    up.size = size;
    up.started = time(nullptr);
    int fd = ::openat(dfd, up.tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        int e = errno;
        ::close(dfd);
        err = msg("create temp file", e);
        return errCode(e);
    }
    up.fd = fd;
    uploads_[upload_id] = up;
    offset = 0;
    return OK;
}

int Service::uploadWrite(const std::string& upload_id, uint64_t offset, const char* data, size_t n, uint64_t& received, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = uploads_.find(upload_id);
    if (it == uploads_.end()) {
        err = "upload session not found";
        return NOT_FOUND;
    }
    Upload& up = it->second;
    received = up.offset;
    if (offset != up.offset) {
        err = "offset mismatch";
        return CONFLICT;
    }
    if (up.offset + n > up.size) {
        err = "write beyond declared size";
        return TOO_LARGE;
    }
    int fd = up.fd;
    if (fd < 0) {
        err = "upload session closed";
        return IO_ERROR;
    }
    size_t done = 0;
    while (done < n) {
        ssize_t w = ::pwrite(fd, data + done, n - done, (off_t)(offset + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            err = msg("write", e);
            return errCode(e);
        }
        done += (size_t)w;
    }
    up.offset += n;
    received = up.offset;
    return OK;
}

int Service::uploadStatus(const std::string& upload_id, uint64_t& offset, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = uploads_.find(upload_id);
    if (it == uploads_.end()) {
        err = "upload session not found";
        return NOT_FOUND;
    }
    offset = it->second.offset;
    return OK;
}

int Service::uploadFinish(const std::string& upload_id, uint64_t& size, int64_t& mtime, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = uploads_.find(upload_id);
    if (it == uploads_.end()) {
        err = "upload session not found";
        return NOT_FOUND;
    }
    Upload up = it->second;
    if (up.offset != up.size) {
        err = "upload incomplete";
        return CONFLICT;
    }
    int fd = up.fd;
    if (fd < 0) {
        err = "upload session closed";
        return IO_ERROR;
    }
    if (::fsync(fd) != 0) {
        int e = errno;
        err = msg("fsync", e);
        return errCode(e);
    }
    ::fchmod(fd, 0644);
    ::close(fd);
    up.fd = -1;
    it->second.fd = -1;
    if (::renameat(up.dir_fd, up.tmp.c_str(), up.dir_fd, up.name.c_str()) != 0) {
        int e = errno;
        ::unlinkat(up.dir_fd, up.tmp.c_str(), 0);
        ::close(up.dir_fd);
        uploads_.erase(it);
        err = msg("rename into place", e);
        return errCode(e);
    }
    ::fsync(up.dir_fd);
    struct stat st;
    if (::fstatat(up.dir_fd, up.name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        size = (uint64_t)st.st_size;
        mtime = (int64_t)st.st_mtime;
    }
    else {
        size = up.size;
        mtime = (int64_t)time(nullptr);
    }
    ::close(up.dir_fd);
    uploads_.erase(it);
    return OK;
}

int Service::uploadCancel(const std::string& upload_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = uploads_.find(upload_id);
    if (it == uploads_.end()) return NOT_FOUND;
    if (it->second.fd >= 0) {
        ::close(it->second.fd);
        it->second.fd = -1;
    }
    if (it->second.dir_fd >= 0) {
        ::unlinkat(it->second.dir_fd, it->second.tmp.c_str(), 0);
        ::close(it->second.dir_fd);
    }
    uploads_.erase(it);
    return OK;
}

} // namespace rtfs
