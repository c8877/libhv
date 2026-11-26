#pragma once
#include <string>

// rtunnel 平台能力提示词库（relay/admin/backend/ai/prompt_library.md）。
// 构建期嵌入二进制，运行期只解压/解析一次；提供按问题检索的上下文拼装。
struct AiPromptSection {
    std::string id;
    std::string keywords;   // 逗号分隔；always 段常驻
    std::string body;
    bool        always = false;
};

const std::string& aiPromptLibraryVersion();   // md 头部 <!-- rtunnel-ai-prompt vN -->
const std::string& aiPromptLibraryAll();       // 整份语料（预热摘要用）
// 常驻段 + 与 query 最相关的若干段。永远不返回空串。
std::string aiPromptContext(const std::string& query, size_t max_sections = 4);
