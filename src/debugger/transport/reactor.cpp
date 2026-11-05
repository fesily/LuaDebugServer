#include "transport/reactor.h"

#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <utility>

namespace luadebug::debugger::transport {

    Reactor& Reactor::instance() {
        static Reactor* reactor = new Reactor();
        return *reactor;
    }

    Reactor::Reactor()
        : guard_(boost::asio::make_work_guard(context_)) {
    }

    void Reactor::async(std::function<void()> fn) {
        tasks_.push_back(std::move(fn));
    }

    boost::asio::awaitable<void> Reactor::next_update() {
        co_await async_next_update(boost::asio::use_awaitable);
    }

    void Reactor::update(int timeout_ms) {
        if (in_update_) {
            // A completion tried to pump the loop again. Running the context
            // recursively would interleave two sets of handlers on the same
            // state, so the request is dropped instead (the outer round keeps
            // running anyway).
            return;
        }

        // Front of the round: everything queued before this call, including the
        // coroutines waiting for the next round.
        if (!tasks_.empty()) {
            auto tasks = std::move(tasks_);
            tasks_.clear();
            for (auto& task : tasks) {
                task();
            }
        }

        struct update_scope {
            bool& flag;
            explicit update_scope(bool& value)
                : flag(value) {
                flag = true;
            }
            ~update_scope() {
                flag = false;
            }
        } scope(in_update_);

        context_.restart();
        if (timeout_ms <= 0) {
            context_.poll();
            return;
        }
        context_.run_for(std::chrono::milliseconds(timeout_ms));
    }

}  // namespace luadebug::debugger::transport
