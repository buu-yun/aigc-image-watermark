# ============================================================
#  M1 一键验收脚本  (PowerShell 5.1+ / pwsh 7+)
#  用法:  pwsh -File run_m1_acceptance.ps1
#  判据:  末尾 SUMMARY 全 PASS 且 exit code = 0
# ============================================================
$ErrorActionPreference = 'Continue'
$script:fail = 0

function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "  [PASS] $name" }
    else       { Write-Host "  [FAIL] $name" -ForegroundColor Red; $script:fail++ }
}

Write-Host "=== 0. 编译 ==="
$out = & gcc -std=c99 -O2 -Wall -Wextra -o wm.exe bmp.c lsb.c crc16.c dct.c dctwm.c tilewm.c main.c -lm 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $out.Trim() -eq '') "wm.exe 编译通过且零警告"
$null = & gcc -std=c99 -O2 -Wall -Wextra -o make_test_image.exe make_test_image.c bmp.c 2>&1
Check ($LASTEXITCODE -eq 0) "make_test_image.exe 编译通过"

Write-Host "=== 1. 生成 512x512 测试载体 ==="
$null = .\make_test_image.exe test_image.bmp 512 512
Check (Test-Path test_image.bmp) "生成 test_image.bmp"

Write-Host "=== 2. 内嵌自检 ==="
$null = .\wm.exe selftest
Check ($LASTEXITCODE -eq 0) "wm selftest 全部通过"

Write-Host "=== 3. 中文消息嵌入 ==="
$null = .\wm.exe embed -i test_image.bmp -o stego.bmp -f sample_msg_utf8.txt -m lsb -q
Check ($LASTEXITCODE -eq 0) "embed 返回 0"
Check (Test-Path stego.bmp) "生成 stego.bmp"

Write-Host "=== 4. 提取并逐字节比对 ==="
$null = .\wm.exe extract -i stego.bmp -o extracted.txt -q
Check ($LASTEXITCODE -eq 0) "extract 返回 0"
$src = [System.IO.File]::ReadAllBytes("$PWD\sample_msg_utf8.txt")
$dst = [System.IO.File]::ReadAllBytes("$PWD\extracted.txt")
Check ($src.Length -eq $dst.Length) "长度一致 ($($src.Length) 字节)"
Check ([System.Linq.Enumerable]::SequenceEqual($src, $dst)) "内容逐字节一致"

Write-Host "=== 5. 视觉不可见 ==="
$diffOut = .\wm.exe diff -i test_image.bmp -j stego.bmp | Out-String
Check ($diffOut -match '最大通道差\s*:\s*[01]\s') "最大通道差 <= 1"

Write-Host "=== 6. 篡改检出（涂白 60x60） ==="
Copy-Item stego.bmp tampered.bmp -Force
$b = [System.IO.File]::ReadAllBytes("$PWD\tampered.bmp")
$w = 512; $h = 512; $stride = 1536; $off = 54
for ($y = 100; $y -lt 160; $y++) {
    $base = $off + ($h - 1 - $y) * $stride + 100 * 3   # BMP 自下而上存储
    for ($i = 0; $i -lt 60 * 3; $i++) { $b[$base + $i] = 255 }
}
[System.IO.File]::WriteAllBytes("$PWD\tampered.bmp", $b)
Remove-Item probe.txt -ErrorAction SilentlyContinue
$null = .\wm.exe extract -i tampered.bmp -o probe.txt -q 2>$null
$tamperDetected = $true
if ($LASTEXITCODE -eq 0) {
    $d = [System.IO.File]::ReadAllBytes("$PWD\probe.txt")
    $tamperDetected = -not [System.Linq.Enumerable]::SequenceEqual($src, $d)
}
Check $tamperDetected "篡改后无法原样取回原文（检出篡改）"
Remove-Item probe.txt -ErrorAction SilentlyContinue

Write-Host "=== 7. 边界与异常 ==="
$null = .\make_test_image.exe tiny.bmp 32 32
$big = 'x' * 400
$null = .\wm.exe embed -i tiny.bmp -o tiny_s.bmp -t $big -m lsb 2>$null
Check ($LASTEXITCODE -ne 0) "消息超出容量时返回非 0"
$null = .\wm.exe extract -i test_image.bmp -q 2>$null
Check ($LASTEXITCODE -ne 0) "未水印图提取返回非 0"

Write-Host ""
if ($script:fail -eq 0) {
    Write-Host "SUMMARY: ALL PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "SUMMARY: $($script:fail) FAILED" -ForegroundColor Red
    exit 1
}
