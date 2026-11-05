#include "target/proto_view.h"

#include "vm/vm.h"

#include <algorithm>
#include <string_view>

namespace luadebug::target {

    std::string LineInfo::define_key(int line) const {
        auto it = define_lines_.find(line);
        return it == define_lines_.end() ? std::string() : it->second;
    }

    bool LineInfo::is_active(int line) const {
        return std::binary_search(active_lines_.begin(), active_lines_.end(), line);
    }

    int LineInfo::next_active_line(int line) const {
        const std::string key = define_key(line);
        if (key.empty()) {
            // Outside every function range: parser.lua would loop forever here,
            // so refuse instead of guessing.
            return 0;
        }
        for (int current = line; current <= max_line_; ++current) {
            if (define_key(current) != key) {
                return 0;
            }
            if (is_active(current)) {
                return current;
            }
        }
        return 0;
    }

    void LineInfo::add_function(FunctionLines lines) {
        int start = lines.start_line;
        int end   = lines.end_line;
        if (end == 0) {
            // Main chunk: Lua does not fill lastlinedefined.
            start = 1;
            end   = 0;
            for (int line : lines.active_lines) {
                end = (std::max)(end, line);
            }
            lines.start_line = start;
            lines.end_line   = end;
        }
        lines.key = std::to_string(lines.start_line) + "-" + std::to_string(lines.end_line);

        // define_lines covers the whole range, including inactive lines, so a
        // breakpoint on a blank line knows which function it belongs to.
        for (int line = lines.start_line; line <= lines.end_line; ++line) {
            auto inserted = define_lines_.emplace(line, lines.key);
            if (!inserted.second && inserted.first->second != lines.key) {
                // Nested/overlapping ranges: the innermost function wins, which
                // is the order the walker reports them in.
                inserted.first->second = lines.key;
            }
            max_line_ = (std::max)(max_line_, line);
        }
        for (int line : lines.active_lines) {
            max_line_ = (std::max)(max_line_, line);
        }

        functions_.push_back(std::move(lines));
    }

    void LineInfo::finalize() {
        active_lines_.clear();
        for (const auto& function : functions_) {
            for (int line : function.active_lines) {
                active_lines_.push_back(line);
            }
        }
        std::sort(active_lines_.begin(), active_lines_.end());
        active_lines_.erase(std::unique(active_lines_.begin(), active_lines_.end()), active_lines_.end());
    }

    bool build_line_info(vm::State* L, int func_index, LineInfo& out, std::string& error) {
        if (!L) {
            error = "no target state";
            return false;
        }
        // The prototype walker is per profile: the shared side never reads a
        // Proto itself.
        if (!L->vm()->api().collect_proto_lines(L->vm()->raw(), L->raw(), func_index, out, error)) {
            return false;
        }
        out.finalize();
        return true;
    }

    bool build_line_info_from_source(vm::State* L, std::string_view source, LineInfo& out, std::string& error) {
        if (!L) {
            error = "no target state";
            return false;
        }
        const int top = L->gettop();
        if (L->loadbuffer(source.data(), source.size(), "=(breakpoints)") != 0) {
            error = L->tostring(-1) ? L->tostring(-1) : "compile error";
            L->settop(top);
            return false;
        }
        bool ok = build_line_info(L, L->gettop(), out, error);
        L->settop(top);
        return ok;
    }

}  // namespace luadebug::target
