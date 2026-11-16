#include "fs_archive.hpp"
#include "fs_service.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rtfs {
namespace {

const size_t BLOCK = 512;
const size_t FILE_CHUNK = 32 * 1024;
const uint64_t MAX_ENTRIES = 1000000;
const size_t MAX_DEPTH = 64;

void putStr(char* p, size_t n, const std::string& s) {
    std::memset(p, 0, n);
    std::memcpy(p, s.data(), s.size() < n ? s.size() : n);
}

void putOct(char* p, size_t n, uint64_t v) {
    if (n == 12 && v > 077777777777ull) {
        // GNU base-256，支持大于 8 GiB 的文件
        p[0] = (char)0x80;
        for (int i = 1; i <= 3; ++i) p[i] = 0;
        for (int i = 0; i < 8; ++i) p[4 + i] = (char)(v >> (8 * (7 - i)));
        return;
    }
    char tmp[32];
    std::snprintf(tmp, sizeof(tmp), "%0*llo", (int)(n - 1), (unsigned long long)v);
    std::memcpy(p, tmp, n - 1);
    p[n - 1] = '\0';
}

void setChecksum(char* h) {
    std::memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (size_t i = 0; i < BLOCK; ++i) sum += (unsigned char)h[i];
    char tmp[8];
    std::snprintf(tmp, sizeof(tmp), "%06o", sum);
    std::memcpy(h + 148, tmp, 6);
    h[154] = '\0';
    h[155] = ' ';
}

void makeHeader(char* h, const std::string& name, const std::string& prefix, const struct stat& st, char type,
                const std::string& link) {
    std::memset(h, 0, BLOCK);
    putStr(h, 100, name);
    putOct(h + 100, 8, (uint64_t)(st.st_mode & 07777));
    putOct(h + 108, 8, 0);
    putOct(h + 116, 8, 0);
    putOct(h + 124, 12, (type == '0' || type == 'L' || type == 'K') ? (uint64_t)st.st_size : 0);
    putOct(h + 136, 12, st.st_mtime > 0 ? (uint64_t)st.st_mtime : 0);
    h[156] = type;
    putStr(h + 157, 100, link);
    std::memcpy(h + 257, "ustar", 6);
    std::memcpy(h + 263, "00", 2);
    putOct(h + 329, 8, 0);
    putOct(h + 337, 8, 0);
    putStr(h + 345, 155, prefix);
    setChecksum(h);
}

bool splitName(const std::string& path, std::string& name, std::string& prefix) {
    if (path.size() <= 100) {
        name = path;
        prefix.clear();
        return true;
    }
    size_t len = path.size();
    size_t start = len > 100 ? len - 100 : 0;
    for (size_t i = start; i < len; ++i) {
        if (path[i] != '/') continue;
        size_t suffix = len - i - 1;
        if (i <= 155 && suffix > 0 && suffix <= 100) {
            prefix = path.substr(0, i);
            name = path.substr(i + 1);
            return true;
        }
    }
    return false;
}

void appendLong(std::string& out, char type, const std::string& data) {
    struct stat st;
    std::memset(&st, 0, sizeof(st));
    st.st_mode = 0644;
    st.st_size = (off_t)(data.size() + 1);
    char h[BLOCK];
    makeHeader(h, "././@LongLink", "", st, type, "");
    out.append(h, BLOCK);
    out.append(data);
    out.push_back('\0');
    size_t total = data.size() + 1;
    size_t pad = (BLOCK - (total % BLOCK)) % BLOCK;
    out.append(pad, '\0');
}

void emitEntry(std::string& out, const std::string& path, const struct stat& st, char type, const std::string& link) {
    std::string name, prefix;
    if (!splitName(path, name, prefix)) {
        appendLong(out, 'L', path);
        name = path.substr(0, 100);
        prefix.clear();
    }
    if (type == '2' && link.size() > 100) appendLong(out, 'K', link);
    char h[BLOCK];
    makeHeader(h, name, prefix, st, type, link.size() > 100 ? link.substr(0, 100) : link);
    out.append(h, BLOCK);
}

} // namespace

struct ArchiveStream::Impl {
    int top_fd = -1;
    std::string base;

    struct Frame {
        DIR* dir = nullptr;
        std::string prefix;
        Frame() {}
        Frame(DIR* d, std::string p) : dir(d), prefix(std::move(p)) {}
    };
    std::vector<Frame> stack;

    int file_fd = -1;
    uint64_t file_left = 0;
    uint64_t file_size = 0;

    bool started = false;
    bool tar_done = false;
    bool zstarted = false;
    bool zdone = false;
    bool broken = false;
    uint64_t entries = 0;
    z_stream zs;
    std::string pending;
    size_t pend_off = 0;

    ~Impl() {
        if (file_fd >= 0) ::close(file_fd);
        for (auto& f : stack)
            if (f.dir) ::closedir(f.dir);
        if (top_fd >= 0) ::close(top_fd);
        if (zstarted) deflateEnd(&zs);
    }

    bool nextPiece(std::string& out, std::string& err) {
        if (file_fd >= 0) {
            char buf[FILE_CHUNK];
            size_t want = (size_t)(file_left < FILE_CHUNK ? file_left : FILE_CHUNK);
            ssize_t r;
            do {
                r = ::read(file_fd, buf, want);
            } while (r < 0 && errno == EINTR);
            if (r < 0) {
                err = std::string("read: ") + std::strerror(errno);
                ::close(file_fd);
                file_fd = -1;
                return false;
            }
            if (r == 0) {
                // 读取过程中文件被截短，补零保持 tar 结构完整
                out.append((size_t)file_left, '\0');
                file_left = 0;
            } else {
                out.append(buf, (size_t)r);
                file_left -= (uint64_t)r;
            }
            if (file_left == 0) {
                ::close(file_fd);
                file_fd = -1;
                size_t pad = (BLOCK - (size_t)(file_size % BLOCK)) % BLOCK;
                out.append(pad, '\0');
            }
            return true;
        }

        for (;;) {
            if (stack.empty()) {
                if (!started) {
                    struct stat st;
                    if (::fstat(top_fd, &st) != 0) {
                        err = std::string("stat: ") + std::strerror(errno);
                        return false;
                    }
                    emitEntry(out, base + "/", st, '5', "");
                    int dfd = ::dup(top_fd);
                    DIR* d = dfd >= 0 ? ::fdopendir(dfd) : nullptr;
                    if (d == nullptr) {
                        if (dfd >= 0) ::close(dfd);
                        err = std::string("opendir: ") + std::strerror(errno);
                        return false;
                    }
                    stack.push_back(Frame{d, base});
                    started = true;
                    return true;
                }
                out.append(2 * BLOCK, '\0');
                tar_done = true;
                return true;
            }

            Frame& f = stack.back();
            struct dirent* de = ::readdir(f.dir);
            if (de == nullptr) {
                ::closedir(f.dir);
                f.dir = nullptr;
                stack.pop_back();
                continue;
            }
            std::string nm = de->d_name;
            if (nm == "." || nm == "..") continue;
            if (++entries > MAX_ENTRIES) {
                err = "too many entries";
                return false;
            }
            struct stat st;
            if (::fstatat(::dirfd(f.dir), nm.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
            std::string path = f.prefix.empty() ? nm : f.prefix + "/" + nm;

            if (S_ISDIR(st.st_mode)) {
                int cfd = ::openat(::dirfd(f.dir), nm.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (cfd < 0) continue;
                DIR* cd = ::fdopendir(cfd);
                if (cd == nullptr) {
                    ::close(cfd);
                    continue;
                }
                if (stack.size() >= MAX_DEPTH) {
                    ::closedir(cd);
                    err = "directory too deep";
                    return false;
                }
                emitEntry(out, path + "/", st, '5', "");
                stack.push_back(Frame{cd, path});
                return true;
            }
            if (S_ISREG(st.st_mode)) {
                if (st.st_size == 0) {
                    emitEntry(out, path, st, '0', "");
                    return true;
                }
                int fd = ::openat(::dirfd(f.dir), nm.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0) continue;
                emitEntry(out, path, st, '0', "");
                file_fd = fd;
                file_left = (uint64_t)st.st_size;
                file_size = (uint64_t)st.st_size;
                return true;
            }
            if (S_ISLNK(st.st_mode)) {
                char lb[4096];
                ssize_t n = ::readlinkat(::dirfd(f.dir), nm.c_str(), lb, sizeof(lb) - 1);
                if (n < 0) continue;
                std::string target(lb, (size_t)n);
                emitEntry(out, path, st, '2', target);
                return true;
            }
            // fifo/socket/device 不打包
        }
    }
};

ArchiveStream::ArchiveStream(int dir_fd, std::string base_name) {
    impl_ = new Impl();
    impl_->top_fd = dir_fd;
    impl_->base = base_name.empty() || base_name == "/" ? "root" : base_name;
    std::memset(&impl_->zs, 0, sizeof(impl_->zs));
    if (deflateInit2(&impl_->zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        impl_->broken = true;
    } else {
        impl_->zstarted = true;
    }
}

ArchiveStream::~ArchiveStream() {
    delete impl_;
    impl_ = nullptr;
}

int ArchiveStream::read(char* buf, size_t cap, size_t& n, std::string& err) {
    n = 0;
    if (impl_ == nullptr) return OK;
    Impl& p = *impl_;
    if (p.broken) {
        err = "gzip init failed";
        return IO_ERROR;
    }
    if (p.zdone && p.pending.empty()) return OK;
    while (n < cap) {
        if (p.pend_off >= p.pending.size()) {
            p.pending.clear();
            p.pend_off = 0;
            if (!p.tar_done) {
                if (!p.nextPiece(p.pending, err)) return IO_ERROR;
            }
        }
        bool finishing = p.tar_done && p.pend_off >= p.pending.size();
        p.zs.next_in = (Bytef*)(p.pending.data() + p.pend_off);
        p.zs.avail_in = (uInt)(p.pending.size() - p.pend_off);
        size_t room = cap - n;
        p.zs.next_out = (Bytef*)(buf + n);
        p.zs.avail_out = (uInt)room;
        int rc = deflate(&p.zs, finishing ? Z_FINISH : Z_NO_FLUSH);
        size_t produced = room - p.zs.avail_out;
        n += produced;
        p.pend_off = p.pending.size() - p.zs.avail_in;
        if (rc == Z_STREAM_END) {
            p.zdone = true;
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            err = "gzip compress failed";
            return IO_ERROR;
        }
        if (produced == 0 && finishing) {
            err = "gzip finalize failed";
            return IO_ERROR;
        }
    }
    return OK;
}

} // namespace rtfs
