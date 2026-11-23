param (
    [Parameter(Mandatory)][string] $Ffmpeg,
    [Parameter(Mandatory)][string] $Destination
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
function New-Fixture([string[]] $FfmpegArguments) {
    & $Ffmpeg -hide_banner -loglevel error -y @FfmpegArguments
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg could not generate the test fixture." }
}
New-Fixture @('-f', 'lavfi', '-i', 'testsrc2=size=160x96:rate=10', '-frames:v', '1',
    '-threads', '1', (Join-Path $Destination 'photo.png'))
New-Fixture @('-f', 'lavfi', '-i', 'testsrc=size=160x96:rate=10', '-t', '3',
    '-vf', "setpts='(N+floor(N/3))/(10*TB)'", '-fps_mode', 'vfr', (Join-Path $Destination 'animation.gif'))
New-Fixture @('-f', 'lavfi', '-i', 'testsrc2=size=160x96:rate=10', '-t', '3',
    '-vf', "setpts='(N+floor(N/3))/(10*TB)'", '-fps_mode', 'vfr', '-c:v', 'libx264',
    '-g', '20', '-pix_fmt', 'yuv420p', (Join-Path $Destination 'sample.mp4'))
