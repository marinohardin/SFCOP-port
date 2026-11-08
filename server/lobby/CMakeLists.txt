# sfcop-lobby: the lobby API (LOBBY_API.md). The UDP rendezvous and the relay are sfcop-relay (N4).
# sfcop_lobby_core holds everything with logic (no sockets); the executable adds the socket loop.
set(SFCOP_LOBBY_CORE_SOURCES
    Base64Url.cpp Config.cpp Http.cpp Json.cpp Lobby.cpp Log.cpp RateLimit.cpp RelayClient.cpp
    Sha256.cpp Ticket.cpp)

add_library(sfcop_lobby_core STATIC ${SFCOP_LOBBY_CORE_SOURCES})
target_include_directories(sfcop_lobby_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(COMMAND sfcop_strict_warnings)
    sfcop_strict_warnings(sfcop_lobby_core)
endif()

add_executable(sfcop-lobby main.cpp Server.cpp)
target_link_libraries(sfcop-lobby PRIVATE sfcop_lobby_core)
if(COMMAND sfcop_strict_warnings)
    sfcop_strict_warnings(sfcop-lobby)
endif()
install(TARGETS sfcop-lobby RUNTIME DESTINATION bin)

if(SFCOP_SERVER_TESTS)
    file(GLOB SFCOP_LOBBY_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp")
    add_executable(sfcop_lobby_tests TestMain.cpp ${SFCOP_LOBBY_TEST_SOURCES})
    find_package(Threads REQUIRED)
    target_link_libraries(sfcop_lobby_tests PRIVATE sfcop_lobby_core sfcop_doctest Threads::Threads)
    if(COMMAND sfcop_strict_warnings)
        sfcop_strict_warnings(sfcop_lobby_tests)
    endif()
    add_test(NAME sfcop_lobby_tests COMMAND sfcop_lobby_tests --no-version)
endif()
