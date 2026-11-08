param(
    [string]$Compiler,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot

if (-not $Compiler) {
    $portableCompiler = Join-Path $projectRoot '.tools/w64devkit/bin/g++.exe'
    if (Test-Path -LiteralPath $portableCompiler) {
        $Compiler = $portableCompiler
    } else {
        $installedCompiler = Get-Command 'g++' -ErrorAction SilentlyContinue
        if (-not $installedCompiler) {
            throw 'GCC not found. Pass -Compiler <path-to-g++.exe> or place w64devkit in .tools/w64devkit.'
        }
        $Compiler = $installedCompiler.Source
    }
}

$buildDir = Join-Path $projectRoot 'build/gcc'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$logPath = Join-Path $buildDir 'validation.log'
$flags = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-DNDEBUG', '-march=native')

function Write-ValidationLog {
    param([string]$Message)
    Add-Content -LiteralPath $logPath -Value $Message -Encoding UTF8
    Write-Output $Message
}

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    $null = Get-Command $Program -ErrorAction Stop
    Write-ValidationLog ('> {0} {1}' -f $Program, ($Arguments -join ' '))
    # Keep native stderr in the log; the native exit code decides success.
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $nativeOutput = & $Program @Arguments 2>&1
        $nativeExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    $nativeOutput | ForEach-Object { Write-ValidationLog ($_.ToString()) }
    if ($nativeExit -ne 0) {
        throw ('Command failed with exit code {0}. See {1}' -f $nativeExit, $logPath)
    }
}

('GCC validation at {0:yyyy-MM-dd HH:mm:ss zzz}' -f (Get-Date)) | Set-Content -LiteralPath $logPath -Encoding UTF8
Invoke-Checked -Program $Compiler -Arguments @('--version')
Invoke-Checked -Program $Compiler -Arguments @('-dumpmachine')

# The organizer supplies main(). Locally the unchanged example supplies it.
$exampleExe = Join-Path $buildDir 'runner_example.exe'
$rootSources = @(Get-ChildItem -LiteralPath $projectRoot -Filter '*.cpp' -File | Sort-Object Name | ForEach-Object { $_.FullName })
Invoke-Checked -Program $Compiler -Arguments ($flags + @('-o', $exampleExe) + $rootSources)

$testsExe = Join-Path $buildDir 'tests.exe'
Invoke-Checked -Program $Compiler -Arguments ($flags + @('-I.', '-o', $testsExe,
    'tests/test_main.cpp', 'scheduler.cpp', 'execution_model.cpp'))

$referenceExe = Join-Path $buildDir 'reference_replay.exe'
Invoke-Checked -Program $Compiler -Arguments ($flags + @('-I.', '-o', $referenceExe,
    'tests/reference_replay.cpp', 'scheduler.cpp', 'execution_model.cpp'))

if (-not $SkipTests) {
    Invoke-Checked -Program $testsExe -Arguments @()
    Invoke-Checked -Program $testsExe -Arguments @('--demo')
    Invoke-Checked -Program $referenceExe -Arguments @()
    Write-ValidationLog 'The original runner below is an API smoke check, not proof of a feasible schedule.'
    Invoke-Checked -Program $exampleExe -Arguments @()
}

Write-Output "GCC validation succeeded. Log: $logPath"
