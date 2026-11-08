#include "lz4_util.hpp"
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <mutex>

namespace {
typedef int (*compress_bound_t)(int);
typedef int (*compress_t)(const char*, char*, int, int);
typedef int (*decompress_t)(const char*, char*, int, int);

struct Lz4Lib {
    void* h = nullptr;
    compress_bound_t bound = nullptr;
    compress_t comp = nullptr;
    decompress_t decomp = nullptr;
    bool ok() const { return h && bound && comp && decomp; }
};

Lz4Lib& lib() {
    static Lz4Lib l;
    static std::once_flag once;
    std::call_once(once, []() {
        const char* names[] = {"liblz4.so.1", "liblz4.so"};
        for (auto n : names) {
            l.h = dlopen(n, RTLD_LAZY | RTLD_LOCAL);
            if (l.h) break;
        }
        if (!l.h) return;
        l.bound = (compress_bound_t)dlsym(l.h, "LZ4_compressBound");
        l.comp = (compress_t)dlsym(l.h, "LZ4_compress_default");
        l.decomp = (decompress_t)dlsym(l.h, "LZ4_decompress_safe");
        if (!l.ok()) { dlclose(l.h); l.h = nullptr; }
    });
    return l;
}
}

bool lz4Available() { return lib().ok(); }

bool lz4Compress(const std::string& in, std::string& out) {
    auto& l = lib();
    if (!l.ok() || in.empty()) return false;
    int cap = l.bound((int)in.size());
    if (cap <= 0) return false;
    out.resize(4 + cap);
    int n = l.comp(in.data(), &out[4], (int)in.size(), cap);
    if (n <= 0) return false;
    out[0] = (char)(in.size() & 0xFF);
    out[1] = (char)((in.size() >> 8) & 0xFF);
    out[2] = (char)((in.size() >> 16) & 0xFF);
    out[3] = (char)((in.size() >> 24) & 0xFF);
    out.resize(4 + n);
    return true;
}

bool lz4Decompress(const char* data, size_t len, std::string& out) {
    auto& l = lib();
    if (!l.ok() || len < 5) return false;
    uint32_t usize = (uint8_t)data[0] | ((uint32_t)(uint8_t)data[1] << 8) |
                     ((uint32_t)(uint8_t)data[2] << 16) | ((uint32_t)(uint8_t)data[3] << 24);
    if (usize == 0 || usize > 64 * 1024 * 1024) return false;
    out.resize(usize);
    int n = l.decomp(data + 4, &out[0], (int)(len - 4), (int)usize);
    if (n != (int)usize) { out.clear(); return false; }
    return true;
}
