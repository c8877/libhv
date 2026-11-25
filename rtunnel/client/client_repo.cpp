// rtunnel-client repo：管理台文件仓库的增删查传。
//
// 只走管理面 HTTP(S)（默认 UTF-8 JSON / 原始字节），不建立隧道，所以不依赖
// 设备在线；atk- 授权，权限就是令牌所属账号的 repo.read / repo.write / repo.delete。
//
// 复用的服务端接口（不新增协议）：
//   POST   /api/v1/files/:name?action=mkdir&dir=P
//   POST   /api/v1/files/:name?dir=P[&upload=UID&append=1&offset=N|&finalize=1]
//   GET    /api/v1/files/:name?dir=P                      （支持单段 Range）
//   GET    /api/v1/files?dir=P                            （size/sha256/mtime）
//   GET    /api/v1/uploads/:name?dir=P&upload=UID         （已收到字节数）
//   DELETE /api/v1/files/:name?dir=P
#include "client_repo.hpp"

#include "common/sha256.hpp"
#include "cpputil/json.hpp"

#include "http/client/HttpClient.h"
#include "hssl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

const size_t kChunkDefault = 8 * 1024 * 1024;   // 服务端单次 POST 上限就是 8 MiB
const size_t kChunkMax = 8 * 1024 * 1024;

struct RepoOpts {
    std::string rt;          // https://host:9443
    std::string atk;         // atk-...
    std::string atk_file;
    std::string dir;         // 仓库内目录，不带首尾 /
    std::string name;        // 文件名
    std::string file;        // put 的本地文件
    std::string out;         // get 的输出路径
    std::string tls_ca;
    size_t      chunk = kChunkDefault;
    bool        insecure = false;
    bool        force = false;
    bool        resume = false;
    bool        json_out = false;
    bool        quiet = false;
    int         timeout = 60;
};

void usage() {
    fprintf(stderr,
        "用法: rtunnel-client repo <子命令> --rt URL [--atk-file F | --atk T] [选项]\n"
        "\n"
        "子命令:\n"
        "  mkdir --dir D                 递归创建仓库目录（已存在视为成功）\n"
        "  put   --file F [--dir D]      上传文件（分片、断点续传；默认跳过同 sha256）\n"
        "  get   --dir D --name N --out P  下载文件（支持 --resume 续传，落盘后校验 sha256）\n"
        "  stat  --dir D --name N        查看远端大小与 sha256（--json）\n"
        "  ls    --dir D [--json]        列出目录\n"
        "  rm    --dir D --name N        删除文件\n"
        "\n"
        "通用选项:\n"
        "  --rt URL       管理面地址，例如 https://192.168.1.133:9443（也可用环境变量 RT）\n"
        "  --atk T        atk- 管理令牌（也可用 --atk-file 或环境变量 ATK）\n"
        "  --atk-file F   读取令牌的文件（取第一行非空内容，权限应为 0600）\n"
        "  --tls-ca F     HTTPS 自签证书的 CA\n"
        "  --insecure     HTTPS 跳过证书校验（仅调试）\n"
        "  --chunk BYTES  分片大小，默认 8MiB，上限 8MiB\n"
        "  --timeout S    单次请求超时，默认 60s\n"
        "  --force        put 时同 sha256 也重传\n"
        "  --resume       get 时从已有文件的长度继续\n"
        "  --quiet        不打印进度\n"
        "\n"
        "退出码: 0 成功, 2 参数错误, 3 缺少令牌, 4 请求/服务端失败, 5 校验失败\n");
}

bool isFile(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

long long fileSize(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return -1;
    return (long long)st.st_size;
}

std::string baseName(const std::string& p) {
    size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string urlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

// 仓库目录：去掉多余的 /，并拒绝 .. 与绝对路径
bool normDir(const std::string& in, std::string& out, std::string& err) {
    std::string s = trim(in);
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.find("..") != std::string::npos) { err = "目录不能包含 .."; return false; }
    out = s;
    return true;
}

struct Api {
    RepoOpts opt;
    bool     https = false;

    bool init(std::string& err) {
        if (opt.rt.empty()) {
            const char* rt = getenv("RT");
            if (rt) opt.rt = rt;
        }
        if (opt.rt.empty()) { err = "缺少 --rt（或环境变量 RT）"; return false; }
        while (!opt.rt.empty() && opt.rt.back() == '/') opt.rt.pop_back();
        if (opt.rt.rfind("https://", 0) == 0) https = true;
        else if (opt.rt.rfind("http://", 0) != 0) { err = "--rt 需要 http:// 或 https:// 开头"; return false; }

        if (opt.atk.empty() && !opt.atk_file.empty()) {
            std::ifstream f(opt.atk_file);
            if (!f) { err = "读不到 --atk-file: " + opt.atk_file; return false; }
            std::string line;
            while (std::getline(f, line)) {
                line = trim(line);
                if (!line.empty()) { opt.atk = line; break; }
            }
        }
        if (opt.atk.empty()) {
            const char* atk = getenv("ATK");
            if (atk) opt.atk = atk;
        }
        if (opt.atk.empty()) { err = "缺少管理令牌：--atk-file / --atk / ATK"; return false; }
        if (opt.atk.rfind("atk-", 0) != 0) { err = "管理令牌需要 atk- 前缀（rtk- 不能访问文件仓库）"; return false; }
        return true;
    }

    // 返回 HTTP 状态码；0 表示传输层失败（err 里带原因）
    int call(const char* method, const std::string& path_and_query, const std::string& body,
             std::string& resp_body, std::string& err) {
        hv::HttpClient cli;
        if (https && (opt.insecure || !opt.tls_ca.empty())) {
            hssl_ctx_opt_t ssl_opt;
            memset(&ssl_opt, 0, sizeof(ssl_opt));
            ssl_opt.endpoint = HSSL_CLIENT;
            ssl_opt.verify_peer = opt.insecure ? 0 : 1;
            if (!opt.tls_ca.empty()) ssl_opt.ca_file = opt.tls_ca.c_str();
            if (cli.newSslCtx(&ssl_opt) != 0) {
                err = "初始化 TLS 失败";
                return 0;
            }
        }

        HttpRequest req;
        req.method = http_method_enum(method);
        req.url = opt.rt + path_and_query;
        req.timeout = opt.timeout;
        req.headers["Authorization"] = "Bearer " + opt.atk;
        req.headers["Accept"] = "application/json";
        if (!body.empty()) {
            req.headers["Content-Type"] = "application/octet-stream";
            req.body = body;
        }

        HttpResponse resp;
        int ret = cli.send(&req, &resp);
        if (ret != 0) {
            err = "请求失败: " + std::string(strerror(errno ? errno : 0));
            return 0;
        }
        resp_body = resp.body;
        return resp.status_code;
    }
};

std::string errText(const std::string& body) {
    json j = json::parse(body, nullptr, false);
    if (!j.is_discarded() && j.is_object() && j.contains("error") && j["error"].is_string())
        return j["error"].get<std::string>();
    return body.size() > 200 ? body.substr(0, 200) : body;
}

bool ensureDir(Api& api, const std::string& dir, std::string& err) {
    std::string d;
    if (!normDir(dir, d, err)) return false;
    if (d.empty()) return true;
    std::string prefix;
    size_t pos = 0;
    while (pos <= d.size()) {
        size_t slash = d.find('/', pos);
        std::string comp = d.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!comp.empty()) {
            std::string parent = prefix;
            std::string q = "/api/v1/files/" + urlEncode(comp) + "?action=mkdir&dir=" + urlEncode(parent);
            std::string body;
            int code = api.call("POST", q, std::string(), body, err);
            if (code != 200 && code != 201 && code != 409) {
                err = "创建目录 " + (parent.empty() ? comp : parent + "/" + comp) + " 失败: HTTP " +
                      std::to_string(code) + " " + errText(body);
                return false;
            }
            prefix = parent.empty() ? comp : parent + "/" + comp;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

struct RemoteFile {
    bool        exists = false;
    long long   size = 0;
    long long   mtime_ms = 0;
    std::string sha256;
};

bool statRemote(Api& api, const std::string& dir, const std::string& name, RemoteFile& out,
                std::string& err) {
    std::string body;
    int code = api.call("GET", "/api/v1/files?dir=" + urlEncode(dir) + "&sort=name", std::string(), body, err);
    if (code != 200) {
        err = "列目录失败: HTTP " + std::to_string(code) + " " + errText(body);
        return false;
    }
    json arr = json::parse(body, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) { err = "服务端返回不是 JSON 数组"; return false; }
    for (auto& it : arr) {
        if (!it.is_object()) continue;
        if (it.value("name", std::string()) != name) continue;
        if (it.value("type", std::string()) != "file") continue;
        out.exists = true;
        out.size = it.value("size", (long long)0);
        out.mtime_ms = it.value("mtime_ms", (long long)0);
        out.sha256 = it.value("sha256", std::string());
        return true;
    }
    return true;
}

bool readAt(const std::string& path, size_t off, size_t len, std::string& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "打不开 " + path; return false; }
    f.seekg((std::streamoff)off);
    out.resize(len);
    f.read(&out[0], (std::streamsize)len);
    if ((size_t)f.gcount() != len) { err = "读取 " + path + " 失败"; return false; }
    return true;
}

void progress(const char* what, long long done, long long total, bool quiet) {
    if (quiet) return;
    if (total > 0) {
        fprintf(stderr, "\r%s %lld/%lld (%.1f%%)", what, done, total, 100.0 * (double)done / (double)total);
    } else {
        fprintf(stderr, "\r%s %lld", what, done);
    }
    if (total > 0 && done >= total) fputc('\n', stderr);
    fflush(stderr);
}

// ---------------------------------------------------------------- 子命令

int repoMkdir(Api& api) {
    std::string err;
    if (!ensureDir(api, api.opt.dir, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 4;
    }
    printf("ok %s\n", api.opt.dir.c_str());
    return 0;
}

int repoPut(Api& api) {
    RepoOpts& o = api.opt;
    std::string err;
    if (o.file.empty() || !isFile(o.file)) {
        fprintf(stderr, "--file 不存在或不是普通文件: %s\n", o.file.c_str());
        return 2;
    }
    if (o.name.empty()) o.name = baseName(o.file);
    std::string dir;
    if (!normDir(o.dir, dir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (!ensureDir(api, dir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 4; }

    long long size = fileSize(o.file);
    if (size <= 0) { fprintf(stderr, "空文件不上传: %s\n", o.file.c_str()); return 2; }
    std::string sha = rtutil::sha256File(o.file);
    if (sha.empty()) { fprintf(stderr, "计算 sha256 失败: %s\n", o.file.c_str()); return 5; }

    RemoteFile rf;
    if (!statRemote(api, dir, o.name, rf, err)) { fprintf(stderr, "%s\n", err.c_str()); return 4; }
    if (rf.exists && !o.force && rf.sha256 == sha && rf.size == size) {
        printf("skip %s (sha256 已一致)\n", o.name.c_str());
        return 0;
    }

    std::string uid = sha.substr(0, 16);
    const std::string base_q = "?dir=" + urlEncode(dir) + "&upload=" + uid;
    long long off = 0;
    {
        std::string body;
        int code = api.call("GET", "/api/v1/uploads/" + urlEncode(o.name) + base_q, std::string(), body, err);
        if (code == 200) {
            json j = json::parse(body, nullptr, false);
            if (!j.is_discarded()) off = j.value("received", (long long)0);
        }
    }
    if (off > size) off = 0;   // 远端残留比本地大，从头来
    if (off > 0 && !o.quiet) fprintf(stderr, "续传 %s：从 %lld 字节继续\n", o.name.c_str(), off);

    if (o.chunk == 0 || o.chunk > kChunkMax) o.chunk = kChunkMax;
    while (off < size) {
        size_t n = (size_t)((size - off < (long long)o.chunk) ? (size - off) : (long long)o.chunk);
        std::string chunk;
        if (!readAt(o.file, (size_t)off, n, chunk, err)) { fprintf(stderr, "%s\n", err.c_str()); return 4; }
        std::string body;
        int code = api.call("POST",
                            "/api/v1/files/" + urlEncode(o.name) + base_q +
                                "&append=1&offset=" + std::to_string(off),
                            chunk, body, err);
        if (code != 200) {
            fprintf(stderr, "\n上传分片失败 (offset=%lld): HTTP %d %s\n", off, code, errText(body).c_str());
            return 4;
        }
        json j = json::parse(body, nullptr, false);
        long long got = j.is_discarded() ? off + (long long)n : j.value("received", off + (long long)n);
        if (got != off + (long long)n) {
            fprintf(stderr, "\n服务端收到的字节数不符: 期望 %lld 实际 %lld\n", off + (long long)n, got);
            return 4;
        }
        off = got;
        progress("上传", off, size, o.quiet);
    }

    std::string body;
    int code = api.call("POST", "/api/v1/files/" + urlEncode(o.name) + base_q + "&finalize=1",
                        std::string(), body, err);
    if (code != 200) {
        fprintf(stderr, "落盘失败: HTTP %d %s\n", code, errText(body).c_str());
        return 4;
    }
    json j = json::parse(body, nullptr, false);
    std::string rsha = j.is_discarded() ? std::string() : j.value("sha256", std::string());
    long long rsize = j.is_discarded() ? -1 : j.value("size", (long long)-1);
    if (rsha != sha || rsize != size) {
        fprintf(stderr, "校验失败：本地 %lld/%s，远端 %lld/%s\n", size, sha.c_str(), rsize, rsha.c_str());
        return 5;
    }
    printf("ok %s/%s size=%lld sha256=%s\n", dir.c_str(), o.name.c_str(), size, sha.c_str());
    return 0;
}

int repoGet(Api& api) {
    RepoOpts& o = api.opt;
    std::string err, dir;
    if (o.name.empty() || o.out.empty()) { fprintf(stderr, "需要 --name 与 --out\n"); return 2; }
    if (!normDir(o.dir, dir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }

    RemoteFile rf;
    if (!statRemote(api, dir, o.name, rf, err)) { fprintf(stderr, "%s\n", err.c_str()); return 4; }
    if (!rf.exists) { fprintf(stderr, "仓库里没有 %s/%s\n", dir.c_str(), o.name.c_str()); return 4; }

    long long have = o.resume ? fileSize(o.out) : -1;
    if (have > rf.size) have = -1;

    std::string body;
    hv::HttpClient cli;
    if (api.https && (o.insecure || !o.tls_ca.empty())) {
        hssl_ctx_opt_t ssl_opt;
        memset(&ssl_opt, 0, sizeof(ssl_opt));
        ssl_opt.endpoint = HSSL_CLIENT;
        ssl_opt.verify_peer = o.insecure ? 0 : 1;
        if (!o.tls_ca.empty()) ssl_opt.ca_file = o.tls_ca.c_str();
        if (cli.newSslCtx(&ssl_opt) != 0) { fprintf(stderr, "初始化 TLS 失败\n"); return 4; }
    }
    HttpRequest req;
    req.method = HTTP_GET;
    req.url = o.rt + "/api/v1/files/" + urlEncode(o.name) + "?dir=" + urlEncode(dir);
    req.timeout = o.timeout;
    req.headers["Authorization"] = "Bearer " + o.atk;
    if (have > 0) req.headers["Range"] = "bytes=" + std::to_string(have) + "-";
    HttpResponse resp;
    int ret = cli.send(&req, &resp);
    if (ret != 0) { fprintf(stderr, "下载失败: 请求未完成\n"); return 4; }
    if (resp.status_code != 200 && resp.status_code != 206) {
        fprintf(stderr, "下载失败: HTTP %d %s\n", resp.status_code, errText(resp.body).c_str());
        return 4;
    }
    if (have > 0 && resp.status_code != 206) have = -1;   // 服务端没按 Range 返回，重写整个文件

    std::ofstream f(o.out, have > 0 ? (std::ios::binary | std::ios::app) : std::ios::binary);
    if (!f) { fprintf(stderr, "写不了 %s\n", o.out.c_str()); return 4; }
    f.write(resp.body.data(), (std::streamsize)resp.body.size());
    f.close();
    if (!f) { fprintf(stderr, "写入 %s 失败\n", o.out.c_str()); return 4; }
    progress("下载", (long long)resp.body.size() + (have > 0 ? have : 0), rf.size, o.quiet);

    std::string sha = rtutil::sha256File(o.out);
    long long got = fileSize(o.out);
    if (got != rf.size || (!rf.sha256.empty() && sha != rf.sha256)) {
        fprintf(stderr, "校验失败：期望 %lld/%s，实际 %lld/%s（文件保留，可 --resume 重试）\n",
                rf.size, rf.sha256.c_str(), got, sha.c_str());
        return 5;
    }
    printf("ok %s size=%lld sha256=%s\n", o.out.c_str(), got, sha.c_str());
    return 0;
}

int repoStat(Api& api) {
    std::string err, dir;
    if (api.opt.name.empty()) { fprintf(stderr, "需要 --name\n"); return 2; }
    if (!normDir(api.opt.dir, dir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    RemoteFile rf;
    if (!statRemote(api, dir, api.opt.name, rf, err)) { fprintf(stderr, "%s\n", err.c_str()); return 4; }
    if (!rf.exists) { fprintf(stderr, "not found\n"); return 4; }
    if (api.opt.json_out) {
        json j;
        j["name"] = api.opt.name;
        j["dir"] = dir;
        j["size"] = rf.size;
        j["mtime_ms"] = rf.mtime_ms;
        j["sha256"] = rf.sha256;
        printf("%s\n", j.dump().c_str());
    } else {
        printf("%s/%s size=%lld mtime_ms=%lld sha256=%s\n", dir.c_str(), api.opt.name.c_str(),
               rf.size, rf.mtime_ms, rf.sha256.c_str());
    }
    return 0;
}

int repoLs(Api& api) {
    std::string err, dir;
    if (!normDir(api.opt.dir, dir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    std::string body;
    int code = api.call("GET", "/api/v1/files?dir=" + urlEncode(dir) + "&sort=name", std::string(), body, err);
    if (code != 200) { fprintf(stderr, "列目录失败: HTTP %d %s\n", code, errText(body).c_str()); return 4; }
    json arr = json::parse(body, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) { fprintf(stderr, "服务端返回不是 JSON 数组\n"); return 4; }
    if (api.opt.json_out) {
        printf("%s\n", arr.dump().c_str());
        return 0;
    }
    for (auto& it : arr) {
        std::string type = it.value("type", std::string());
        std::string name = it.value("name", std::string());
        if (type == "dir") {
            printf("d %10s  %s/\n", "-", name.c_str());
        } else {
            printf("f %10lld  %s  %s\n", it.value("size", (long long)0), name.c_str(),
                   it.value("sha256", std::string()).c_str());
        }
    }
    return 0;
}

int repoRm(Api& api) {
    std::string err, dir;
    if (api.opt.name.empty()) { fprintf(stderr, "需要 --name\n"); return 2; }
    if (!normDir(api.opt.dir, dir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    std::string body;
    int code = api.call("DELETE", "/api/v1/files/" + urlEncode(api.opt.name) + "?dir=" + urlEncode(dir),
                        std::string(), body, err);
    if (code != 200) { fprintf(stderr, "删除失败: HTTP %d %s\n", code, errText(body).c_str()); return 4; }
    printf("ok %s/%s\n", dir.c_str(), api.opt.name.c_str());
    return 0;
}

}  // namespace

int clientRepoMain(int argc, char** argv) {
    // argv[0] == "repo"
    if (argc < 2) { usage(); return 2; }
    std::string sub = argv[1];
    if (sub == "-h" || sub == "--help" || sub == "help") { usage(); return 0; }

    RepoOpts o;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s 缺少参数\n", name);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--rt") o.rt = need("--rt");
        else if (a == "--atk") o.atk = need("--atk");
        else if (a == "--atk-file") o.atk_file = need("--atk-file");
        else if (a == "--dir") o.dir = need("--dir");
        else if (a == "--name") o.name = need("--name");
        else if (a == "--file") o.file = need("--file");
        else if (a == "--out") o.out = need("--out");
        else if (a == "--tls-ca") o.tls_ca = need("--tls-ca");
        else if (a == "--chunk") o.chunk = (size_t)strtoull(need("--chunk"), NULL, 10);
        else if (a == "--timeout") o.timeout = atoi(need("--timeout"));
        else if (a == "--insecure") o.insecure = true;
        else if (a == "--force") o.force = true;
        else if (a == "--resume") o.resume = true;
        else if (a == "--json") o.json_out = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else {
            fprintf(stderr, "未知参数: %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    Api api;
    api.opt = o;
    std::string err;
    if (!api.init(err)) { fprintf(stderr, "%s\n", err.c_str()); return 3; }

    if (sub == "mkdir") return repoMkdir(api);
    if (sub == "put") return repoPut(api);
    if (sub == "get") return repoGet(api);
    if (sub == "stat") return repoStat(api);
    if (sub == "ls") return repoLs(api);
    if (sub == "rm") return repoRm(api);

    fprintf(stderr, "未知子命令: %s\n", sub.c_str());
    usage();
    return 2;
}
