[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Destination,
    [string] $MakeAppx,
    [string] $SigningThumbprint,
    [string] $SignTool
)

$ErrorActionPreference = 'Stop'
$directory = (Get-Item -LiteralPath $Destination).FullName
foreach ($file in 'Viewery.exe', 'VieweryShell.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $file) -PathType Leaf)) {
        throw "Build $file and place it in $directory before creating the Explorer package."
    }
}

function Find-SdkTool([string] $name) {
    $tool = Get-Command $name -ErrorAction SilentlyContinue
    if ($tool) {
        return $tool.Source
    }
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    Get-ChildItem -LiteralPath $kits -Directory -ErrorAction SilentlyContinue |
        Where-Object Name -Match '^\d+(\.\d+){3}$' |
        Sort-Object { [version] $_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\$name" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}
if (-not $MakeAppx) { $MakeAppx = Find-SdkTool 'makeappx.exe' }
if (-not $MakeAppx) { throw 'MakeAppx.exe was not found. Install the Windows SDK or pass -MakeAppx.' }

if ($SigningThumbprint) {
    $SigningThumbprint = $SigningThumbprint -replace '\s', ''
    if ($SigningThumbprint -notmatch '^[A-Fa-f0-9]{40}$') { throw 'Expected a 40-digit certificate thumbprint.' }
    $cert = Get-Item -LiteralPath "Cert:\CurrentUser\My\$SigningThumbprint"
    if ($cert.Subject -ne 'CN=Viewery' -or -not $cert.HasPrivateKey) {
        throw 'The signing certificate must have subject CN=Viewery and an accessible private key.'
    }
    if (-not $SignTool) { $SignTool = Find-SdkTool 'signtool.exe' }
    if (-not $SignTool) { throw 'SignTool.exe was not found. Install the Windows SDK or pass -SignTool.' }
}

Add-Type -AssemblyName System.Drawing
$icon = [System.Drawing.Icon]::ExtractAssociatedIcon((Join-Path $directory 'Viewery.exe'))
try {
    $image = $icon.ToBitmap()
    try {
        foreach ($size in 44, 50, 150) {
            $logo = [System.Drawing.Bitmap]::new($size, $size)
            try {
                $graphics = [System.Drawing.Graphics]::FromImage($logo)
                try {
                    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $graphics.DrawImage($image, 0, 0, $size, $size)
                } finally { $graphics.Dispose() }
                $path = Join-Path $directory "VieweryLogo$size.png"
                $logo.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
            } finally { $logo.Dispose() }
        }
    } finally { $image.Dispose() }
} finally { $icon.Dispose() }

$manifest = Join-Path $PSScriptRoot 'ExplorerAppxManifest.xml'
$package = Join-Path $directory 'Viewery.Explorer.msix'
$mapping = Join-Path $directory ('Viewery.PackageFiles.' + [guid]::NewGuid().ToString('N') + '.txt')
try {
    @('[Files]', ('"{0}" "AppxManifest.xml"' -f $manifest)) | Set-Content -LiteralPath $mapping -Encoding Unicode
    & $MakeAppx pack /o /nv /f $mapping /p $package
    if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed ($LASTEXITCODE)." }
    if ($SigningThumbprint) {
        & $SignTool sign /fd SHA256 /s My /sha1 $SigningThumbprint $package
        if ($LASTEXITCODE -ne 0) { throw "SignTool failed ($LASTEXITCODE)." }
    } else {
        Write-Host 'Created an unsigned MSIX for signing. Classic Explorer integration is available without signing.'
    }
} finally {
    if (Test-Path -LiteralPath $mapping) { Remove-Item -LiteralPath $mapping }
}
