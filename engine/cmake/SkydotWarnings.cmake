# SPDX-License-Identifier: GPL-3.0-or-later
#
# Warning set as an interface target, same as bethconv's: packs are still
# untrusted files.
add_library(skydot_warnings INTERFACE)
add_library(skydot::warnings ALIAS skydot_warnings)

if(MSVC)
    target_compile_options(skydot_warnings INTERFACE
        /W4 /permissive- /utf-8
        /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14545 /w14546
        /w14547 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
    if(SKYDOT_WERROR)
        target_compile_options(skydot_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(skydot_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
        -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
    if(SKYDOT_WERROR)
        target_compile_options(skydot_warnings INTERFACE -Werror)
    endif()
endif()
