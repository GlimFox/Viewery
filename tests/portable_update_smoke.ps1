param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+[IO.Path]::PathSeparator+$env:PSModulePath
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root=Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$helper=Join-Path $PSScriptRoot '../scripts/Update-Portable.ps1'
$ps=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
function Run-Update($zip,$sums,$target) {
 $old=$ErrorActionPreference;$ErrorActionPreference='Continue'
 try {& $ps -NoProfile -File $helper -Version '0.2.0-beta.9' -Destination $target -ParentPid 0 -TestMode -TestArchive $zip -TestChecksums $sums *> (Join-Path $root 'last-run.log');return $LASTEXITCODE}
 finally {$ErrorActionPreference=$old}
}
$payload=Join-Path $root 'payload';$target=Join-Path $root 'app'
New-Item -ItemType Directory -Path $payload,$target | Out-Null
[IO.File]::WriteAllText((Join-Path $payload 'Viewery.exe'),'new executable')
[IO.File]::WriteAllText((Join-Path $payload 'VieweryShell.dll'),'new shell')
[IO.File]::WriteAllText((Join-Path $target 'Viewery.exe'),'old executable')
[IO.File]::WriteAllText((Join-Path $target 'VieweryShell.dll'),'old shell')
[IO.File]::WriteAllText((Join-Path $target 'collection.sqlite3'),'private database')
[IO.File]::WriteAllText((Join-Path $target 'private.jpg'),'private media')
$zip=Join-Path $root 'payload.zip';Compress-Archive -Path (Join-Path $payload '*') -DestinationPath $zip
$sums=Join-Path $root 'SHA256SUMS.txt'
function Set-Sums($path) {((Get-FileHash -LiteralPath $path).Hash+'  Viewery-0.2.0-beta.9-Windows-x64-portable.zip') | Set-Content -LiteralPath $sums -Encoding ascii}
Set-Sums $zip
$lock=[IO.File]::Open((Join-Path $target 'VieweryShell.dll'),[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try {if((Run-Update $zip $sums $target) -eq 0) {throw 'Locked DLL accepted'}} finally {$lock.Dispose()}
if([IO.File]::ReadAllText((Join-Path $target 'Viewery.exe')) -ne 'old executable') {throw 'Rollback failed'}
if((Run-Update $zip $sums $target) -ne 0) {Get-Content (Join-Path $root 'last-run.log');throw 'Valid update failed'}
if([IO.File]::ReadAllText((Join-Path $target 'Viewery.exe')) -ne 'new executable') {throw 'Executable not replaced'}
if([IO.File]::ReadAllText((Join-Path $target 'collection.sqlite3')) -ne 'private database' -or [IO.File]::ReadAllText((Join-Path $target 'private.jpg')) -ne 'private media') {throw 'Private files changed'}
('0'*64+'  Viewery-0.2.0-beta.9-Windows-x64-portable.zip') | Set-Content -LiteralPath $sums
if((Run-Update $zip $sums $target) -eq 0) {throw 'Corrupt hash accepted'}
$bad=Join-Path $root 'unsafe.zip';$archive=[IO.Compression.ZipFile]::Open($bad,[IO.Compression.ZipArchiveMode]::Create)
try {[void]$archive.CreateEntry('../escaped.txt')} finally {$archive.Dispose()}
Set-Sums $bad
if((Run-Update $bad $sums $target) -eq 0 -or (Test-Path (Join-Path $root 'escaped.txt'))) {throw 'Unsafe ZIP accepted'}
Write-Output 'Portable update: replacement, preservation, hash rejection, traversal rejection and rollback passed.'
