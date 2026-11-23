[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Install', 'Uninstall')]
    [string] $Action,

    [string] $Executable,
    [switch] $StartAtLogon,
    [switch] $ClassicOnly
)

$ErrorActionPreference = 'Stop'

# Keep this list in sync with src/media_types.hpp.
$extensions = '.jpg', '.jpeg', '.png', '.bmp', '.webp', '.tif', '.tiff', '.gif',
    '.mp4', '.m4v', '.mov', '.mkv', '.webm', '.avi', '.wmv', '.mpg', '.mpeg',
    '.m2ts', '.mts', '.ts', '.flv', '.ogv', '.3gp'
$associations = 'Software\Classes\SystemFileAssociations'
$runPath = 'Software\Microsoft\Windows\CurrentVersion\Run'
$settingsPath = 'Software\Viewery'
$verbName = 'Viewery.Open'
$title = 'Открыть во Viewery'
$packageName = 'Viewery.Explorer'
$packagePublisher = 'CN=Viewery'

if ($Action -eq 'Install') {
    if (-not $Executable) { throw 'Supply -Executable with the path to Viewery.exe.' }
    $file = Get-Item -LiteralPath $Executable
    if ($file -isnot [System.IO.FileInfo] -or $file.Extension -ne '.exe') {
        throw 'The executable path must point to a Windows .exe file.'
    }
    $application = '"{0}"' -f $file.FullName
    $command = '{0} --open "%1"' -f $application
} elseif ($Executable -or $StartAtLogon -or $ClassicOnly) {
    throw '-Executable, -StartAtLogon, and -ClassicOnly apply to Install only.'
}

if (-not $PSCmdlet.ShouldProcess('Viewery integration for the current user', $Action)) { return }

$hive = [Microsoft.Win32.Registry]::CurrentUser
$stringType = [Microsoft.Win32.RegistryValueKind]::String

function Get-VieweryPackage {
    Get-AppxPackage -Name $packageName -ErrorAction Stop |
        Where-Object { $_.Name -eq $packageName -and $_.Publisher -eq $packagePublisher }
}

function Register-ModernMenu([System.IO.FileInfo] $file) {
    # A complete MSIX already owns the modern command. Never replace it with a sparse package.
    foreach ($installed in Get-VieweryPackage) {
        if ($installed.InstallLocation -and
            (Test-Path -LiteralPath (Join-Path $installed.InstallLocation 'Viewery.exe') -PathType Leaf)) {
            if ($installed.InstallLocation.Equals($file.DirectoryName,[StringComparison]::OrdinalIgnoreCase)) {
                $script:fullPackage = $true
                return $true
            }
            Write-Warning 'The installed full Viewery application owns the modern menu. A portable installation will use the classic menu.'
            return $false
        }
    }
    $package = Join-Path $file.DirectoryName 'Viewery.Explorer.msix'
    $dll = Join-Path $file.DirectoryName 'VieweryShell.dll'
    if ($file.Name -ne 'Viewery.exe' -or
        -not (Test-Path -LiteralPath $package -PathType Leaf) -or
        -not (Test-Path -LiteralPath $dll -PathType Leaf)) {
        Write-Warning 'The modern menu requires VieweryShell.dll and Viewery.Explorer.msix beside Viewery.exe.'
        return $false
    }

    try {
        $settings = $hive.CreateSubKey($settingsPath)
        try {
            $hash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
            $existing = @(Get-VieweryPackage)
            if ($settings.GetValue('ExplorerExternalLocation') -eq $file.DirectoryName -and
                $settings.GetValue('ExplorerPackageHash') -eq $hash -and $existing.Count -gt 0) {
                return $true
            }

            $signature = Get-AuthenticodeSignature -FilePath $package
            if (-not $signature.SignerCertificate -or
                $signature.SignerCertificate.Subject -ne $packagePublisher) {
                Write-Warning 'The modern menu requires an MSIX signed for CN=Viewery by a trusted publisher.'
                return $false
            }

            foreach ($item in $existing) {
                Remove-AppxPackage -Package $item.PackageFullName -ErrorAction Stop
            }
            Add-AppxPackage -Path $package -ExternalLocation $file.DirectoryName -ErrorAction Stop
            if (-not (Get-VieweryPackage)) { throw 'Windows did not report the package as installed.' }
            $settings.SetValue('ExplorerExternalLocation', $file.DirectoryName, $stringType)
            $settings.SetValue('ExplorerPackageHash', $hash, $stringType)
            return $true
        } finally { $settings.Dispose() }
    } catch {
        Write-Warning "Windows did not register the modern menu. $($_.Exception.Message)"
        return $false
    }
}

$modern = $false
$fullPackage = $false
if ($Action -eq 'Install' -and -not $ClassicOnly) {
    $modern = Register-ModernMenu $file
}

foreach ($extension in $extensions) {
    $verbPath = "$associations\$extension\shell\$verbName"
    if ($Action -eq 'Install' -and -not $modern) {
        $verb = $hive.CreateSubKey($verbPath)
        try {
            $verb.SetValue('MUIVerb', $title, $stringType)
            $verb.SetValue('Icon', "$application,0", $stringType)
            $verb.SetValue('MultiSelectModel', 'Single', $stringType)
            $commandKey = $verb.CreateSubKey('command')
            try {
                $commandKey.SetValue('', $command, $stringType)
                if ($commandKey.GetValue('') -ne $command) {
                    throw "Explorer command verification failed for $extension."
                }
            } finally { $commandKey.Dispose() }
        } finally { $verb.Dispose() }
    } else {
        $hive.DeleteSubKeyTree($verbPath, $false)
    }
}

if ($Action -eq 'Install') {
    if ($StartAtLogon) {
        $run = $hive.CreateSubKey($runPath)
        try {
            $startupApplication = $application
            if (-not $fullPackage) {
                $fullPackage = @(Get-VieweryPackage | Where-Object {
                    $_.InstallLocation -and $_.InstallLocation.Equals($file.DirectoryName,[StringComparison]::OrdinalIgnoreCase) -and
                    (Test-Path -LiteralPath (Join-Path $_.InstallLocation 'Viewery.exe') -PathType Leaf)
                }).Count -gt 0
            }
            if ($fullPackage) {
                # AppExecutionAlias survives changes to the MSIX installation directory.
                $alias = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Microsoft\WindowsApps\Viewery.exe'
                $startupApplication = '"{0}"' -f $alias
            }
            $run.SetValue('Viewery', "$startupApplication --background", $stringType)
        }
        finally { $run.Dispose() }
    }
    if ($modern) { Write-Host 'Viewery registered in the Windows 11 context menu.' }
    else { Write-Host 'Viewery registered under Show more options (Shift+F10).' }
} else {
    $run = $hive.OpenSubKey($runPath, $true)
    if ($run) {
        try {
            $launch = $run.GetValue('Viewery')
            if ($launch -is [string] -and $launch.EndsWith(' --background')) {
                $run.DeleteValue('Viewery', $false)
            }
        } finally { $run.Dispose() }
    }
    foreach ($item in Get-VieweryPackage) {
        if (Test-Path -LiteralPath (Join-Path $item.InstallLocation 'Viewery.exe') -PathType Leaf) {
            Write-Warning 'The modern command belongs to the installed full Viewery application. Remove the app in Windows Settings to remove that command.'
        } else {
            Remove-AppxPackage -Package $item.PackageFullName -ErrorAction Stop
        }
    }
    $settings = $hive.OpenSubKey($settingsPath, $true)
    if ($settings) {
        try {
            $settings.DeleteValue('ExplorerExternalLocation', $false)
            $settings.DeleteValue('ExplorerPackageHash', $false)
        }
        finally { $settings.Dispose() }
    }
}

if (-not ('Viewery.ShellNotifications' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

namespace Viewery {
    public static class ShellNotifications {
        [DllImport("shell32.dll")]
        public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
    }
}
"@
}

[Viewery.ShellNotifications]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)


