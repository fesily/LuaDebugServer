#include <autoattach/wait_dll.h>
#ifdef _WIN32

#    include <common/path_utf8.h>
#    include <windows.h>
#    include <winternl.h>
#else
#    include <gumpp.hpp>
#    ifdef __APPLE__
#        include <dlfcn.h>
#        include <mach-o/dyld.h>
#        include <mach/mach.h>

#        include <condition_variable>
#        include <deque>
#        include <mutex>
#        include <string>
#        include <thread>
#        include <unordered_set>
struct _GumDarwinAllImageInfos {
    int format;

    uint64_t info_array_address;
    size_t info_array_count;
    size_t info_array_size;

    uint64_t notification_address;

    bool libsystem_initialized;

    uint64_t dyld_image_load_address;

    uint64_t shared_cache_base_address;
};
extern "C" bool gum_darwin_query_all_image_infos(mach_port_t task, _GumDarwinAllImageInfos* infos);
#    endif
#endif
namespace luadebug::autoattach {
#ifdef _WIN32
    bool wait_dll(bool (*loaded)(const std::string&)) {
        typedef struct _LDR_DLL_UNLOADED_NOTIFICATION_DATA {
            ULONG Flags;                   // Reserved.
            PCUNICODE_STRING FullDllName;  // The full path name of the DLL module.
            PCUNICODE_STRING BaseDllName;  // The base file name of the DLL module.
            PVOID DllBase;                 // A pointer to the base address for the DLL in memory.
            ULONG SizeOfImage;             // The size of the DLL image, in bytes.
        } LDR_DLL_UNLOADED_NOTIFICATION_DATA, *PLDR_DLL_UNLOADED_NOTIFICATION_DATA;
        typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA {
            ULONG Flags;                   // Reserved.
            PCUNICODE_STRING FullDllName;  // The full path name of the DLL module.
            PCUNICODE_STRING BaseDllName;  // The base file name of the DLL module.
            PVOID DllBase;                 // A pointer to the base address for the DLL in memory.
            ULONG SizeOfImage;             // The size of the DLL image, in bytes.
        } LDR_DLL_LOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA;
        typedef union _LDR_DLL_NOTIFICATION_DATA {
            LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
            LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
        } LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;
        enum LdrDllNotificationReason : ULONG {
            LDR_DLL_NOTIFICATION_REASON_LOADED = 1,
            LDR_DLL_NOTIFICATION_REASON_UNLOADED
        };
        typedef VOID(CALLBACK * LdrDllNotification)(
            _In_ LdrDllNotificationReason NotificationReason,
            _In_ PLDR_DLL_NOTIFICATION_DATA const NotificationData,
            _In_opt_ PVOID Context
        );
        typedef NTSTATUS(NTAPI * LdrRegisterDllNotification)(
            _In_ ULONG Flags,
            _In_ LdrDllNotification NotificationFunction,
            _In_opt_ PVOID Context,
            _Out_ PVOID * Cookie
        );
        typedef NTSTATUS(NTAPI * LdrUnregisterDllNotification)(
            _In_ PVOID Cookie
        );
        struct DllNotification {
            LdrRegisterDllNotification dllRegisterNotification;
            LdrUnregisterDllNotification dllUnregisterNotification;
            PVOID Cookie;
            explicit operator bool() const {
                return dllRegisterNotification && dllUnregisterNotification;
            }
        };
        static DllNotification dllNotification = []() -> DllNotification {
            HMODULE hmodule;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, "ntdll.dll", &hmodule)) {
                return {};
            }
            return { (LdrRegisterDllNotification)GetProcAddress(hmodule, "LdrRegisterDllNotification"), (LdrUnregisterDllNotification)GetProcAddress(hmodule, "LdrUnregisterDllNotification") };
        }();
        if (!dllNotification) {
            return false;
        }
        dllNotification.dllRegisterNotification(
            0, [](LdrDllNotificationReason NotificationReason, PLDR_DLL_NOTIFICATION_DATA const NotificationData, PVOID Context) {
                if (NotificationReason == LdrDllNotificationReason::LDR_DLL_NOTIFICATION_REASON_LOADED) {
                    auto path = debugger::wide_to_utf8(NotificationData->Loaded.FullDllName->Buffer, NotificationData->Loaded.FullDllName->Length);
                    auto f    = (decltype(loaded))Context;
                    if (f(path)) {
                        if (dllNotification.Cookie)
                            dllNotification.dllUnregisterNotification(dllNotification.Cookie);
                    }
                }
            },
            loaded, &dllNotification.Cookie
        );
        return true;
    }
#elif defined(__APPLE__)
    using WaitDllCallBack_t = bool (*)(const std::string&);

    namespace {
        // Registered through the public dyld hook. Hooking dyld's internal
        // `gAllImages.notification` (the previous approach) requires that pointer
        // to be populated, which it is not until something registers for image
        // notifications: attaching an interceptor to a null address is what used
        // to crash the launcher in any process whose Lua was not loaded yet.
        //
        // `_dyld_register_func_for_add_image` replays every already-loaded image
        // synchronously. Instead of masking that replay with a flag (which drops
        // images that load during the registration window), the set of headers
        // known before registration is recorded, and the replay is recognised by
        // header. The callback then never needs a window that can lose a real
        // notification.
        struct State {
            std::mutex mtx;
            std::condition_variable cv;
            std::deque<std::string> pending;
            std::unordered_set<const struct mach_header*> known;
            WaitDllCallBack_t callback = nullptr;
            // Set once the probe reported success: the watcher is done.
            bool stopped = false;
        };

        // Intentionally never destroyed: a detached worker may still be waiting on
        // the condition variable at process exit, and static destruction order is
        // not something a loader hook can respect.
        State& state() {
            static State* instance = new State();
            return *instance;
        }

        void pending_worker() {
            auto& st = state();
            for (;;) {
                std::string path;
                WaitDllCallBack_t callback = nullptr;
                {
                    std::unique_lock lock(st.mtx);
                    st.cv.wait(lock, [&st]() { return st.stopped || !st.pending.empty(); });
                    if (st.stopped) {
                        return;
                    }
                    path = std::move(st.pending.front());
                    st.pending.pop_front();
                    callback = st.callback;
                }
                if (!callback) {
                    continue;
                }
                // `true` means the target was found and initialization was
                // scheduled; stop scanning every later image for it.
                if (callback(path)) {
                    std::lock_guard lock(st.mtx);
                    st.stopped = true;
                    st.pending.clear();
                    st.cv.notify_all();
                    return;
                }
            }
        }

        void on_dyld_image_added(const struct mach_header* header, intptr_t /*slide*/) {
            if (!header) {
                return;
            }
            auto& st = state();
            {
                // Replay and real additions take the same path; only headers that
                // were not known before registration are queued.
                std::lock_guard lock(st.mtx);
                if (st.stopped || !st.known.insert(header).second) {
                    return;
                }
            }
            Dl_info info {};
            if (dladdr(header, &info) == 0 || !info.dli_fname) {
                return;
            }
            {
                std::lock_guard lock(st.mtx);
                if (st.stopped) {
                    return;
                }
                st.pending.emplace_back(info.dli_fname);
            }
            st.cv.notify_one();
        }
    }  // namespace

    bool wait_dll(WaitDllCallBack_t loaded) {
        if (!loaded) {
            return false;
        }
        auto& st          = state();
        const auto before = _dyld_image_count();
        {
            std::lock_guard lock(st.mtx);
            st.callback = loaded;
            for (uint32_t i = 0; i < before; ++i) {
                st.known.insert(_dyld_get_image_header(i));
            }
        }
        static std::once_flag worker_once;
        std::call_once(worker_once, []() {
            std::thread(pending_worker).detach();
        });
        // Images that appear from here on are queued by the callback; the replay
        // of the ones recorded above is skipped as already known.
        _dyld_register_func_for_add_image(on_dyld_image_added);
        return true;
    }
#else

    // TODO: support linux

    bool wait_dll(bool (*loaded)(const std::string&)) {
        return false;
    }
#endif
}
