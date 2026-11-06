#pragma once
// 管理台审计：所有写操作记录 who(role)/when/what，落盘 + 内存 ring（1000 条）

#include <string>
#include <vector>

class AuditLog {
public:
    void init(const std::string& path);   // 空 path = 仅内存，不落盘
    void write(const std::string& role, const std::string& action, const std::string& detail);
    std::string tailJson(int n) const;    // 最近 n 条，JSON 数组（新到旧）

private:
    std::string              path_;
    std::vector<std::string> ring_;
};
