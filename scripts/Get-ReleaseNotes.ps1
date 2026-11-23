[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidatePattern('^\d+\.\d+\.\d+(-beta\.[1-9]\d*)?$')] [string] $Version,
    [string] $Changelog = (Join-Path (Split-Path -Parent $PSScriptRoot) 'CHANGELOG.md'),
    [string] $Destination
)

$ErrorActionPreference = 'Stop'
$text = [IO.File]::ReadAllText((Get-Item -LiteralPath $Changelog).FullName,[Text.Encoding]::UTF8)
$pattern = '(?m)^##[ \t]+' + [regex]::Escape($Version) + '[ \t]*\r?$'
$sections = [regex]::Matches($text,$pattern)
if ($sections.Count -ne 1) { throw "CHANGELOG.md must have one exact section for $Version." }
$section = $sections[0]
$remaining = $text.Substring($section.Index + $section.Length)
$nextSection = [regex]::Match($remaining,'(?m)^##[ \t]+')
if ($nextSection.Success) { $remaining = $remaining.Substring(0,$nextSection.Index) }
$notes = $section.Value.Trim() + "`n`n" + $remaining.Trim() + "`n"
if (-not $remaining.Trim()) { throw "CHANGELOG section $Version is empty." }
if ($Destination) {
    $path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
    [IO.File]::WriteAllText($path,$notes,[Text.UTF8Encoding]::new($false))
} else {
    $notes
}
