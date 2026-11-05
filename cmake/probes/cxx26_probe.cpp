// Toolchain probe: the configuration refuses to continue unless the compiler
// really provides the C++26 language mode with working coroutines and the
// standard facilities the sources use, and unless Boost.Asio/Boost.Process v2
// coroutine integration compiles against the pinned Boost.
//
// This file is used twice:
//   * at configure time, compiled (not linked) by ToolchainChecks.cmake, so a
//     missing toolchain fails the configure step with one fixed error;
//   * as the `luadebug_toolchain_probe` target, which also *links* Boost and
//     fmt and runs the coroutine suspend/resume assertions under CTest.
//
// The static assertions cannot be turned off with a macro: a toolchain that
// lacks the features has to be installed, never emulated.

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#if !defined(__cplusplus) || __cplusplus <= 202302L
#    error "lua-debug requires the C++26 language mode (__cplusplus > 202302L)"
#endif

#if defined(_MSVC_LANG) && _MSVC_LANG <= 202302L
#    error "lua-debug requires the C++26 language mode (_MSVC_LANG > 202302L)"
#endif

#if !defined(__cpp_impl_coroutine) || __cpp_impl_coroutine < 201902L
#    error "lua-debug requires compiler coroutine support (__cpp_impl_coroutine >= 201902L)"
#endif

#if !defined(__cpp_lib_coroutine) || __cpp_lib_coroutine < 201902L
#    error "lua-debug requires <coroutine> (__cpp_lib_coroutine >= 201902L)"
#endif

#include <bit>
#include <span>

#if !defined(__cpp_lib_bit_cast) || __cpp_lib_bit_cast < 201806L
#    error "lua-debug requires std::bit_cast"
#endif

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/process/v2/process.hpp>
#include <fmt/format.h>

namespace probe {

    enum class tiny : unsigned char { zero, one };

    // Replaces bee/nonstd/bit.h, bee/utility/span.h, bee/nonstd/to_underlying.h
    // and bee/nonstd/unreachable.h: all four are standard now.
    inline unsigned float_bits(float value) {
        return std::bit_cast<unsigned>(value);
    }

    inline std::size_t span_size(const int* data, std::size_t count) {
        std::span<const int> view(data, count);
        return view.size();
    }

    inline unsigned underlying() {
        return std::to_underlying(tiny::one);
    }

    inline void assert_unreachable(bool flag) {
        if (!flag) {
            std::unreachable();
        }
    }

    // A coroutine whose body only runs when the coroutine is really supported:
    // it must suspend once and resume through its awaiter.
    struct resume_once {
        bool* flag = nullptr;
        bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> handle) const {
            handle.resume();
        }
        void await_resume() const noexcept {
        }
    };

    inline std::coroutine_handle<> chain(std::coroutine_handle<> handle) noexcept {
        return handle;
    }

    struct suspend_probe {
        struct promise_type {
            suspend_probe get_return_object() {
                return suspend_probe {std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_always initial_suspend() noexcept {
                return {};
            }
            std::suspend_always final_suspend() noexcept {
                return {};
            }
            void return_void() noexcept {
            }
            void unhandled_exception() noexcept {
            }
        };

        std::coroutine_handle<promise_type> handle;
    };

    inline suspend_probe coroutine_probe(bool* reached_body) {
        *reached_body = true;
        co_return;
    }

    // Asio: the transport layer is written as awaitable coroutines over
    // co_spawn + redirect_error(use_awaitable).
    inline boost::asio::awaitable<void> asio_probe(bool* done) {
        co_await boost::asio::post(boost::asio::use_awaitable);
        boost::system::error_code ignored;
        co_await boost::asio::post(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        *done = true;
        co_return;
    }

    // Boost.Process v2: async_wait is awaited as a coroutine, never as a
    // callback state machine.
    inline boost::asio::awaitable<int> process_probe(boost::process::v2::process& child) {
        co_await child.async_wait(boost::asio::use_awaitable);
        co_return 0;
    }

} // namespace probe

int main() {
    bool reached_body = false;
    auto coroutine    = probe::coroutine_probe(&reached_body);
    // initial_suspend is suspend_always, so the body must not have run yet.
    if (reached_body) {
        std::fputs("probe: coroutine body ran before resume\n", stderr);
        return 1;
    }
    coroutine.handle.resume();
    // done() is only true once the coroutine is suspended at its final suspend
    // point, which is where a resumed coroutine with suspend_always ends up.
    if (!reached_body || !coroutine.handle.done()) {
        std::fputs("probe: coroutine did not reach its final suspend\n", stderr);
        return 1;
    }
    coroutine.handle.destroy();

    if (probe::float_bits(1.0f) != 0x3f800000u || probe::span_size(nullptr, 0) != 0
        || probe::underlying() != 1u) {
        std::fputs("probe: standard library facilities misbehaved\n", stderr);
        return 1;
    }
    probe::assert_unreachable(true);

    bool asio_done = false;
    boost::asio::io_context context;
    boost::asio::co_spawn(
        context,
        [&asio_done]() -> boost::asio::awaitable<void> {
            co_await probe::asio_probe(&asio_done);
        },
        boost::asio::detached);
    context.run();
    if (!asio_done) {
        std::fputs("probe: asio awaitable coroutine did not complete\n", stderr);
        return 1;
    }

    std::string text = fmt::format("probe: c++{} coroutines + asio + fmt ok", 26);
    std::puts(text.c_str());
    (void) &probe::process_probe;
    return 0;
}
