$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $projectRoot 'output'
$submissionRoot = Join-Path $outputRoot 'submission'
$files = @('scheduler.cpp', 'execution_model.cpp', 'execution_model.h', 'spec_scheduler.h', 'spec_system.h')
New-Item -ItemType Directory -Force -Path $submissionRoot | Out-Null
$unexpected = Get-ChildItem -LiteralPath $submissionRoot -Force | Where-Object { $_.Name -notin $files -or $_.PSIsContainer }
if ($unexpected) { throw 'The output/submission directory contains unexpected files; review it before export.' }
foreach ($file in $files) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination (Join-Path $submissionRoot $file) -Force
}
$archive = Join-Path $outputRoot 'mvp-source.zip'
$sources = $files | ForEach-Object { Join-Path $submissionRoot $_ }
Compress-Archive -LiteralPath $sources -DestinationPath $archive -Force
Write-Output "Exported $archive"
Write-Output 'Source library only: the organizer supplies the runner and main().'

