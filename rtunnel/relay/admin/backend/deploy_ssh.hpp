#pragma once
// 部署用的 SSH 客户端：libssh2 直连，不依赖目标 relay 上的 ssh/scp/sshpass。
//
// 只做部署需要的三件事：执行命令、可选写 stdin（sudo -S）、SFTP 上传文件。
// 认证支持密码和私钥；主机密钥按 known_hosts 做 accept-new，指纹变了直接拒绝。

#include <functional>
#include <string>

struct DeploySshTarget {
    std::string host;
    int         port = 22;
    std::string user;
    std::string auth;        // password / key
    std::string secret;      // 密码或私钥内容
    std::string known_hosts; // 首次连接时自动追加
};

struct DeploySshResult {
    bool        ok = false;
    int         status = -1;   // 远端退出码；-1 表示没跑起来
    std::string out;           // stdout + stderr（合并，便于看日志）
    std::string err;           // 本地侧错误（连接/认证/超时/取消）
};

class DeploySsh {
public:
    // cancel 返回 true 时尽快中断；deadline_s 是整次调用的墙钟上限
    using CancelFn = std::function<bool()>;

    DeploySsh() = default;
    ~DeploySsh();
    DeploySsh(const DeploySsh&) = delete;
    DeploySsh& operator=(const DeploySsh&) = delete;

    bool connect(const DeploySshTarget& t, std::string& err);

    // 远端命令用登录用户的 shell 执行（同 ssh host 'cmd'）。
    // stdin_data 非空时先写入再关 stdin，用于 sudo -S。
    DeploySshResult exec(const std::string& cmd, const std::string& stdin_data,
                         int timeout_s, const CancelFn& cancel);

    // SFTP 上传（覆盖同名文件，0600）；大文件按块写，带超时与取消
    bool upload(const std::string& local, const std::string& remote,
                int timeout_s, const CancelFn& cancel, std::string& err);

    void close();
    const std::string& lastError() const { return last_error_; }

private:
    void*       session_ = nullptr;
    void*       sftp_ = nullptr;
    int         sock_ = -1;
    std::string last_error_;
    bool        waitSocket(long ms);
};
