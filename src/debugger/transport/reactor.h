#pragma once

// Process-wide transport scheduler.
//
// Replaces the bee `bpoll` reactor with a single `boost::asio::io_context` that
// every channel in this process shares. There is deliberately no I/O thread: the
// adapter and the target-side runtime drive it from their own loop
// (`update()`), so all channel state stays on its owner thread and needs no
// strands or locks.
//
// Two round-level behaviours are preserved from the previous implementation,
// because the transport semantics depend on them:
//   * work queued with `async()` runs at the front of the next round,
//   * `next_update()` suspends until the next round starts, which is what makes
//     a failed connection retry only once per round instead of spinning.

#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

namespace luadebug::debugger::transport {

    class Reactor {
    public:
        // Deliberately leaked: transports are used from static destructors (the
        // session hub drains its last events at process exit), and a
        // function-local static object would already be destroyed by then.
        static Reactor& instance();

        boost::asio::io_context& context() noexcept {
            return context_;
        }

        // Queues a task for the front of the next update round.
        void async(std::function<void()> fn);

        // Runs the queued tasks, then the round callbacks registered before this
        // call, then drives the I/O: `poll()` for a non-positive timeout, a
        // bounded `run_for()` otherwise.
        void update(int timeout_ms);

        // True while update() is on the stack. Completions must not pump again.
        bool in_update() const noexcept {
            return in_update_;
        }

        // Suspends the calling coroutine until the next update round starts.
        boost::asio::awaitable<void> next_update();

        // Implementation of next_update(): an async operation whose completion is
        // invoked at the front of the next update round.
        template <typename CompletionToken>
        auto async_next_update(CompletionToken&& token) {
            return boost::asio::async_initiate<CompletionToken, void()>(
                [this](auto&& handler) {
                    auto holder = std::make_shared<std::decay_t<decltype(handler)>>(std::move(handler));
                    async([holder]() { (*holder)(); });
                },
                token
            );
        }

    private:
        Reactor();

        boost::asio::io_context context_;
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_;
        std::vector<std::function<void()>> tasks_;
        std::vector<std::function<void()>> round_callbacks_;
        bool in_update_ = false;
    };

}  // namespace luadebug::debugger::transport
