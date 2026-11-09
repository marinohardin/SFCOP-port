# The hosted services (CONVENTIONS §2: server/ has one directory per service, each with its own
# tests). Today: lobby/ (sfcop-lobby), relay/ (sfcop-relay), directory/ (sfcop-directory) and auth/
# (sfcop-auth). It builds standalone, which is how tests/test_server.py and
# the deploy script build it:
#
#   cmake -S server -B <dir> -G Ninja && cmake --build <dir> && ctest --test-dir <dir>
#
# A parent project may also add_subdirectory(server) (the integrator's one line, server/README.md);
# then the doctest target and the warning function are the parent's.
cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(sfcop_server CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SfcopWarnings.cmake")
    enable_testing()
endif()

option(SFCOP_SERVER_TESTS "Build the services' doctest runners" ON)

if(SFCOP_SERVER_TESTS AND NOT TARGET sfcop_doctest)
    # The same pinned release as third_party/CMakeLists.txt. SFCOP_DOCTEST_DIR points at an
    # existing extraction (a build tree's _deps/doctest-src) to avoid the download.
    if(SFCOP_DOCTEST_DIR)
        set(_doctest_dir "${SFCOP_DOCTEST_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(doctest
            URL https://github.com/doctest/doctest/releases/download/v2.5.3/doctest-v2.5.3.tar.gz
            URL_HASH SHA256=eb4a39729d091e43d4d593888bf4d145ee9a0f0e13f7404484b6f2fb60d5d50f
            DOWNLOAD_EXTRACT_TIMESTAMP ON
            SOURCE_SUBDIR do-not-add-its-cmake)
        FetchContent_MakeAvailable(doctest)
        set(_doctest_dir "${doctest_SOURCE_DIR}")
    endif()
    add_library(sfcop_doctest INTERFACE)
    target_include_directories(sfcop_doctest SYSTEM INTERFACE "${_doctest_dir}")
endif()

# Every service is added here and nowhere else.
add_subdirectory(lobby)
add_subdirectory(relay)
add_subdirectory(directory)
add_subdirectory(auth)
