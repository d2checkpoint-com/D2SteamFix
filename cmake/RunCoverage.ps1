param(
    [string]$BuildDirectory = "build-coverage",
    [string]$OpenCppCoverage = ""
)

$ErrorActionPreference = "Stop"
$sourceRoot = (Get-Item (Join-Path $PSScriptRoot "..")).FullName
$buildRoot = (Resolve-Path $BuildDirectory).Path
$inventory = Join-Path $buildRoot "Debug/steamfix_coverage_inventory.exe"
if (-not (Test-Path $inventory)) {
    throw "Build Debug with STEAMFIX_ENABLE_COVERAGE=ON before collecting coverage."
}

if (-not $OpenCppCoverage) {
    $coverageCommand = Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue
    if ($coverageCommand) {
        $OpenCppCoverage = $coverageCommand.Source
    } else {
        $OpenCppCoverage = Join-Path $env:ProgramFiles "OpenCppCoverage/OpenCppCoverage.exe"
    }
}
if (-not (Test-Path $OpenCppCoverage)) {
    throw "OpenCppCoverage was not found. Install OpenCppCoverage or pass -OpenCppCoverage."
}

$outputRoot = Join-Path $buildRoot "coverage"
New-Item -ItemType Directory -Force $outputRoot | Out-Null
$archivedTestLog = Join-Path $outputRoot "LastTest.log"
if (Test-Path $archivedTestLog) {
    Remove-Item -LiteralPath $archivedTestLog -Force
}
$coverageArguments = @(
    "--cover_children",
    "--continue_after_cpp_exception",
    "--modules", $buildRoot,
    "--sources", (Join-Path $sourceRoot "src"),
    "--sources", (Join-Path $sourceRoot "include"),
    "--export_type", "cobertura:$(Join-Path $outputRoot 'coverage.xml')",
    "--export_type", "html:$(Join-Path $outputRoot 'html')",
    "--", (Get-Command cmake).Source,
    "-DSTEAMFIX_INVENTORY=$inventory",
    "-DSTEAMFIX_CTEST=$((Get-Command ctest).Source)",
    "-DSTEAMFIX_BINARY_DIR=$buildRoot",
    "-P", (Join-Path $PSScriptRoot "RunCoverageTests.cmake")
)
& $OpenCppCoverage @coverageArguments
$coverageStatus = $LASTEXITCODE
if (Test-Path "LastCoverageResults.log") {
    Copy-Item "LastCoverageResults.log" (Join-Path $outputRoot "LastCoverageResults.log") -Force
}
$testLog = Join-Path $buildRoot "Testing/Temporary/LastTest.log"
if (Test-Path $testLog) {
    Copy-Item $testLog $archivedTestLog -Force
}

if (Test-Path (Join-Path $outputRoot "coverage.xml")) {
    $summaryArguments = @(
        (Join-Path $PSScriptRoot "SummarizeCoverage.py"),
        (Join-Path $outputRoot "coverage.xml"), "--source-root", $sourceRoot,
        "--output", (Join-Path $outputRoot "summary.md")
    )
    if ($coverageStatus -ne 0) {
        $summaryArguments += "--test-run-failed"
    }
    python @summaryArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Coverage report is incomplete; inspect coverage/summary.md."
    }
} else {
    throw "OpenCppCoverage did not create the Cobertura report."
}
if ($coverageStatus -ne 0) {
    throw "Coverage collection or a regression test failed with exit code $coverageStatus."
}
