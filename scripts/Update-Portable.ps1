param(
 [Parameter(Mandatory)][ValidatePattern('^v?\d+\.\d+\.\d+(-beta\.[1-9]\d*)?$')][string]$Version,
 [Parameter(Mandatory)][string]$Destination,
 [Parameter(Mandatory)][int]$ParentPid,
 [string]$ReadyEvent,
 [switch]$TestMode,
 [string]$TestArchive,
 [string]$TestChecksums
)
$ErrorActionPreference='Stop'
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+[IO.Path]::PathSeparator+$env:PSModulePath
$ready=$false
$stage=Join-Path ([IO.Path]::GetTempPath()) ('Viewery-update-'+[guid]::NewGuid().ToString('N'))
$changed=[Collections.Generic.List[object]]::new()
try {
 $target=(Get-Item -LiteralPath $Destination).FullName
 if(-not (Test-Path -LiteralPath (Join-Path $target 'Viewery.exe'))) {throw 'Viewery.exe not found in destination'}
 $probe=Join-Path $target ([guid]::NewGuid().ToString('N')+'.tmp')
 [IO.File]::WriteAllText($probe,'');Remove-Item -LiteralPath $probe
 New-Item -ItemType Directory -Path $stage | Out-Null
 $number=$Version.TrimStart('v');$name="Viewery-$number-Windows-x64-portable.zip"
 $zip=Join-Path $stage $name;$sums=Join-Path $stage 'SHA256SUMS.txt'
 if($TestMode) {Copy-Item -LiteralPath $TestArchive -Destination $zip;Copy-Item -LiteralPath $TestChecksums -Destination $sums}
 else {
  [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
  $base="https://github.com/GlimFox/Viewery/releases/download/v$number/"
  Invoke-WebRequest -UseBasicParsing -Uri ($base+'SHA256SUMS.txt') -OutFile $sums -TimeoutSec 60
  Invoke-WebRequest -UseBasicParsing -Uri ($base+$name) -OutFile $zip -TimeoutSec 180
 }
 $matches=@(Get-Content -LiteralPath $sums | Where-Object {$_ -match ('^[0-9a-fA-F]{64}\s+\*?'+[regex]::Escape($name)+'$')})
 if($matches.Count -ne 1 -or (($matches[0] -split '\s+')[0] -ne (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)) {throw 'ZIP SHA256 verification failed'}
 Add-Type -AssemblyName System.IO.Compression.FileSystem
 $archive=[IO.Compression.ZipFile]::OpenRead($zip)
 try {
  foreach($entry in $archive.Entries) {
   $relative=$entry.FullName.Replace('\','/')
   if($relative -match '^/|:|(^|/)\.\.(/|$)' -or $relative.Length -gt 512) {throw 'Unsafe archive path'}
   if($relative.EndsWith('/')) {continue}
   if($relative -notmatch '^(Viewery\.exe|VieweryShell\.dll|Get-MediaTools\.ps1|Set-ExplorerIntegration\.ps1|Update-Portable\.ps1|README\.md|CHANGELOG\.md|THIRD_PARTY\.md|LICENSE|docs/[^:]+|licenses/[^:]+)$') {throw ('Unexpected update file: '+$relative)}
  }
 } finally {$archive.Dispose()}
 $payload=Join-Path $stage 'payload';Expand-Archive -LiteralPath $zip -DestinationPath $payload
 if(-not (Test-Path -LiteralPath (Join-Path $payload 'Viewery.exe'))) {throw 'Incomplete update archive'}
 if(-not $TestMode) {
  $parent=Get-Process -Id $ParentPid -ErrorAction Stop
  if(-not $parent.Path.Equals((Join-Path $target 'Viewery.exe'),[StringComparison]::OrdinalIgnoreCase)) {throw 'Updater parent mismatch'}
  $signal=[Threading.EventWaitHandle]::OpenExisting($ReadyEvent)
  try {[void]$signal.Set();$ready=$true} finally {$signal.Dispose()}
  if(-not $parent.WaitForExit(120000)) {throw 'Viewery did not exit; update cancelled'}
 }
 $backup=Join-Path $stage 'backup';New-Item -ItemType Directory -Path $backup | Out-Null
 foreach($file in Get-ChildItem -LiteralPath $payload -File -Recurse) {
  $relative=$file.FullName.Substring($payload.Length+1)
  $output=[IO.Path]::GetFullPath((Join-Path $target $relative))
  if(-not $output.StartsWith($target.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) {throw 'Update path escaped destination'}
  # Reject directory links so updates cannot write outside their install folder.
  $ancestor=Split-Path -Parent $output
  while($ancestor.Length -ge $target.Length) {
   if((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) {throw 'Linked destination directory is not supported'}
   $ancestor=Split-Path -Parent $ancestor
  }
  $saved=Join-Path $backup $relative;$existed=Test-Path -LiteralPath $output
  if($existed -and ((Get-Item -LiteralPath $output).Attributes -band [IO.FileAttributes]::ReparsePoint)) {throw 'Linked destination file is not supported'}
  if($existed) {New-Item -ItemType Directory -Path (Split-Path -Parent $saved) -Force | Out-Null;Copy-Item -LiteralPath $output -Destination $saved}
  $changed.Add([pscustomobject]@{Path=$output;Backup=$saved;Existed=$existed})
  New-Item -ItemType Directory -Path (Split-Path -Parent $output) -Force | Out-Null
  Copy-Item -LiteralPath $file.FullName -Destination $output -Force
 }
 if(-not $TestMode) {Start-Process -FilePath (Join-Path $target 'Viewery.exe') -WorkingDirectory $target -WindowStyle Hidden}
 Write-Output 'Portable update installed'
} catch {
 $failure=$_
 for($i=$changed.Count-1;$i -ge 0;$i--) {
  $entry=$changed[$i]
  try {if($entry.Existed) {Copy-Item -LiteralPath $entry.Backup -Destination $entry.Path -Force} else {Remove-Item -LiteralPath $entry.Path -Force -ErrorAction SilentlyContinue}} catch {}
 }
 if($ready -and -not $TestMode -and $parent.HasExited) {
  Start-Process -FilePath (Join-Path $target 'Viewery.exe') -WorkingDirectory $target -WindowStyle Hidden
  Add-Type -AssemblyName System.Windows.Forms
  [void][Windows.Forms.MessageBox]::Show('Обновление не установлено. Предыдущая версия восстановлена. Подробности: '+$failure.Exception.Message,'Viewery')
 }
 Write-Error $failure -ErrorAction Continue
 exit 1
}
