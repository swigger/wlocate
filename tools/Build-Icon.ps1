# Reproducible vector artwork and multi-resolution Windows icon. No image libraries
# beyond Windows' System.Drawing are needed; the original Everything image is not used.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path (Split-Path $PSScriptRoot -Parent) 'assets'
New-Item -ItemType Directory -Path $assets -Force | Out-Null
$svg = @'
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 64 64">
  <title>WLocate green magnifying glass</title>
  <defs><linearGradient id="green" x1="0" y1="2" x2="0" y2="48" gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#68eb8d"/><stop offset="1" stop-color="#16a34a"/>
  </linearGradient></defs>
  <path d="M7 56 L25 38" fill="none" stroke="#14532d" stroke-width="13" stroke-linecap="round"/>
  <path d="M7 55 L25 37" fill="none" stroke="#22c55e" stroke-width="8" stroke-linecap="round"/>
  <circle cx="39" cy="25" r="19" fill="none" stroke="#14532d" stroke-width="10"/>
  <circle cx="39" cy="25" r="19" fill="none" stroke="url(#green)" stroke-width="7"/>
  <path d="M22.66 21.53 A16.7 16.7 0 0 1 42.47 8.66" fill="none" stroke="#c0fcd1" stroke-width="1.6" stroke-linecap="round"/>
</svg>
'@
[IO.File]::WriteAllText((Join-Path $assets 'wlocate.svg'), $svg, [Text.UTF8Encoding]::new($false))
$frames = [Collections.Generic.List[byte[]]]::new()
$sizes = @(16,20,24,32,40,48,64,128,256)
foreach ($size in $sizes) {
    $high = [Drawing.Bitmap]::new($size * 4, $size * 4, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $canvas = [Drawing.Graphics]::FromImage($high)
    $canvas.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $canvas.Clear([Drawing.Color]::Transparent)
    $canvas.ScaleTransform($size / 16.0, $size / 16.0)
    $outline = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#14532d'), 13.0)
    $outline.StartCap = $outline.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $canvas.DrawLine($outline, 7.0, 56.0, 25.0, 38.0)
    $handle = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#22c55e'), 8.0)
    $handle.StartCap = $handle.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $canvas.DrawLine($handle, 7.0, 55.0, 25.0, 37.0)
    $outline.Width = 10.0
    $canvas.DrawEllipse($outline, 20.0, 6.0, 38.0, 38.0)
    $gradient = [Drawing.Drawing2D.LinearGradientBrush]::new([Drawing.PointF]::new(0,2), [Drawing.PointF]::new(0,48), [Drawing.ColorTranslator]::FromHtml('#68eb8d'), [Drawing.ColorTranslator]::FromHtml('#16a34a'))
    $ring = [Drawing.Pen]::new($gradient, 7.0)
    $canvas.DrawEllipse($ring, 20.0, 6.0, 38.0, 38.0)
    $highlight = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#c0fcd1'), 1.6)
    $highlight.StartCap = $highlight.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $canvas.DrawArc($highlight, 22.3, 8.3, 33.4, 33.4, 192.0, 90.0)
    $canvas.Dispose(); $outline.Dispose(); $handle.Dispose(); $ring.Dispose(); $gradient.Dispose(); $highlight.Dispose()
    $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $canvas = [Drawing.Graphics]::FromImage($bitmap)
    $canvas.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
    $canvas.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $canvas.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $canvas.DrawImage($high, [Drawing.Rectangle]::new(0,0,$size,$size), 0,0,$high.Width,$high.Height, [Drawing.GraphicsUnit]::Pixel)
    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
    $frames.Add($stream.ToArray())
    if ($size -in @(16,32,256)) { $bitmap.Save((Join-Path $assets "wlocate-$size.png"), [Drawing.Imaging.ImageFormat]::Png) }
    $stream.Dispose(); $canvas.Dispose(); $bitmap.Dispose(); $high.Dispose()
}
$file = [IO.File]::Create((Join-Path $assets 'wlocate.ico'))
$writer = [IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; ++$i) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write($frame) }
} finally { $writer.Dispose(); $file.Dispose() }
Get-Item -LiteralPath (Join-Path $assets 'wlocate.ico')
