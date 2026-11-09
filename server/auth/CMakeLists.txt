# sfcop-auth: the stand-in for the original account login, profile search and version check
# (src/net/DIRECTORY_API.md). sfcop_auth_core holds the protocol, which the tests drive on
# bytes; the executable adds the sockets. Added by server/CMakeLists.txt, which supplies
# sfcop_doctest and sfcop_strict_warnings.
set(SFCOP_AUTH_CORE_SOURCES
    Accounts.cpp Config.cpp Health.cpp Md5.cpp Sessions.cpp TcpServer.cpp Text.cpp)

find_package(Threads REQUIRED)
add_library(sfcop_auth_core STATIC ${SFCOP_AUTH_CORE_SOURCES})
target_include_directories(sfcop_auth_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sfcop_auth_core PUBLIC Threads::Threads)
if(COMMAND sfcop_strict_warnings)
    sfcop_strict_warnings(sfcop_auth_core)
endif()

add_executable(sfcop-auth main.cpp)
target_link_libraries(sfcop-auth PRIVATE sfcop_auth_core)
if(COMMAND sfcop_strict_warnings)
    sfcop_strict_warnings(sfcop-auth)
endif()
install(TARGETS sfcop-auth RUNTIME DESTINATION bin)

if(SFCOP_SERVER_TESTS)
    file(GLOB SFCOP_AUTH_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp")
    add_executable(sfcop_auth_tests TestMain.cpp ${SFCOP_AUTH_TEST_SOURCES})
    target_link_libraries(sfcop_auth_tests PRIVATE sfcop_auth_core sfcop_doctest)
    if(COMMAND sfcop_strict_warnings)
        sfcop_strict_warnings(sfcop_auth_tests)
    endif()
    add_test(NAME sfcop_auth_tests COMMAND sfcop_auth_tests --no-version)
endif()
