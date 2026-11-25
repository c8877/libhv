#pragma once
// 管理台审计：所有写操作记 who/what/target/result，内存 ring（1000 条）+ 两份落盘：
//   <path>        人读的文本行（保留原有格式，方便 grep/tail）
//   <path>.jsonl  机器读的 JSON 行（AI 与外部系统按字段聚合）
// 两者同一把锁写入，字段内容一致。

#include <string>
#include <vector>
#include <mutex>

struct AuditEntry {
    std::string at;            // UTC ISO8601
    std::string actor_type;    // user | client | token | ai | anonymous
    std::string actor;         // 完整标识（含 id / 令牌 / 来源）
    std::string action;        // 如 device.exec / permission.group.save
    std::string target_type;   // device / account / group / token / ...
    std::string target_id;
    std::string result;        // ok | denied | error
    std::string detail;        // 人读细节
    std::string op_id;         // operations.id（可空）
};

class AuditLog {
public:
    void init(const std::string& path);   // 空 path = 仅内存，不落盘
    void write(const std::string& role, const std::string& action, const std::string& detail);
    void writeEntry(const AuditEntry& e);
    std::string tailJson(int n) const;    // 最近 n 条，JSON 数组（新到旧）
    void rotate(size_t max_bytes);        // 超过阈值转存 .1（文本与 jsonl 各自轮转）

private:
    std::string              path_;
    std::vector<std::string> ring_;       // 每项是一个 JSON 对象
    mutable std::mutex mu_;
};
