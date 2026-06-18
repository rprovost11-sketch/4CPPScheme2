# coverage.ps1 — cppScheme2 line coverage (test-coverage initiative, Phase 1)
#
# Tool: OpenCppCoverage (winget id OpenCppCoverage.OpenCppCoverage). MSVC has no
# gcov/lcov; OpenCppCoverage instruments the existing PDB-bearing build at runtime.
# Requires a RelWithDebInfo build (optimized + PDBs); writes build/RelWithDebInfo,
# leaving build/Release untouched. Artifacts land in build/coverage/ (gitignored).
#
# Baseline (2026-06-18, full battery + gc_test merged): 73.9% line coverage.
#
# Usage:  pwsh -File coverage.ps1            # build if needed, run battery + gc_test, merge
#         pwsh -File coverage.ps1 -NoBuild   # reuse existing RelWithDebInfo build
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'

$occ   = "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$root  = $PSScriptRoot
$rel   = Join-Path $root "build\RelWithDebInfo"
$cov   = Join-Path $root "build\coverage"
$tests = "D:\SWDEV\Languages\Lisp\scheme-tests"   # or $env:SCHEME_TESTS_DIR
$srfi  = "D:\SWDEV\Languages\Lisp\SRFI"           # so (srfi 64) resolves in macro suites

if (-not $NoBuild) {
    & $cmake --build (Join-Path $root "build") --config RelWithDebInfo --target cppscheme2 gc_test
}
New-Item -ItemType Directory -Force -Path $cov | Out-Null

# 1) Full gated battery (]suites all) — driven via stdin pass-through.
"]suites all" | & $occ --sources 4CPPScheme2 --excluded_sources mini-gmp --modules $rel `
    --export_type "binary:$cov\battery.cov" `
    --export_type "html:$cov\battery-html" `
    --export_type "cobertura:$cov\battery.xml" `
    --quiet -- "$rel\cppscheme2.exe" -T $tests -L $srfi

# 2) White-box GC suite (undercarriage gc_test) — internals the battery can't reach.
& $occ --sources 4CPPScheme2 --excluded_sources mini-gmp --modules $rel `
    --export_type "binary:$cov\gctest.cov" `
    --quiet -- "$rel\gc_test.exe"

# 3) Merge for the overall number + browsable report.
& $occ --input_coverage "$cov\battery.cov" --input_coverage "$cov\gctest.cov" `
    --export_type "html:$cov\merged-html" --export_type "cobertura:$cov\merged.xml" --quiet

$lr = ([xml](Get-Content "$cov\merged.xml")).coverage.'line-rate'
"cppScheme2 overall line coverage (battery + gc_test): {0:P1}" -f [double]$lr
"  HTML: $cov\merged-html\index.html"
