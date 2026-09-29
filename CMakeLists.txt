cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(PawnDB LANGUAGES C CXX)
find_package(Threads REQUIRED)

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
set(CLIENT_PLUGIN_ZSTD OFF CACHE STRING "" FORCE)
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
  include
  "${PAWNDB_LIBPQ_INCLUDE_DIR}"
  "${PAWNDB_LIBPQ_COMMON_INCLUDE_DIR}"
  third_party/mariadb-connector-c/include
  "${PROJECT_BINARY_DIR}/third_party/mariadb-connector-c/include")
target_link_libraries(pawndb_dependencies INTERFACE
  Threads::Threads
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
    target_include_directories(${target} PRIVATE
      third_party/omp-pawn-compiler/source
      third_party/omp-pawn-compiler/source/linux)
    target_compile_definitions(${target} PRIVATE HAVE_STDINT_H=1 PAWN_CELL_SIZE=32 PAWNDB_OMP=1)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
      target_compile_options(${target} PRIVATE -Wno-attributes) # open.mp SDK marks x64 declarations cdecl.
    endif()
  else()
    target_include_directories(${target} PRIVATE third_party/samp-plugin-sdk)
    target_compile_definitions(${target} PRIVATE HAVE_STDINT_H=1)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_compile_definitions(${target} PRIVATE LINUX)
    endif()
    if(WIN32)
      target_sources(${target} PRIVATE src/pawndb_samp.def)
    endif()
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

add_executable(pawndb_lifecycle_check tests/lifecycle_check.cpp)
target_include_directories(pawndb_lifecycle_check PRIVATE include)
target_link_libraries(pawndb_lifecycle_check PRIVATE Threads::Threads)
add_test(NAME lifecycle_check COMMAND pawndb_lifecycle_check)

add_executable(pawndb_handle_registry_check tests/handle_registry_check.cpp)
target_include_directories(pawndb_handle_registry_check PRIVATE include)
add_test(NAME handle_registry_check COMMAND pawndb_handle_registry_check)

add_executable(pawndb_worker_pool_check tests/worker_pool_check.cpp)
target_include_directories(pawndb_worker_pool_check PRIVATE include)
target_link_libraries(pawndb_worker_pool_check PRIVATE Threads::Threads)
add_test(NAME worker_pool_check COMMAND pawndb_worker_pool_check)

add_executable(pawndb_update_checker_check tests/update_checker_check.cpp)
target_include_directories(pawndb_update_checker_check PRIVATE include
  third_party/curl/include third_party/json/include)
add_test(NAME update_checker_check COMMAND pawndb_update_checker_check)

add_executable(pawndb_dispatch_check tests/dispatch_check.cpp)
target_include_directories(pawndb_dispatch_check PRIVATE include)
target_link_libraries(pawndb_dispatch_check PRIVATE Threads::Threads)
add_test(NAME dispatch_check COMMAND pawndb_dispatch_check)

add_executable(pawndb_connection_config_check tests/connection_config_check.cpp)
target_include_directories(pawndb_connection_config_check PRIVATE include)
target_link_libraries(pawndb_connection_config_check PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME connection_config_check COMMAND pawndb_connection_config_check)

add_executable(pawndb_connection_manager_check tests/connection_manager_check.cpp)
target_include_directories(pawndb_connection_manager_check PRIVATE include)
target_link_libraries(pawndb_connection_manager_check PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME connection_manager_check COMMAND pawndb_connection_manager_check)

add_executable(pawndb_telemetry_check tests/telemetry_check.cpp)
target_include_directories(pawndb_telemetry_check PRIVATE include)
target_link_libraries(pawndb_telemetry_check PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME telemetry_check COMMAND pawndb_telemetry_check)

add_executable(pawndb_shutdown_check tests/shutdown_check.cpp)
target_include_directories(pawndb_shutdown_check PRIVATE include)
target_link_libraries(pawndb_shutdown_check PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME shutdown_check COMMAND pawndb_shutdown_check)

add_executable(pawndb_connection_natives_check tests/connection_natives_check.cpp)
target_include_directories(pawndb_connection_natives_check PRIVATE
  include third_party/omp-pawn-compiler/source third_party/omp-pawn-compiler/source/linux)
target_compile_definitions(pawndb_connection_natives_check PRIVATE
  HAVE_STDINT_H=1 PAWN_CELL_SIZE=32 PAWNDB_OMP=1)
target_link_libraries(pawndb_connection_natives_check PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME connection_natives_check COMMAND pawndb_connection_natives_check)

add_executable(pawndb_result_natives_check tests/result_natives_check.cpp)
target_include_directories(pawndb_result_natives_check PRIVATE
  include third_party/omp-pawn-compiler/source third_party/omp-pawn-compiler/source/linux)
target_compile_definitions(pawndb_result_natives_check PRIVATE
  HAVE_STDINT_H=1 PAWN_CELL_SIZE=32 PAWNDB_OMP=1)
target_link_libraries(pawndb_result_natives_check PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME result_natives_check COMMAND pawndb_result_natives_check)

add_executable(pawndb_async_query_check tests/async_query_check.cpp)
target_include_directories(pawndb_async_query_check PRIVATE
  include third_party/omp-pawn-compiler/source third_party/omp-pawn-compiler/source/linux)
target_compile_definitions(pawndb_async_query_check PRIVATE
  HAVE_STDINT_H=1 PAWN_CELL_SIZE=32 PAWNDB_OMP=1)
target_link_libraries(pawndb_async_query_check PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME async_query_check COMMAND pawndb_async_query_check)

add_executable(pawndb_result_lifecycle_check tests/result_lifecycle_check.cpp)
target_include_directories(pawndb_result_lifecycle_check PRIVATE include)
target_link_libraries(pawndb_result_lifecycle_check PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME result_lifecycle_check COMMAND pawndb_result_lifecycle_check)

add_executable(pawndb_sql_formatter_check tests/sql_formatter_check.cpp)
target_include_directories(pawndb_sql_formatter_check PRIVATE include third_party/json/include)
add_test(NAME sql_formatter_check COMMAND pawndb_sql_formatter_check)

add_executable(pawndb_mariadb_session_check tests/mariadb_session_check.cpp)
target_link_libraries(pawndb_mariadb_session_check PRIVATE pawndb_dependencies)
add_test(NAME mariadb_session_check COMMAND pawndb_mariadb_session_check)

add_executable(pawndb_postgres_session_check tests/postgres_session_check.cpp)
target_link_libraries(pawndb_postgres_session_check PRIVATE pawndb_dependencies)
add_test(NAME postgres_session_check COMMAND pawndb_postgres_session_check)

if(WIN32)
  add_executable(pawndb_omp_entry_check tests/omp_entry_check.cpp)
  target_link_libraries(pawndb_omp_entry_check PRIVATE OMP-SDK)
  add_test(NAME omp_entry_check COMMAND pawndb_omp_entry_check $<TARGET_FILE:pawndb_omp>)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(pawndb_samp_lifecycle_check tests/samp_lifecycle_check.cpp)
    target_include_directories(pawndb_samp_lifecycle_check PRIVATE third_party/samp-plugin-sdk)
    target_compile_definitions(pawndb_samp_lifecycle_check PRIVATE HAVE_STDINT_H=1)
    add_test(NAME samp_lifecycle_check COMMAND pawndb_samp_lifecycle_check $<TARGET_FILE:pawndb_samp>)
  endif()
endif()
