# Generates app/icon.ico (multi-size) and app/icon256.png for exsearcher.
# Design: flat #0F6CBD background (Fluent blue), bold white magnifier,
# amber "index spark" dot inside the lens.
Add-Type -AssemblyName System.Drawing

$outDir = Join-Path $PSScriptRoot "..\app"
$sizes = 16, 24, 32, 48, 64, 128, 256

function Draw-Icon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $s = $size / 256.0

    # Rounded-square background — flat Fluent blue #0F6CBD
    $pad = [float](8 * $s)
    $r = [float](44 * $s)
    $rect = New-Object System.Drawing.RectangleF($pad, $pad, (256 * $s - 2 * $pad), (256 * $s - 2 * $pad))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [float](2 * $r)
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    # Flat Fluent blue fill
    $bgColor = [System.Drawing.Color]::FromArgb(255, 15, 108, 189)   # #0F6CBD
    $bgBrush = New-Object System.Drawing.SolidBrush($bgColor)
    $g.FillPath($bgBrush, $path)
    $bgBrush.Dispose()

    # Subtle 1px darker border for definition at small sizes
    $borderColor = [System.Drawing.Color]::FromArgb(180, 10, 79, 138)  # #0A4F8A
    $borderPen = New-Object System.Drawing.Pen($borderColor, [float][Math]::Max(1.0, 1.5 * $s))
    $g.DrawPath($borderPen, $path)
    $borderPen.Dispose()
    $path.Dispose()

    # Magnifier lens (white ring), slightly up-left of center
    $cx = [float](116 * $s); $cy = [float](116 * $s)
    $lensR = [float](60 * $s)
    $penW = [float][Math]::Max(1.0, 22 * $s)
    $white = [System.Drawing.Color]::FromArgb(255, 255, 255, 255)
    $pen = New-Object System.Drawing.Pen($white, $penW)
    $g.DrawEllipse($pen, $cx - $lensR, $cy - $lensR, 2 * $lensR, 2 * $lensR)

    # Handle: from lens edge toward bottom-right, round caps
    $hpen = New-Object System.Drawing.Pen($white, ([float]($penW * 1.18)))
    $hpen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $hpen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $ang = [Math]::PI / 4.0
    $hx1 = [float]($cx + ($lensR + $penW * 0.35) * [Math]::Cos($ang))
    $hy1 = [float]($cy + ($lensR + $penW * 0.35) * [Math]::Sin($ang))
    $hx2 = [float](207 * $s); $hy2 = [float](207 * $s)
    $g.DrawLine($hpen, $hx1, $hy1, $hx2, $hy2)

    # Amber spark dot inside lens (the "index hit") — keep amber on blue
    $amber = [System.Drawing.Color]::FromArgb(255, 251, 191, 36)
    $dotR = [float](16 * $s)
    if ($size -ge 24) {
        $brush = New-Object System.Drawing.SolidBrush($amber)
        $g.FillEllipse($brush, $cx - $dotR, $cy - $dotR, 2 * $dotR, 2 * $dotR)
        $brush.Dispose()
    }

    $g.Dispose()
    $pen.Dispose(); $hpen.Dispose()
    return $bmp
}

# Render PNGs per size
$pngBytes = @{}
foreach ($size in $sizes) {
    $bmp = Draw-Icon $size
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBytes[$size] = $ms.ToArray()
    $ms.Dispose()
    if ($size -eq 256) {
        $bmp.Save((Join-Path $outDir "icon256.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $bmp.Dispose()
}

# Pack ICO container (PNG-compressed entries are valid on Vista+)
$icoPath = Join-Path $outDir "icon.ico"
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
foreach ($size in $sizes) {
    $bytes = $pngBytes[$size]
    $dim = if ($size -ge 256) { 0 } else { $size }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim)
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$bytes.Length); $bw.Write([uint32]$offset)
    $offset += $bytes.Length
}
foreach ($size in $sizes) { $bw.Write($pngBytes[$size]) }
$bw.Close(); $fs.Close()

"Wrote $icoPath ($((Get-Item $icoPath).Length) bytes) and icon256.png"
