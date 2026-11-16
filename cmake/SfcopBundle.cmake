# SFCOP.app: the native client as a macOS application bundle, for its owner's own use (DESIGN
# §12.2: no binary leaves the owner; this is local packaging, not a release).
#
#   sfcop_app             target, part of ALL with the lift on macOS: writes
#                         ${CMAKE_BINARY_DIR}/dist/SFCOP.app (packaging/macos/make_bundle.sh), every
#                         build, so CI builds it as a smoke test; scripts/dist.sh copies it to dist/
#                         at the repository root
#   sfcop_app_selftest    CTest: packaging/macos/check_bundle.py refuses each kind of file it must
#   sfcop_app_contents    CTest: the built bundle holds only the client's own files, none in the
#                         retail hash list; skipped when the bundle has not been built
#
# Windows and Linux packaging (a zip with the client beside its libraries, an AppImage) will be
# cases of their own here; nothing is built for them yet.
include_guard(GLOBAL)

if(NOT APPLE)
    return()
endif()

set(SFCOP_BUNDLE_ID "io.github.marinohardin.sfcop" CACHE STRING "The bundle identifier of SFCOP.app")
set(SFCOP_APP_DIR "${CMAKE_BINARY_DIR}/dist/SFCOP.app")
set(_sfcop_packaging "${CMAKE_SOURCE_DIR}/packaging/macos")

add_custom_target(sfcop_app ALL
    COMMAND "${CMAKE_COMMAND}" -E env "SFCOP_PYTHON=${Python3_EXECUTABLE}"
            "SFCOP_DECOMP_DIR=${SFCOP_DECOMP_DIR}"
            sh "${_sfcop_packaging}/make_bundle.sh" "${SFCOP_APP_DIR}" "$<TARGET_FILE:sfcop>"
            "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE:SDL3-shared>" "${CMAKE_SOURCE_DIR}"
            "${SFCOP_BUNDLE_ID}" "${PROJECT_VERSION}"
    COMMENT "Assembling ${SFCOP_APP_DIR}"
    VERBATIM)
add_dependencies(sfcop_app sfcop SDL3-shared)

add_test(NAME sfcop_app_selftest
    COMMAND "${Python3_EXECUTABLE}" "${_sfcop_packaging}/check_bundle.py" --self-test
            "${CMAKE_BINARY_DIR}/check-bundle-selftest")
add_test(NAME sfcop_app_contents
    COMMAND sh -c "[ -d \"$1\" ] || { echo \"skip: $1 is not built\"; exit 77; }; exec \"$0\" \"$2\" \"$1\" \"$3\""
            "${Python3_EXECUTABLE}" "${SFCOP_APP_DIR}" "${_sfcop_packaging}/check_bundle.py"
            "${SFCOP_DECOMP_DIR}/analysis/results/retail_hashes.sha256")
set_tests_properties(sfcop_app_contents PROPERTIES SKIP_RETURN_CODE 77)
