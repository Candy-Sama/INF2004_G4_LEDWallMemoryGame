param(
    [string]$Compiler = 'C:/msys64/mingw64/bin/gcc.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'build/host-tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

# Always compile fresh executables; do not accidentally run an older passing build.
$suites = @(
    @{ Name = 'mqtt_handler'; Includes = @('tests/mqtt_stubs', 'src/network', 'config'); Sources = @('tests/mqtt_handler_test.c', 'src/network/mqtt_handler.c') },
    @{ Name = 'wifi_manager'; Includes = @('tests/wifi_stubs', 'src/network'); Sources = @('tests/wifi_manager_test.c', 'src/network/wifi_manager.c') },
    @{ Name = 'motion_detector'; Includes = @('config', 'src/sensors'); Sources = @('tests/motion_detector_test.c') },
    @{ Name = 'wifi_setup'; Includes = @('tests/wifi_stubs', 'src/network'); Sources = @('tests/wifi_setup_test.c') }
)

foreach ($suite in $suites) {
    $arguments = @('-std=c11', '-Wall', '-Wextra', '-Werror')
    foreach ($include in $suite.Includes) {
        $arguments += '-I' + (Join-Path $projectRoot $include)
    }
    foreach ($source in $suite.Sources) {
        $arguments += Join-Path $projectRoot $source
    }
    $executable = Join-Path $outputDirectory ($suite.Name + '_test.exe')
    $arguments += @('-o', $executable)
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "$($suite.Name): compilation failed" }

    $testOutput = @(& $executable 2>&1)
    $testExitCode = $LASTEXITCODE
    $testOutput | Set-Content -LiteralPath (Join-Path $outputDirectory ($suite.Name + '.log'))
    if ($testExitCode -ne 0) {
        $testOutput | Write-Output
        throw "$($suite.Name): test failed (exit $testExitCode)"
    }
    if ($suite.Name -eq 'wifi_setup' -and
        (($testOutput -join "`n") -match 'HiddenFirst123|HiddenSecond456')) {
        throw 'wifi_setup: test password was echoed'
    }
    Write-Output "PASS: $($suite.Name)"
}

Write-Output "All four host test suites passed. Detailed logs: $outputDirectory"
Write-Output 'These mock-based tests do not verify the FreeRTOS scheduler or a real Pico/network.'
