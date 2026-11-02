param([Parameter(Mandatory = $true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$frames = [System.Collections.Generic.List[byte[]]]::new()
$sizes = @(16, 24, 32, 48, 64, 256)
foreach ($size in $sizes) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $scale = $size / 32.0
    $graphics.ScaleTransform($scale, $scale)
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc(0, 0, 14, 14, 180, 90)
    $path.AddArc(18, 0, 14, 14, 270, 90)
    $path.AddArc(18, 18, 14, 14, 0, 90)
    $path.AddArc(0, 18, 14, 14, 90, 90)
    $path.CloseFigure()
    $background = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 37, 33, 45))
    $foreground = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 221, 201, 255))
    $graphics.FillPath($background, $path)
    $points = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(18, 4), [System.Drawing.PointF]::new(8, 18),
        [System.Drawing.PointF]::new(15, 18), [System.Drawing.PointF]::new(13, 28),
        [System.Drawing.PointF]::new(24, 13), [System.Drawing.PointF]::new(17, 13)
    )
    $graphics.FillPolygon($foreground, $points)
    $stream = [System.IO.MemoryStream]::new()
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $frames.Add($stream.ToArray())
    $stream.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
    $background.Dispose(); $foreground.Dispose(); $path.Dispose()
}
$file = [System.IO.File]::Create($OutputPath)
$writer = [System.IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    for ($i = 0; $i -lt $frames.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame) }
} finally { $writer.Dispose(); $file.Dispose() }
