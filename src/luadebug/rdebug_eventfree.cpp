#include "rdebug_eventfree.h"

#include <memory>

#include "thunk/thunk.h"

namespace luadebug::eventfree {
    struct userdata {
        vm::Alloc l_allocf;
        void* l_ud;
        notify cb;
        void* ud;
#if !defined(LUADEBUG_DISABLE_THUNK)
        std::unique_ptr<thunk> f;
#endif
    };
    static void* fake_allocf(void* ud, void* ptr, size_t osize, size_t nsize) {
        userdata* self = (userdata*)ud;
        if (ptr != NULL && nsize == 0 && self->cb) {
            self->cb(self->ud, ptr);
        }
        return self->l_allocf(self->l_ud, ptr, osize, nsize);
    }
    void* create(vm::State* L, notify cb, void* ud) {
        userdata* self = new userdata;
        self->cb       = cb;
        self->ud       = ud;
        self->l_allocf = L->getallocf(&self->l_ud);
#if defined(LUADEBUG_DISABLE_THUNK)
        L->setallocf(fake_allocf, self);
#else
        self->f.reset(thunk_create_allocf((intptr_t)self, (intptr_t)fake_allocf));
        L->setallocf((vm::Alloc)self->f->data, self->l_ud);
#endif
        return self;
    }
    void destroy(vm::State* L, void* handle) {
        userdata* self = (userdata*)handle;
        L->setallocf(self->l_allocf, self->l_ud);
        delete self;
    }
}
