#include "target/value_store.h"

#include <type_traits>
#include <utility>

#include "target/protected.h"

namespace luadebug::target {

    namespace {

        // Resolution runs inside a protected C frame, which Lua counts as an
        // extra stack level: frame-relative paths must be shifted for the
        // duration of the call and shifted back afterwards.
        void shift_frames(std::vector<refvalue::value>& path, int delta) {
            for (auto& item : path) {
                luadebug::visit([delta](auto& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, refvalue::FRAME_LOCAL> || std::is_same_v<T, refvalue::FRAME_FUNC>) {
                        arg.frame = static_cast<std::uint16_t>(
                            static_cast<int>(arg.frame) + delta
                        );
                    }
                },
                                item);
            }
        }

    }  // namespace

    ValueStore::ValueStore(vm::State* target)
        : target_(target) {
    }

    ValueStore::~ValueStore() {
        release_all();
    }

    void ValueStore::begin_stop() {
        // A new stop invalidates everything that pointed at the old one.
        invalidate();
        ++epoch_;
    }

    void ValueStore::invalidate() {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].kind == HandleKind::path) {
                alive_[i] = false;
                entries_[i].path.clear();
            }
        }
    }

    HandleId ValueStore::add_path(std::vector<refvalue::value> path) {
        HandleId handle = next_handle_++;
        Entry entry;
        entry.kind  = HandleKind::path;
        entry.epoch = epoch_;
        entry.path  = std::move(path);
        entries_.push_back(std::move(entry));
        alive_.push_back(true);
        return handle;
    }

    HandleId ValueStore::extend_path(HandleId parent, refvalue::value member) {
        auto* entry = find(parent);
        if (!entry || entry->kind != HandleKind::path) {
            return 0;
        }
        std::vector<refvalue::value> path = entry->path;
        path.push_back(member);
        return add_path(std::move(path));
    }

    HandleId ValueStore::pin_top() {
        if (!target_) {
            return 0;
        }
        HandleId handle = next_handle_++;
        Entry entry;
        entry.kind         = HandleKind::pinned;
        entry.epoch        = epoch_;
        entry.registry_ref = target_->ref();
        entries_.push_back(std::move(entry));
        alive_.push_back(true);
        return handle;
    }

    ValueStore::Entry* ValueStore::find(HandleId handle) {
        if (handle == 0 || handle > entries_.size()) {
            return nullptr;
        }
        auto index = static_cast<std::size_t>(handle - 1);
        if (!alive_[index]) {
            return nullptr;
        }
        return &entries_[index];
    }

    void ValueStore::clear(Entry& entry) {
        if (entry.kind == HandleKind::pinned && entry.registry_ref >= 0 && target_) {
            target_->unref(entry.registry_ref);
            entry.registry_ref = -1;
        }
        entry.path.clear();
    }

    EvalStatus ValueStore::push(HandleId handle, std::string& error) {
        auto* entry = find(handle);
        if (!entry) {
            error = "invalid or stale value handle";
            return EvalStatus::invalid_handle;
        }
        if (!target_) {
            error = "no target state";
            return EvalStatus::unresolved;
        }
        if (entry->kind == HandleKind::pinned) {
            target_->rawgeti(vm::idx::registry, entry->registry_ref);
            return EvalStatus::ok;
        }
        // Resolution runs under the target error boundary: a metatable access
        // during resolution must not longjmp into the caller.
        struct Context {
            std::vector<refvalue::value>* path;
            int type;
        } context { &entry->path, vm::type::none };
        shift_frames(entry->path, 1);
        auto result = pcall(target_, [](vm::State* L, void* ud) -> int {
            auto* ctx = static_cast<Context*>(ud);
            int type  = refvalue::eval(ctx->path->data(), L);
            ctx->type = type;
            return type == vm::type::none ? 0 : 1; }, &context, 0, 1);
        shift_frames(entry->path, -1);
        if (result.status == CallStatus::error) {
            error = result.message;
            return EvalStatus::unresolved;
        }
        if (context.type == vm::type::none) {
            error = "value is no longer reachable";
            return EvalStatus::unresolved;
        }
        return EvalStatus::ok;
    }

    EvalStatus ValueStore::assign(HandleId handle, std::string& error) {
        auto* entry = find(handle);
        if (!entry || entry->kind != HandleKind::path) {
            error = "invalid or stale value handle";
            return EvalStatus::invalid_handle;
        }
        // The new value is on top of the target stack; it is passed as the
        // single argument of the protected call so the callback can see it.
        struct Context {
            std::vector<refvalue::value>* path;
            bool ok;
        } context { &entry->path, false };
        shift_frames(entry->path, 1);
        auto result = pcall(target_, [](vm::State* L, void* ud) -> int {
            auto* ctx = static_cast<Context*>(ud);
            ctx->ok  = refvalue::assign(ctx->path->data(), L);
            return 0; }, &context, 1, 0);
        shift_frames(entry->path, -1);
        if (result.status == CallStatus::error) {
            error = result.message;
            return EvalStatus::unresolved;
        }
        if (!context.ok) {
            error = "the target refused the assignment";
            return EvalStatus::unresolved;
        }
        return EvalStatus::ok;
    }

    void ValueStore::release(HandleId handle) {
        auto* entry = find(handle);
        if (!entry) {
            return;
        }
        clear(*entry);
        alive_[static_cast<std::size_t>(handle - 1)] = false;
    }

    void ValueStore::release_all() {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            clear(entries_[i]);
            alive_[i] = false;
        }
        entries_.clear();
        alive_.clear();
        next_handle_ = 1;
    }

    std::size_t ValueStore::pinned_count() const {
        std::size_t count = 0;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (alive_[i] && entries_[i].kind == HandleKind::pinned) {
                ++count;
            }
        }
        return count;
    }

}  // namespace luadebug::target
