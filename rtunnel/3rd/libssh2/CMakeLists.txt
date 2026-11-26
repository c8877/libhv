# libssh2 静态库（裁剪版，加密后端 OpenSSL）
#
# 上层只通过 deploy_ssh.cpp 使用它；不要把这个库的符号暴露到管理台 API。
cmake_minimum_required(VERSION 3.7)

set(LIBSSH2_DIR ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB LIBSSH2_SOURCES CONFIGURE_DEPENDS ${LIBSSH2_DIR}/src/*.c)
file(GLOB LIBSSH2_HEADERS CONFIGURE_DEPENDS ${LIBSSH2_DIR}/src/*.h)

# 只需要 POSIX/Linux 的这一组能力；值为 1 才写进生成的头
set(HAVE_UNISTD_H 1)
set(HAVE_INTTYPES_H 1)
set(HAVE_SYS_SELECT_H 1)
set(HAVE_SYS_UIO_H 1)
set(HAVE_SYS_SOCKET_H 1)
set(HAVE_SYS_IOCTL_H 1)
set(HAVE_SYS_TIME_H 1)
set(HAVE_SYS_UN_H 1)
set(HAVE_ARPA_INET_H 1)
set(HAVE_NETINET_IN_H 1)
set(HAVE_GETTIMEOFDAY 1)
set(HAVE_STRTOLL 1)
set(HAVE_SNPRINTF 1)
set(HAVE_EXPLICIT_BZERO 1)
set(HAVE_POLL 1)
set(HAVE_SELECT 1)
set(HAVE_O_NONBLOCK 1)

configure_file(${LIBSSH2_DIR}/src/libssh2_config_cmake.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/libssh2_config.h)

add_library(rtunnel_ssh2 STATIC ${LIBSSH2_SOURCES})

target_include_directories(rtunnel_ssh2
    PUBLIC ${LIBSSH2_DIR}/include
    PRIVATE ${LIBSSH2_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})

target_compile_definitions(rtunnel_ssh2 PRIVATE
    HAVE_CONFIG_H
    LIBSSH2_OPENSSL
    LIBSSH2_LIBRARY)

# 上游默认对内部符号隐藏；静态库场景下直接打开全部
target_compile_options(rtunnel_ssh2 PRIVATE -w)
