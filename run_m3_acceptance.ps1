# ============================================================
#  M3 验收脚本：Tile 分块冗余（抗裁剪）+ 篡改定位 + 性能
#  用法:  powershell -ExecutionPolicy Bypass -File run_m3_acceptance.ps1
#  判据:  末尾 SUMMARY 全 PASS 且 exit code = 0
# ============================================================
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
$script:fail = 0
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "  [PASS] $name" }
    else       { Write-Host "  [FAIL] $name" -ForegroundColor Red; $script:fail++ }
}

Write-Host "=== 0. 编译（含 M3 新增：tilewm.c）==="
$out = & gcc -std=c99 -O2 -Wall -Wextra -o wm.exe bmp.c lsb.c crc16.c dct.c dctwm.c tilewm.c main.c -lm 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $out.Trim() -eq '') "编译通过且零警告"

Write-Host "=== 1. 准备载体与 ≤54 字节消息 ==="
$null = .\make_test_image.exe big.bmp 1024 768
$msg = "AIGC|steg-trace|2025-11-05|sha=9f2c7a1e|diff-v2.3"
[System.IO.File]::WriteAllText("$root\msg_m3.txt", $msg, (New-Object System.Text.UTF8Encoding($false)))
$src = [System.IO.File]::ReadAllBytes("$root\msg_m3.txt")
Check ($src.Length -le 54) "消息 $($src.Length) 字节（Tile 上限 54）"

Write-Host "=== 2. Tile 嵌入 / 提取（无损）==="
$null = .\wm.exe embed -i big.bmp -o t3.bmp -f msg_m3.txt -m tile
Check ($LASTEXITCODE -eq 0) "embed -m tile 返回 0"
$null = .\wm.exe extract -i t3.bmp -o got3.txt -q
Check ([System.Linq.Enumerable]::SequenceEqual($src, [System.IO.File]::ReadAllBytes("$root\got3.txt"))) "无损往返逐字节一致"
$v = .\wm.exe verify -i t3.bmp -q 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $v -match '【真】') "原图判定【真】，exit=0"

Write-Host "=== 3. 抗裁剪（这是 M3 的核心）==="
$cases = @(
    @{ name = '右裁 25%';   w = 768;  h = 768;  expect = $true },
    @{ name = '右裁 50%';   w = 512;  h = 768;  expect = $true },
    @{ name = '右裁 75%';   w = 256;  h = 768;  expect = $true },
    @{ name = '下裁 33%';   w = 1024; h = 512;  expect = $true },
    @{ name = '下裁 66%';   w = 1024; h = 256;  expect = $true },
    @{ name = '右裁 100px（非 8 的倍数，走重同步）'; w = 924; h = 768; expect = $true },
    @{ name = '左裁 100px（已知局限，应失败）';      w = 924; h = 768; expect = $false; left = 100 }
)
foreach ($c in $cases) {
    $img = [System.Drawing.Bitmap]::FromFile("$root\t3.bmp")
    $x = if ($c.left) { $c.left } else { 0 }
    $rect = New-Object System.Drawing.Rectangle $x, 0, $c.w, $c.h
    $crop = $img.Clone($rect, $img.PixelFormat); $img.Dispose()
    $crop.Save("$root\_crop.bmp", [System.Drawing.Imaging.ImageFormat]::Bmp); $crop.Dispose()

    $null = .\wm.exe extract -i _crop.bmp -o _c.txt -q 2>$null
    $ok = $LASTEXITCODE -eq 0
    $same = $ok -and (Test-Path "_c.txt") -and [System.Linq.Enumerable]::SequenceEqual($src, [System.IO.File]::ReadAllBytes("$root\_c.txt"))
    Check (($same -eq $c.expect)) ("裁剪 " + $c.name + " → " + $(if ($same) { "逐字节一致" } else { "读不出" }))
}
Remove-Item _crop.bmp, _c.txt -ErrorAction SilentlyContinue

Write-Host "=== 4. Tile 模式抗 JPEG ==="
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
foreach ($q in 90, 75, 60, 30) {
    $img = [System.Drawing.Bitmap]::FromFile("$root\t3.bmp")
    $ep = New-Object System.Drawing.Imaging.EncoderParameters(1)
    $ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, [long]$q)
    $img.Save("$root\_q.jpg", $codec, $ep); $img.Dispose(); $ep.Dispose()
    $b = [System.Drawing.Bitmap]::FromFile("$root\_q.jpg"); $b.Save("$root\_q.bmp", [System.Drawing.Imaging.ImageFormat]::Bmp); $b.Dispose()
    $null = .\wm.exe extract -i _q.bmp -o _q.txt -q 2>$null
    $ok = $LASTEXITCODE -eq 0
    $same = $ok -and (Test-Path "_q.txt") -and [System.Linq.Enumerable]::SequenceEqual($src, [System.IO.File]::ReadAllBytes("$root\_q.txt"))
    Check $same "JPEG q$q 仍能逐字节取回"
}
Remove-Item _q.jpg, _q.bmp, _q.txt -ErrorAction SilentlyContinue

Write-Host "=== 5. 篡改定位 ==="
$null = .\wm.exe tamper -i t3.bmp -o t3_tamp.bmp -r 600,300,200,200 2>&1
$v = .\wm.exe verify -i t3_tamp.bmp -q 2>&1 | Out-String
Check ($LASTEXITCODE -eq 2 -and $v -match '【可疑】') "局部涂改 200x200 → 【可疑】，exit=2"
Check ($v -match '篡改定位') "给出了格子级定位信息"
$h = .\wm.exe heatmap -i t3_tamp.bmp -o t3_heat.bmp 2>&1 | Out-String
Check ($h -match '标记 [1-9]') "热力图标记了被改的格子"

Write-Host "=== 6. 自检（23 项）==="
$null = .\wm.exe selftest
Check ($LASTEXITCODE -eq 0) "wm selftest 全部通过"

Write-Host "=== 7. 性能基准 ==="
$b = .\wm.exe bench | Out-String
Check ($b -match 'Tile') "bench 输出三种方法的耗时与 PSNR"

Write-Host ""
if ($script:fail -eq 0) { Write-Host "SUMMARY: ALL PASSED" -ForegroundColor Green; exit 0 }
else { Write-Host "SUMMARY: $($script:fail) FAILED" -ForegroundColor Red; exit 1 }
