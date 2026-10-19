$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$legacy = 'build/tools/legacy-samp'
if (-not (Test-Path "$legacy/pawncc.exe")) {
  $zip = 'build/tools/samp037_svr_R2-1-1_win32.zip'
  New-Item -ItemType Directory -Force build/tools | Out-Null
  Invoke-WebRequest 'https://files.sampmobile.com/samp037_svr_R2-1-1_win32.zip' -OutFile $zip
  if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne 'E12E7483D4DF0349F52E2C5F47D6AFD3F782ACBC2BBB19FA61ADCED3BFFF2D90') {
    throw 'SA-MP compiler archive hash mismatch'
  }
  Expand-Archive $zip build/tools/samp037 -Force
  New-Item -ItemType Directory -Force $legacy | Out-Null
  Copy-Item build/tools/samp037/pawno/pawncc.exe, build/tools/samp037/pawno/pawnc.dll $legacy
}
cmake -S third_party/omp-pawn-compiler/source/compiler -B build/pawncc-modern32 -G Ninja -DCMAKE_BUILD_TYPE=Release '-DCMAKE_C_FLAGS=/DPAWN_CELL_SIZE=32' -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build/pawncc-modern32 --target pawncc --parallel 2
