#include "frontend/temp_symbols.h"

#include <cstdlib>
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include "common/log.h"
#include "common/path_utf8.h"

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <unistd.h>
#endif

namespace luadebug::debugger::frontend {

    namespace {

        // One entry per distinct table; `counter` only numbers new files, so a
        // reused table never burns a name.
        struct Registry {
            std::map<std::string, std::string> files;  // serialized table -> path
            unsigned counter  = 0;
            bool atexit_added = false;
        };

        Registry& registry();
        void remove_created_files();

        Registry& registry() {
            static Registry instance;
            if (!instance.atexit_added) {
                instance.atexit_added = true;
                // Registered after the registry's own construction, so it runs
                // before the map is destroyed.
                std::atexit(remove_created_files);
            }
            return instance;
        }

        void remove_created_files() {
            std::error_code ec;
            for (const auto& item : registry().files) {
                fs::remove(path_from_utf8(item.second), ec);
            }
            registry().files.clear();
        }

        long process_id() {
#if defined(_WIN32)
            return static_cast<long>(GetCurrentProcessId());
#else
            return static_cast<long>(getpid());
#endif
        }

        // The table names addresses and patterns inside a running target; it is
        // not world-readable material. POSIX creates it 0600 before any byte
        // lands (the later truncating open keeps the mode).
        bool reserve_private_file(const fs::path& path) {
#if defined(_WIN32)
            std::error_code ec;
            return !fs::exists(path, ec);
#else
            const std::string text = path_to_utf8(path);
            const int fd           = ::open(text.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (fd < 0) {
                return false;
            }
            ::close(fd);
            return true;
#endif
        }

    }  // namespace

    bool temp_symbols_file(const std::vector<SymbolEntry>& symbols, std::string& path, std::string& error) {
        // The content is the identity: a second document built from the same
        // table reuses the file instead of orphaning another copy.
        const std::string content = dump_symbols(symbols).dump(
            -1, ' ', false, nlohmann::json::error_handler_t::replace
        );

        auto& state   = registry();
        auto existing = state.files.find(content);
        if (existing != state.files.end()) {
            path = existing->second;
            return true;
        }

        std::error_code ec;
        auto directory = fs::temp_directory_path(ec);
        if (ec) {
            error = "cannot locate a temp directory for the symbol table: " + ec.message();
            return false;
        }

        // A collision is only possible with a leftover file from another adapter
        // that had the same pid, so a few attempts are plenty.
        for (int attempt = 0; attempt < 64; ++attempt) {
            const unsigned serial = ++state.counter;
            auto candidate        = directory / ("luadebug-symbols-" + std::to_string(process_id()) + "-" + std::to_string(serial) + ".json");
            if (!reserve_private_file(candidate)) {
                continue;
            }
            const std::string candidate_text = path_to_utf8(candidate);
            if (!save_symbols_file(candidate_text, symbols, error)) {
                fs::remove(candidate, ec);
                return false;
            }
            log_info("frontend: symbol table moved to %s (%zu entries, %zu bytes)", candidate_text.c_str(), symbols.size(), content.size());
            state.files.emplace(content, candidate_text);
            path = std::move(candidate_text);
            return true;
        }
        error = "cannot create a temp file for the symbol table in " + path_to_utf8(directory);
        return false;
    }

}  // namespace luadebug::debugger::frontend
