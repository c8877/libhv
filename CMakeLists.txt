# 本地 cmake 入口：只放本分支的默认选项，其余全部来自 CMakeLists.master
# （= 上游 master 的 CMakeLists.txt 原样保存；合并上游时直接覆盖该文件即可）。
# 上游的 option() 不会覆盖已在 cache 中的值，因此这里的默认值生效，
# 而 cmake -DXXX=... 仍可覆盖（set(... CACHE ...) 不写 FORCE）。
cmake_minimum_required(VERSION 3.6)

set(WITH_MQTT ON CACHE BOOL "compile mqtt")
set(WITH_RPC ON CACHE BOOL "compile hrpc (libhrpc, needs protobuf)")

include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.master)

# 本地新增组件（上游没有，放在 include 之后定义目标）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/rtunnel/CMakeLists.txt)
    add_subdirectory(rtunnel)
endif()
