#include "ai_prompt_library.hpp"

#include "common/gzip_util.hpp"
#include "ai_prompt_library_blob.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

struct Parsed {
    std::string raw;
    std::string version;
    std::vector<AiPromptSection> sections;
};

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        unsigned char u = (unsigned char)c;
        if (u >= 'A' && u <= 'Z') c = (char)(u - 'A' + 'a');
    }
    return s;
}

// 内容指纹：改了 md 但忘了改版本号，也一定会让预热备忘失效
std::string fnvHex(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
}

std::string aiClampUtf8Local(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

bool contains(const std::string& hay, const std::string& needle) {
    return !needle.empty() && hay.find(needle) != std::string::npos;
}

// 逗号分隔（中英文逗号都认）
std::vector<std::string> splitKeywords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == '，' || c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

Parsed parse(const std::string& raw) {
    Parsed p;
    p.raw = raw;
    size_t v = raw.find("rtunnel-ai-prompt ");
    if (v != std::string::npos) {
        size_t e = raw.find("-->", v);
        if (e != std::string::npos) {
            p.version = raw.substr(v + strlen("rtunnel-ai-prompt "), e - (v + strlen("rtunnel-ai-prompt ")));
            while (!p.version.empty() && isspace((unsigned char)p.version.back())) p.version.pop_back();
        }
    }
    if (p.version.empty()) p.version = "v0";
    p.version += "-" + fnvHex(raw).substr(0, 8);

    AiPromptSection cur;
    size_t pos = 0;
    bool in = false;
    auto flush = [&] {
        if (!in) return;
        while (!cur.body.empty() && (cur.body.back() == '\n' || cur.body.back() == '\r'))
            cur.body.pop_back();
        if (!cur.body.empty()) p.sections.push_back(cur);
        cur = AiPromptSection();
        in = false;
    };
    while (pos <= raw.size()) {
        size_t nl = raw.find('\n', pos);
        std::string line = raw.substr(pos, (nl == std::string::npos ? raw.size() : nl) - pos);
        if (line.rfind("## ", 0) == 0) {
            flush();
            std::string h = line.substr(3);
            size_t bar = h.find('|');
            cur.id = bar == std::string::npos ? h : h.substr(0, bar);
            cur.keywords = bar == std::string::npos ? std::string() : h.substr(bar + 1);
            while (!cur.id.empty() && isspace((unsigned char)cur.id.back())) cur.id.pop_back();
            while (!cur.keywords.empty() && isspace((unsigned char)cur.keywords.front()))
                cur.keywords.erase(cur.keywords.begin());
            for (auto& k : splitKeywords(cur.keywords))
                if (k == "always") cur.always = true;
            in = true;
        } else if (in) {
            cur.body += line;
            cur.body += '\n';
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    flush();
    return p;
}

const Parsed& parsed() {
    static Parsed* p = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string raw;
        if (kAiPromptBlobLen > 0) {
            if (kAiPromptBlobGzip) {
                if (!gzipDecompress((const char*)kAiPromptBlob, kAiPromptBlobLen, raw, 4 * 1024 * 1024))
                    raw.clear();
            } else {
                raw.assign((const char*)kAiPromptBlob, kAiPromptBlobLen);
            }
        }
        p = new Parsed(parse(raw));
    });
    return *p;
}

}  // namespace

const std::string& aiPromptLibraryVersion() { return parsed().version; }

const std::string& aiPromptLibraryAll() { return parsed().raw; }

std::string aiPromptContext(const std::string& query, size_t max_sections) {
    const Parsed& p = parsed();
    std::string q = lowerAscii(query);

    std::string always;
    struct Hit {
        int score;
        const AiPromptSection* s;
    };
    std::vector<Hit> hits;
    for (auto& s : p.sections) {
        if (s.body.empty()) continue;
        if (s.always) {
            always += s.body;
            always += "\n\n";
            continue;
        }
        int score = 0;
        if (contains(q, lowerAscii(s.id))) score += 3;
        for (auto& k : splitKeywords(s.keywords)) {
            std::string lk = lowerAscii(k);
            if (lk.empty() || lk == "always") continue;
            if (contains(q, lk)) score += 2;
        }
        // 问题里没提，但正文覆盖了问题关键词的，也给一点分（帮助“内网装包”这类同义问法）
        if (score == 0) continue;
        hits.push_back({score, &s});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        return a.score > b.score;
    });
    if (hits.empty()) {
        for (auto& s : p.sections) {
            if (s.id == "troubleshooting" || s.id == "capability-matrix") {
                if (s.id == "capability-matrix") continue;   // 常驻段已经有
                always += s.body;
                always += "\n\n";
            }
        }
    }
    // 手册部分控制在 ~9KB：小模型/本地模型的上下文也能放下；超了就不再追加次要章节
    const size_t kManualBudget = 9000;
    std::string out = always;
    if (out.size() > kManualBudget) out = aiClampUtf8Local(out, kManualBudget);
    for (size_t i = 0; i < hits.size() && i < max_sections; ++i) {
        if (out.size() + hits[i].s->body.size() > kManualBudget) break;
        out += hits[i].s->body;
        out += "\n\n";
    }
    if (out.empty() && !p.sections.empty()) out = p.sections.front().body + "\n";
    return out;
}
