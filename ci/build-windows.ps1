param(
  [ValidateSet('x86', 'x64')][string]$Arch,
  [ValidateSet('omp', 'samp')][string]$Adapter
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = (Get-Location).Path
$deps = Join-Path $root 'build/deps'
New-Item -ItemType Directory -Force $deps | Out-Null
$env:PATH = "C:\Program Files\NASM;C:\ProgramData\chocolatey\bin;C:\Strawberry\perl\bin;$env:PATH"

$opensslBuild = Join-Path $deps 'openssl-build'
$opensslInstall = Join-Path $deps 'openssl'
New-Item -ItemType Directory -Force $opensslBuild | Out-Null
Push-Location $opensslBuild
try {
  $opensslTarget = if ($Arch -eq 'x86') { 'VC-WIN32' } else { 'VC-WIN64A' }
  perl "$root/third_party/openssl/Configure" $opensslTarget no-shared no-tests no-module "--prefix=$($opensslInstall.Replace('\', '/'))"
  nmake build_libs
  nmake install_dev
} finally { Pop-Location }
$pkgDir = Join-Path $opensslInstall 'lib/pkgconfig'
New-Item -ItemType Directory -Force $pkgDir | Out-Null
@'
prefix=__PREFIX__
libdir=${prefix}/lib
includedir=${prefix}/include
Name: OpenSSL
Description: Pinned static OpenSSL
Version: 3.5.8
Libs: -L${libdir} -lssl -lcrypto -lws2_32 -lcrypt32
Cflags: -I${includedir}
'@.Replace('__PREFIX__', $opensslInstall.Replace('\', '/')) | Set-Content (Join-Path $pkgDir 'openssl.pc')
$env:PKG_CONFIG_PATH = $pkgDir.Replace('\', '/')
if ((pkg-config --modversion openssl) -ne '3.5.8') { throw 'Pinned OpenSSL not found by pkg-config' }

$pgSource = Join-Path $deps 'postgres-source'
git clone --depth 1 --branch REL_18_6 https://github.com/postgres/postgres.git $pgSource
if ((git -C $pgSource rev-parse HEAD) -ne '724edf9bde9d356724ad384a2e196edc3c9f80f7') {
  throw 'PostgreSQL source revision changed'
}
$pgBuild = Join-Path $deps 'postgres-build'
meson setup $pgBuild $pgSource --buildtype=release -Db_vscrt=mt -Dauto_features=disabled -Dssl=openssl -Ddocs=disabled "-Dextra_include_dirs=$($opensslInstall.Replace('\', '/'))/include" "-Dextra_lib_dirs=$($opensslInstall.Replace('\', '/'))/lib"
ninja -C $pgBuild src/interfaces/libpq/libpq.a src/common/libpgcommon_shlib.a src/port/libpgport.a

$pgExtra = "$pgBuild/src/common/libpgcommon_shlib.a;$pgBuild/src/port/libpgport.a;ws2_32;secur32;crypt32"
cmake -S . -B build/ci -G Ninja -DCMAKE_BUILD_TYPE=Release "-DOPENSSL_ROOT_DIR=$opensslInstall" "-DLIB_EAY_RELEASE=$opensslInstall/lib/libcrypto.lib" "-DSSL_EAY_RELEASE=$opensslInstall/lib/libssl.lib" "-DPAWNDB_LIBPQ_STATIC_LIBRARY=$pgBuild/src/interfaces/libpq/libpq.a" "-DPAWNDB_LIBPQ_INCLUDE_DIR=$pgSource/src/interfaces/libpq" "-DPAWNDB_LIBPQ_COMMON_INCLUDE_DIR=$pgSource/src/include" "-DPAWNDB_LIBPQ_EXTRA_LIBRARIES=$pgExtra"
$target = "pawndb_$Adapter"
$adapterCheck = if ($Adapter -eq 'omp') { 'pawndb_omp_entry_check' } else { 'pawndb_samp_lifecycle_check' }
$testPattern = if ($Adapter -eq 'omp') { '^(linkage_check|lifecycle_check|handle_registry_check|worker_pool_check|omp_entry_check)$' } else { '^(linkage_check|lifecycle_check|handle_registry_check|worker_pool_check|samp_lifecycle_check)$' }
cmake --build build/ci --target $target pawndb_linkage_check pawndb_lifecycle_check pawndb_handle_registry_check pawndb_worker_pool_check $adapterCheck --parallel 2
ctest --test-dir build/ci --output-on-failure -R $testPattern
$binary = "build/ci/$target.dll"
if (-not (Test-Path $binary)) { throw "Missing $binary" }
$imports = dumpbin /DEPENDENTS $binary
$imports | Out-File build/ci/dependencies.txt
if ($imports -match '(?i)(libssl|libcrypto|libmariadb|libpq|libcurl|vcruntime|msvcp).*\.dll') {
  throw 'Unexpected dynamic dependency'
}
