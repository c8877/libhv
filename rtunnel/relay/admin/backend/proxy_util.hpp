#pragma once
#include "admin_server.hpp"
#include <atomic>
#include <string>
#include <cstdint>

extern std::atomic<int> g_proxy_active;
struct ProxyGuard { ProxyGuard(); ~ProxyGuard(); };

int tcpConnect(const char* host, int port, int timeout_ms);
bool writeAll(int fd, const void* p, size_t n);
bool readAll(int fd, void* p, size_t n);
void put16(std::string& s, uint16_t v);
void put32(std::string& s, uint32_t v);
void sendFrame(int fd, uint8_t type, uint32_t sid, const std::string& payload, uint16_t flags = 0);
void sendData(int fd, uint32_t sid, const std::string& p);
void sendWindow(int fd, uint32_t sid, uint32_t n);
bool recvFrame(int fd, uint8_t& type, uint32_t& sid, std::string& payload);
std::string rtnpStr(const std::string& s);
std::string proxyUrlDecode(const std::string& in);
std::string lower(std::string s);
std::string trim(const std::string& s);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
std::string dechunk(const std::string& in);
int openStream(const AdminConfig& cfg, const std::string& devid, int port, std::string& err);
int openTtyStream(const AdminConfig& cfg, const std::string& devid, const std::string& path,
                  int baud, int data_bits, char parity, int stop_bits, std::string& err);
