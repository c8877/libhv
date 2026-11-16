#pragma once
// agent 本地 Web：只读设备详情页（内嵌 HTML + /api/v1/sysinfo）
//
// 与 relay 管理台的设备详情同源：数据来自 AgentApp::buildSysinfo()，
// 由 loop 线程每 2s 刷新成 JSON 快照，HTTP 线程只读缓存，避免跨线程访问 agent 状态。

#include <string>

class AgentApp;

// listen: "8080" | "127.0.0.1:8080" | "0.0.0.0:8080" | "[::1]:8080"
// 成功返回 0；失败填充 err 并返回 -1。
int agentWebStart(AgentApp* app, const std::string& listen, std::string& err);
void agentWebStop();
