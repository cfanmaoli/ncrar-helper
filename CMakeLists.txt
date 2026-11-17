cmake_minimum_required(VERSION 3.20)
project(ncrar-helper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0)

set(UNRAR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/unrar")
if(NOT EXISTS "${UNRAR_DIR}/dll.hpp")
    message(FATAL_ERROR "vendor/unrar is missing - run scripts/bootstrap-unrar.sh first")
endif()

# The same subset that the upstream 'make lib' target builds: the DLL API plus everything it needs.
set(UNRAR_OBJECTS
    rar strlist strfn pathfn smallfn global file filefn filcreat
    archive arcread unicode system crypt crc rawread encname resource match timefn rdwrfn
    consio options errhnd rarvm secpassword rijndael getbits sha1 sha256 blake2s hash extinfo
    extract volume list find unpack headers threadpool rs16 cmddata ui largepage
    filestr scantree dll qopen)

set(UNRAR_SOURCES)
foreach(obj ${UNRAR_OBJECTS})
    list(APPEND UNRAR_SOURCES "${UNRAR_DIR}/${obj}.cpp")
endforeach()

add_executable(ncrar-helper src/main.cpp ${UNRAR_SOURCES})
target_include_directories(ncrar-helper PRIVATE "${UNRAR_DIR}")
target_compile_definitions(ncrar-helper PRIVATE RAR_SMP RARDLL _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
target_link_libraries(ncrar-helper PRIVATE pthread)
target_compile_options(ncrar-helper PRIVATE -fvisibility=hidden)
# The wrapper is compiled with the project's own warning level and hardening; the vendored UnRAR
# sources keep compiling exactly as they ship upstream.
set_source_files_properties(src/main.cpp PROPERTIES COMPILE_OPTIONS
    "-Wall;-Wextra;-Werror")
# Protect every translation unit, including the vendored UnRAR parser - that is the code most
# exposed to malicious archives, so it must not rely on the wrapper's flags.
target_compile_options(ncrar-helper PRIVATE -fstack-protector-strong)
target_compile_definitions(ncrar-helper PRIVATE $<$<CONFIG:Release>:_FORTIFY_SOURCE=2>)

# Optional sanitizer build (used by CI): the wrapper gets ASAN+UBSAN, the vendored parser keeps
# both but loses only the alignment check - UnRAR deliberately performs unaligned loads in
# RawGet8(), which is harmless on arm64 yet undefined by the letter of the standard.
option(NCRAR_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(NCRAR_SANITIZE)
    target_compile_options(ncrar-helper PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(ncrar-helper PRIVATE -fsanitize=address,undefined)
    foreach(obj ${UNRAR_OBJECTS})
        set_source_files_properties("${UNRAR_DIR}/${obj}.cpp" PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment")
    endforeach()
endif()
