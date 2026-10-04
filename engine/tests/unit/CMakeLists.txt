# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native unit tests of skydot_core, the Godot-free half of the extension. No
# editor and no test pack: the inputs are built in memory (support/). They link
# skydot_core_testable, the same sources compiled like the tests (see
# extension/CMakeLists.txt for why not skydot_core itself).
include(Catch)

add_executable(skydot_core_tests
    support/script_builder.cpp
    support/script_builder.hpp
    support/vm_rig.cpp
    support/vm_rig.hpp
    support/world_builder.cpp
    support/world_builder.hpp
    test_actors.cpp
    test_packages.cpp
    test_vm.cpp
    test_vm_objects.cpp
    test_vm_save.cpp
    test_vm_threads.cpp
    test_vpath.cpp
)
target_link_libraries(skydot_core_tests
    PRIVATE skydot_core_testable skydot::warnings Catch2::Catch2WithMain)

catch_discover_tests(skydot_core_tests PROPERTIES LABELS unit)
