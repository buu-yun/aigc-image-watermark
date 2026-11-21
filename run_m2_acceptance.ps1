# ============================================================
#  M2 验收脚本：DCT 抗 JPEG + CRC 三态判定
#  用法:  powershell -ExecutionPolicy Bypass -File run_m2_acceptance.ps1
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

Write-Host "=== 0. 编译 ===（含 M2 新增：crc16.c / dct.c / dctwm.c）"
$out = & gcc -std=c99 -O2 -Wall -Wextra -o wm.exe bmp.c lsb.c crc16.c dct.c dctwm.c tilewm.c main.c -lm 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $out.Trim() -eq '') "编译通过且零警告"

Write-Host "=== 1. 生成 1024x768 载体与短消息 ==="
$null = .\make_test_image.exe big.bmp 1024 768
Check (Test-Path big.bmp) "生成 big.bmp（DCT 容量更大）"

$msg = "M2-DCT | team=steg-trace | model=diffusion-v2.3 | sha=9f2c7a1e | 频域水印，抗 JPEG 压缩。"
[System.IO.File]::WriteAllText("$root\msg_m2.txt", $msg, (New-Object System.Text.UTF8Encoding($false)))
$src = [System.IO.File]::ReadAllBytes("$root\msg_m2.txt")
Check ($src.Length -lt 200) "消息 $($src.Length) 字节（1024x768 容量约 495 字节）"

Write-Host "=== 2. DCT 嵌入 / 提取（无损） ==="
$null = .\wm.exe embed -i big.bmp -o big_stego.bmp -f msg_m2.txt -m dct
Check ($LASTEXITCODE -eq 0) "embed -m dct 返回 0"
$null = .\wm.exe extract -i big_stego.bmp -o got.txt -q
$got = [System.IO.File]::ReadAllBytes("$root\got.txt")
Check ([System.Linq.Enumerable]::SequenceEqual($src, $got)) "无损往返逐字节一致"

Write-Host "=== 3. 三态判定（原图应为【真】） ==="
$v = .\wm.exe verify -i big_stego.bmp -q 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $v -match '【真】') "原图判定为【真】，exit=0"

Write-Host "=== 4. 抗 JPEG：q95 → q10 ==="
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$firstFail = $null
Write-Host "   质量 |   大小 | 提取 | 判定"
foreach ($q in 95, 85, 75, 65, 50, 40, 30, 20, 10) {
    $img = [System.Drawing.Bitmap]::FromFile("$root\big_stego.bmp")
    $ep = New-Object System.Drawing.Imaging.EncoderParameters(1)
    $ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, [long]$q)
    $img.Save("$root\q$q.jpg", $codec, $ep)
    $img.Dispose(); $ep.Dispose()

    $b = [System.Drawing.Bitmap]::FromFile("$root\q$q.jpg")
    $b.Save("$root\q$q.bmp", [System.Drawing.Imaging.ImageFormat]::Bmp)
    $b.Dispose()

    $null = .\wm.exe extract -i "q$q.bmp" -o "q$q.txt" -q 2>$null
    $ok = $LASTEXITCODE -eq 0
    $same = $false
    if ($ok -and (Test-Path "q$q.txt")) {
        $same = [System.Linq.Enumerable]::SequenceEqual($src, [System.IO.File]::ReadAllBytes("$root\q$q.txt"))
    }
    $vv = .\wm.exe verify -i "q$q.bmp" -q 2>&1 | Select-String '结论' | ForEach-Object { ($_.Line -replace '.*【','【') }
    Write-Host ("   {0,4} | {1,6:N0}B | {2} | {3}" -f $q, (Get-Item "q$q.jpg").Length, $(if($same){"OK  "}else{"FAIL"}), $vv)
    if (-not $same -and -not $firstFail) { $firstFail = $q }
}
Check ($null -ne $firstFail -and $firstFail -le 40) "JPEG q40 仍能逐字节取回（失守点 q$firstFail）"

Write-Host "=== 5. 篡改：两种破坏程度，两种结论 ==="
# 5a 只涂画面中部：头部完好 → 内容仍可读出，但哨兵大面积损坏 → 【可疑】
$null = .\wm.exe tamper -i big_stego.bmp -o mid.bmp -r 100,100,500,400 2>&1
$null = .\wm.exe verify -i mid.bmp -q 2>$null
$codeA = $LASTEXITCODE
$vA = .\wm.exe verify -i mid.bmp -q 2>&1 | Out-String
Check ($codeA -eq 2 -and $vA -match '【可疑】') "中部涂改 25% → 【可疑】（内容可读、像素被动过），exit=2"

# 5b 连头部一起涂掉：CRC 彻底失败 → 【假】
$null = .\wm.exe tamper -i big_stego.bmp -o heavy.bmp -r 10,10,900,650 2>&1
$null = .\wm.exe verify -i heavy.bmp -q 2>$null
$codeB = $LASTEXITCODE
$vB = .\wm.exe verify -i heavy.bmp -q 2>&1 | Out-String
Check ($codeB -eq 3 -and $vB -match '【假】') "大面积涂改破坏头部 → 【假】，exit=3"

Write-Host "=== 6. 无水印图 → 【无水印】 ==="
$null = .\wm.exe verify -i big.bmp -q 2>$null
$nv = .\wm.exe verify -i big.bmp -q 2>&1 | Out-String
Check ($LASTEXITCODE -eq 4 -and $nv -match '【无水印】') "未嵌水印判定为【无水印】，exit=4"

Write-Host "=== 7. 自检（23 项） ==="
$null = .\wm.exe selftest
Check ($LASTEXITCODE -eq 0) "wm selftest 全部通过"

Write-Host ""
if ($script:fail -eq 0) { Write-Host "SUMMARY: ALL PASSED" -ForegroundColor Green; exit 0 }
else { Write-Host "SUMMARY: $($script:fail) FAILED" -ForegroundColor Red; exit 1 }
