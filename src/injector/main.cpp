// The injector helper: a standalone, per-architecture process the adapter runs
// for one injection. It must match the *target's* architecture (an x64 target
// under Rosetta needs this binary as x86_64), which is why it is not linked into
// the adapter.
//
// Usage: lua-debug-injector-<arch> <backend> --pid <decimal> --library <path> [--data <json>]
//
// `frida` is the backend this binary implements; the lldb backend drives the
// system lldb directly and needs no helper. Results are structured on stdout and
// the exit code distinguishes an explicit permission failure (retry with
// escalation) from any other failure, so no caller has to parse English error
// text.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "frida_inject.h"

namespace {

    // Fixed entry point of the launcher (src/launcher/main.cpp).
    constexpr const char* kEntrypoint = "luadebug_frida_entry";

    constexpr const char* kBackendFrida = "frida";

    struct Options {
        std::string backend;
        long pid = 0;
        std::string library;
        std::string data;
        bool has_pid     = false;
        bool has_library = false;
        bool has_data    = false;
    };

    void print_json_string(const std::string& text) {
        std::fputc('"', stdout);
        for (unsigned char c : text) {
            switch (c) {
            case '"':
                std::fputs("\\\"", stdout);
                break;
            case '\\':
                std::fputs("\\\\", stdout);
                break;
            case '\n':
                std::fputs("\\n", stdout);
                break;
            case '\r':
                std::fputs("\\r", stdout);
                break;
            case '\t':
                std::fputs("\\t", stdout);
                break;
            default:
                if (c < 0x20) {
                    std::fprintf(stdout, "\\u%04x", c);
                } else {
                    std::fputc(c, stdout);
                }
            }
        }
        std::fputc('"', stdout);
    }

    int fail(const std::string& reason, int exit_code) {
        std::fputs("{\"ok\":false,\"status\":\"invalid_arguments\",\"message\":", stdout);
        print_json_string(reason);
        std::fputs("}\n", stdout);
        return exit_code;
    }

    bool parse_args(int argc, char** argv, Options& options, std::string& error) {
        int first = 1;
        // The backend is a subcommand: `... frida --pid ...`.
        if (argc > 1 && argv[1][0] != '-') {
            options.backend = argv[1];
            first           = 2;
        }
        if (options.backend.empty()) {
            error = "missing backend; use `frida`";
            return false;
        }
        if (options.backend != kBackendFrida) {
            error = "unknown backend `" + options.backend + "`; use `frida`";
            return false;
        }
        for (int i = first; i < argc; ++i) {
            std::string name   = argv[i];
            bool* seen         = nullptr;
            std::string* value = nullptr;
            if (name == "--pid") {
                seen = &options.has_pid;
            } else if (name == "--library") {
                seen  = &options.has_library;
                value = &options.library;
            } else if (name == "--data") {
                seen  = &options.has_data;
                value = &options.data;
            } else {
                error = "unknown argument `" + name + "`";
                return false;
            }
            if (*seen) {
                error = "duplicate argument `" + name + "`";
                return false;
            }
            if (i + 1 >= argc) {
                error = "missing value for `" + name + "`";
                return false;
            }
            *seen         = true;
            std::string v = argv[++i];
            if (name == "--pid") {
                char* end     = nullptr;
                errno         = 0;
                long long pid = std::strtoll(v.c_str(), &end, 10);
                // Reject overflow and anything that would narrow to a different pid
                // when handed to frida (unsigned int).
                if (v.empty() || !end || *end != '\0' || errno == ERANGE || pid <= 0 || static_cast<unsigned long long>(pid) > 0xffffffffULL) {
                    error = "invalid --pid `" + v + "`";
                    return false;
                }
                options.pid = static_cast<long>(pid);
            } else if (value) {
                *value = v;
            }
        }
        if (!options.has_pid) {
            error = "--pid is required";
            return false;
        }
        if (!options.has_library || options.library.empty()) {
            error = "--library is required";
            return false;
        }
        return true;
    }

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parse_args(argc, argv, options, error)) {
        return fail(error, luadebug::injector::kExitFailed);
    }

    auto result = luadebug::injector::inject_library(
        static_cast<unsigned int>(options.pid), options.library, kEntrypoint, options.data
    );

    switch (result.status) {
    case luadebug::injector::Status::ok:
        std::fprintf(stdout, "{\"ok\":true,\"id\":%u}\n", result.id);
        return luadebug::injector::kExitOk;
    case luadebug::injector::Status::permission_denied:
        std::fputs("{\"ok\":false,\"status\":\"permission_denied\",\"domain\":", stdout);
        std::fprintf(stdout, "%u,\"code\":%d,\"message\":", result.error_domain, result.error_code);
        print_json_string(result.error_message);
        std::fputs("}\n", stdout);
        return luadebug::injector::kExitPermissionDenied;
    case luadebug::injector::Status::timed_out:
        std::fputs("{\"ok\":false,\"status\":\"timed_out\",\"domain\":", stdout);
        std::fprintf(stdout, "%u,\"code\":%d,\"message\":", result.error_domain, result.error_code);
        print_json_string(result.error_message);
        std::fputs("}\n", stdout);
        return luadebug::injector::kExitFailed;
    case luadebug::injector::Status::failed:
    default:
        std::fputs("{\"ok\":false,\"status\":\"failed\",\"domain\":", stdout);
        std::fprintf(stdout, "%u,\"code\":%d,\"message\":", result.error_domain, result.error_code);
        print_json_string(result.error_message);
        std::fputs("}\n", stdout);
        return luadebug::injector::kExitFailed;
    }
}
