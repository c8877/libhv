# rtunnel：A/B/C 三件套（协议见 docs/design/rtunnel.md）
# C++ 对象化结构：rtnp/ 协议库 + common/ 共享组件 + client|agent|relay 各自目录
if(NOT WITH_OPENSSL)
    message(STATUS "rtunnel: 需要 OpenSSL（SHA-256/HMAC/随机数/证书都复用 libssl），WITH_OPENSSL=OFF 时跳过")
    return()
endif()

include_directories(${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/base ${PROJECT_SOURCE_DIR}/event ${PROJECT_SOURCE_DIR}/ssl
                    ${PROJECT_SOURCE_DIR}/http ${PROJECT_SOURCE_DIR}/http/server ${PROJECT_SOURCE_DIR}/evpp ${PROJECT_SOURCE_DIR}/cpputil
                    ${CMAKE_CURRENT_SOURCE_DIR})

find_package(ZLIB REQUIRED)
add_library(rtunnel_gzip STATIC common/gzip_util.cpp)
target_link_libraries(rtunnel_gzip PUBLIC ZLIB::ZLIB)

add_library(rtnp STATIC rtnp/codec.cpp)
target_include_directories(rtnp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rtnp PUBLIC ${HV_LIBRARIES})

add_library(rtunnel_common STATIC common/endpoint.cpp common/tunnel_link.cpp common/shell_spawn.cpp
                                 common/sha256.cpp common/token_util.cpp)
target_link_libraries(rtunnel_common PUBLIC rtnp rtunnel_gzip)

# 嵌入式目标静态链接 C++ 运行时（目标机无 libstdc++ 时使用，如 MP135）
option(RTUNNEL_STATIC_LIBGCC "静态链接 libstdc++/libgcc" OFF)

add_library(rtunnel_sqlite STATIC 3rd/sqlite3/sqlite3.c)
target_compile_definitions(rtunnel_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1)
set_target_properties(rtunnel_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(rtunnel-relay  relay/main.cpp relay/relay_server.cpp relay/audit_log.cpp
               relay/usage_meter.cpp
               common/endpoint.cpp
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
               relay/admin/backend/admin_db_permission.cpp
               relay/admin/backend/admin_db_dict.cpp
               relay/admin/backend/admin_db_relay.cpp
               relay/admin/backend/api_relay.cpp
               relay/admin/backend/admin_db_accounts.cpp
               relay/admin/backend/admin_db_usage.cpp
               relay/admin/backend/admin_db_tenant.cpp
               relay/admin/backend/admin_db_billing.cpp
               relay/admin/backend/api_usage.cpp
               relay/admin/backend/api_tenant.cpp
               relay/admin/backend/api_billing.cpp
               relay/admin/backend/admin_db_admin_tokens.cpp
               relay/admin/backend/api_accounts.cpp
               relay/admin/backend/api_admin_tokens.cpp
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
               relay/admin/backend/relay_sync.cpp
               relay/admin/backend/terminal.cpp)

# agent 两件套：rtunnel-agent 带本地设备详情页；rtunnel-agent-noweb 不含 Web 代码。
# 页面是内嵌单文件 HTML，无外部资源，适合离线设备。
option(RTUNNEL_AGENT_WEB "rtunnel-agent 内置本地 Web 设备详情页" ON)
set(RTUNNEL_AGENT_SOURCES agent/main.cpp agent/agent_app.cpp agent/ota_pkg.cpp
                           agent/fs_service.cpp agent/fs_session.cpp agent/fs_archive.cpp
                           agent/svc_service.cpp agent/svc_session.cpp agent/net_service.cpp
                           agent/proc_service.cpp agent/storage_service.cpp)
# 共享前端：同一份 fs-ui.js / svc-ui.js / file-view.js 同时给本地 Web 和 relay-admin 使用。
# 本地 Web 在配置阶段把资源和设备页 HTML 压成 gzip，以字节数组嵌进二进制：
# 运行时直接发压缩体，不再额外占堆保存副本。没有 gzip 命令的平台退回原文嵌入。
set(RTUNNEL_FS_UI_JS "${CMAKE_CURRENT_SOURCE_DIR}/shared/web/fs-ui.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RTUNNEL_FS_UI_JS}")
set(RTUNNEL_SVC_UI_JS "${CMAKE_CURRENT_SOURCE_DIR}/shared/web/svc-ui.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RTUNNEL_SVC_UI_JS}")
set(RTUNNEL_TABLE_UI_JS "${CMAKE_CURRENT_SOURCE_DIR}/shared/web/table-ui.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RTUNNEL_TABLE_UI_JS}")
set(RTUNNEL_FILE_VIEW_JS "${CMAKE_CURRENT_SOURCE_DIR}/shared/web/file-view.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RTUNNEL_FILE_VIEW_JS}")
set(RTUNNEL_STORAGE_UI_JS "${CMAKE_CURRENT_SOURCE_DIR}/shared/web/storage-ui.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RTUNNEL_STORAGE_UI_JS}")
# 编辑器组件（CodeMirror IIFE）：由 npm run build:editor 生成，缺失时本地页编辑功能不可用。
set(RTUNNEL_CM_EDITOR_JS "${CMAKE_CURRENT_SOURCE_DIR}/shared/web/cm-editor.bundle.js")
if(NOT EXISTS "${RTUNNEL_CM_EDITOR_JS}")
    message(WARNING "rtunnel-agent: cm-editor.bundle.js 未构建，本地页的编辑功能不可用（先在 relay/admin/web 执行 npm run build:editor）")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

find_program(RTUNNEL_GZIP_EXE gzip)
# 生成 { blob, len, isGzip } 三件套；源文件缺失时生成空资源，调用方无需判断。
function(rtunnel_embed_asset asset symbol header comment)
    if(NOT EXISTS "${asset}")
        file(WRITE "${header}" "#pragma once\n// ${comment} 缺失\nstatic const unsigned char ${symbol}[] = {0};\nstatic const unsigned int ${symbol}Len = 0;\nstatic const bool ${symbol}Gzip = false;\n")
        return()
    endif()
    set(blob "${asset}")
    set(is_gzip false)
    if(RTUNNEL_GZIP_EXE)
        set(gz "${CMAKE_CURRENT_BINARY_DIR}/generated/${symbol}.gz")
        execute_process(COMMAND "${RTUNNEL_GZIP_EXE}" -9 -n -c "${asset}" OUTPUT_FILE "${gz}" RESULT_VARIABLE rc)
        if(rc EQUAL 0)
            set(blob "${gz}")
            set(is_gzip true)
        endif()
    endif()
    file(READ "${blob}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    file(SIZE "${blob}" blob_size)
    set(tmp "${header}.tmp")
    file(WRITE "${tmp}" "#pragma once\n// 由 CMake 从 ${comment} 生成，勿手改。\nstatic const unsigned char ${symbol}[] = {${hex}};\nstatic const unsigned int ${symbol}Len = ${blob_size};\nstatic const bool ${symbol}Gzip = ${is_gzip};\n")
    # 内容不变时不更新时间戳，避免每次 configure 都重编 agent_web.cpp
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tmp}" "${header}")
    file(REMOVE "${tmp}")
endfunction()

rtunnel_embed_asset("${RTUNNEL_FS_UI_JS}" kFsUiBlob "${CMAKE_CURRENT_BINARY_DIR}/generated/fs_ui_js.hpp" "shared/web/fs-ui.js")
rtunnel_embed_asset("${RTUNNEL_SVC_UI_JS}" kSvcUiBlob "${CMAKE_CURRENT_BINARY_DIR}/generated/svc_ui_js.hpp" "shared/web/svc-ui.js")
rtunnel_embed_asset("${RTUNNEL_TABLE_UI_JS}" kTableUiBlob "${CMAKE_CURRENT_BINARY_DIR}/generated/table_ui_js.hpp" "shared/web/table-ui.js")
rtunnel_embed_asset("${RTUNNEL_FILE_VIEW_JS}" kFileViewBlob "${CMAKE_CURRENT_BINARY_DIR}/generated/file_view_js.hpp" "shared/web/file-view.js")
rtunnel_embed_asset("${RTUNNEL_STORAGE_UI_JS}" kStorageUiBlob "${CMAKE_CURRENT_BINARY_DIR}/generated/storage_ui_js.hpp" "shared/web/storage-ui.js")
rtunnel_embed_asset("${RTUNNEL_CM_EDITOR_JS}" kCmEditorBlob "${CMAKE_CURRENT_BINARY_DIR}/generated/cm_editor_js.hpp" "shared/web/cm-editor.bundle.js")
set(RTUNNEL_AGENT_HTML "${CMAKE_CURRENT_SOURCE_DIR}/agent/web/index.html")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RTUNNEL_AGENT_HTML}")
rtunnel_embed_asset("${RTUNNEL_AGENT_HTML}" kAgentWebBlob "${CMAKE_CURRENT_BINARY_DIR}/generated/agent_web_html.hpp" "agent/web/index.html")

add_executable(rtunnel-agent ${RTUNNEL_AGENT_SOURCES})
if (RTUNNEL_AGENT_WEB)
    target_sources(rtunnel-agent PRIVATE agent/agent_web.cpp)
    target_compile_definitions(rtunnel-agent PRIVATE RTUNNEL_AGENT_WEB=1)
    target_include_directories(rtunnel-agent PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_executable(rtunnel-agent-noweb ${RTUNNEL_AGENT_SOURCES})
    target_link_libraries(rtunnel-agent-noweb rtunnel_common crypt)
endif()

add_executable(rtunnel-client client/main.cpp client/client_app.cpp client/shell_client.cpp)
add_executable(rtnp-codec-test tests/codec_test.cpp)
add_executable(rtunnel-endpoint-test tests/endpoint_test.cpp)

target_include_directories(rtunnel-relay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rd/sqlite3)
target_link_libraries(rtunnel-relay  rtnp rtunnel_sqlite rtunnel_gzip)
target_link_libraries(rtunnel-agent  rtunnel_common crypt)
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
            "${RTUNNEL_DEPLOY_DIR}/agent/etc"
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
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/deploy/agent/install.sh"
            "${RTUNNEL_DEPLOY_DIR}/agent/install.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/deploy/agent/agent-boot-check.sh"
            "${RTUNNEL_DEPLOY_DIR}/agent/bin/agent-boot-check.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/deploy/agent/rtunnel-agent.json"
            "${RTUNNEL_DEPLOY_DIR}/agent/etc/rtunnel-agent.json"
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
                "${RTUNNEL_DEPLOY_DIR}/agent-web/etc"
                "${RTUNNEL_DEPLOY_DIR}/agent-web/service"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rtunnel-agent> "${RTUNNEL_DEPLOY_DIR}/agent-web/bin/rtunnel-agent"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rtunnel-client> "${RTUNNEL_DEPLOY_DIR}/agent-web/bin/rtunnel-client"
        ${RTUNNEL_AGENT_WEB_STRIP_ARGS}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/deploy/agent/rtunnel-agent.service"
                "${RTUNNEL_DEPLOY_DIR}/agent-web/service/rtunnel-agent.service"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/deploy/agent/install.sh"
                "${RTUNNEL_DEPLOY_DIR}/agent-web/install.sh"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/deploy/agent/agent-boot-check.sh"
                "${RTUNNEL_DEPLOY_DIR}/agent-web/bin/agent-boot-check.sh"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/deploy/agent/rtunnel-agent.json"
                "${RTUNNEL_DEPLOY_DIR}/agent-web/etc/rtunnel-agent.json"
        DEPENDS rtunnel-agent rtunnel-client
        COMMENT "assembling rtunnel agent-web deploy directory")
endif()
