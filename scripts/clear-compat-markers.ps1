# Removes the compatibility markers Windows adds to Armymen.exe on its own.
#
# Run in an ADMINISTRATOR PowerShell (the machine-wide entries need it):
#   powershell -ExecutionPolicy Bypass -File clear-compat-markers.ps1 "<path to Armymen.exe>"
# With no argument it cleans the test copy.
#
# Windows keeps three markers per exe path:
#   HKCU ...\AppCompatFlags\Layers                         the setting (e.g. DWM8And16BitMitigation)
#   HKLM ...\AppCompatFlags\Layers                         the same, machine-wide ("$" = added by Windows)
#   HKCU ...\AppCompatFlags\Compatibility Assistant\Store  the Compatibility Assistant's own record
# Only use this on a copy that runs with our ddraw.dll. The original game needs these settings.
param(
    [string]$Exe = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "Army Men 1280 test\Armymen.exe")
)

$flags = "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags"
$keys = @(
    "HKCU:\$flags\Layers",
    "HKLM:\$flags\Layers",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers",
    "HKCU:\$flags\Compatibility Assistant\Store"
)

Write-Host "Cleaning compatibility markers for: $Exe"
foreach ($key in $keys) {
    if (-not (Test-Path $key)) { continue }
    $props = (Get-ItemProperty $key).PSObject.Properties.Name
    if ($props -notcontains $Exe) { Write-Host "  none in $key"; continue }
    $value = (Get-ItemProperty $key).$Exe
    if ($value -is [byte[]]) { $value = "<record>" }
    try {
        Remove-ItemProperty -Path $key -Name $Exe -ErrorAction Stop
        Write-Host "  removed from $key (was: $value)"
    } catch {
        Write-Host "  COULD NOT remove from $key : $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "  (run this script as administrator)" -ForegroundColor Red
    }
}
