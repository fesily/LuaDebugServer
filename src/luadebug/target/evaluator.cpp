#include "target/evaluator.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace luadebug::target {

    namespace {

        struct Capture {
            std::string name;
            char kind = 'l';  // 'l' local, 'u' upvalue
            int index = 0;    // lua_getlocal / lua_getupvalue index
            int ref   = -1;   // registry reference to the live value
        };

        bool is_temporary_name(const char* name) {
            // Lua marks internal temporaries with a leading '('.
            return !name || name[0] == '(';
        }

        // Wrapper chunk, mirroring eval/readonly.lua and eval/readwrite.lua:
        // the frame's live names become locals of the chunk, and the returned
        // closure captures them when the user source references them.
        std::string build_wrapper(const std::vector<Capture>& captures, std::string_view source, bool statement) {
            std::string text;
            text.reserve(source.size() + captures.size() * 8 + 128);
            if (!captures.empty()) {
                text += "local ";
                for (std::size_t i = 0; i < captures.size(); ++i) {
                    if (i) {
                        text += ',';
                    }
                    text += captures[i].name;
                }
                text += '\n';
            }
            text += "return function(...)\n";
            if (!statement) {
                text += "return ";
            }
            text.append(source.data(), source.size());
            text += "\nend";
            if (statement) {
                // Second result: the values to write back. The C++ side only
                // applies entries whose name the closure actually captured.
                text += ",\nfunction()\nreturn {";
                for (std::size_t i = 0; i < captures.size(); ++i) {
                    if (i) {
                        text += ',';
                    }
                    text += "{name='";
                    text += captures[i].name;
                    text += "',val=";
                    text += captures[i].name;
                    text += '}';
                }
                text += "}\nend";
            }
            text += '\n';
            return text;
        }

        void set_chunk_environment(vm::State* L, int env_ref) {
            if (env_ref < 0) {
                return;
            }
            L->rawgeti(vm::idx::registry, env_ref);
            if (L->vm()->api().lua_version_num() == 501) {
                // 5.1 (and LuaJIT) attach the environment to the function.
                L->setfenv(-2);
            } else {
                // 5.2+ reach it through `_ENV`, the chunk's first upvalue.
                L->setupvalue(-2, 1);
            }
        }

        std::string take_message(vm::State* L) {
            std::string message = L->tostring(-1) ? L->tostring(-1) : "evaluation failed";
            L->pop(1);
            return message;
        }

        int find_capture(const std::vector<Capture>& captures, const char* name) {
            for (std::size_t i = 0; i < captures.size(); ++i) {
                if (captures[i].name == name) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

    }  // namespace

    Evaluator::Evaluator(vm::State* target, int frame_level)
        : target_(target)
        , frame_level_(frame_level) {
    }

    EvalOutcome Evaluator::run(std::string_view source, EvalMode mode) {
        EvalOutcome outcome;
        if (!target_) {
            outcome.status = CallStatus::error;
            outcome.error  = "no target state";
            return outcome;
        }

        vm::State* L         = target_;
        const bool statement = mode == EvalMode::statement;

        // 1. Read the frame's live names. These are plain reads and must happen
        //    on the *suspended* thread, at the level the caller measured.
        vm::FrameInfo outer;
        if (L->getstack(frame_level_, &outer) == 0) {
            outcome.status = CallStatus::error;
            outcome.error  = "the frame is no longer available";
            return outcome;
        }
        if (L->getinfo("f", &outer) == 0) {
            outcome.status = CallStatus::error;
            outcome.error  = "cannot inspect the frame's function";
            return outcome;
        }
        const int func_slot = L->gettop();

        std::vector<Capture> captures;
        int env_ref = -1;
        for (int i = 1;; ++i) {
            const char* name = L->getupvalue(func_slot, i);
            if (!name) {
                break;
            }
            if (name[0] == '\0') {
                L->pop(1);
                continue;
            }
            if (std::string_view(name) == "_ENV") {
                L->pushvalue(-1);
                env_ref = L->ref();
                L->pop(1);
                continue;
            }
            L->pushvalue(-1);
            captures.push_back(Capture { name, 'u', i, L->ref() });
            L->pop(1);
        }
        for (int i = 1;; ++i) {
            const char* name = L->getlocal(&outer, i);
            if (!name) {
                break;
            }
            if (is_temporary_name(name)) {
                L->pop(1);
                continue;
            }
            const int lt    = L->type(-1);
            const double ln = (lt == vm::type::number) ? L->tonumber(-1) : -1.0;
            const int ltop  = L->gettop();
            L->pushvalue(-1);
            captures.push_back(Capture { name, 'l', i, L->ref() });
            L->pop(1);
        }
        if (env_ref < 0 && L->vm()->api().lua_version_num() == 501) {
            L->getfenv(func_slot);
            env_ref = L->ref();
        }
        L->pop(1);  // frame function

        if (captures.empty() && source.empty()) {
            outcome.status = CallStatus::error;
            outcome.error  = "empty evaluation";
            return outcome;
        }

        // 2. Everything that *executes* code runs on a fresh thread: nesting a
        //    protected call into the suspended frame reuses the frame's stack
        //    slots and corrupts it.
        vm::State* co         = L->newthread();
        const int thread_slot = L->gettop();

        std::string names;
        for (std::size_t i = 0; i < captures.size(); ++i) {
            if (i) {
                names += ',';
            }
            names += captures[i].name;
        }
        std::string wrapper;
        if (!names.empty()) {
            wrapper = "local " + names + "\n";
        }
        wrapper += "return function(...)\n";
        if (!statement) {
            wrapper += "return ";
        }
        wrapper.append(source.data(), source.size());
        wrapper += "\nend";
        if (statement) {
            // Second chunk result: the current values, in capture order. Only the
            // names the user's code referenced are written back (see below).
            wrapper += ",\nfunction()\nreturn " + names + "\nend";
        }
        wrapper += "\n";

        if (co->loadbuffer(wrapper.data(), wrapper.size(), "=(EVAL)") != 0) {
            outcome.status = CallStatus::error;
            outcome.error  = co->tostring(-1) ? co->tostring(-1) : "compile error";
            L->settop(thread_slot - 1);
            return outcome;
        }
        if (env_ref >= 0) {
            // The chunk's _ENV upvalue, when the wrapper has one.
            co->rawgeti(vm::idx::registry, env_ref);
            if (!co->setupvalue(-2, 1)) {
                co->pop(1);
            }
        }
        if (co->pcall(0, statement ? 2 : 1, 0) != 0) {
            outcome.status = CallStatus::error;
            outcome.error  = co->tostring(-1) ? co->tostring(-1) : "compile error";
            L->settop(thread_slot - 1);
            return outcome;
        }

        const int update_slot  = statement ? co->gettop() : 0;
        const int closure_slot = statement ? update_slot - 1 : co->gettop();
        // Bind the wrapper's captured names to the live values.
        std::vector<std::string> referenced;
        for (int i = 1;; ++i) {
            const char* name = co->getupvalue(closure_slot, i);
            if (!name) {
                break;
            }
            if (std::string_view(name) == "_ENV") {
                co->pop(1);
                continue;
            }
            int capture = -1;
            for (std::size_t c = 0; c < captures.size(); ++c) {
                if (captures[c].name == name) {
                    capture = static_cast<int>(c);
                    break;
                }
            }
            if (capture < 0) {
                co->pop(1);
                continue;
            }
            referenced.emplace_back(name);
            co->rawgeti(vm::idx::registry, captures[static_cast<std::size_t>(capture)].ref);
            co->setupvalue(closure_slot, i);
        }

        // Forward the frame's varargs, read from the suspended frame.
        std::vector<int> vararg_refs;
        for (int i = 1;; ++i) {
            const char* name = L->getlocal(&outer, -i);
            if (!name) {
                break;
            }
            vararg_refs.push_back(L->ref());
        }
        co->pushvalue(closure_slot);
        // `call_base` is the slot *below* the first result after the call.
        const int call_base = co->gettop() - 1;
        for (std::size_t i = 0; i < vararg_refs.size(); ++i) {
            co->rawgeti(vm::idx::registry, vararg_refs[i]);
        }
        const int status = co->pcall(static_cast<int>(vararg_refs.size()), vm::kMultiRet, 0);
        for (int ref : vararg_refs) {
            L->unref(ref);
        }
        if (status != 0) {
            outcome.status = CallStatus::error;
            outcome.error  = co->tostring(-1) ? co->tostring(-1) : "runtime error";
            L->settop(thread_slot - 1);
            return outcome;
        }

        const int results = co->gettop() - call_base;
        if (statement) {
            // Ask the update closure for the current values, then write back the
            // ones the user's code actually referenced.
            const int update_values_base = co->gettop();
            co->pushvalue(update_slot);
            if (co->pcall(0, vm::kMultiRet, 0) != 0) {
                outcome.status = CallStatus::error;
                outcome.error  = co->tostring(-1) ? co->tostring(-1) : "update error";
                L->settop(thread_slot - 1);
                return outcome;
            }
            const int update_count = co->gettop() - update_values_base;
            for (std::size_t c = 0; c < captures.size(); ++c) {
                if (static_cast<int>(c) >= update_count || !write_back_) {
                    break;
                }
                bool used = false;
                for (const auto& name : referenced) {
                    if (name == captures[c].name) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    continue;
                }
                std::vector<refvalue::value> path;
                if (captures[c].kind == 'l') {
                    path.push_back(refvalue::FRAME_LOCAL { static_cast<std::uint16_t>(frame_level_), static_cast<std::int16_t>(captures[c].index) });
                } else {
                    path.push_back(refvalue::FRAME_FUNC { static_cast<std::uint16_t>(frame_level_) });
                    path.push_back(refvalue::UPVALUE { captures[c].index });
                }
                write_back_(path.data(), path.size(), co, update_values_base + 1 + static_cast<int>(c));
            }
        } else {
            // Move the results onto the caller's stack *before* dropping the
            // thread, so they end up above it and survive the remove below.
            for (int i = 0; i < results; ++i) {
                co->pushvalue(call_base + 1 + i);
                co->xmove(L, 1);
            }
        }

        for (const auto& capture : captures) {
            L->unref(capture.ref);
        }
        if (env_ref >= 0) {
            L->unref(env_ref);
        }
        // Drop the thread itself; results (if any) sit above it and shift down.
        L->remove(thread_slot);

        outcome.status   = CallStatus::ok;
        outcome.nresults = statement ? 0 : results;
        return outcome;
    }

}  // namespace luadebug::target
