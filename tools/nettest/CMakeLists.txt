# sfcop_nettest, the Rendezvous test harness (README.md). A project of its own, not part of the
# client's build: it compiles the few sources of src/ it needs. The same scenarios run in
# sfcop_tests as src/net/NetPairTest.cpp.
#
#   cmake -S tools/nettest -B <build> -G Ninja && cmake --build <build> -j4
cmake_minimum_required(VERSION 3.28)
project(sfcop_nettest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(SFCOP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Threads REQUIRED)
add_executable(sfcop_nettest
    main.cpp
    "${SFCOP_ROOT}/src/app/Sha256.cpp"
    "${SFCOP_ROOT}/src/net/Envelope.cpp"
    "${SFCOP_ROOT}/src/net/PeerLink.cpp"
    "${SFCOP_ROOT}/src/net/PeerNames.cpp"
    "${SFCOP_ROOT}/src/net/RendezvousEngine.cpp"
    "${SFCOP_ROOT}/src/net/RendezvousMessages.cpp"
    "${SFCOP_ROOT}/src/net/RendezvousTransport.cpp"
    "${SFCOP_ROOT}/src/platform/Transport/TransportHost.cpp")
target_include_directories(sfcop_nettest PRIVATE "${SFCOP_ROOT}/src")
target_compile_options(sfcop_nettest PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sfcop_nettest PRIVATE Threads::Threads)
