#include "frontend/injector_helper.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <system_error>

#include "common/path_utf8.h"
#include "frontend/platform.h"

namespace luadebug::debugger::frontend {

    namespace {

        // `/usr/bin/arch` is the documented way to run a universal binary as its
        // other slice; the helper must be that architecture, not the adapter's.
        bool needs_arch_runner(const std::string& target_arch) {
#if defined(__APPLE__)
            const bool target_is_x64 = target_arch == "x64";
            return target_is_x64 != (host_arch() == "x86_64");
#else
            (void)target_arch;
            return false;
#endif
        }

        void append_arch_runner(std::vector<std::string>& command, const std::string& target_arch) {
            if (!needs_arch_runner(target_arch)) {
                return;
            }
            std::vector<std::string> prefix { "/usr/bin/arch", "-x86_64" };
            command.insert(command.begin(), prefix.begin(), prefix.end());
        }

        // Empty for an architecture this distribution has no helper for: guessing
        // "arm64" turned an unknown name into a misleading "Not found ...-arm64".
        std::string helper_suffix(const std::string& target_arch) {
            if (target_arch == "x64") {
                return "x64";
            }
            if (target_arch == "arm64") {
                return "arm64";
            }
            return std::string();
        }

        // ---- Mach-O / ELF architecture -------------------------------------

        constexpr std::uint32_t kMachMagic64  = 0xfeedfacfu;  // MH_MAGIC_64
        constexpr std::uint32_t kFatMagic     = 0xcafebabeu;
        constexpr std::uint32_t kFatMagic64   = 0xcafebabfu;
        constexpr std::uint32_t kCpuTypeArm64 = 0x0100000cu;
        constexpr std::uint32_t kCpuTypeX64   = 0x01000007u;

        std::uint32_t read_be32(const unsigned char* p) {
            return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
        }

        std::uint32_t read_le32(const unsigned char* p) {
            return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | std::uint32_t(p[0]);
        }

        std::string arch_name(std::uint32_t cpu_type) {
            switch (cpu_type) {
            case kCpuTypeArm64:
                return "arm64";
            case kCpuTypeX64:
                return "x64";
            default:
                return std::string();
            }
        }

    }  // namespace

    std::string executable_arch(const std::string& path) {
#if !defined(__APPLE__)
        (void)path;
        return std::string();
#else
        std::ifstream file(path_from_utf8(path), std::ios::binary);
        if (!file) {
            return std::string();
        }
        unsigned char header[8] {};
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (file.gcount() < 8) {
            return std::string();
        }
        // Thin Mach-O stores the magic little endian (0xfeedfacf in the file is
        // CF FA ED FE); a fat header stores it big endian, like its records.
        if (read_le32(header) == kMachMagic64) {
            return arch_name(read_le32(header + 4));
        }
        const std::uint32_t magic_be = read_be32(header);
        if (magic_be != kFatMagic && magic_be != kFatMagic64) {
            return std::string();
        }
        // Fat header: nfat_arch, then one (cputype, cpusubtype, offset, size,
        // align) record per slice, all big endian.
        unsigned char count[4] {};
        file.read(reinterpret_cast<char*>(count), sizeof(count));
        if (file.gcount() < 4) {
            return std::string();
        }
        const std::uint32_t slices = read_be32(count);
        const bool is_64           = magic_be == kFatMagic64;
        const std::size_t record   = is_64 ? 32 : 20;
        std::string host_slice;
        std::string any_slice;
        for (std::uint32_t i = 0; i < slices && i < 8; ++i) {
            unsigned char entry[32] {};
            file.read(reinterpret_cast<char*>(entry), static_cast<std::streamsize>(record));
            if (file.gcount() < static_cast<std::streamsize>(record)) {
                break;
            }
            const std::string name = arch_name(read_be32(entry));
            if (name.empty()) {
                continue;
            }
            if (any_slice.empty()) {
                any_slice = name;
            }
            // The OS runs the host's own slice when the binary has one.
            if ((host_arch() == "x86_64" && name == "x64") || (host_arch() != "x86_64" && name == "arm64")) {
                host_slice = name;
            }
        }
        return host_slice.empty() ? any_slice : host_slice;
#endif
    }

    bool resolve_injector_helper(const InjectContext& context, const std::string& target_arch, InjectorHelper& helper, std::string& error) {
        helper = InjectorHelper {};
        if (target_arch.empty()) {
            error = "cannot tell the target's architecture, so no injector helper can be chosen";
            return false;
        }
        const std::string suffix = helper_suffix(target_arch);
        if (suffix.empty()) {
            error = "no injector helper exists for target architecture '" + target_arch + "'";
            return false;
        }
        const auto bin       = path_from_utf8(context.workdir) / "bin";
        const auto candidate = bin / ("lua-debug-injector-" + suffix);
        std::error_code ec;
        if (!fs::exists(candidate, ec)) {
            error = "Not found " + path_to_utf8(candidate.filename()) + ".";
            return false;
        }
        helper.path  = path_to_utf8(candidate);
        helper.label = path_to_utf8(candidate.filename());
        if (needs_arch_runner(target_arch)) {
            helper.prefix = { "/usr/bin/arch", "-x86_64" };
        }
        return true;
    }

    std::vector<std::string> helper_command(const InjectorHelper& helper, const std::vector<std::string>& arguments) {
        std::vector<std::string> command = helper.prefix;
        command.push_back(helper.path);
        command.insert(command.end(), arguments.begin(), arguments.end());
        return command;
    }

    std::vector<std::string> run_as_target_arch(std::vector<std::string> command, const std::string& target_arch) {
        append_arch_runner(command, target_arch);
        return command;
    }

}  // namespace luadebug::debugger::frontend
