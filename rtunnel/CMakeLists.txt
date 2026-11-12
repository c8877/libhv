# rtunnel：A/B/C 三件套（协议见 docs/design/rtunnel.md）
# C++ 对象化结构：rtnp/ 协议库 + common/ 共享组件 + client|agent|relay 各自目录
include_directories(${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/base ${PROJECT_SOURCE_DIR}/event ${PROJECT_SOURCE_DIR}/ssl
                    ${PROJECT_SOURCE_DIR}/http ${PROJECT_SOURCE_DIR}/http/server ${PROJECT_SOURCE_DIR}/evpp ${PROJECT_SOURCE_DIR}/cpputil
                    ${CMAKE_CURRENT_SOURCE_DIR})

find_package(ZLIB REQUIRED)
add_library(rtunnel_gzip STATIC common/gzip_util.cpp)
target_link_libraries(rtunnel_gzip PUBLIC ZLIB::ZLIB)

add_library(rtnp STATIC rtnp/codec.cpp)
target_include_directories(rtnp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rtnp PUBLIC ${HV_LIBRARIES})

add_library(rtunnel_common STATIC common/tunnel_link.cpp common/shell_spawn.cpp
                                 common/sha256.cpp common/token_util.cpp)
target_link_libraries(rtunnel_common PUBLIC rtnp rtunnel_gzip)

# 嵌入式目标静态链接 C++ 运行时（目标机无 libstdc++ 时使用，如 MP135）
option(RTUNNEL_STATIC_LIBGCC "静态链接 libstdc++/libgcc" OFF)

add_library(rtunnel_sqlite STATIC 3rd/sqlite3/sqlite3.c)
target_compile_definitions(rtunnel_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1)
set_target_properties(rtunnel_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(rtunnel-relay  relay/main.cpp relay/relay_server.cpp relay/audit_log.cpp
               common/sha256.cpp common/token_util.cpp
               relay/admin/backend/admin_server.cpp
               relay/admin/backend/admin_routes.cpp
               relay/admin/backend/api_agents.cpp
               relay/admin/backend/api_streams.cpp
               relay/admin/backend/api_ops.cpp
               relay/admin/backend/api_certs.cpp
               relay/admin/backend/api_files.cpp
               relay/admin/backend/api_upload.cpp
               relay/admin/backend/api_tty.cpp
               relay/admin/backend/api_metric_series.cpp
               relay/admin/backend/api_tty_ws.cpp
               relay/admin/backend/admin_db.cpp
               relay/admin/backend/admin_db_groups.cpp
               relay/admin/backend/admin_db_permission.cpp
               relay/admin/backend/admin_db_dict.cpp
               relay/admin/backend/admin_db_accounts.cpp
               relay/admin/backend/api_accounts.cpp
               relay/admin/backend/admin_db_dist.cpp
               relay/admin/backend/admin_db_tty.cpp
               relay/admin/backend/admin_db_metric_series.cpp
               relay/admin/backend/api_users.cpp
               relay/admin/backend/api_groups.cpp
               relay/admin/backend/api_permission.cpp
               relay/admin/backend/api_proxy.cpp
               relay/admin/backend/api_proxy_ws.cpp
               relay/admin/backend/proxy_util.cpp
               relay/admin/backend/api_settings.cpp
               relay/admin/backend/api_dist.cpp
               relay/admin/backend/hash_util.cpp
               relay/admin/backend/dist_scheduler.cpp
               relay/admin/backend/terminal.cpp)
add_executable(rtunnel-agent  agent/main.cpp agent/agent_app.cpp)
add_executable(rtunnel-client client/main.cpp client/client_app.cpp client/shell_client.cpp)
add_executable(rtnp-codec-test tests/codec_test.cpp)

target_include_directories(rtunnel-relay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rd/sqlite3)
target_link_libraries(rtunnel-relay  rtnp rtunnel_sqlite rtunnel_gzip)
target_link_libraries(rtunnel-agent  rtunnel_common)
target_link_libraries(rtunnel-client rtunnel_common)
target_link_libraries(rtnp-codec-test rtnp)

if (RTUNNEL_STATIC_LIBGCC)
    target_link_options(rtunnel-relay  PRIVATE -static-libstdc++ -static-libgcc)
    target_link_options(rtunnel-agent  PRIVATE -static-libstdc++ -static-libgcc)
    target_link_options(rtunnel-client PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS rtunnel-relay rtunnel-agent rtunnel-client
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
