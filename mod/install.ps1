$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'install.ps1 must run elevated: it writes HKLM and ProgramData.'
}

$modId = 'local@dynamic-island-openmeteo'
$out = Join-Path $PSScriptRoot 'out'
$name = (Get-Content (Join-Path $out 'name.txt')).Trim()
$version = (Get-Content (Join-Path $out 'ver.txt')).Trim()
$engineMods = 'C:\ProgramData\Windhawk\Engine\Mods'

Copy-Item (Join-Path $out "64\$name") "$engineMods\64\"
Copy-Item (Join-Path $out "32\$name") "$engineMods\32\"
Copy-Item (Join-Path $PSScriptRoot 'dynamic-island-openmeteo.wh.cpp') "C:\ProgramData\Windhawk\ModsSource\$modId.wh.cpp" -Force

$key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$modId"
Set-ItemProperty $key LibraryFileName $name
Set-ItemProperty $key Version $version
Set-ItemProperty $key SettingsChangeTime ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord

$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline) {
    $candidates = Get-CimInstance Win32_Process -Filter "Name = 'windhawk.exe'" |
        Where-Object { $_.CommandLine -like "*-tool-mod*$modId*" }
    foreach ($candidate in $candidates) {
        $process = Get-Process -Id $candidate.ProcessId -ErrorAction SilentlyContinue
        if ($process -and ($process.Modules | Where-Object { $_.ModuleName -eq $name })) {
            "Installed $version ($name), tool-mod pid $($process.Id)"
            return
        }
    }
    Start-Sleep -Milliseconds 500
}
throw "No -tool-mod process for $modId loaded $name within 20 s. Is Windhawk running?"
