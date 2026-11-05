#pragma once

#include <stddef.h>

#include "vm/vm.h"

namespace luadebug::stdio {

    // Native consumer of the captured `print` / `io.write` calls.
    //
    // A sink belongs to one VM binding, not to the process: two VMs of the same
    // module stay distinguishable even when both capture output, and a session
    // that ends cannot leave a redirect writing into a sink that is gone.
    struct output_sink {
        virtual ~output_sink() = default;
        // `name` is "print" or "iowrite"; the arguments start at `first`.
        // Returns true when the call was consumed.
        virtual bool output(vm::State* hL, const char* name, int first) = 0;
    };

    // Installs (`sink != nullptr`) or drops the sink for `vm`.
    void set_output_sink(vm::Vm* vm, output_sink* sink);
    output_sink* output_sink_of(vm::Vm* vm);

    // Installs/removes the `print` and `io.write` patches on the target state.
    void open_print(vm::State* hL, bool enable);
    void open_iowrite(vm::State* hL, bool enable);
    enum class std_fd {
        STDIN = 0,
        STDOUT,
        STDERR,
    };

    class std_redirect {
    public:
#if defined(_WIN32)
        typedef void* handle_t;
#else
        typedef int handle_t;
#endif
    public:
        std_redirect();
        ~std_redirect();
        bool open(std_fd type);
        void close();
        size_t read(char* buf, size_t len);
#if defined(_WIN32)
        size_t peek();
#endif
    private:
        handle_t m_pipe[2];
        handle_t m_old;
        std_fd m_type;
    };
}
