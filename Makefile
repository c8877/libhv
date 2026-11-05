# 本地构建入口。
#   make x86 / mp135 / clean ...  -> enp_build.sh（CMake 构建）
#   make unittest / evpp / libhrpc / ... -> 转交 Makefile.master
# Makefile.master = 上游 master 的 Makefile 原样保存，合并上游时直接覆盖该文件即可。

MASTER_MAKEFILE := Makefile.master

.DEFAULT_GOAL := x86

x86:
	./enp_build.sh --static --shared --examples --benchs
x86_install:
	./enp_build.sh --static --shared --install
# 主机：只出静态库（libhv.a）
static:
	./enp_build.sh --static --no-shared
# 主机：带示例程序（examples/ 里的 httpd、curl、wrk 等）
x86_examples:
	./enp_build.sh --static --shared --examples

mp135:
	./enp_build_mp135.sh
# MP135：只出静态库（ARM libhv.a）
mp135_static:
	./enp_build_mp135.sh --no-shared
# MP135：带示例程序（交叉编译，examples/ 全部）
mp135_examples:
	./enp_build_mp135.sh --examples
mp135_install:
	./enp_build_mp135.sh ~/.local/mp135

clean:
	./enp_build.sh --clean
	rm -rf deploy
clean-master:
	$(MAKE) -f $(MASTER_MAKEFILE) clean

# rtunnel 端到端验收（需先 make x86 或 ./enp_build.sh 构建）
rtunnel_test:
	python3 rtunnel/tests/e2e.py

# master Makefile 的目标，逐个转交（也可直接 make -f Makefile.master <target>）。
# 必须是 .PHONY：evpp/、unittest/、examples/ 等同名目录存在，否则会被当成已是最新。
# master 新增目标时在下面补一项即可。
MASTER_TARGETS := \
	all default libhv libhrpc examples prepare install uninstall \
	unittest run-unittest check evpp webbench echo-servers echo-benchmark \
	nmap kcptun kcptun_client kcptun_server \
	jsonrpc jsonrpc_client jsonrpc_server \
	protorpc protorpc_protoc protorpc_client protorpc_server \
	hrpc hrpc_calc_protoc hrpc_calc_client hrpc_calc_server \
	consul curl wget wrk httpd tinyhttpd tinyproxyd nc host \
	hmain_test htimer_test hloop_test pipe_test \
	tcp_client_test tcp_echo_server tcp_chat_server tcp_proxy_server \
	udp_echo_server udp_proxy_server socks5_proxy_server socks5_client_test \
	multi-acceptor-processes multi-acceptor-threads one-acceptor-multi-workers \
	http_server_test http_client_test websocket_server_test websocket_client_test \
	mqtt_sub mqtt_pub mqtt_client_test hvlua hvjs \
	redis_client_example redis_subscriber_example

$(MASTER_TARGETS):
	@test -f $(MASTER_MAKEFILE) || { echo "缺少 $(MASTER_MAKEFILE)" >&2; exit 1; }
	$(MAKE) -f $(MASTER_MAKEFILE) $@

.PHONY: x86 x86_install static x86_examples mp135 mp135_static mp135_examples mp135_install clean clean-master rtunnel_test $(MASTER_TARGETS)
