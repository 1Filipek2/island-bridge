param(
    [int]$Days = 7,
    [string]$LogDirectory = (Join-Path $env:LOCALAPPDATA 'IslandBridge\logs'),
    [switch]$NoSystemEvents
)

$ErrorActionPreference = 'Stop'
$since = (Get-Date).AddDays(-$Days)
$linePattern = '^(?<time>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} [+-]\d{2}:\d{2}) (?<level>[A-Z]{3}) (?<message>.*)$'

$files = Get-ChildItem -Path $LogDirectory -Filter 'islandbridge*.log' -ErrorAction SilentlyContinue
if (-not $files) {
    throw "No IslandBridge logs in $LogDirectory"
}

$entries = foreach ($file in $files) {
    $stream = [IO.FileStream]::new($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader = [IO.StreamReader]::new($stream)
    $lines = $reader.ReadToEnd() -split "`r?`n"
    $reader.Dispose()
    foreach ($line in $lines) {
        if ($line -match $linePattern) {
            $time = [DateTimeOffset]::ParseExact($Matches.time, 'yyyy-MM-dd HH:mm:ss.fff zzz', [Globalization.CultureInfo]::InvariantCulture)
            if ($time.LocalDateTime -ge $since) {
                [pscustomobject]@{ Time = $time; Level = $Matches.level; Message = $Matches.message }
            }
        }
    }
}

$entries = @($entries | Sort-Object Time)
if ($entries.Count -eq 0) {
    throw "No log lines in the last $Days days"
}

function Format-Span([TimeSpan]$span) {
    if ($span.TotalHours -ge 1) { return '{0:0}h {1:00}m' -f [Math]::Floor($span.TotalHours), $span.Minutes }
    if ($span.TotalMinutes -ge 1) { return '{0:0}m {1:00}s' -f [Math]::Floor($span.TotalMinutes), $span.Seconds }
    return '{0:0.0}s' -f $span.TotalSeconds
}

$first = $entries[0].Time
$last = $entries[-1].Time
"IslandBridge soak report"
"Period: $($first.LocalDateTime.ToString('yyyy-MM-dd HH:mm')) to $($last.LocalDateTime.ToString('yyyy-MM-dd HH:mm')) ($(Format-Span ($last - $first)))"
""

$starts = @($entries | Where-Object { $_.Message -match '^start (manual|autostart)$' })
$unclean = 0
for ($index = 1; $index -lt $starts.Count; $index++) {
    $between = $entries | Where-Object { $_.Time -gt $starts[$index - 1].Time -and $_.Time -lt $starts[$index].Time -and $_.Message -eq 'exit' }
    if (-not $between) { $unclean++ }
}
"Process"
"  starts: $($starts.Count) ($(@($starts | Where-Object Message -eq 'start autostart').Count) autostart), without a logged exit before the next start (crash or kill): $unclean"
""

$outages = [Collections.Generic.List[object]]::new()
$readyTime = [TimeSpan]::Zero
$readySince = $null
$downSince = $null
$downReason = ''
foreach ($entry in $entries) {
    if ($entry.Message -match '^start ') {
        if ($readySince) { $readyTime += $entry.Time - $readySince; $readySince = $null }
        $downSince = $null
        continue
    }
    if ($entry.Message -notmatch '^connection (?<state>\w+)(?: \((?<reason>[^)]*)\))?') { continue }
    $state = $Matches.state
    if ($state -eq 'Ready') {
        if ($downSince) {
            $outages.Add([pscustomobject]@{ Start = $downSince; Duration = $entry.Time - $downSince; Reason = $downReason })
            $downSince = $null
        }
        if (-not $readySince) { $readySince = $entry.Time }
    }
    elseif ($readySince) {
        $readyTime += $entry.Time - $readySince
        $readySince = $null
        $downSince = $entry.Time
        $downReason = "$state $($Matches.reason)".Trim()
    }
}
if ($readySince) { $readyTime += $last - $readySince }
if ($downSince) { $outages.Add([pscustomobject]@{ Start = $downSince; Duration = $last - $downSince; Reason = "$downReason (still down at the end)" }) }

$readyCount = @($entries | Where-Object Message -eq 'connection Ready').Count
"iPhone connection"
"  reached Ready $readyCount times, ready for $(Format-Span $readyTime) of $(Format-Span ($last - $first)) logged"
"  outages after Ready: $($outages.Count)"
if ($outages.Count -gt 0) {
    $sorted = @($outages | Sort-Object Duration)
    $median = $sorted[[int][Math]::Floor(($sorted.Count - 1) / 2)].Duration
    "  median outage $(Format-Span $median), longest $(Format-Span $sorted[-1].Duration)"
    "  longest outages:"
    foreach ($outage in ($outages | Sort-Object Duration -Descending | Select-Object -First 5)) {
        "    $($outage.Start.LocalDateTime.ToString('MM-dd HH:mm:ss'))  $(Format-Span $outage.Duration)  $($outage.Reason)"
    }
}
""

$iphone = @($entries | Where-Object { $_.Message -match '^notification uid=\d+ app=\S+ category=\S+ (?<decision>\w+)$' } | ForEach-Object { $null = $_.Message -match ' (?<decision>\w+)$'; $Matches.decision })
$windows = @($entries | Where-Object { $_.Message -match '^toast uid=\d+ app=\S+ \w+$' } | ForEach-Object { $null = $_.Message -match ' (?<decision>\w+)$'; $Matches.decision })
function Format-Decisions($decisions) {
    if ($decisions.Count -eq 0) { return '0' }
    $groups = $decisions | Group-Object | Sort-Object Count -Descending | ForEach-Object { "$($_.Name) $($_.Count)" }
    return "$($decisions.Count) ($($groups -join ', '))"
}
"Notifications"
"  iPhone: $(Format-Decisions $iphone)"
"  Windows: $(Format-Decisions $windows)"
"  test: $(@($entries | Where-Object Message -eq 'test notification sent').Count)"
$icons = @($entries | Where-Object { $_.Message -match '^icon app=' })
"  icon lookups: $($icons.Count) ($(@($icons | Where-Object { $_.Message -match ' none in ' }).Count) without an icon)"
""

"Island pipe"
"  connected: $(@($entries | Where-Object Message -eq 'island pipe connected').Count), unavailable: $(@($entries | Where-Object { $_.Message -like 'island pipe unavailable*' }).Count), untrusted server: $(@($entries | Where-Object { $_.Message -like 'island pipe server is not*' }).Count), queue rejected: $(@($entries | Where-Object Message -eq 'island queue rejected a notification').Count)"
""

$problems = @($entries | Where-Object { $_.Level -in 'WRN', 'ERR' })
"Warnings and errors: $($problems.Count)"
foreach ($group in ($problems | ForEach-Object { "$($_.Level) " + ($_.Message -replace '\d+', 'N') } | Group-Object | Sort-Object Count -Descending | Select-Object -First 10)) {
    "  $($group.Count)x $($group.Name)"
}

if (-not $NoSystemEvents) {
    ""
    $filter = @{ LogName = 'System'; StartTime = $first.LocalDateTime; EndTime = $last.LocalDateTime.AddMinutes(1); Id = 1, 42, 506, 507 }
    $power = @(Get-WinEvent -FilterHashtable $filter -ErrorAction SilentlyContinue |
        Where-Object { $_.ProviderName -in 'Microsoft-Windows-Kernel-Power', 'Microsoft-Windows-Power-Troubleshooter' } |
        Sort-Object TimeCreated)
    $wakes = [Collections.Generic.List[datetime]]::new()
    foreach ($event in ($power | Where-Object { ($_.Id -eq 1 -and $_.ProviderName -eq 'Microsoft-Windows-Power-Troubleshooter') -or $_.Id -eq 507 })) {
        if ($wakes.Count -eq 0 -or ($event.TimeCreated - $wakes[-1]).TotalSeconds -gt 10) { $wakes.Add($event.TimeCreated) }
    }
    $sleeps = [Collections.Generic.List[datetime]]::new()
    foreach ($event in ($power | Where-Object { $_.Id -in 42, 506 })) {
        if ($sleeps.Count -eq 0 -or ($event.TimeCreated - $sleeps[-1]).TotalSeconds -gt 10) { $sleeps.Add($event.TimeCreated) }
    }
    "Sleep and wake: $($sleeps.Count) sleeps, $($wakes.Count) wakes"
    $window = [TimeSpan]::FromMinutes(10)
    foreach ($wake in ($wakes | Select-Object -Last 10)) {
        $after = @($entries | Where-Object { $_.Time.LocalDateTime -ge $wake -and $_.Time.LocalDateTime -le $wake + $window -and $_.Message -match '^(connection |start )' })
        $before = $entries | Where-Object { $_.Time.LocalDateTime -lt $wake -and $_.Message -match '^connection ' } | Select-Object -Last 1
        $ready = $after | Where-Object Message -eq 'connection Ready' | Select-Object -First 1
        $result = if ($after.Count -eq 0) {
            if ($before -and $before.Message -eq 'connection Ready') { 'stayed connected' } else { 'no connection activity within 10 min' }
        }
        elseif ($ready) { "Ready after $(Format-Span ($ready.Time.LocalDateTime - $wake))" }
        else { 'not Ready within 10 min' }
        "  wake $($wake.ToString('MM-dd HH:mm:ss')) -> $result"
    }
}
