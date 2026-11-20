# 把 BMP 压成 JPEG（指定质量）再解回 24 位 BMP
# 用途：模拟"图片被微信/网站压缩过一次"，验证 DCT 水印的抗压缩能力
#   powershell -File tools\jpeg_roundtrip.ps1 -In stego.bmp -Out q75.bmp -Quality 75
param(
    [Parameter(Mandatory=$true)][string]$In,
    [Parameter(Mandatory=$true)][string]$Out,
    [int]$Quality = 75
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
         Where-Object { $_.MimeType -eq 'image/jpeg' }

$jpgPath = [System.IO.Path]::ChangeExtension($Out, '.jpg')

# ① BMP -> JPEG（有损，这一步会毁掉全部 LSB）
$img = [System.Drawing.Bitmap]::FromFile((Resolve-Path $In))
$ep = New-Object System.Drawing.Imaging.EncoderParameters(1)
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
    [System.Drawing.Imaging.Encoder]::Quality, [long]$Quality)
$img.Save($jpgPath, $codec, $ep)
$img.Dispose(); $ep.Dispose()

# ② JPEG -> 24 位 BMP（无损解码，供 C 程序读取）
$b = [System.Drawing.Bitmap]::FromFile($jpgPath)
$b.Save($Out, [System.Drawing.Imaging.ImageFormat]::Bmp)
$b.Dispose()

$jf = Get-Item $jpgPath
$bf = Get-Item $Out
Write-Host ("JPEG q{0}: {1:N0} 字节  →  {2}: {3:N0} 字节" -f $Quality, $jf.Length, $bf.Name, $bf.Length)
