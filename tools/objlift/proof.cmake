# The object lift's proof build (tools/objlift/README.md, "Proof"): the guest-mode spike's guest
# test, spike/guest-mode/guest_test.c, linked as sfcop is, with no guest mode.
#
#   cmake -B <build> --preset macos-arm64 -DCMAKE_PROJECT_INCLUDE=<repo>/tools/objlift/proof.cmake \
#         [-DSFCOP_OBJLIFT=OFF]
#
# With SFCOP_OBJLIFT ON the claimed functions are their lifted VC6 objects; OFF, the exe lift's
# bodies. The test prints the same [test] lines for both. (spike/guest-mode/guest.cmake builds the
# same test, but turns guest mode on whenever the build has HLE guest shims.)
include_guard(GLOBAL)

function(_sfcop_objlift_proof)
    if(NOT TARGET sfcop_lifted OR NOT TARGET sfcop_boot)
        message(STATUS "objlift proof: no lifted build here; nothing added")
        return()
    endif()
    set(_root "${CMAKE_SOURCE_DIR}")
    get_target_property(_defs sfcop_boot COMPILE_DEFINITIONS)
    get_target_property(_incs sfcop_boot INCLUDE_DIRECTORIES)
    add_executable(sfcop_guest_test "${_root}/spike/guest-mode/guest_test.c")
    target_include_directories(sfcop_guest_test PRIVATE ${_incs} "${_root}/src")
    target_compile_definitions(sfcop_guest_test PRIVATE ${_defs})
    target_compile_options(sfcop_guest_test PRIVATE -fno-strict-aliasing -fwrapv)
    target_link_libraries(sfcop_guest_test PRIVATE sfcop_src sfcop_boot sfcop_win32hle sfcop_lifted
        SDL2::SDL2 pthread m)
    if(TARGET sfcop_hle_guest)
        target_link_libraries(sfcop_guest_test PRIVATE sfcop_hle_guest)
    endif()
    add_dependencies(sfcop_guest_test SDL3-shared)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _sfcop_objlift_proof)
