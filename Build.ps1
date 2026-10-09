param([string]$Configuration = 'Release', [string]$Generator = 'Visual Studio 18 2026')
$ErrorActionPreference = 'Stop'
if (-not (Test-Path "$PSScriptRoot\third_party\pcre2\deps\sljit\sljit_src\sljitLir.c")) {
    git -C "$PSScriptRoot\third_party\pcre2" submodule update --init --depth 1
    if ($LASTEXITCODE) { throw 'Could not initialize the pinned SLJIT dependency.' }
}
cmake -S $PSScriptRoot -B "$PSScriptRoot\build" -G $Generator -A x64
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
cmake --build "$PSScriptRoot\build" --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
ctest --test-dir "$PSScriptRoot\build" -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Core regression tests failed.' }
Get-ChildItem "$PSScriptRoot\build\bin\$Configuration\*.exe"
