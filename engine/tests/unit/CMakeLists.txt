# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native unit tests of skydot_core, the Godot-free half of the extension. No
# editor and no test pack: the inputs are built in memory (support/).
include(Catch)

add_executable(skydot_core_tests
    support/script_builder.cpp
    support/script_builder.hpp
    support/vm_rig.cpp
    support/vm_rig.hpp
    test_vm.cpp
    test_vm_objects.cpp
    test_vm_save.cpp
    test_vm_threads.cpp
    test_vpath.cpp
)
target_link_libraries(skydot_core_tests
    PRIVATE skydot_core skydot::warnings Catch2::Catch2WithMain)

catch_discover_tests(skydot_core_tests PROPERTIES LABELS unit)
