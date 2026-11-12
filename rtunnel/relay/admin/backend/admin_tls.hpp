#pragma once
// 管理台 HTTPS 的自签证书：首次启动生成，快到期时自动重签。
// 仅在 WITH_OPENSSL 构建下可用；其余情况返回 false + err。

#include <string>

// 构建是否带 TLS 后端（编译期 + libhv 运行期都在）
bool admin_tls_supported();

// 确保 dir 下有可用的 admin.crt/admin.key：
//   - 已存在且剩余有效期 >= renew_days：直接复用
//   - 否则生成自签证书（EC P-256，SAN 覆盖 localhost/hostname/本机 IP/extra_host）
// 目录权限 0700，私钥 0600。成功返回 true 并回填路径。
bool admin_tls_ensure_selfsigned(const std::string& dir, const std::string& extra_host,
                                 int days, int renew_days,
                                 std::string& cert_path, std::string& key_path,
                                 std::string& err);
