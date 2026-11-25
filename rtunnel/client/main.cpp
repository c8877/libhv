#include "client_app.hpp"
#include "client_repo.hpp"
#include "hversion.h"

#include <string.h>
#include <stdio.h>

int main(int argc, char** argv) {
    // 部署包与自举脚本靠 --version 对齐版本，必须与 agent/relay 一致
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("%s\n", hv_version());
        return 0;
    }
    // 文件仓库子命令：纯 HTTP(S)，不建立隧道
    if (argc >= 2 && strcmp(argv[1], "repo") == 0) {
        return clientRepoMain(argc - 1, argv + 1);
    }
    ClientApp app;
    return app.run(argc, argv);
}
