// 管理台 HTTPS 自签证书：生成 / 复用 / 续期。
#include "admin_tls.hpp"

#include "common/logging.hpp"
#include "hssl.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(WITH_OPENSSL)
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <vector>

static std::string sslErr() {
    unsigned long e = ERR_get_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    return buf[0] ? buf : "unknown openssl error";
}

static bool mkdirP(const std::string& dir, std::string& err) {
    if (dir.empty()) { err = "empty directory"; return false; }
    std::string cur;
    size_t pos = 0;
    while (pos <= dir.size()) {
        size_t slash = dir.find('/', pos);
        cur = dir.substr(0, slash == std::string::npos ? dir.size() : slash);
        if (!cur.empty() && mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) {
            err = "mkdir " + cur + ": " + strerror(errno);
            return false;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

static bool writeFile0600(const std::string& path, const std::string& data, std::string& err) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { err = "open " + tmp + ": " + strerror(errno); return false; }
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n <= 0) {
            err = "write " + tmp + ": " + strerror(errno);
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        off += (size_t)n;
    }
    close(fd);
    chmod(tmp.c_str(), 0600);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        err = "rename " + tmp + ": " + strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// 证书还在有效期且剩余天数 >= renew_days
static bool certStillGood(const std::string& path, int renew_days) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return false;
    X509* x = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);
    if (!x) return false;
    int pday = 0, psec = 0;
    bool ok = ASN1_TIME_diff(&pday, &psec, NULL, X509_getm_notAfter(x)) == 1 && pday >= renew_days;
    X509_free(x);
    return ok;
}

static std::string toPem(X509* x, EVP_PKEY* key, std::string& certPem, std::string& keyPem) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return sslErr();
    PEM_write_bio_X509(bio, x);
    char* p = NULL;
    long n = BIO_get_mem_data(bio, &p);
    certPem.assign(p ? p : "", n > 0 ? (size_t)n : 0);
    BIO_free(bio);

    bio = BIO_new(BIO_s_mem());
    if (!bio) return sslErr();
    PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL);
    p = NULL;
    n = BIO_get_mem_data(bio, &p);
    keyPem.assign(p ? p : "", n > 0 ? (size_t)n : 0);
    BIO_free(bio);
    return "";
}

static bool isIpLiteral(const std::string& host, bool& v6) {
    unsigned char buf[16];
    if (inet_pton(AF_INET, host.c_str(), buf) == 1) { v6 = false; return true; }
    if (inet_pton(AF_INET6, host.c_str(), buf) == 1) { v6 = true; return true; }
    return false;
}

static void appendSan(std::string& san, const std::string& item) {
    if (!san.empty()) san += ",";
    san += item;
}

// SAN：localhost + 主机名 + 本机所有 IP + 监听地址
static std::string buildSan(const std::string& extra_host) {
    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0) hostname[0] = '\0';

    std::string san;
    appendSan(san, "DNS:localhost");
    if (hostname[0] && strcmp(hostname, "localhost") != 0) {
        appendSan(san, std::string("DNS:") + hostname);
    }
    if (!extra_host.empty()) {
        bool v6 = false;
        if (isIpLiteral(extra_host, v6)) {
            appendSan(san, "IP:" + extra_host);
        } else if (extra_host != "localhost" && extra_host != "0.0.0.0" && extra_host != "::") {
            appendSan(san, "DNS:" + extra_host);
        }
    }
    appendSan(san, "IP:127.0.0.1");
    appendSan(san, "IP:::1");

    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        std::vector<std::string> seen;
        seen.push_back("127.0.0.1");   // 上面已加入 SAN
        seen.push_back("::1");
        for (struct ifaddrs* i = ifa; i; i = i->ifa_next) {
            if (!i->ifa_addr) continue;
            char buf[INET6_ADDRSTRLEN] = {0};
            if (i->ifa_addr->sa_family == AF_INET) {
                auto* sin = (struct sockaddr_in*)i->ifa_addr;
                if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
            } else if (i->ifa_addr->sa_family == AF_INET6) {
                auto* sin6 = (struct sockaddr_in6*)i->ifa_addr;
                if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
                if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) continue;
                if (!inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) continue;
            } else {
                continue;
            }
            std::string ip = buf;
            bool dup = false;
            for (auto& s : seen) {
                if (s == ip) { dup = true; break; }
            }
            if (dup) continue;
            seen.push_back(ip);
            appendSan(san, "IP:" + ip);
        }
        freeifaddrs(ifa);
    }
    return san;
}

static bool generate(const std::string& dir, const std::string& extra_host, int days,
                     std::string& cert_path, std::string& key_path, std::string& err) {
    EVP_PKEY* pkey = NULL;
    X509* x = NULL;
    bool ok = false;
    char hostname[256] = {0};

    EC_KEY* ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!ec) { err = "EC key init failed: " + sslErr(); return false; }
    if (!EC_KEY_generate_key(ec)) { err = "EC keygen failed: " + sslErr(); EC_KEY_free(ec); return false; }
    pkey = EVP_PKEY_new();
    if (!pkey || !EVP_PKEY_assign_EC_KEY(pkey, ec)) {
        err = "EVP_PKEY init failed: " + sslErr();
        EC_KEY_free(ec);
        if (pkey) EVP_PKEY_free(pkey);
        return false;
    }
    ec = NULL;   // pkey 接管

    x = X509_new();
    if (!x) { err = "X509_new failed"; goto done; }
    X509_set_version(x, 2);
    {
        unsigned char rnd[8] = {0};
        RAND_bytes(rnd, sizeof(rnd));
        unsigned long long serial = (unsigned long long)time(NULL);
        for (size_t i = 0; i < sizeof(rnd); ++i) serial = (serial << 8) ^ rnd[i];
        serial &= 0x7fffffffffffffffULL;
        ASN1_INTEGER_set_uint64(X509_get_serialNumber(x), serial);
    }
    X509_gmtime_adj(X509_getm_notBefore(x), -300);   // 容忍时钟偏差
    X509_gmtime_adj(X509_getm_notAfter(x), (long)days * 86400);

    if (gethostname(hostname, sizeof(hostname) - 1) != 0 || !hostname[0]) {
        snprintf(hostname, sizeof(hostname), "rtunnel-admin");
    }
    X509_NAME_add_entry_by_txt(X509_get_subject_name(x), "CN", MBSTRING_ASC,
                               (const unsigned char*)hostname, -1, -1, 0);
    X509_set_issuer_name(x, X509_get_subject_name(x));   // 自签：issuer = subject
    X509_set_pubkey(x, pkey);

    {
        X509V3_CTX v3;
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, x, x, NULL, NULL, 0);
        struct Ext { int nid; const char* value; };
        const Ext exts[] = {
            {NID_basic_constraints, "critical,CA:FALSE"},
            {NID_key_usage, "critical,digitalSignature,keyEncipherment"},
            {NID_ext_key_usage, "serverAuth"},
        };
        for (const auto& e : exts) {
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &v3, e.nid, e.value);
            if (!ext) continue;
            X509_add_ext(x, ext, -1);
            X509_EXTENSION_free(ext);
        }
        std::string san = buildSan(extra_host);
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &v3, NID_subject_alt_name, san.c_str());
        if (!ext) { err = "SAN ext failed: " + sslErr(); goto done; }
        X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
    }

    if (!X509_sign(x, pkey, EVP_sha256())) { err = "X509_sign failed: " + sslErr(); goto done; }

    {
        std::string certPem, keyPem, werr;
        std::string pemErr = toPem(x, pkey, certPem, keyPem);
        if (!pemErr.empty()) { err = "PEM encode failed: " + pemErr; goto done; }
        if (!writeFile0600(key_path, keyPem, werr)) { err = werr; goto done; }
        if (!writeFile0600(cert_path, certPem, werr)) { err = werr; goto done; }
    }
    ok = true;

done:
    if (x) X509_free(x);
    if (pkey) EVP_PKEY_free(pkey);
    return ok;
}
#endif  // WITH_OPENSSL

bool admin_tls_supported() {
#if defined(WITH_OPENSSL)
    return HV_WITH_SSL;
#else
    return false;
#endif
}

bool admin_tls_ensure_selfsigned(const std::string& dir, const std::string& extra_host,
                                 int days, int renew_days,
                                 std::string& cert_path, std::string& key_path,
                                 std::string& err) {
#if !defined(WITH_OPENSSL)
    (void)dir; (void)extra_host; (void)days; (void)renew_days;
    (void)cert_path; (void)key_path;
    err = "built without OpenSSL (WITH_OPENSSL=OFF)";
    return false;
#else
    cert_path = dir + "/admin.crt";
    key_path = dir + "/admin.key";
    if (days <= 0) days = 3650;
    if (renew_days <= 0) renew_days = 30;
    if (!mkdirP(dir, err)) return false;
    chmod(dir.c_str(), 0700);

    bool cert_ok = certStillGood(cert_path, renew_days);
    bool key_ok = access(key_path.c_str(), R_OK) == 0;
    if (cert_ok && key_ok) {
        logf_("[admin-tls] reuse %s", cert_path.c_str());
        return true;
    }

    logf_("[admin-tls] generating self-signed cert in %s (days=%d)", dir.c_str(), days);
    if (!generate(dir, extra_host, days, cert_path, key_path, err)) return false;
    logf_("[admin-tls] generated %s (SAN: localhost/hostname/local IPs)", cert_path.c_str());
    return true;
#endif
}

bool admin_tls_cert_days_left(const std::string& cert_path, int& days) {
#if !defined(WITH_OPENSSL)
    (void)cert_path; (void)days;
    return false;
#else
    FILE* fp = fopen(cert_path.c_str(), "r");
    if (!fp) return false;
    X509* x = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);
    if (!x) return false;
    int pday = 0, psec = 0;
    bool ok = ASN1_TIME_diff(&pday, &psec, NULL, X509_getm_notAfter(x)) == 1;
    X509_free(x);
    days = pday;
    return ok;
#endif
}
