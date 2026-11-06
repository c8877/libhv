#include "relay_server.hpp"

int main(int argc, char** argv) {
    RelayServer server;
    return server.run(argc, argv);
}
