#pragma once
// agent 本地 Web：设备自带页面（内嵌 HTML + 只读 JSON 接口）
//
// 与 relay 管理台的设备详情同源：
//   GET /                      设备页（概览/存储/文件/服务/进程/网络/监控/串口/能力）
//   GET /api/v1/sysinfo        系统快照，loop 线程生成、HTTP 线程只读
//   GET /api/v1/procs|net|metrics|storage/tree   按需查询，走共享 worker
//   GET /ws/fs|/ws/svc         文件/服务管理 WebSocket
// 页面打开时会续期进程与服务预热（requestWarm），关闭后自动停止。

#include <string>

class AgentApp;

// listen: "8080" | "127.0.0.1:8080" | "0.0.0.0:8080" | "[::1]:8080"
// 成功返回 0；失败填充 err 并返回 -1。
int agentWebStart(AgentApp* app, const std::string& listen, std::string& err);
void agentWebStop();
