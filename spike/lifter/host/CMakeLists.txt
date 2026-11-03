# Spike build: lifted StarFleetOP.exe + pcrecomp win32hle + host/main.c, native
# arm64 macOS. Shaped after redalert2-recomp's Linux/macOS branch.
#
#   cmake -S spike/lifter/host -B $WORK/build -G Ninja -DLIFTER=$WORK
#   cmake --build $WORK/build -j 4
cmake_minimum_required(VERSION 3.20)
project(sfcop_spike C)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(LIFTER "$SFCOP_ROOT/work/lifter" CACHE PATH "work/lifter")
set(PCRECOMP "${LIFTER}/pcrecomp")
set(GEN_DIR "${LIFTER}/out/gen")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
set(HLE ${PCRECOMP}/runtime/win32hle)
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_executable(sfcop main.c ${HLE_SOURCES} ${GEN_SOURCES})
# include/ first: its SDL2/SDL_ttf.h stands in for the missing SDL2_ttf.
target_include_directories(sfcop BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(sfcop PRIVATE ${GEN_DIR} ${HLE} ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(sfcop PRIVATE RECOMP_RETADDR=0u RECOMP_LOCAL_REGS
                                         RECOMP_MEM_BASE=0x100000000000ull)
set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${GEN_OPT};-fno-strict-aliasing;-fwrapv")
target_compile_options(sfcop PRIVATE -fno-strict-aliasing -fwrapv)
target_link_libraries(sfcop PRIVATE PkgConfig::SDL2 pthread m)
