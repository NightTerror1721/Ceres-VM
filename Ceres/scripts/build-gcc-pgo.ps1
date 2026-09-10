<#
.SYNOPSIS
    Runs the full GCC Profile-Guided Optimization cycle for Ceres: instrument, train,
    reoptimize, verify.

.DESCRIPTION
    PGO replaces the compiler's static guesses about which branches are hot with real
    counts recorded from an actual run. That takes two builds against the same directory:

      1) Configure+build with CERES_PGO_GENERATE=ON - every branch gets an instrumentation
         counter.
      2) Run a representative workload against that instrumented build. This script uses
         libs/vm/benchmarks (it exercises the instruction dispatch loop directly) plus the
         full test suite for broader coverage.
      3) Reconfigure the *same* build directory with CERES_PGO_USE=ON and rebuild: the
         compiler now optimizes using the counts recorded in step 2.

    A "profile count data file not found" warning during step 3 is expected and benign for
    any .cpp the training run in step 2 never actually exercised (apps/cli/main.cpp, for
    instance, if you never ran ceres.exe itself) - the compiler just optimizes that one
    file without profile data, same as a plain -O3 build would.

    Only wired for GCC/Clang; MSVC's PGO workflow needs its own script once someone can
    verify its exact flags against a real MSVC build.

.PARAMETER BuildDir
    Where to configure and build, relative to the repo root. Re-used across both passes,
    since -fprofile-use looks for its .gcda files under here. Default: build\pgo

.PARAMETER SkipTrainingTests
    Skip running ctest during the training pass (step 2). The benchmark always runs -
    it's the one training workload this script can't meaningfully skip.

.EXAMPLE
    .\scripts\build-gcc-pgo.ps1
    .\scripts\build-gcc-pgo.ps1 -BuildDir build\pgo-custom -SkipTrainingTests
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build\pgo",
    [switch]$SkipTrainingTests
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot

Push-Location $repoRoot
try {
    $fullBuildDir = Join-Path $repoRoot $BuildDir
    $bench = Join-Path $fullBuildDir "bin\Release\ceres_vm_benchmarks.exe"

    Write-Host "==> [1/4] Configuring an instrumented build (CERES_PGO_GENERATE=ON)" -ForegroundColor Cyan
    # -DCERES_PGO_USE=OFF is not optional here: re-running this script against a $BuildDir that a
    # previous run left in step 4's CERES_PGO_USE=ON state would otherwise leave that cache
    # variable in place, and CeresSettings.cmake refuses a build with both flags on at once.
    cmake -S . -B $BuildDir -G "Ninja Multi-Config" -DCMAKE_CXX_COMPILER=g++ -DCERES_PGO_GENERATE=ON -DCERES_PGO_USE=OFF
    if ($LASTEXITCODE -ne 0) { throw "configure (generate) failed (exit $LASTEXITCODE)" }

    Write-Host "==> [2/4] Building the instrumented Release binaries" -ForegroundColor Cyan
    cmake --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) { throw "build (generate) failed (exit $LASTEXITCODE)" }

    Write-Host "==> [3/4] Training: running the benchmark to record a profile" -ForegroundColor Cyan
    if (-not (Test-Path $bench)) { throw "benchmark executable not found: $bench" }
    & $bench
    if ($LASTEXITCODE -ne 0) { throw "training benchmark run failed (exit $LASTEXITCODE)" }

    if (-not $SkipTrainingTests) {
        Write-Host "    ...and the test suite, for broader coverage" -ForegroundColor Cyan
        ctest --test-dir $BuildDir -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "training test run failed (exit $LASTEXITCODE)" }
    }

    Write-Host "==> [4/4] Reconfiguring to use the recorded profile (CERES_PGO_USE=ON)" -ForegroundColor Cyan
    cmake -S . -B $BuildDir -DCERES_PGO_GENERATE=OFF -DCERES_PGO_USE=ON
    if ($LASTEXITCODE -ne 0) { throw "configure (use) failed (exit $LASTEXITCODE)" }

    Write-Host "==> Rebuilding, now optimized against the recorded profile" -ForegroundColor Cyan
    cmake --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) { throw "build (use) failed (exit $LASTEXITCODE)" }

    Write-Host "==> Verifying the PGO-optimized build still passes every test" -ForegroundColor Cyan
    ctest --test-dir $BuildDir -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "post-PGO tests failed (exit $LASTEXITCODE)" }

    Write-Host "`n==> Benchmark (PGO-optimized, Release)" -ForegroundColor Cyan
    & $bench
    if ($LASTEXITCODE -ne 0) { throw "final benchmark run failed (exit $LASTEXITCODE)" }

    Write-Host "`nDone. PGO-optimized binaries in $BuildDir\bin\Release\" -ForegroundColor Green
    Write-Host "Re-run this script whenever the code changes enough that the recorded profile might be stale." -ForegroundColor DarkGray
}
finally {
    Pop-Location
}
