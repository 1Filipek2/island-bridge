param(
    [string]$Version = '16.35.0',
    [int]$Size = 64,
    [double]$GlyphScale = 0.58
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop') {
    throw 'Run this with Windows PowerShell 5.1 (powershell.exe); it needs WPF for rendering.'
}

Add-Type -AssemblyName PresentationCore, PresentationFramework, WindowsBase

$root = Split-Path -Parent $PSScriptRoot
$bundled = Join-Path $root 'src\IslandBridge.Core\Icons\Bundled'
$brands = Get-Content (Join-Path $bundled 'brands.txt') | Where-Object { $_.Trim() } | ForEach-Object {
    $parts = $_.Split('|')
    [pscustomobject]@{ Slug = $parts[0]; Title = $parts[1] }
}

$base = "https://cdn.jsdelivr.net/npm/simple-icons@$Version"
$data = Invoke-RestMethod "$base/data/simple-icons.json"
Get-ChildItem $bundled -Filter '*.png' | Remove-Item

foreach ($brand in $brands) {
    $meta = $data | Where-Object { $_.slug -eq $brand.Slug } | Select-Object -First 1
    if (-not $meta) {
        $meta = $data | Where-Object { ($_.title -replace '[^A-Za-z0-9]', '').ToLowerInvariant() -eq $brand.Slug } | Select-Object -First 1
    }
    if (-not $meta) {
        throw "Simple Icons $Version has no icon for $($brand.Slug)"
    }

    $svg = (Invoke-WebRequest "$base/icons/$($brand.Slug).svg" -UseBasicParsing).Content
    if ($svg -notmatch ' d="(?<path>[^"]+)"') {
        throw "No path in $($brand.Slug).svg"
    }

    $hex = $meta.hex
    $red = [Convert]::ToByte($hex.Substring(0, 2), 16)
    $green = [Convert]::ToByte($hex.Substring(2, 2), 16)
    $blue = [Convert]::ToByte($hex.Substring(4, 2), 16)
    $luminance = (0.2126 * $red + 0.7152 * $green + 0.0722 * $blue) / 255
    $background = [Windows.Media.Color]::FromRgb($red, $green, $blue)
    $foreground = if ($luminance -gt 0.62) { [Windows.Media.Colors]::Black } else { [Windows.Media.Colors]::White }

    $geometry = [Windows.Media.Geometry]::Parse($Matches.path)
    $glyph = $Size * $GlyphScale
    $offset = ($Size - $glyph) / 2
    $transform = [Windows.Media.TransformGroup]::new()
    $transform.Children.Add([Windows.Media.ScaleTransform]::new($glyph / 24, $glyph / 24))
    $transform.Children.Add([Windows.Media.TranslateTransform]::new($offset, $offset))

    $visual = [Windows.Media.DrawingVisual]::new()
    $context = $visual.RenderOpen()
    $context.DrawRectangle([Windows.Media.SolidColorBrush]::new($background), $null, [Windows.Rect]::new(0, 0, $Size, $Size))
    $context.PushTransform($transform)
    $context.DrawGeometry([Windows.Media.SolidColorBrush]::new($foreground), $null, $geometry)
    $context.Pop()
    $context.Close()

    $bitmap = [Windows.Media.Imaging.RenderTargetBitmap]::new($Size, $Size, 96, 96, [Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($visual)
    $encoder = [Windows.Media.Imaging.PngBitmapEncoder]::new()
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $target = Join-Path $bundled "$($brand.Slug).png"
    $stream = [IO.File]::Create($target)
    try { $encoder.Save($stream) } finally { $stream.Dispose() }
    '{0,-16} #{1}  {2} bytes' -f $brand.Slug, $hex, (Get-Item $target).Length
}

"Rendered $($brands.Count) icons from Simple Icons $Version into $bundled"
