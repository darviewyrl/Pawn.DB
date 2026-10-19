#!/usr/bin/env bash
set -euo pipefail
sanitizer=${1:?address or thread sanitizer required}
output="build/crypto-$sanitizer"
mkdir -p "$output"
flags=(-pthread "-fsanitize=$sanitizer" -fno-omit-frame-pointer -fPIE -g)
for source in argon2 core blake2/blake2b thread encoding ref; do
  gcc -std=c99 "${flags[@]}" -Ithird_party/argon2/include -Ithird_party/argon2/src \
    -c "third_party/argon2/src/$source.c" -o "$output/${source##*/}.o"
done
g++ -std=c++20 "${flags[@]}" -pie -Iinclude -Ithird_party/json/include \
  -Ithird_party/argon2/include -Ithird_party/omp-pawn-compiler/source \
  -Ithird_party/omp-pawn-compiler/source/linux -DHAVE_STDINT_H=1 \
  -DPAWN_CELL_SIZE=32 -DPAWNDB_OMP=1 tests/crypto_check.cpp "$output/"*.o \
  -lcrypto -o "$output/crypto_check"
"$output/crypto_check"

gcc -std=c99 "${flags[@]}" -DAMX_NODYNALOAD=1 -DHAVE_STDINT_H=1 -DHAVE_ENDIAN_H=1 -DPAWN_CELL_SIZE=32 \
  -Ithird_party/omp-pawn-compiler/source/linux -c third_party/omp-pawn-compiler/source/amx/amx.c -o "$output/amx.o"
g++ -std=c++20 "${flags[@]}" -pie -Iinclude -Ithird_party/json/include -Ithird_party/argon2/include \
  -Ithird_party/omp-pawn-compiler/source -Ithird_party/omp-pawn-compiler/source/linux \
  -DHAVE_STDINT_H=1 -DHAVE_ENDIAN_H=1 -DPAWN_CELL_SIZE=32 -DPAWNDB_OMP=1 \
  tests/compatibility_check.cpp "$output/"*.o -lcrypto -o "$output/compatibility_check"
"$output/compatibility_check"
