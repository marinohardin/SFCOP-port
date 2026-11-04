# The new code under src/ and its tests, collected by name so that a package adds files without
# editing this build (CONVENTIONS §2, §3, §6):
#   src/**/*.c, *.cpp (and *.mm on macOS)      -> sfcop_src, linked into sfcop and sfcop_tests
#   *_macos.*, *_windows.*, *_linux.*           -> only on that platform (CONVENTIONS §4.2)
#   src/**/*Test.cpp, third_party/*Test.cpp     -> sfcop_tests, the doctest runner (CTest)
#   *Mock.*                                     -> tests only
#   src/hle/**/*Guest.c, *Guest.cpp             -> SFCOP_GUEST_SOURCES: the HLE modules' shims in
#                                                  pcrecomp's win32hle form (src/hle/README.md), built
#                                                  by the top-level CMakeLists.txt with the lift's
#                                                  headers, never into sfcop_src or the tests
# Left out of sfcop_src: src/app/main.cpp (the entry point), the lift-dependent files of src/app/guest/
# (the guest host, built with the lift by the top-level CMakeLists.txt; its tests run on a lifted
# program, not in sfcop_tests) and src/app/boot/ (the temporary boot platform's sources, no longer built).
include_guard(GLOBAL)

if(APPLE)
    set(SFCOP_PLATFORM_SUFFIX macos)
elseif(WIN32)
    set(SFCOP_PLATFORM_SUFFIX windows)
else()
    set(SFCOP_PLATFORM_SUFFIX linux)
endif()

file(GLOB_RECURSE _sfcop_src CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.c" "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.mm")
set(_sfcop_tests ${_sfcop_src})
set(SFCOP_GUEST_SOURCES ${_sfcop_src})
list(FILTER SFCOP_GUEST_SOURCES INCLUDE REGEX "/src/hle/.*Guest\\.(c|cpp)$")
list(FILTER _sfcop_src EXCLUDE REGEX "Guest\\.(c|cpp)$")
list(FILTER _sfcop_tests EXCLUDE REGEX "Guest\\.(c|cpp)$")
list(FILTER _sfcop_src EXCLUDE REGEX "/src/app/main\\.cpp$")
list(FILTER _sfcop_src EXCLUDE REGEX "/src/app/boot/")
# The guest host's own files include the lift's headers, so only they and the guest tests stay out;
# the lift-free helpers (GuestPump, GuestShots, FaultClass) and their doctests are built as usual.
set(_sfcop_lift_only "/src/app/guest/(Guest(Host|Exceptions|Game)|override|tests/)")
list(FILTER _sfcop_src EXCLUDE REGEX "${_sfcop_lift_only}")
list(FILTER _sfcop_tests EXCLUDE REGEX "${_sfcop_lift_only}")
list(FILTER _sfcop_src EXCLUDE REGEX "(Test|Mock)\\.(c|cpp|mm)$")
foreach(_other IN ITEMS macos windows linux)
    if(NOT _other STREQUAL SFCOP_PLATFORM_SUFFIX)
        list(FILTER _sfcop_src EXCLUDE REGEX "_${_other}\\.(c|cpp|mm)$")
    endif()
endforeach()
if(NOT APPLE)
    list(FILTER _sfcop_src EXCLUDE REGEX "\\.mm$")
endif()
list(FILTER _sfcop_tests INCLUDE REGEX "(Test|Mock)\\.(c|cpp|mm)$")
file(GLOB _sfcop_third_party_tests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/third_party/*Test.cpp")

if(_sfcop_src MATCHES "\\.mm(;|$)" OR _sfcop_tests MATCHES "\\.mm(;|$)")
    enable_language(OBJCXX)
endif()

if(_sfcop_src)
    add_library(sfcop_src STATIC ${_sfcop_src})
else()
    add_library(sfcop_src INTERFACE)
endif()
target_include_directories(sfcop_src INTERFACE "${CMAKE_SOURCE_DIR}/src")
if(_sfcop_src)
    target_include_directories(sfcop_src PRIVATE "${CMAKE_SOURCE_DIR}/src")
    sfcop_strict_warnings(sfcop_src)
endif()
# The backends' libraries (third_party/CMakeLists.txt): SDL3 for Window, Gpu and Input, miniaudio
# for Audio. PUBLIC so that the client and the tests see the same include paths.
if(_sfcop_src)
    target_link_libraries(sfcop_src PUBLIC SDL3::SDL3-static sfcop_miniaudio)
endif()

add_executable(sfcop_tests "${CMAKE_SOURCE_DIR}/cmake/TestMain.cpp" ${_sfcop_tests} ${_sfcop_third_party_tests})
target_include_directories(sfcop_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
sfcop_strict_warnings(sfcop_tests)
target_link_libraries(sfcop_tests PRIVATE sfcop_src sfcop_doctest SDL3::SDL3-static sfcop_miniaudio)
add_test(NAME sfcop_tests COMMAND sfcop_tests --no-version)
