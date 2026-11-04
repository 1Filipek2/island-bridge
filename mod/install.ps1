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

function Wait-ToolMod([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $candidates = Get-CimInstance Win32_Process -Filter "Name = 'windhawk.exe'" |
            Where-Object { $_.CommandLine -like "*-tool-mod*$modId*" }
        foreach ($candidate in $candidates) {
            $process = Get-Process -Id $candidate.ProcessId -ErrorAction SilentlyContinue
            if ($process -and ($process.Modules | Where-Object { $_.ModuleName -eq $name })) {
                return $process.Id
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

$toolModPid = Wait-ToolMod 10
if (-not $toolModPid) {
    'The tool-mod did not come back after the swap; restarting the mod once.'
    Set-ItemProperty $key Disabled 1 -Type DWord
    Set-ItemProperty $key SettingsChangeTime ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) -Type DWord
    Start-Sleep -Seconds 3
    Set-ItemProperty $key Disabled 0 -Type DWord
    Set-ItemProperty $key SettingsChangeTime ([int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 1) -Type DWord
    $toolModPid = Wait-ToolMod 20
}

if (-not $toolModPid) {
    throw "No -tool-mod process for $modId loaded $name. Is Windhawk running?"
}

"Installed $version ($name), tool-mod pid $toolModPid"
