#requires -Version 7.2
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$srcAssets = Join-Path $root 'Assets'
$dstAssets = Join-Path $root '.local/build/layout/Assets'
New-Item -ItemType Directory -Force $dstAssets | Out-Null

foreach ($name in @('StoreLogo.png', 'Square150x150Logo.png', 'Square44x44Logo.png', 'SplashScreen.png')) {
    $src = Join-Path $srcAssets $name
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $dstAssets $name) -Force
    } else {
        Add-Type -AssemblyName System.Drawing
        $dims = switch ($name) {
            'StoreLogo.png'         { @(50, 50) }
            'Square150x150Logo.png' { @(150, 150) }
            'Square44x44Logo.png'   { @(44, 44) }
            'SplashScreen.png'      { @(620, 300) }
        }
        $bitmap = [Drawing.Bitmap]::new($dims[0], $dims[1])
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $font = [Drawing.Font]::new('Georgia', [float]([Math]::Min($dims[0], $dims[1]) * 0.38), [Drawing.FontStyle]::Bold)
        try {
            $graphics.Clear([Drawing.Color]::FromArgb(18, 14, 18))
            $format = [Drawing.StringFormat]::new()
            $format.Alignment = 'Center'
            $format.LineAlignment = 'Center'
            $graphics.DrawString('BB', $font, [Drawing.Brushes]::Crimson, [Drawing.RectangleF]::new(0, 0, $dims[0], $dims[1]), $format)
            $format.Dispose()
            $bitmap.Save((Join-Path $dstAssets $name), [Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $font.Dispose()
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
}
