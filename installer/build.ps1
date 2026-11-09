param(
    [Parameter(Mandatory)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,
    [string]$Output = (Join-Path $PSScriptRoot '..\artifacts')
)

# Builds the island, publishes IslandBridge with Native AOT, then creates the installer,
# a portable zip and SHA256 sums in $Output\release.

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$Output = [IO.Path]::GetFullPath($Output)
$publish = Join-Path $Output 'publish'
$licenses = Join-Path $Output 'licenses'
$release = Join-Path $Output 'release'

$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
if ((Test-Path $installer) -and ($env:PATH -notlike "*$installer*")) {
    $env:PATH += ";$installer"
}

$iscc = @(
    (Get-Command iscc.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $iscc) {
    throw 'Inno Setup 6 (ISCC.exe) was not found.'
}

foreach ($folder in $publish, $licenses, $release) {
    if (Test-Path $folder) {
        Remove-Item $folder -Recurse -Force
    }
    New-Item -ItemType Directory $folder | Out-Null
}

& (Join-Path $root 'island\build.cmd') Release $Version
if ($LASTEXITCODE -ne 0) { throw 'island build failed' }

dotnet publish (Join-Path $root 'src\IslandBridge') -c Release -r win-x64 -o $publish
if ($LASTEXITCODE -ne 0) { throw 'publish failed' }
dotnet publish (Join-Path $root 'src\IslandBridge.Updater') -c Release -r win-x64 -o $publish
if ($LASTEXITCODE -ne 0) { throw 'updater publish failed' }
foreach ($exe in 'IslandBridge.Island.exe', 'IslandBridge.Updater.exe') {
    if (-not (Test-Path (Join-Path $publish $exe))) { throw "the publish folder has no $exe" }
}

# Windows App SDK license and notices ship next to its redistributable files.
$packages = if ($env:NUGET_PACKAGES) { $env:NUGET_PACKAGES } else { Join-Path $env:USERPROFILE '.nuget\packages' }
$sdk = [xml](Get-Content (Join-Path $root 'Directory.Packages.props'))
$sdkVersion = ($sdk.Project.ItemGroup.PackageVersion | Where-Object Include -eq 'Microsoft.WindowsAppSDK.WinUI').Version
Copy-Item (Join-Path $packages "microsoft.windowsappsdk.winui\$sdkVersion\license.txt") (Join-Path $licenses 'WindowsAppSDK-license.txt')
Get-ChildItem (Join-Path $packages 'microsoft.windowsappsdk.*') -Directory |
    ForEach-Object { Join-Path $_.FullName "$sdkVersion\NOTICE.txt" } |
    Where-Object { Test-Path $_ } |
    ForEach-Object { Get-Content $_ -Raw } |
    Set-Content (Join-Path $licenses 'WindowsAppSDK-notices.txt')

& $iscc /Q "/DAppVersion=$Version" "/DSourceDir=$publish" "/DOutputDir=$release" (Join-Path $PSScriptRoot 'IslandBridge.iss')
if ($LASTEXITCODE -ne 0) { throw 'installer build failed' }

$portable = Join-Path $Output 'portable'
if (Test-Path $portable) { Remove-Item $portable -Recurse -Force }
New-Item -ItemType Directory (Join-Path $portable 'licenses') -Force | Out-Null
Copy-Item (Join-Path $publish '*') $portable -Recurse -Exclude '*.pdb'
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $portable 'licenses\LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD-PARTY-NOTICES.txt') (Join-Path $portable 'licenses')
Copy-Item (Join-Path $licenses '*') (Join-Path $portable 'licenses')
Compress-Archive -Path (Join-Path $portable '*') -DestinationPath (Join-Path $release "IslandBridge-$Version-portable-x64.zip")

Get-ChildItem $release -File | Where-Object Extension -in '.exe', '.zip' | ForEach-Object {
    '{0} *{1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
} | Set-Content (Join-Path $release 'SHA256SUMS.txt')

# Update manifest for the in-app updater (format in UpdateManifest.cs). The release workflow signs
# it; a local build leaves it unsigned, and the updater rejects an unsigned manifest.
$setup = Get-Item (Join-Path $release "IslandBridge-$Version-setup-x64.exe")
$manifest = "format=islandbridge-update-1`nversion=$Version`nfile=$($setup.Name)`nsize=$($setup.Length)`nsha256=$((Get-FileHash $setup.FullName -Algorithm SHA256).Hash.ToLowerInvariant())`n"
[IO.File]::WriteAllBytes((Join-Path $release 'update-manifest.txt'), [Text.Encoding]::ASCII.GetBytes($manifest))

Get-ChildItem $release -File | Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } } | Format-Table -AutoSize
