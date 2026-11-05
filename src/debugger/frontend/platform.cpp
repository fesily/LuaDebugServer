#include "frontend/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/log.h"
#include "common/path_utf8.h"

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <unistd.h>
#endif

#if defined(__APPLE__)
#    include <mach-o/dyld.h>

#    include <cstdint>
#endif

namespace luadebug::debugger::frontend {

    namespace {
        std::vector<std::string> split_on(const std::string& text, char sep) {
            std::vector<std::string> out;
            std::string current;
            for (char c : text) {
                if (c == sep) {
                    out.push_back(current);
                    current.clear();
                } else {
                    current.push_back(c);
                }
            }
            out.push_back(current);
            return out;
        }

        // Mirrors `line:match "^([^%s].*[^%s])%s+(%d+)%s*$"`: a name that starts
        // and ends with a non-space, then whitespace, then digits.
        bool parse_ps_line(const std::string& line, std::string& name, long& pid) {
            std::size_t end = line.find_last_not_of(" \t\r\n");
            if (end == std::string::npos || end == 0) {
                return false;
            }
            std::size_t digits_end = end;
            std::size_t pos        = end;
            bool saw_digit         = false;
            while (pos > 0 && line[pos] >= '0' && line[pos] <= '9') {
                saw_digit = true;
                --pos;
            }
            if (!saw_digit || pos == digits_end) {
                return false;
            }
            // At least one whitespace separator is required.
            if (line[pos] != ' ' && line[pos] != '\t') {
                return false;
            }
            while (pos > 0 && (line[pos] == ' ' || line[pos] == '\t')) {
                --pos;
            }
            if (line[pos] == ' ' || line[pos] == '\t') {
                return false;
            }
            name = line.substr(0, pos + 1);
            // Name must not start with whitespace.
            if (name.empty() || name.front() == ' ' || name.front() == '\t') {
                return false;
            }
            std::string pid_text = line.substr(pos + 1, digits_end - pos);
            pid                  = std::strtol(pid_text.c_str(), nullptr, 10);
            return true;
        }

    }  // namespace

    HostOs host_os() {
#if defined(_WIN32)
        return HostOs::windows;
#elif defined(__APPLE__)
        return HostOs::macos;
#elif defined(__linux__)
        return HostOs::linux_;
#else
        return HostOs::other;
#endif
    }

    const char* host_os_name(HostOs os) {
        switch (os) {
        case HostOs::windows:
            return "windows";
        case HostOs::macos:
            return "macos";
        case HostOs::linux_:
            return "linux";
        case HostOs::other:
            break;
        }
        return "unknown";
    }

    void init_platform(const Json& arguments, PlatformState& state) {
        state.os      = host_os();
        state.use_wsl = false;
        if (state.os == HostOs::windows && has(arguments, "useWSL") && arguments["useWSL"].is_boolean() && arguments["useWSL"].get<bool>()) {
            state.os      = HostOs::linux_;
            state.use_wsl = true;
        }
        state.use_utf8 = get_string(arguments, "sourceCoding") == "utf8";
    }

    std::string host_arch() {
#if defined(_WIN32)
        const char* value = std::getenv("PROCESSOR_ARCHITECTURE");
        const char* wow   = std::getenv("PROCESSOR_ARCHITEW6432");
        bool is64         = (value && std::string(value) == "AMD64") || (wow && std::string(wow) == "AMD64");
        return is64 ? "x86_64" : "x86";
#elif defined(__APPLE__)
        return "arm64";
#else
        return "x86_64";
#endif
    }

    std::vector<long> query_processes(const std::string& name) {
        std::string command;
        int skip = 0;
        switch (host_os()) {
        case HostOs::windows: {
            auto pwsh = find_powershell();
            if (!pwsh.empty()) {
                command = pwsh + " -NoProfile -Command \"Get-CimInstance Win32_Process | Select-Object Name,ProcessId\"";
                skip    = 3;
            } else {
                command = "wmic process get name,processid";
                skip    = 1;
            }
            break;
        }
        case HostOs::linux_:
            command = "ps axww -o comm=,pid=";
            break;
        case HostOs::macos:
            command = "ps axww -o comm=,pid= -c";
            break;
        case HostOs::other:
            return {};
        }

        std::vector<long> result;
#if defined(_WIN32)
        FILE* pipe = _popen(command.c_str(), "r");
#else
        FILE* pipe = popen(command.c_str(), "r");
#endif
        if (!pipe) {
            log_error("query_processes: cannot run %s", command.c_str());
            return result;
        }
        char buffer[4096];
        int line_no = 0;
        while (std::fgets(buffer, sizeof(buffer), pipe)) {
            ++line_no;
            if (line_no <= skip) {
                continue;
            }
            std::string line = buffer;
            std::string proc_name;
            long pid = 0;
            if (!parse_ps_line(line, proc_name, pid)) {
                continue;
            }
            if (proc_name == name) {
                result.push_back(pid);
            }
        }
#if defined(_WIN32)
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return result;
    }

    std::string find_powershell() {
#if defined(_WIN32)
        const char* path   = std::getenv("PATH");
        const char* pathex = std::getenv("PATHEXT");
        if (!path || !pathex) {
            return {};
        }
        auto dirs = split_on(path, ';');
        auto exts = split_on(pathex, ';');
        for (const char* candidate : { "pwsh", "powershell" }) {
            for (const auto& dir : dirs) {
                if (dir.empty()) {
                    continue;
                }
                for (const auto& ext : exts) {
                    if (ext.empty()) {
                        continue;
                    }
                    std::error_code ec;
                    auto full = fs::path(dir) / (std::string(candidate) + ext);
                    if (fs::exists(full, ec)) {
                        return candidate;
                    }
                }
            }
        }
        return {};
#else
        return {};
#endif
    }

    std::string current_executable_path() {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH * 4];
        DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        if (size == 0) {
            return {};
        }
        std::wstring wide(buffer, size);
        return path_to_utf8(fs::path(wide));
#elif defined(__APPLE__)
        char buffer[4096];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) != 0) {
            return {};
        }
        std::error_code ec;
        auto canonical = fs::weakly_canonical(fs::path(buffer), ec);
        if (ec) {
            return buffer;
        }
        return path_to_utf8(canonical);
#else
        std::error_code ec;
        auto path = fs::read_symlink("/proc/self/exe", ec);
        if (ec) {
            return {};
        }
        return path_to_utf8(path);
#endif
    }

    std::string workdir_from_executable() {
        auto exe = current_executable_path();
        if (exe.empty()) {
            return {};
        }
        return path_to_utf8(path_from_utf8(exe).parent_path().parent_path());
    }

}  // namespace luadebug::debugger::frontend
