// Native debug adapter entry point.
//
// Behavioural port of extension/script/frontend/main.lua:
//   * no arguments  -> DAP over stdio,
//   * one argument  -> listen on 127.0.0.1:<port> (the debugServer workflow).
//
// The adapter no longer hosts a Lua VM: `-e` and dofile-based startup are gone,
// which is why every caller that used `bin/lua-debug` as a Lua interpreter now
// points at runtime/<platform>/<luaver>/lua instead.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>

#include "common/log.h"
#include "common/path_utf8.h"
#include "frontend/platform.h"
#include "frontend/proxy.h"
#include "transport/channel.h"
#include "transport/reactor.h"

namespace {

    using namespace luadebug::debugger;

    int parse_port(const char* text) {
        if (!text || !*text) {
            return 0;
        }
        char* end  = nullptr;
        long value = std::strtol(text, &end, 10);
        if (end == text || *end != '\0' || value <= 0 || value > 65535) {
            return 0;
        }
        return static_cast<int>(value);
    }

    int run_adapter(int port, const std::string& workdir) {
        std::unique_ptr<transport::Channel> client;
        if (port != 0) {
            std::string spec = "listen:127.0.0.1:" + std::to_string(port);
            std::string error;
            client = transport::open_socket_channel(spec, error);
            if (!client) {
                log_error("cannot listen on 127.0.0.1:%d: %s", port, error.c_str());
                return 1;
            }
        } else {
            std::string error;
            client = transport::open_stdio_channel(error);
            if (!client) {
                log_error("cannot open stdio: %s", error.c_str());
                return 1;
            }
        }

        frontend::Proxy proxy(std::move(client), workdir);
        int iterations = 0;
        while (proxy.update(10)) {
            ++iterations;
        }
        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    using namespace luadebug::debugger;

    auto workdir = frontend::workdir_from_executable();
    if (workdir.empty()) {
        std::error_code ec;
        workdir = path_to_utf8(fs::current_path(ec));
    }
    Log::instance().set_file((path_from_utf8(workdir) / "client.log").string());

    int port = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-e" || arg == "--eval") {
            std::fprintf(stderr,
                         "lua-debug is a native debug adapter and cannot evaluate Lua code.\n"
                         "Use runtime/<platform>/<luaver>/lua -e instead.\n");
            return 1;
        }
        if (port == 0) {
            port = parse_port(argv[i]);
        }
    }

    int rc = run_adapter(port, workdir);
    std::fflush(stdout);
    std::fflush(stderr);
    return rc;
}
