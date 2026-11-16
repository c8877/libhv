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
               relay/admin/backend/admin_tls.cpp
               relay/admin/backend/alert_notifier.cpp
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
               relay/admin/backend/api_fs_ws.cpp
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

# agent 两件套：rtunnel-agent 带本地设备详情页；rtunnel-agent-noweb 不含 Web 代码。
# 页面是内嵌单文件 HTML，无外部资源，适合离线设备。
option(RTUNNEL_AGENT_WEB "rtunnel-agent 内置本地 Web 设备详情页" ON)
set(RTUNNEL_AGENT_SOURCES agent/main.cpp agent/agent_app.cpp
                           agent/fs_service.cpp agent/fs_session.cpp agent/fs_archive.cpp)
# 共享文件管理前端：同一个 fs-ui.js 同时给本地 Web 和 relay-admin 使用。
# 本地 Web 需要把 JS 嵌进二进制，这里在配置阶段生成头文件。
set(RTUNNEL_FS_UI_JS "${CMAKE_CURRENT_SOURCE_DIR}/shared/web/fs-ui.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RTUNNEL_FS_UI_JS}")
file(READ "${RTUNNEL_FS_UI_JS}" RTUNNEL_FS_UI_JS_CONTENT)
# 编辑器组件（CodeMirror IIFE）：由 npm run build:editor 生成，缺失时页面退化为 textarea。
set(RTUNNEL_CM_EDITOR_JS "${CMAKE_CURRENT_SOURCE_DIR}/shared/web/cm-editor.bundle.js")
set(RTUNNEL_CM_EDITOR_JS_CONTENT "")
if(EXISTS "${RTUNNEL_CM_EDITOR_JS}")
    file(READ "${RTUNNEL_CM_EDITOR_JS}" RTUNNEL_CM_EDITOR_JS_CONTENT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RTUNNEL_CM_EDITOR_JS}")
else()
    message(STATUS "rtunnel-agent: cm-editor.bundle.js 未构建，本地页编辑器使用 textarea（先在 relay/admin/web 执行 npm run build:editor）")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/fs_ui_js.hpp"
    "#pragma once\n// 由 CMake 从 shared/web/fs-ui.js 生成，勿手改。\nstatic const char kFsUiJs[] = R\"RTFSJS(\n${RTUNNEL_FS_UI_JS_CONTENT}\n)RTFSJS\";\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/cm_editor_js.hpp"
    "#pragma once\n// 由 CMake 从 shared/web/cm-editor.bundle.js 生成，勿手改。\nstatic const char kCmEditorJs[] = R\"RTCMJS(\n${RTUNNEL_CM_EDITOR_JS_CONTENT}\n)RTCMJS\";\n")
add_executable(rtunnel-agent ${RTUNNEL_AGENT_SOURCES})
if (RTUNNEL_AGENT_WEB)
    target_sources(rtunnel-agent PRIVATE agent/agent_web.cpp)
    target_compile_definitions(rtunnel-agent PRIVATE RTUNNEL_AGENT_WEB=1)
    target_include_directories(rtunnel-agent PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_executable(rtunnel-agent-noweb ${RTUNNEL_AGENT_SOURCES})
    target_link_libraries(rtunnel-agent-noweb rtunnel_common)
endif()

add_executable(rtunnel-client client/main.cpp client/client_app.cpp client/shell_client.cpp)
add_executable(rtnp-codec-test tests/codec_test.cpp)
add_executable(rtunnel-endpoint-test tests/endpoint_test.cpp)

target_include_directories(rtunnel-relay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rd/sqlite3)
target_link_libraries(rtunnel-relay  rtnp rtunnel_sqlite rtunnel_gzip)
target_link_libraries(rtunnel-agent  rtunnel_common)
target_link_libraries(rtunnel-client rtunnel_common)
target_link_libraries(rtnp-codec-test rtnp)
target_link_libraries(rtunnel-endpoint-test rtunnel_common)

if (RTUNNEL_STATIC_LIBGCC)
    target_link_options(rtunnel-relay  PRIVATE -static-libstdc++ -static-libgcc)
    target_link_options(rtunnel-agent  PRIVATE -static-libstdc++ -static-libgcc)
    if (TARGET rtunnel-agent-noweb)
        target_link_options(rtunnel-agent-noweb PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    target_link_options(rtunnel-client PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS rtunnel-relay rtunnel-agent rtunnel-client
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------- 部署目录
# build/deploy/relay/{bin,service,web/dist}
# build/deploy/agent/{bin,service}      不带本地 Web
# build/deploy/agent-web/{bin,service}  带本地 Web（页面内嵌，无需额外文件）
set(RTUNNEL_DEPLOY_DIR "${CMAKE_SOURCE_DIR}/build/deploy" CACHE PATH "rtunnel deploy output directory")
set(RTUNNEL_WEB_DIST "${CMAKE_CURRENT_SOURCE_DIR}/relay/admin/web/dist")
if(BUILD_SHARED)
    message(WARNING "rtunnel-deploy: BUILD_SHARED=ON，部署二进制依赖 libhv.so；请用 -DBUILD_SHARED=OFF 的构建目录")
endif()
if(NOT EXISTS "${RTUNNEL_WEB_DIST}/admin.html")
    message(STATUS "rtunnel-deploy: ${RTUNNEL_WEB_DIST} 未构建，先执行 npm run build")
endif()
if (TARGET rtunnel-agent-noweb)
    set(RTUNNEL_AGENT_LITE_TARGET rtunnel-agent-noweb)
else()
    set(RTUNNEL_AGENT_LITE_TARGET rtunnel-agent)
endif()
# 只裁剪部署副本，保留构建目录里的调试符号
set(RTUNNEL_STRIP_ARGS "")
if(CMAKE_STRIP)
    list(APPEND RTUNNEL_STRIP_ARGS
        COMMAND ${CMAKE_STRIP} --strip-all "${RTUNNEL_DEPLOY_DIR}/relay/bin/rtunnel-relay"
        COMMAND ${CMAKE_STRIP} --strip-all "${RTUNNEL_DEPLOY_DIR}/agent/bin/rtunnel-client"
        COMMAND ${CMAKE_STRIP} --strip-all "${RTUNNEL_DEPLOY_DIR}/agent/bin/rtunnel-agent")
endif()
set(RTUNNEL_AGENT_WEB_STRIP_ARGS "")
if(CMAKE_STRIP AND RTUNNEL_AGENT_WEB)
    list(APPEND RTUNNEL_AGENT_WEB_STRIP_ARGS
        COMMAND ${CMAKE_STRIP} --strip-all "${RTUNNEL_DEPLOY_DIR}/agent-web/bin/rtunnel-agent"
        COMMAND ${CMAKE_STRIP} --strip-all "${RTUNNEL_DEPLOY_DIR}/agent-web/bin/rtunnel-client")
endif()
add_custom_target(rtunnel-deploy
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${RTUNNEL_DEPLOY_DIR}/relay/bin"
            "${RTUNNEL_DEPLOY_DIR}/relay/service"
            "${RTUNNEL_DEPLOY_DIR}/relay/web"
            "${RTUNNEL_DEPLOY_DIR}/agent/bin"
            "${RTUNNEL_DEPLOY_DIR}/agent/service"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rtunnel-relay> "${RTUNNEL_DEPLOY_DIR}/relay/bin/rtunnel-relay"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rtunnel-client> "${RTUNNEL_DEPLOY_DIR}/agent/bin/rtunnel-client"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${RTUNNEL_AGENT_LITE_TARGET}> "${RTUNNEL_DEPLOY_DIR}/agent/bin/rtunnel-agent"
    ${RTUNNEL_STRIP_ARGS}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/deploy/relay/rtunnel-relay.service"
            "${RTUNNEL_DEPLOY_DIR}/relay/service/rtunnel-relay.service"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/deploy/agent/rtunnel-agent.service"
            "${RTUNNEL_DEPLOY_DIR}/agent/service/rtunnel-agent.service"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
            "${RTUNNEL_DEPLOY_DIR}/relay/web/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${RTUNNEL_WEB_DIST}" "${RTUNNEL_DEPLOY_DIR}/relay/web/dist"
    DEPENDS rtunnel-relay rtunnel-client ${RTUNNEL_AGENT_LITE_TARGET}
    COMMENT "assembling rtunnel deploy directory (agent: build_mp135.sh)")

if (RTUNNEL_AGENT_WEB)
    add_custom_target(rtunnel-deploy-web
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${RTUNNEL_DEPLOY_DIR}/agent-web/bin"
                "${RTUNNEL_DEPLOY_DIR}/agent-web/service"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rtunnel-agent> "${RTUNNEL_DEPLOY_DIR}/agent-web/bin/rtunnel-agent"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rtunnel-client> "${RTUNNEL_DEPLOY_DIR}/agent-web/bin/rtunnel-client"
        ${RTUNNEL_AGENT_WEB_STRIP_ARGS}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/deploy/agent/rtunnel-agent-web.service"
                "${RTUNNEL_DEPLOY_DIR}/agent-web/service/rtunnel-agent.service"
        DEPENDS rtunnel-agent rtunnel-client
        COMMENT "assembling rtunnel agent-web deploy directory")
endif()
