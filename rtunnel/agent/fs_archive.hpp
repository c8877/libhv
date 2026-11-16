#pragma once
// 目录打包下载：把目录树按 ustar 格式写成 tar，再用 gzip 压缩成流。
// 只读遍历，不跟随符号链接；数据由调用方按块取走，不落临时文件。

#include <stdint.h>
#include <string>

namespace rtfs {

class ArchiveStream {
public:
    ArchiveStream(int dir_fd, std::string base_name);
    ~ArchiveStream();
    ArchiveStream(const ArchiveStream&) = delete;
    ArchiveStream& operator=(const ArchiveStream&) = delete;

    // 读取最多 cap 字节压缩数据；n==0 表示流结束。成功返回 OK。
    int read(char* buf, size_t cap, size_t& n, std::string& err);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace rtfs
