# SPDX-License-Identifier: GPL-3.0-or-later
#
# The parsers run under ASan+UBSan, so a missing sanitizer runtime must never
# go unnoticed. Locally it is dropped with a warning; with
# BETHCONV_SANITIZE_STRICT (set in CI) it is a configure error.
include(CheckCXXSourceCompiles)

option(BETHCONV_SANITIZE_STRICT
    "Fail configuration if a requested sanitizer is unavailable (set ON in CI)" OFF)

add_library(bethconv_sanitizers INTERFACE)
add_library(bethconv::sanitizers ALIAS bethconv_sanitizers)

function(_bethconv_sanitizer_available san out_var)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=${san}")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=${san}")
    # Cache key must include the sanitizer name or the first result sticks.
    check_cxx_source_compiles("int main() { return 0; }"
        BETHCONV_SAN_LINKS_${san})
    set(${out_var} "${BETHCONV_SAN_LINKS_${san}}" PARENT_SCOPE)
endfunction()

if(BETHCONV_SANITIZE)
    if(MSVC)
        # MSVC ships ASan only; UBSan does not exist there.
        if("address" IN_LIST BETHCONV_SANITIZE)
            target_compile_options(bethconv_sanitizers INTERFACE /fsanitize=address)
            message(STATUS "bethconv: sanitizers enabled: address")
        endif()
    else()
        # Distro package names do not match sanitizer names.
        set(_pkg_address "libasan")
        set(_pkg_undefined "libubsan")
        set(_pkg_thread "libtsan")
        set(_pkg_leak "liblsan")

        set(_enabled "")
        foreach(san IN LISTS BETHCONV_SANITIZE)
            set(_pkg "${_pkg_${san}}")
            if(NOT _pkg)
                set(_pkg "lib${san}san")
            endif()
            _bethconv_sanitizer_available("${san}" _ok)
            if(_ok)
                list(APPEND _enabled "${san}")
            elseif(BETHCONV_SANITIZE_STRICT)
                message(FATAL_ERROR
                    "bethconv: sanitizer '${san}' was requested but its runtime does not "
                    "link. Install it (Fedora: 'sudo dnf install ${_pkg}'; Debian/Ubuntu: "
                    "it ships with gcc/clang) or drop it from BETHCONV_SANITIZE.")
            else()
                message(WARNING
                    "bethconv: sanitizer '${san}' is unavailable and was dropped; this "
                    "build does not check for memory errors it would otherwise catch. "
                    "Fedora: 'sudo dnf install ${_pkg}'.")
            endif()
        endforeach()

        if(_enabled)
            string(REPLACE ";" "," _bethconv_san "${_enabled}")
            target_compile_options(bethconv_sanitizers INTERFACE
                -fsanitize=${_bethconv_san} -fno-omit-frame-pointer
                -fno-sanitize-recover=all)
            target_link_options(bethconv_sanitizers INTERFACE -fsanitize=${_bethconv_san})
            message(STATUS "bethconv: sanitizers enabled: ${_bethconv_san}")
        endif()
    endif()
endif()
