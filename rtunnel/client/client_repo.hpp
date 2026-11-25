#pragma once

// rtunnel-client repo <sub>：管理台文件仓库的 HTTP(S) 客户端。
// 只依赖 libhv HttpClient，不建立隧道；鉴权用 atk- 管理令牌。
int clientRepoMain(int argc, char** argv);
