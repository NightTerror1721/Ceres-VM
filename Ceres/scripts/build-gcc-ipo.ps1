<#
.SYNOPSIS
    Configures and builds Ceres in Release with GCC + IPO/LTO (the gcc-ipo preset).

.DESCRIPTION
    IPO/LTO defers final optimization to link time, when the compiler can see the whole
    program - every static library, not just the .cpp being compiled - so it can inline
    and eliminate dead code across the libvm/libcore/libdevices/... boundaries a normal
    build can't cross. It costs a slower link, nothing at runtime, so it's the one that
    should be on for anything you distribute or benchmark.

    Requires g++ and ninja on PATH (this repo builds them via MSYS2/mingw64 in CI).

.PARAMETER SkipTests
    Skip running ctest after the build.

.PARAMETER SkipBenchmark
    Skip running libs/vm/benchmarks after the build.

.EXAMPLE
    .\scripts\build-gcc-ipo.ps1
    .\scripts\build-gcc-ipo.ps1 -SkipTests -SkipBenchmark
#>
[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$SkipBenchmark
)

$ErrorActionPreference = "Stop"

# scripts\ sits directly under the Ceres/ CMake root, wherever this repo happens to be checked
# out - so cd there by the script's own location instead of assuming the caller's cwd.
$repoRoot = Split-Path -Parent $PSScriptRoot

Push-Location $repoRoot
try {
    Write-Host "==> Configuring gcc-ipo (GCC, Ninja Multi-Config, IPO/LTO on)" -ForegroundColor Cyan
    cmake --preset gcc-ipo
    if ($LASTEXITCODE -ne 0) { throw "cmake --preset gcc-ipo failed (exit $LASTEXITCODE)" }

    Write-Host "==> Building gcc-ipo-release" -ForegroundColor Cyan
    cmake --build --preset gcc-ipo-release
    if ($LASTEXITCODE -ne 0) { throw "cmake --build --preset gcc-ipo-release failed (exit $LASTEXITCODE)" }

    if (-not $SkipTests) {
        Write-Host "==> Running the test suite" -ForegroundColor Cyan
        ctest --preset gcc-ipo-release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "tests failed (exit $LASTEXITCODE)" }
    }

    if (-not $SkipBenchmark) {
        $bench = Join-Path $repoRoot "build\gcc-ipo\bin\Release\ceres_vm_benchmarks.exe"
        if (Test-Path $bench) {
            Write-Host "==> Benchmark (gcc-ipo, Release)" -ForegroundColor Cyan
            & $bench
            if ($LASTEXITCODE -ne 0) { throw "benchmark run failed (exit $LASTEXITCODE)" }
        }
        else {
            Write-Warning "Benchmark executable not found at $bench - skipping."
        }
    }

    Write-Host "`nDone. Binaries in build\gcc-ipo\bin\Release\" -ForegroundColor Green
}
finally {
    Pop-Location
}
