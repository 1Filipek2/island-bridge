$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'install.ps1 must run elevated: it writes HKLM and ProgramData.'
}

$modId = 'local@dynamic-island-openmeteo'
$out = Join-Path $PSScriptRoot 'out'
$name = (Get-Content (Join-Path $out 'name.txt')).Trim()
$version = (Get-Content (Join-Path $out 'ver.txt')).Trim()
$windhawk = 'C:\Program Files\Windhawk\windhawk.exe'
$engineMods = 'C:\ProgramData\Windhawk\Engine\Mods'

Copy-Item (Join-Path $out "64\$name") "$engineMods\64\"
Copy-Item (Join-Path $out "32\$name") "$engineMods\32\"
Copy-Item (Join-Path $PSScriptRoot 'dynamic-island-openmeteo.wh.cpp') "C:\ProgramData\Windhawk\ModsSource\$modId.wh.cpp" -Force

& $windhawk -exit -wait
Start-Sleep -Seconds 3

$key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$modId"
Set-ItemProperty $key LibraryFileName $name
Set-ItemProperty $key Version $version
Set-ItemProperty $key SettingsChangeTime ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord

Start-Process $windhawk -ArgumentList '-tray-only'

$deadline = (Get-Date).AddSeconds(30)
$toolMod = $null
while (-not $toolMod -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 1
    $toolMod = Get-CimInstance Win32_Process -Filter "Name = 'windhawk.exe'" |
        Where-Object { $_.CommandLine -like "*-tool-mod*$modId*" } |
        Select-Object -First 1
}
if (-not $toolMod) { throw "The -tool-mod process for $modId did not start within 30 s." }

$loaded = $false
$deadline = (Get-Date).AddSeconds(15)
while (-not $loaded -and (Get-Date) -lt $deadline) {
    $loaded = [bool]((Get-Process -Id $toolMod.ProcessId).Modules | Where-Object { $_.ModuleName -eq $name })
    if (-not $loaded) { Start-Sleep -Seconds 1 }
}
if (-not $loaded) { throw "The tool-mod process $($toolMod.ProcessId) is running but has not loaded $name." }

"Installed $version ($name), tool-mod pid $($toolMod.ProcessId)"
