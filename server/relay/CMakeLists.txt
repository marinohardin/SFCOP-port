# sfcop-relay: the UDP rendezvous and relay (RENDEZVOUS.md). sfcop_relay_core holds everything with
# logic behind the Sender, TimeSource and Random seams; the executable adds the sockets.
# Added by server/CMakeLists.txt, which supplies sfcop_doctest and sfcop_strict_warnings.
set(SFCOP_RELAY_CORE_SOURCES
    Config.cpp Health.cpp RelayService.cpp Sha256.cpp UdpSocket.cpp Wire.cpp)

find_package(Threads REQUIRED)
add_library(sfcop_relay_core STATIC ${SFCOP_RELAY_CORE_SOURCES})
target_include_directories(sfcop_relay_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sfcop_relay_core PUBLIC Threads::Threads)
if(COMMAND sfcop_strict_warnings)
    sfcop_strict_warnings(sfcop_relay_core)
endif()

add_executable(sfcop-relay main.cpp)
target_link_libraries(sfcop-relay PRIVATE sfcop_relay_core)
add_executable(sfcop-relay-bench Bench.cpp)
target_link_libraries(sfcop-relay-bench PRIVATE sfcop_relay_core)
if(COMMAND sfcop_strict_warnings)
    sfcop_strict_warnings(sfcop-relay)
    sfcop_strict_warnings(sfcop-relay-bench)
endif()
install(TARGETS sfcop-relay RUNTIME DESTINATION bin)

if(SFCOP_SERVER_TESTS)
    file(GLOB SFCOP_RELAY_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp")
    add_executable(sfcop_relay_tests TestMain.cpp ${SFCOP_RELAY_TEST_SOURCES})
    target_link_libraries(sfcop_relay_tests PRIVATE sfcop_relay_core sfcop_doctest)
    if(COMMAND sfcop_strict_warnings)
        sfcop_strict_warnings(sfcop_relay_tests)
    endif()
    add_test(NAME sfcop_relay_tests COMMAND sfcop_relay_tests --no-version)
endif()
