[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Destination,

    [ValidateSet('All', 'Playback', 'Converter')]
    [string] $Component = 'All',

    [switch] $IncludeProbe
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('viewery-media-' + [guid]::NewGuid())

function Install-MediaArchive {
    param(
        [string] $Uri,
        [string] $Sha256,
        [string[]] $Files
    )

    $name = [System.IO.Path]::GetFileName($Uri)
    $archive = Join-Path $work $name
    $extract = Join-Path $work ([System.IO.Path]::GetFileNameWithoutExtension($name))
    Write-Host "Downloading $name"
    Invoke-WebRequest -Uri $Uri -OutFile $archive -UseBasicParsing
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Sha256) {
        throw "SHA-256 verification failed for $name."
    }

    Expand-Archive -LiteralPath $archive -DestinationPath $extract
    $sources = foreach ($file in $Files) {
        $candidates = @(Get-ChildItem -LiteralPath $extract -File -Recurse -Filter $file)
        if ($candidates.Count -ne 1) {
            throw "Expected exactly one $file in $name."
        }
        $candidates[0].FullName
    }
    foreach ($source in $sources) {
        $target = Join-Path $Destination ([IO.Path]::GetFileName($source))
        $staged = Join-Path $Destination ('.viewery-' + [guid]::NewGuid().ToString('N') + '.tmp')
        try {
            # Complete the verified file on the destination volume before publishing its usable name.
            # Cancellation during copying cannot leave a partial libmpv/FFmpeg at the runtime path.
            [IO.File]::Copy($source,$staged,$false)
            if ([IO.File]::Exists($target)) { [IO.File]::Replace($staged,$target,[NullString]::Value) }
            else { [IO.File]::Move($staged,$target) }
        } finally {
            if ([IO.File]::Exists($staged)) { [IO.File]::Delete($staged) }
        }
    }
}

try {
    [void] [System.IO.Directory]::CreateDirectory($work)
    [void] [System.IO.Directory]::CreateDirectory($Destination)
    if ($Component -in 'All', 'Playback') {
        Install-MediaArchive `
            -Uri 'https://github.com/mpv-player/mpv/releases/download/v0.42.0/libmpv-v0.42.0-x86_64-w64-mingw32-lgpl.zip' `
            -Sha256 '8e5f77430efc53ef175804ba6618527e3af0448a0dcabf04d3cc72659d1a6fce' `
            -Files 'libmpv-2.dll'
    }
    if ($Component -in 'All', 'Converter') {
        $converterFiles = @('ffmpeg.exe')
        if ($IncludeProbe) { $converterFiles += 'ffprobe.exe' }
        Install-MediaArchive `
            -Uri 'https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip' `
            -Sha256 '60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba' `
            -Files $converterFiles
    }
    Write-Host "Installed media tools in $Destination"
} finally {
    $temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (Test-Path -LiteralPath $work) {
        $resolvedWork = (Resolve-Path -LiteralPath $work).ProviderPath
        if ($resolvedWork.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
            [System.IO.Path]::GetFileName($resolvedWork) -match '^viewery-media-[0-9a-f-]{36}$') {
            Remove-Item -LiteralPath $resolvedWork -Recurse -Force
        }
    }
}
