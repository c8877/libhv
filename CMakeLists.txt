# 本地 cmake 入口：只放本分支的默认选项，其余全部来自 CMakeLists.master
# （= 上游 master 的 CMakeLists.txt 原样保存；合并上游时直接覆盖该文件即可）。
# 上游的 option() 不会覆盖已在 cache 中的值，因此这里的默认值生效，
# 而 cmake -DXXX=... 仍可覆盖（set(... CACHE ...) 不写 FORCE）。
cmake_minimum_required(VERSION 3.6)

set(WITH_MQTT ON CACHE BOOL "compile mqtt")
set(WITH_RPC ON CACHE BOOL "compile hrpc (libhrpc, needs protobuf)")

# 小体积静态构建：每个函数/数据独立分段，链接时用 --gc-sections 丢掉没有引用的部分。
# 静态链接 libhv 时单个 .o 里会带进大量用不到的分支（DNS、socks5、HTTP 等），
# 打开后只保留真正被引用的代码。发布脚本（build_mp135.sh）会显式打开。
option(HV_SECTION_GC "按函数/数据分段并在链接时回收未引用段" OFF)
if(HV_SECTION_GC)
    add_compile_options(-ffunction-sections -fdata-sections)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.master)

# 本地新增组件（上游没有，放在 include 之后定义目标）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/rtunnel/CMakeLists.txt)
    add_subdirectory(rtunnel)
endif()
