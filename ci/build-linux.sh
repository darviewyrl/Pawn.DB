#!/usr/bin/env bash
set -euo pipefail

arch="$1"
target="pawndb_$2"
root="$(pwd)"
deps="$root/build/deps"
mkdir -p "$deps"

if [[ "$arch" == x86 ]]; then
  bits=-m32
  openssl_target=linux-x86
else
  bits=-m64
  openssl_target=linux-x86_64
fi
export CFLAGS="$bits -fPIC"
export CXXFLAGS="$bits -fPIC"
export LDFLAGS="$bits"

mkdir -p "$deps/openssl-build"
(
  cd "$deps/openssl-build"
  perl "$root/third_party/openssl/Configure" "$openssl_target" no-shared no-tests no-module \
    --prefix="$deps/openssl" --libdir=lib --openssldir="$deps/openssl/ssl"
  make -j2 build_libs
  make install_dev
)
export PKG_CONFIG_PATH="$deps/openssl/lib/pkgconfig"

git clone --depth 1 --branch REL_18_6 https://github.com/postgres/postgres.git "$deps/postgres-source"
test "$(git -C "$deps/postgres-source" rev-parse HEAD)" = 724edf9bde9d356724ad384a2e196edc3c9f80f7
meson setup "$deps/postgres-build" "$deps/postgres-source" --buildtype=release \
  -Dauto_features=disabled -Dssl=openssl -Ddocs=disabled
ninja -C "$deps/postgres-build" \
  src/interfaces/libpq/libpq.a src/common/libpgcommon_shlib.a src/port/libpgport.a

cmake -S . -B build/ci -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$deps/openssl" \
  -DPAWNDB_LIBPQ_STATIC_LIBRARY="$deps/postgres-build/src/interfaces/libpq/libpq.a" \
  -DPAWNDB_LIBPQ_INCLUDE_DIR="$deps/postgres-source/src/interfaces/libpq" \
  -DPAWNDB_LIBPQ_COMMON_INCLUDE_DIR="$deps/postgres-source/src/include" \
  "-DPAWNDB_LIBPQ_EXTRA_LIBRARIES=$deps/postgres-build/src/common/libpgcommon_shlib.a;$deps/postgres-build/src/port/libpgport.a"
cmake --build build/ci --target "$target" pawndb_linkage_check pawndb_lifecycle_check pawndb_handle_registry_check pawndb_worker_pool_check pawndb_dispatch_check pawndb_connection_config_check pawndb_connection_manager_check pawndb_connection_natives_check pawndb_mariadb_session_check --parallel 2
ctest --test-dir build/ci --output-on-failure -R '^(linkage_check|lifecycle_check|handle_registry_check|worker_pool_check|dispatch_check|connection_config_check|connection_manager_check|connection_natives_check|mariadb_session_check)$'

binary="build/ci/$target.so"
test -f "$binary"
readelf -d "$binary" | tee build/ci/dynamic.txt
if grep -E 'NEEDED.*(libstdc\+\+|libgcc_s|libssl|libcrypto|libmariadb|libpq|libcurl)' build/ci/dynamic.txt; then
  echo 'Unexpected dynamic dependency' >&2
  exit 1
fi
