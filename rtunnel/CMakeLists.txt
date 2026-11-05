# rtunnel：A/B/C 三件套（协议见 docs/design/remote-ssh-tunnel.md）
include_directories(${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/base ${PROJECT_SOURCE_DIR}/event ${PROJECT_SOURCE_DIR}/ssl)

add_library(rtnp STATIC rtnp.c)
target_include_directories(rtnp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rtnp PUBLIC ${HV_LIBRARIES})

add_executable(rtunnel-relay  relay.c)
add_executable(rtunnel-agent  agent.c)
add_executable(rtunnel-client client.c)
add_executable(rtnp-codec-test tests/codec_test.c)

target_link_libraries(rtunnel-relay  rtnp)
target_link_libraries(rtunnel-agent  rtnp)
target_link_libraries(rtunnel-client rtnp)
target_link_libraries(rtnp-codec-test rtnp)

install(TARGETS rtunnel-relay rtunnel-agent rtunnel-client
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
