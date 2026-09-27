cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(PawnDB LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Pawn.DB supports Windows and Linux only")
endif()
if(NOT CMAKE_SIZEOF_VOID_P MATCHES "^(4|8)$")
  message(FATAL_ERROR "Pawn.DB requires an x86 or x64 toolchain")
endif()

set(PAWNDB_LIBPQ_STATIC_LIBRARY "" CACHE FILEPATH "Static libpq archive built from PostgreSQL REL_18_6")
set(PAWNDB_LIBPQ_INCLUDE_DIR "" CACHE PATH "Directory containing libpq-fe.h")
set(PAWNDB_LIBPQ_COMMON_INCLUDE_DIR "" CACHE PATH "Directory containing postgres_ext.h")
set(PAWNDB_LIBPQ_EXTRA_LIBRARIES "" CACHE STRING "Additional static libpq archives and system libraries")
if(NOT EXISTS "${PAWNDB_LIBPQ_STATIC_LIBRARY}" OR
   NOT EXISTS "${PAWNDB_LIBPQ_INCLUDE_DIR}/libpq-fe.h" OR
   NOT EXISTS "${PAWNDB_LIBPQ_COMMON_INCLUDE_DIR}/postgres_ext.h")
  message(FATAL_ERROR "Set PAWNDB_LIBPQ_STATIC_LIBRARY and both PAWNDB_LIBPQ include directories to a static PostgreSQL 18.6 build")
endif()

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL 3.5 REQUIRED)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CURL OFF CACHE BOOL "" FORCE)
set(WITH_SSL OPENSSL CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_CACHING_SHA2_PASSWORD STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_SHA256_PASSWORD STATIC CACHE STRING "" FORCE)
foreach(plugin IN ITEMS DIALOG CLIENT_ED25519 PARSEC AUTH_GSSAPI_CLIENT MYSQL_CLEAR_PASSWORD)
  set(CLIENT_PLUGIN_${plugin} OFF CACHE STRING "" FORCE)
endforeach()
set(CLIENT_PLUGIN_PVIO_SHMEM STATIC CACHE STRING "" FORCE)
add_subdirectory(third_party/mariadb-connector-c EXCLUDE_FROM_ALL)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE STRING "" FORCE)
set(CURL_BROTLI OFF CACHE STRING "" FORCE)
set(CURL_ZSTD OFF CACHE STRING "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/curl EXCLUDE_FROM_ALL)

add_subdirectory(third_party/json EXCLUDE_FROM_ALL)
add_subdirectory(third_party/open.mp-sdk EXCLUDE_FROM_ALL)

add_library(pawndb_argon2 STATIC
  third_party/argon2/src/argon2.c
  third_party/argon2/src/core.c
  third_party/argon2/src/blake2/blake2b.c
  third_party/argon2/src/thread.c
  third_party/argon2/src/encoding.c
  third_party/argon2/src/ref.c)
target_include_directories(pawndb_argon2 PUBLIC third_party/argon2/include PRIVATE third_party/argon2/src)

add_library(pawndb_dependencies INTERFACE)
target_include_directories(pawndb_dependencies INTERFACE
  "${PAWNDB_LIBPQ_INCLUDE_DIR}"
  "${PAWNDB_LIBPQ_COMMON_INCLUDE_DIR}"
  third_party/mariadb-connector-c/include
  "${PROJECT_BINARY_DIR}/third_party/mariadb-connector-c/include")
target_link_libraries(pawndb_dependencies INTERFACE
  mariadbclient
  "${PAWNDB_LIBPQ_STATIC_LIBRARY}"
  ${PAWNDB_LIBPQ_EXTRA_LIBRARIES}
  OpenSSL::SSL OpenSSL::Crypto
  libcurl_static
  pawndb_argon2
  nlohmann_json::nlohmann_json)

foreach(target IN ITEMS pawndb_omp pawndb_samp)
  if(target STREQUAL "pawndb_samp" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    continue()
  endif()
  add_library(${target} SHARED src/${target}.cpp)
  set_target_properties(${target} PROPERTIES PREFIX "")
  target_link_libraries(${target} PRIVATE pawndb_dependencies)
  if(target STREQUAL "pawndb_omp")
    target_link_libraries(${target} PRIVATE OMP-SDK)
  else()
    target_include_directories(${target} PRIVATE third_party/samp-plugin-sdk)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
  endif()
endforeach()

add_executable(pawndb_linkage_check tests/linkage_check.cpp)
target_link_libraries(pawndb_linkage_check PRIVATE pawndb_dependencies)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(pawndb_linkage_check PRIVATE -static-libgcc -static-libstdc++)
endif()
enable_testing()
add_test(NAME linkage_check COMMAND pawndb_linkage_check)
