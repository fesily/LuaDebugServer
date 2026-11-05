# Pinned third-party archives.
#
# Every dependency is fetched once into a shared cache directory
# (LUA_DEBUG_DEP_CACHE, by default `3rd/_deps`), verified against a pinned
# SHA256, and extracted there. Build trees never share binaries, but they may
# share this cache: the archives and the extracted sources are
# configuration-independent.
#
# `LUA_DEBUG_DEP_CACHE` may also be pre-populated by hand (or from a release
# tarball) to build offline; the hash of the archive is only checked when the
# archive is actually fetched, and an already extracted tree is reused as-is.

include_guard(GLOBAL)

set(LUA_DEBUG_DEP_CACHE "${CMAKE_SOURCE_DIR}/3rd/_deps" CACHE PATH
    "Directory holding the downloaded (and extracted) third-party dependencies")

# fetch_dep(NAME <name> URL <url> SHA256 <hash> EXTRACT_DIR <dir> STRIP_COMPONENTS <n>
#           [DEST_DIR <absolute dir>] [MARKERS <files...>])
#
# Downloads `<url>` into the cache and extracts it into
# `<LUA_DEBUG_DEP_CACHE>/<EXTRACT_DIR>`. Idempotent: when the extracted marker
# exists the whole step is skipped, so an offline cache works.
function(fetch_dep)
    cmake_parse_arguments(ARG "" "NAME;URL;SHA256;EXTRACT_DIR;STRIP_COMPONENTS;DEST_DIR" "MARKERS" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_URL OR NOT ARG_SHA256 OR (NOT ARG_EXTRACT_DIR AND NOT ARG_DEST_DIR))
        message(FATAL_ERROR "fetch_dep: NAME, URL, SHA256 and EXTRACT_DIR (or DEST_DIR) are required")
    endif()
    # A caller may legitimately ask for 0 (the Frida devkits keep their files at
    # the archive root); only a missing argument means the default.
    if("${ARG_STRIP_COMPONENTS}" STREQUAL "")
        set(ARG_STRIP_COMPONENTS 1)
    endif()

    set(cache_root "${LUA_DEBUG_DEP_CACHE}")
    if(ARG_DEST_DIR)
        # The Frida devkits live in the tree's `3rd/frida_<component>/<os>-<arch>`
        # layout (where the include paths point); only the archive is cached.
        set(dest "${ARG_DEST_DIR}")
    else()
        set(dest "${cache_root}/${ARG_EXTRACT_DIR}")
    endif()

    set(complete TRUE)
    foreach(marker IN LISTS ARG_MARKERS)
        if(NOT EXISTS "${dest}/${marker}")
            set(complete FALSE)
        endif()
    endforeach()
    if(complete)
        message(STATUS "lua-debug: ${ARG_NAME} cached at ${dest}")
        return()
    endif()

    get_filename_component(archive_name "${ARG_URL}" NAME)
    set(archive "${cache_root}/${archive_name}")
    file(MAKE_DIRECTORY "${cache_root}")

    set(need_download TRUE)
    if(EXISTS "${archive}")
        file(SHA256 "${archive}" actual)
        if(actual STREQUAL ARG_SHA256)
            set(need_download FALSE)
        endif()
    endif()
    if(need_download)
        # GitHub's release CDN answers transient 5xx/429 to a burst of unauthenticated
        # downloads (the CI matrix starts several jobs at once), and `file(DOWNLOAD)`
        # has no retry of its own: a single hiccup aborted configure with an HTTP 22.
        # The hash check makes a retry safe - a partial file can only be replaced.
        set(_attempts 1 2 3 4 5)
        set(download_code 1)
        set(download_message "")
        foreach(_attempt IN LISTS _attempts)
            message(STATUS "lua-debug: downloading ${ARG_URL} (attempt ${_attempt})")
            file(REMOVE "${archive}")
            file(DOWNLOAD "${ARG_URL}" "${archive}"
                EXPECTED_HASH "SHA256=${ARG_SHA256}"
                INACTIVITY_TIMEOUT 60
                TIMEOUT 600
                SHOW_PROGRESS
                STATUS download_status
                TLS_VERIFY ON)
            list(GET download_status 0 download_code)
            if(download_code EQUAL 0)
                break()
            endif()
            list(GET download_status 1 download_message)
            message(STATUS "lua-debug: attempt ${_attempt} failed: ${download_message}")
            if(_attempt LESS 5)
                # Escalating pause: the first refusal is usually a burst limit that
                # clears in seconds.
                math(EXPR _delay "${_attempt} * 5")
                execute_process(COMMAND ${CMAKE_COMMAND} -E sleep ${_delay})
            endif()
        endforeach()
        if(NOT download_code EQUAL 0)
            message(FATAL_ERROR
                "lua-debug: cannot fetch ${ARG_NAME} from ${ARG_URL}: ${download_message}\n"
                "  Place the archive at ${archive} or point LUA_DEBUG_DEP_CACHE at a populated cache.")
        endif()
    endif()

    message(STATUS "lua-debug: extracting ${archive_name} into ${dest}")
    # file(ARCHIVE_EXTRACT) has no strip option, so the archive is unpacked next
    # to the destination and the wrapper directory is removed afterwards.
    set(staging "${dest}.extract")
    file(REMOVE_RECURSE "${staging}" "${dest}")
    file(MAKE_DIRECTORY "${staging}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${staging}")

    file(GLOB staging_entries "${staging}/*")
    if(ARG_STRIP_COMPONENTS GREATER 0)
        list(LENGTH staging_entries entry_count)
        if(NOT entry_count EQUAL 1 OR NOT IS_DIRECTORY "${staging_entries}")
            message(FATAL_ERROR "lua-debug: ${archive_name} does not have a single top-level directory; cannot strip components")
        endif()
        file(RENAME "${staging_entries}" "${dest}")
        file(REMOVE_RECURSE "${staging}")
    else()
        file(RENAME "${staging}" "${dest}")
    endif()

    foreach(marker IN LISTS ARG_MARKERS)
        if(NOT EXISTS "${dest}/${marker}")
            message(FATAL_ERROR "lua-debug: ${ARG_NAME} extraction is incomplete (missing ${dest}/${marker})")
        endif()
    endforeach()
endfunction()
