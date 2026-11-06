// 证书面：台账查询 / OpenSSL 签发 / 吊销（黑名单即刻生效）
#include "admin_server.hpp"
#include "cpputil/json.hpp"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>

using nlohmann::json;

#if defined(WITH_OPENSSL)
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/ec.h>

static std::string bioToString(BIO* b) {
    char* p = NULL;
    long n = BIO_get_mem_data(b, &p);
    return std::string(p, n);
}

static bool loadCA(const AdminConfig& cfg, X509** ca, EVP_PKEY** caKey) {
    *ca = NULL;
    *caKey = NULL;
    FILE* fp = fopen(cfg.ca_cert.c_str(), "r");
    if (!fp) return false;
    *ca = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);
    fp = fopen(cfg.ca_key.c_str(), "r");
    if (!fp) { X509_free(*ca); *ca = NULL; return false; }
    *caKey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);
    return *ca && *caKey;
}

// CSR 签名：私钥不出设备，relay 只做 CA 签名。身份取 CSR 的 SAN（缺省回退 CN），
// 签出的证书统一写入 SAN，保证能通过 relay 的 SAN==身份校验。
static bool validSan(const std::string& s);
static int signCsr(const AdminConfig& cfg, const std::string& csrPem, int days,
                   std::string& sanOut, std::string& certPem, unsigned long long& serial) {
    BIO* in = BIO_new_mem_buf(csrPem.data(), (int)csrPem.size());
    X509_REQ* req = PEM_read_bio_X509_REQ(in, NULL, NULL, NULL);
    BIO_free(in);
    if (!req) return -1;

    int rc = -1;
    X509* ca = NULL;
    EVP_PKEY* caKey = NULL;
    EVP_PKEY* pkey = NULL;
    X509* x = NULL;
    BIO* out = NULL;
    STACK_OF(X509_EXTENSION)* exts = NULL;

    do {
        if (X509_REQ_verify(req, X509_REQ_get_pubkey(req)) != 1) break;   // CSR 自签名校验

        exts = X509_REQ_get_extensions(req);
        if (exts) {
            for (int i = 0; i < sk_X509_EXTENSION_num(exts); ++i) {
                X509_EXTENSION* e = sk_X509_EXTENSION_value(exts, i);
                if (OBJ_obj2nid(X509_EXTENSION_get_object(e)) != NID_subject_alt_name) continue;
                BIO* b = BIO_new(BIO_s_mem());
                X509V3_EXT_print(b, e, 0, 0);
                char* p = NULL;
                long n = BIO_get_mem_data(b, &p);
                std::string s(p, n);
                size_t pos = s.find("DNS:");
                if (pos != std::string::npos) {
                    sanOut = s.substr(pos + 4);
                    size_t end = sanOut.find_first_of(", \n");
                    if (end != std::string::npos) sanOut = sanOut.substr(0, end);
                }
                BIO_free(b);
                break;
            }
        }
        if (sanOut.empty()) {
            // 回退：用 subject CN 作为身份
            X509_NAME* subj = X509_REQ_get_subject_name(req);
            int idx = X509_NAME_get_index_by_NID(subj, NID_commonName, -1);
            if (idx >= 0) {
                ASN1_STRING* cn = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subj, idx));
                sanOut.assign((const char*)ASN1_STRING_get0_data(cn), ASN1_STRING_length(cn));
            }
        }
        if (!validSan(sanOut)) break;

        pkey = X509_REQ_get_pubkey(req);
        if (!pkey) break;
        if (!loadCA(cfg, &ca, &caKey)) break;

        x = X509_new();
        if (!x) break;
        X509_set_version(x, 2);
        serial = (unsigned long long)time(NULL);
        ASN1_INTEGER_set_uint64(X509_get_serialNumber(x), serial);
        X509_gmtime_adj(X509_get_notBefore(x), 0);
        X509_gmtime_adj(X509_get_notAfter(x), (long)days * 86400);
        X509_NAME_add_entry_by_txt(X509_get_subject_name(x), "CN", MBSTRING_ASC,
                                   (const unsigned char*)sanOut.c_str(), -1, -1, 0);
        X509_set_issuer_name(x, X509_get_subject_name(ca));
        {
            X509V3_CTX v3ctx;
            X509V3_set_ctx_nodb(&v3ctx);
            X509V3_set_ctx(&v3ctx, ca, x, NULL, NULL, 0);
            std::string sanExt = "DNS:" + sanOut;
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &v3ctx, NID_subject_alt_name, sanExt.c_str());
            if (ext) {
                X509_add_ext(x, ext, -1);
                X509_EXTENSION_free(ext);
            }
        }
        X509_set_pubkey(x, pkey);
        if (!X509_sign(x, caKey, EVP_sha256())) break;

        out = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(out, x);
        certPem = bioToString(out);
        rc = 0;
    } while (0);

    if (out) BIO_free(out);
    if (x) X509_free(x);
    if (pkey) EVP_PKEY_free(pkey);
    if (ca) X509_free(ca);
    if (caKey) EVP_PKEY_free(caKey);
    if (exts) sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
    X509_REQ_free(req);
    return rc;
}

static int issueCert(const AdminConfig& cfg, const std::string& san, int days,
                     std::string& certPem, std::string& keyPem,
                     unsigned long long& serial) {
    int rc = -1;
    X509* ca = NULL;
    EVP_PKEY* caKey = NULL;
    X509* x = NULL;
    EVP_PKEY* pkey = NULL;
    EC_KEY* ec = NULL;
    BIO* bio = NULL;

    FILE* fp = fopen(cfg.ca_cert.c_str(), "r");
    if (!fp) return -1;
    ca = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);
    fp = fopen(cfg.ca_key.c_str(), "r");
    if (!fp) { X509_free(ca); return -1; }
    caKey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);
    if (!ca || !caKey) goto done;

    ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!ec || !EC_KEY_generate_key(ec)) goto done;
    pkey = EVP_PKEY_new();
    if (!pkey || !EVP_PKEY_assign_EC_KEY(pkey, ec)) goto done;   // pkey 接管 ec
    ec = NULL;

    x = X509_new();
    if (!x) goto done;
    X509_set_version(x, 2);
    serial = (unsigned long long)time(NULL);
    ASN1_INTEGER_set_uint64(X509_get_serialNumber(x), serial);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), (long)days * 86400);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(x), "CN", MBSTRING_ASC,
                               (const unsigned char*)san.c_str(), -1, -1, 0);
    X509_set_issuer_name(x, X509_get_subject_name(ca));
    {
        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        X509V3_set_ctx(&v3ctx, ca, x, NULL, NULL, 0);
        std::string sanExt = "DNS:" + san;
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &v3ctx, NID_subject_alt_name, sanExt.c_str());
        if (ext) {
            X509_add_ext(x, ext, -1);
            X509_EXTENSION_free(ext);
        }
    }
    X509_set_pubkey(x, pkey);
    if (!X509_sign(x, caKey, EVP_sha256())) goto done;

    bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, x);
    certPem = bioToString(bio);
    BIO_free(bio);
    bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL);
    keyPem = bioToString(bio);
    rc = 0;

done:
    if (bio) BIO_free(bio);
    if (ec) EC_KEY_free(ec);
    if (pkey) EVP_PKEY_free(pkey);
    if (x) X509_free(x);
    if (ca) X509_free(ca);
    if (caKey) EVP_PKEY_free(caKey);
    return rc;
}
#endif

// devid 命名规则（同 A 侧）：字母开头，[A-Za-z0-9._-]{0,63}
static bool validSan(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    if (!isalpha((unsigned char)s[0])) return false;
    for (char c : s) {
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}

void AdminServer::handleCerts(const HttpContextPtr& ctx) {
    std::string body = callRelay([this]() {
        json arr = json::array();
        uint64_t now = hloop_now_ms(relay_->loop);
        FILE* fp = fopen((relay_->cert_dir + "/index.txt").c_str(), "r");
        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                char san[128] = {0}, role[32] = {0};
                unsigned long long serial = 0, issue = 0, expire = 0;
                if (sscanf(line, "%127[^|]|%31[^|]|%llu|%llu|%llu", san, role, &serial, &issue, &expire) != 5) continue;
                json j;
                j["san"] = san;
                j["role"] = role;
                j["serial"] = serial;
                j["issue_ms"] = issue;
                j["expire_ms"] = expire;
                if (relay_->isRevoked(san)) j["status"] = "revoked";
                else if (expire && expire < now) j["status"] = "expired";
                else if (expire && expire < now + 30ULL * 86400 * 1000) j["status"] = "expiring";
                else j["status"] = "valid";
                arr.push_back(j);
            }
            fclose(fp);
        }
        return arr.dump();
    });
    ctx->send(body);
}

void AdminServer::handleCertIssue(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    std::string san = req.value("san", "");
    std::string certRole = req.value("role", "device");
    int days = req.value("days", 30);
    if (!validSan(san) || days < 1 || days > 3650) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad san or days\"}");
        return;
    }
    if (cfg_.ca_cert.empty() || cfg_.ca_key.empty() || cfg_.cert_dir.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"ca/cert-dir not configured\"}");
        return;
    }

#if defined(WITH_OPENSSL)
    std::string certPem, keyPem;
    unsigned long long serial = 0;
    if (issueCert(cfg_, san, days, certPem, keyPem, serial) != 0) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"issue failed\"}");
        return;
    }
    unsigned long long issueMs = (unsigned long long)time(NULL) * 1000;
    unsigned long long expireMs = issueMs + (unsigned long long)days * 86400 * 1000;

    // 落盘：crt/key（0600）+ index.txt 台账
    mkdir(cfg_.cert_dir.c_str(), 0700);
    std::string base = cfg_.cert_dir + "/" + san;
    FILE* fp = fopen((base + ".crt").c_str(), "w");
    if (fp) { fputs(certPem.c_str(), fp); fclose(fp); chmod((base + ".crt").c_str(), 0600); }
    fp = fopen((base + ".key").c_str(), "w");
    if (fp) { fputs(keyPem.c_str(), fp); fclose(fp); chmod((base + ".key").c_str(), 0600); }
    fp = fopen((cfg_.cert_dir + "/index.txt").c_str(), "a");
    if (fp) {
        fprintf(fp, "%s|%s|%llu|%llu|%llu\n", san.c_str(), certRole.c_str(), serial, issueMs, expireMs);
        fclose(fp);
    }

    audit(role, "certs.issue", san + " days=" + std::to_string(days));
    json j;
    j["san"] = san;
    j["serial"] = serial;
    j["expire_ms"] = expireMs;
    j["cert"] = certPem;
    j["key"] = keyPem;
    ctx->send(j.dump());
#else
    ctx->response->status_code = HTTP_STATUS_NOT_IMPLEMENTED;
    ctx->send("{\"error\":\"built without OpenSSL\"}");
#endif
}

void AdminServer::handleCertRevoke(const HttpContextPtr& ctx, const std::string& role) {    json req = json::parse(ctx->body(), nullptr, false);
    std::string san = req.value("san", "");
    if (san.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad san\"}");
        return;
    }
    callRelay([this, &san]() {
        relay_->revokeSan(san);
        return std::string();
    });
    audit(role, "certs.revoke", san);
    json j;
    j["san"] = san;
    j["revoked"] = true;
    ctx->send(j.dump());
}

void AdminServer::handleCertSign(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    std::string csrPem = req.value("csr", "");
    int days = req.value("days", 30);
    if (csrPem.empty() || days < 1 || days > 3650) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad csr or days\"}");
        return;
    }
    if (cfg_.ca_cert.empty() || cfg_.ca_key.empty() || cfg_.cert_dir.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"ca/cert-dir not configured\"}");
        return;
    }
#if defined(WITH_OPENSSL)
    std::string san, certPem;
    unsigned long long serial = 0;
    if (signCsr(cfg_, csrPem, days, san, certPem, serial) != 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"csr parse/verify failed or bad identity\"}");
        return;
    }
    unsigned long long issueMs = (unsigned long long)time(NULL) * 1000;
    unsigned long long expireMs = issueMs + (unsigned long long)days * 86400 * 1000;

    // 只落 crt（私钥不出设备，relay 不接触）
    mkdir(cfg_.cert_dir.c_str(), 0700);
    std::string base = cfg_.cert_dir + "/" + san;
    FILE* fp = fopen((base + ".crt").c_str(), "w");
    if (fp) {
        fputs(certPem.c_str(), fp);
        fclose(fp);
        chmod((base + ".crt").c_str(), 0600);
    }
    fp = fopen((cfg_.cert_dir + "/index.txt").c_str(), "a");
    if (fp) {
        fprintf(fp, "%s|%s|%llu|%llu|%llu\n", san.c_str(), req.value("role", "device").c_str(),
                serial, issueMs, expireMs);
        fclose(fp);
    }

    audit(role, "certs.sign", san + " days=" + std::to_string(days));
    json j;
    j["san"] = san;
    j["serial"] = serial;
    j["expire_ms"] = expireMs;
    j["cert"] = certPem;
    ctx->send(j.dump());
#else
    ctx->response->status_code = HTTP_STATUS_NOT_IMPLEMENTED;
    ctx->send("{\"error\":\"built without OpenSSL\"}");
#endif
}
