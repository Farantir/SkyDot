# SPDX-License-Identifier: GPL-3.0-or-later
#
# Submodules, for dependencies without a vcpkg port.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nifly/CMakeLists.txt")
    message(FATAL_ERROR
        "bethconv: extern/nifly is empty. Run 'git submodule update --init --recursive'.")
endif()

# nifly sets C++17 for its directory. Raise it so the whole build uses one
# standard and we never depend on C++17/C++23 ABI compatibility.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# EXCLUDE_FROM_ALL so a build that never links nifly never compiles it.
add_subdirectory(nifly EXCLUDE_FROM_ALL)

# nifly parses untrusted bytes outside SpanReader, so it gets the same
# sanitizers as our code. Applied as raw options because nifly's
# install(EXPORT) rejects a dependency on our unexported sanitizer target.
get_target_property(_san_c bethconv_sanitizers INTERFACE_COMPILE_OPTIONS)
get_target_property(_san_l bethconv_sanitizers INTERFACE_LINK_OPTIONS)
if(_san_c)
    target_compile_options(nifly PRIVATE ${_san_c})
endif()
if(_san_l)
    target_link_options(nifly PUBLIC ${_san_l})
endif()

# Treat its headers as system headers so our -Werror set ignores them.
get_target_property(_nifly_inc nifly INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(nifly PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_nifly_inc}")
