#pragma once

// Target-side value handles.
//
// Replaces the luadbg userdata wrapper around refvalue: handles are plain
// integers owned by the bridge, and the store decides how long each one may
// resolve.
//
// Lifetime rules (plan: "引用寿命"):
//   * a *path* handle (frame local, table member, upvalue, ...) is only valid
//     for the stop that produced it; `invalidate()` on continue/step makes it
//     fail instead of silently resolving against a new frame,
//   * a *pinned* handle holds a registry reference to a concrete value (evaluation
//     results, variables the client is looking at) and survives continues until
//     `release_all()`/`release()`.

#include <cstdint>
#include <string>
#include <vector>

#include "util/refvalue.h"
#include "vm/vm.h"

namespace luadebug::target {

    using HandleId = std::uint32_t;

    enum class HandleKind {
        path,
        pinned,
    };

    enum class EvalStatus {
        ok,
        invalid_handle,  // stale or unknown handle
        unresolved,      // the path no longer resolves to a value
    };

    class ValueStore {
    public:
        explicit ValueStore(vm::State* target);
        ~ValueStore();

        ValueStore(const ValueStore&)            = delete;
        ValueStore& operator=(const ValueStore&) = delete;

        // Starts a new stop. Path handles from the previous stop are dropped.
        void begin_stop();

        // Drops every path handle (continue/step/detach). Pinned handles are
        // untouched; registry references are freed by release_all().
        void invalidate();

        // Registers a resolution path. The path is stored by value.
        HandleId add_path(std::vector<refvalue::value> path);

        // Derives a child handle: parent path plus one member step.
        HandleId extend_path(HandleId parent, refvalue::value member);

        // Pins the value on top of the target stack and pops it.
        HandleId pin_top();

        // Pushes the handled value onto the target stack. On failure nothing is
        // pushed and `error` explains why.
        EvalStatus push(HandleId handle, std::string& error);

        // Releases one pinned handle (no-op for path handles).
        void release(HandleId handle);

        // Releases every pinned handle; safe to call at VM close.
        void release_all();

        // Assigns to a path handle (setVariable / setExpression write-back).
        EvalStatus assign(HandleId handle, std::string& error);

        std::size_t size() const {
            return entries_.size();
        }
        std::size_t pinned_count() const;

    private:
        struct Entry {
            HandleKind kind     = HandleKind::path;
            std::uint32_t epoch = 0;
            std::vector<refvalue::value> path;
            int registry_ref = -1;  // pinned only
        };

        Entry* find(HandleId handle);
        void clear(Entry& entry);

        vm::State* target_    = nullptr;
        std::uint32_t epoch_  = 0;
        HandleId next_handle_ = 1;
        std::vector<Entry> entries_;
        // handle 0 is never valid, so index by handle - 1.
        std::vector<bool> alive_;
    };

}  // namespace luadebug::target
