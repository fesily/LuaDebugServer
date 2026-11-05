#include "vm/mapping.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#    include <windows.h>
#elif defined(__APPLE__)
#    include <mach/mach.h>
#    include <mach/mach_vm.h>
#    include <sys/mman.h>
#    include <unistd.h>
#else
#    include <cstdio>
#    include <fstream>
#    include <sys/mman.h>
#    include <unistd.h>
#endif

namespace luadebug::vm {

    namespace {

        bool range_valid(const void* address, std::size_t length) {
            if (!address || length == 0) {
                return false;
            }
            const auto start = reinterpret_cast<std::uintptr_t>(address);
            const auto last  = start + (length - 1);
            // A range that wraps the address space cannot be mapped.
            return last >= start;
        }

    }  // namespace

#if defined(_WIN32)

    bool mapping_readable(const void* address, std::size_t length) {
        if (!range_valid(address, length)) {
            return false;
        }
        auto cursor        = reinterpret_cast<std::uintptr_t>(address);
        std::uintptr_t end = cursor + length;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION info {};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &info, sizeof(info)) == 0) {
                return false;
            }
            if (info.State != MEM_COMMIT) {
                return false;
            }
            // `PAGE_GUARD` is a modifier, not a base protection: stripping it with
            // `& 0xFF` would classify a guard page as readable and the read would
            // fault inside the debuggee.
            const DWORD protected_state = info.Protect & 0xFF;
            if ((info.Protect & PAGE_GUARD) != 0 || (info.Protect & PAGE_NOACCESS) != 0 || protected_state == PAGE_EXECUTE) {
                return false;
            }
            const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (region_end <= cursor) {
                return false;
            }
            cursor = region_end;
        }
        return true;
    }

    bool mapping_executable(const void* address) {
        if (!address) {
            return false;
        }
        MEMORY_BASIC_INFORMATION info {};
        if (::VirtualQuery(address, &info, sizeof(info)) == 0) {
            return false;
        }
        if (info.State != MEM_COMMIT) {
            return false;
        }
        if ((info.Protect & PAGE_GUARD) != 0 || (info.Protect & PAGE_NOACCESS) != 0) {
            return false;
        }
        const DWORD state = info.Protect & 0xFF;
        return state == PAGE_EXECUTE || state == PAGE_EXECUTE_READ || state == PAGE_EXECUTE_READWRITE || state == PAGE_EXECUTE_WRITECOPY;
    }
#else

#    if defined(__APPLE__)

    // `mincore` answers "resident", not "readable": a resident PROT_NONE page is
    // reported present and then faults, and a mapped but never-touched page is
    // reported absent. The region's protection is the only honest answer.
    bool mapping_readable(const void* address, std::size_t length) {
        if (!range_valid(address, length)) {
            return false;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        const auto end   = start + length;
        auto cursor      = start;
        while (cursor < end) {
            vm_address_t region = static_cast<vm_address_t>(cursor);
            vm_size_t size      = 0;
            vm_region_basic_info_data_64_t info {};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object_name      = MACH_PORT_NULL;
            if (::vm_region_64(::mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object_name) != KERN_SUCCESS) {
                return false;
            }
            // `vm_region_64` answers with the next region when the address itself is
            // unmapped, so the cursor must lie *inside* the region it returned.
            const auto region_start = static_cast<std::uintptr_t>(region);
            if (size == 0 || cursor < region_start || cursor >= region_start + size) {
                return false;
            }
            if ((info.protection & VM_PROT_READ) == 0) {
                return false;
            }
            cursor = region_start + size;
        }
        return true;
    }

#    else

    // Linux: the mapping's permission column is authoritative (residency is not
    // permission), and `/proc/self/maps` is already the source of truth for
    // executability below.
    bool mapping_readable(const void* address, std::size_t length) {
        if (!range_valid(address, length)) {
            return false;
        }
        std::ifstream maps("/proc/self/maps");
        if (!maps) {
            return false;
        }
        const auto start      = reinterpret_cast<std::uintptr_t>(address);
        const auto end        = start + length;
        std::uintptr_t cursor = start;
        std::string line;
        while (std::getline(maps, line)) {
            std::uintptr_t region_start = 0;
            std::uintptr_t region_end   = 0;
            char permissions[8]         = {};
            if (std::sscanf(line.c_str(), "%lx-%lx %7s", &region_start, &region_end, permissions) != 3) {
                continue;
            }
            while (cursor < end && cursor >= region_start && cursor < region_end) {
                if (permissions[0] != 'r') {
                    return false;
                }
                cursor = std::min(end, region_end);
            }
            if (cursor >= end) {
                return true;
            }
        }
        return false;
    }

#    endif

#    if defined(__APPLE__)

    bool mapping_executable(const void* address) {
        if (!address) {
            return false;
        }
        vm_address_t region = reinterpret_cast<vm_address_t>(address);
        vm_size_t size      = 0;
        vm_region_basic_info_data_64_t info {};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object_name      = MACH_PORT_NULL;
        if (::vm_region_64(::mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object_name) != KERN_SUCCESS) {
            return false;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        // `vm_region_64` answers with the next region when the address itself is
        // unmapped, so only exact containment counts.
        if (start < static_cast<std::uintptr_t>(region) || start >= static_cast<std::uintptr_t>(region) + size) {
            return false;
        }
        return (info.protection & VM_PROT_EXECUTE) != 0;
    }

#    else

    bool mapping_executable(const void* address) {
        if (!address) {
            return false;
        }
        std::ifstream maps("/proc/self/maps");
        if (!maps) {
            return false;
        }
        const auto target = reinterpret_cast<std::uintptr_t>(address);
        std::string line;
        while (std::getline(maps, line)) {
            std::uintptr_t start = 0;
            std::uintptr_t end   = 0;
            char permissions[8]  = {};
            if (std::sscanf(line.c_str(), "%lx-%lx %7s", &start, &end, permissions) != 3) {
                continue;
            }
            if (target < start || target >= end) {
                continue;
            }
            return permissions[2] == 'x';
        }
        return false;
    }

#    endif

#endif

    bool read_cstring(const void* address, std::size_t capacity, std::string& out) {
        out.clear();
        if (capacity == 0 || !mapping_readable(address, capacity)) {
            return false;
        }
        const char* text = static_cast<const char*>(address);
        // A marker may sit at the very end of its segment (5.1's 140-byte ident
        // does), so the whole capacity need not be readable: fall back to a
        // byte-wise check rather than refusing a genuine ident.
        const bool bulk = mapping_readable(address, capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            if (!bulk && !mapping_readable(text + i, 1)) {
                out.clear();
                return false;
            }
            const char c = text[i];
            // 5.1's ident is "$Lua: ... $\n$Authors: ... $\n$URL: ... $\n"; the
            // newline ends the section that carries the version, so it terminates
            // the marker just like NUL does.
            if (c == '\0' || c == '\n') {
                return !out.empty();
            }
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte > 0x7e) {
                out.clear();
                return false;
            }
            out.push_back(c);
        }
        // No terminator inside the bound: not a version marker.
        out.clear();
        return false;
    }

}  // namespace luadebug::vm
