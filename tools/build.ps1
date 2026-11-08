param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
$buildDir = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $compiler) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'MSVC / Visual Studio Build Tools not found.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ tools not found.' }
    Import-Module (Join-Path $installation 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

$flags = @('/nologo', '/std:c++17', '/O2', '/W4', '/WX', '/EHsc', '/DNDEBUG', '/I.', "/Fo$buildDir/")
& cl.exe @flags '/Febuild/runner_example.exe' runner_example.cpp scheduler.cpp execution_model.cpp
if ($LASTEXITCODE -ne 0) { throw 'Example build failed.' }
& cl.exe @flags '/Febuild/tests.exe' tests/test_main.cpp scheduler.cpp execution_model.cpp
if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }
if (-not $SkipTests) {
    & (Join-Path $buildDir 'tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}

