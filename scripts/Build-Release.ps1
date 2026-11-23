[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $BuildDirectory,
    [Parameter(Mandatory)] [string] $Destination,
    [Parameter(Mandatory)] [ValidatePattern('^\d+\.\d+\.\d+(-beta\.[1-9]\d*)?$')] [string] $Version,
    [string] $PackageVersion,
    [ValidateSet('beta', 'stable')] [string] $Channel = 'beta',
    [string] $SigningThumbprint,
    [string] $MakeAppx,
    [string] $SignTool,
    [string] $TimestampServer = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$source = (Get-Item -LiteralPath $BuildDirectory).FullName
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
if (($Channel -eq 'beta') -ne $Version.Contains('-beta.')) { throw 'Channel must match the semantic version.' }
$match = [regex]::Match($Version, '^(\d+)\.(\d+)\.(\d+)(?:-beta\.(\d+))?$')
if (-not $PackageVersion) {
    $revision = if ($match.Groups[4].Success) { [int] $match.Groups[4].Value } else { 65535 }
    $PackageVersion = '{0}.{1}.{2}.{3}' -f $match.Groups[1].Value,$match.Groups[2].Value,$match.Groups[3].Value,$revision
}
$numericVersion = [version] $PackageVersion
if ($numericVersion.Major -eq 0 -and $numericVersion.Minor -eq 0 -and $numericVersion.Build -eq 0 -and $numericVersion.Revision -eq 0) {
    throw 'MSIX version cannot be zero.'
}
foreach ($part in $numericVersion.Major,$numericVersion.Minor,$numericVersion.Build,$numericVersion.Revision) {
    if ($part -lt 0 -or $part -gt 65535) { throw 'MSIX needs four version fields in the range 0..65535.' }
}
$PackageVersion = $numericVersion.ToString(4)
foreach ($file in 'Viewery.exe','VieweryShell.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $source $file) -PathType Leaf)) { throw "Missing $source\$file" }
}
if ($output.Equals($source, [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be separate from the build directory.' }
[void] [IO.Directory]::CreateDirectory($output)
$notesName = "Viewery-$Version-ReleaseNotes.md"
& (Join-Path $PSScriptRoot 'Get-ReleaseNotes.ps1') -Version $Version -Destination (Join-Path $output $notesName)
$work = Join-Path $output ('staging-' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $work 'app'
[void] [IO.Directory]::CreateDirectory($payload)

function Find-SdkTool([string] $Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    Get-ChildItem -LiteralPath $kits -Directory -ErrorAction SilentlyContinue |
        Where-Object Name -Match '^\d+(\.\d+){3}$' |
        Sort-Object { [version] $_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\$Name" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}

function Invoke-Native([string] $Tool, [string[]] $Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed ($LASTEXITCODE)." }
}

try {
    foreach ($file in 'Viewery.exe','VieweryShell.dll') {
        Copy-Item -LiteralPath (Join-Path $source $file) -Destination $payload
    }
    foreach ($file in 'README.md','CHANGELOG.md','LICENSE','THIRD_PARTY.md') {
        Copy-Item -LiteralPath (Join-Path $repo $file) -Destination $payload
    }
    foreach ($file in 'Get-MediaTools.ps1','Set-ExplorerIntegration.ps1') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $payload
    }
    [void] [IO.Directory]::CreateDirectory((Join-Path $payload 'licenses'))
    Copy-Item -LiteralPath (Join-Path $repo 'src/third_party/json.LICENSE.MIT') -Destination (Join-Path $payload 'licenses/nlohmann-json.MIT.txt')
    [void] [IO.Directory]::CreateDirectory((Join-Path $payload 'docs'))
    Copy-Item -LiteralPath (Join-Path $repo 'docs/RELEASE.md') -Destination (Join-Path $payload 'docs/RELEASE.md')
    if ($SigningThumbprint) {
        $SigningThumbprint = $SigningThumbprint -replace '\s',''
        if ($SigningThumbprint -notmatch '^[0-9a-fA-F]{40}$') { throw 'Expected 40-digit signing certificate thumbprint.' }
        $certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$SigningThumbprint"
        $codeSigning = @($certificate.EnhancedKeyUsageList | Where-Object { $_.ObjectId.Value -eq '1.3.6.1.5.5.7.3.3' }).Count -gt 0
        if (-not $certificate.HasPrivateKey -or -not $codeSigning -or $certificate.Subject -ne 'CN=Viewery' -or
            $certificate.NotBefore -gt [DateTime]::Now -or $certificate.NotAfter -le [DateTime]::Now) {
            throw 'Signing needs an unexpired code-signing certificate with private key and subject CN=Viewery.'
        }
        if (-not $SignTool) { $SignTool = Find-SdkTool 'signtool.exe' }
        if (-not $SignTool) { throw 'SignTool was not found.' }
        foreach ($file in 'Viewery.exe','VieweryShell.dll') {
            Invoke-Native $SignTool @('sign','/fd','SHA256','/s','My','/sha1',$SigningThumbprint,'/tr',$TimestampServer,'/td','SHA256',(Join-Path $payload $file))
            Invoke-Native $SignTool @('verify','/pa','/all',(Join-Path $payload $file))
        }
    }
    $portableName = "Viewery-$Version-Windows-x64-portable.zip"
    Compress-Archive -Path (Join-Path $payload '*') -DestinationPath (Join-Path $output $portableName) -Force

    $assets = Join-Path $payload 'Assets'
    [void] [IO.Directory]::CreateDirectory($assets)
    Add-Type -AssemblyName System.Drawing
    $icon = [Drawing.Icon]::ExtractAssociatedIcon((Join-Path $payload 'Viewery.exe'))
    try {
        $bitmap = $icon.ToBitmap()
        try {
            foreach ($size in 44,50,150) {
                $logo = [Drawing.Bitmap]::new($size,$size)
                try {
                    $graphics = [Drawing.Graphics]::FromImage($logo)
                    try {
                        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                        $graphics.DrawImage($bitmap,0,0,$size,$size)
                    } finally { $graphics.Dispose() }
                    $logo.Save((Join-Path $assets "VieweryLogo$size.png"),[Drawing.Imaging.ImageFormat]::Png)
                } finally { $logo.Dispose() }
            }
        } finally { $bitmap.Dispose() }
    } finally { $icon.Dispose() }
    [xml] $manifest = Get-Content -LiteralPath (Join-Path $repo 'packaging/AppxManifest.xml') -Raw
    $manifest.Package.Identity.Version = $PackageVersion
    $manifest.Save((Join-Path $payload 'AppxManifest.xml'))
    if (-not $MakeAppx) { $MakeAppx = Find-SdkTool 'makeappx.exe' }
    if (-not $MakeAppx) { throw 'MakeAppx was not found. Portable ZIP is available; pass -MakeAppx for MSIX.' }
    $packageName = "Viewery-$Version-x64.msix"
    $package = Join-Path $output $packageName
    if (-not $SigningThumbprint) { $package = Join-Path $output "Viewery-$Version-x64.unsigned.msix" }
    # Validation is deliberately enabled. A malformed package must fail the release.
    Invoke-Native $MakeAppx @('pack','/o','/d',$payload,'/p',$package)
    if ($SigningThumbprint) {
        Invoke-Native $SignTool @('sign','/fd','SHA256','/s','My','/sha1',$SigningThumbprint,'/tr',$TimestampServer,'/td','SHA256',$package)
        Invoke-Native $SignTool @('verify','/pa','/all',$package)
    }

    $feedUri = "https://github.com/GlimFox/Viewery/releases/download/updates-$Channel/Viewery.appinstaller"
    $packageUri = "https://github.com/GlimFox/Viewery/releases/download/v$Version/$packageName"
    $installer = [xml] @"
<?xml version="1.0" encoding="utf-8"?>
<AppInstaller xmlns="http://schemas.microsoft.com/appx/appinstaller/2018" Uri="$feedUri" Version="$PackageVersion">
  <MainPackage Name="Viewery.Explorer" Publisher="CN=Viewery" Version="$PackageVersion" ProcessorArchitecture="x64" Uri="$packageUri" />
</AppInstaller>
"@
    # No unattended OnLaunch policy: the application waits for conversion/rename work and explicit installation.
    $feedName = if ($SigningThumbprint) { 'Viewery.appinstaller' } else { 'Viewery.appinstaller.template.xml' }
    $installer.Save((Join-Path $output $feedName))
    $artifacts = @($portableName,[IO.Path]::GetFileName($package),$feedName,$notesName)
    $checksumLines = foreach ($artifact in $artifacts) {
        '{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $output $artifact) -Algorithm SHA256).Hash.ToLowerInvariant(),$artifact
    }
    $checksumLines | Set-Content -LiteralPath (Join-Path $output "Viewery-$Version-SHA256SUMS.txt") -Encoding ascii
    if (-not $SigningThumbprint) {
        Write-Warning 'No certificate: unsigned MSIX and feed TEMPLATE are validation artifacts. Publish the portable ZIP only.'
    }
    Write-Host "Release $Version packaged in $output"
} finally {
    if (Test-Path -LiteralPath $work) {
        $resolved = (Resolve-Path -LiteralPath $work).ProviderPath
        $expectedPrefix = [IO.Path]::GetFullPath($output).TrimEnd('\') + '\'
        if (-not $resolved.StartsWith($expectedPrefix,[StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolved) -notmatch '^staging-[0-9a-f]{32}$') { throw 'Unsafe staging cleanup rejected.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
