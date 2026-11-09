# sfcop-directory: the stand-in for the original directory and its firewall detector
# (src/net/DIRECTORY_API.md). sfcop_directory_core holds the protocol, which the tests drive on
# bytes; the executable adds the sockets. Added by server/CMakeLists.txt, which supplies
# sfcop_doctest and sfcop_strict_warnings.
set(SFCOP_DIRECTORY_CORE_SOURCES
    Campaigns.cpp Config.cpp Directory.cpp Health.cpp TcpServer.cpp Wire.cpp)

find_package(Threads REQUIRED)
add_library(sfcop_directory_core STATIC ${SFCOP_DIRECTORY_CORE_SOURCES})
target_include_directories(sfcop_directory_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sfcop_directory_core PUBLIC Threads::Threads)
if(COMMAND sfcop_strict_warnings)
    sfcop_strict_warnings(sfcop_directory_core)
endif()

add_executable(sfcop-directory main.cpp)
target_link_libraries(sfcop-directory PRIVATE sfcop_directory_core)
if(COMMAND sfcop_strict_warnings)
    sfcop_strict_warnings(sfcop-directory)
endif()
install(TARGETS sfcop-directory RUNTIME DESTINATION bin)

if(SFCOP_SERVER_TESTS)
    file(GLOB SFCOP_DIRECTORY_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp")
    add_executable(sfcop_directory_tests TestMain.cpp ${SFCOP_DIRECTORY_TEST_SOURCES})
    target_link_libraries(sfcop_directory_tests PRIVATE sfcop_directory_core sfcop_doctest)
    if(COMMAND sfcop_strict_warnings)
        sfcop_strict_warnings(sfcop_directory_tests)
    endif()
    add_test(NAME sfcop_directory_tests COMMAND sfcop_directory_tests --no-version)
endif()
