# ============================================================
#  M4 验收脚本：库接口 + 密钥防伪 + 交付示例
#  用法:  powershell -ExecutionPolicy Bypass -File run_m4_acceptance.ps1
#  判据:  末尾 SUMMARY 全 PASS 且 exit code = 0
# ============================================================
$ErrorActionPreference = 'Continue'
$script:fail = 0
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "  [PASS] $name" }
    else       { Write-Host "  [FAIL] $name" -ForegroundColor Red; $script:fail++ }
}

Write-Host "=== 0. 编译全部产物（含 libwm.a 与 wm_demo.exe）==="
$null = cmd /c "build.bat" 2>&1
Check ($LASTEXITCODE -eq 0) "build.bat 成功"
Check ((Test-Path libwm.a) -and (Test-Path wm_demo.exe)) "生成了 libwm.a 与 wm_demo.exe"

Write-Host "=== 1. 密码学自检（FIPS 180-4 / RFC 4231 标准向量）==="
$ct = .\wm_demo.exe --crypto-test 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $ct -match '0 项失败') "SHA-256 与 HMAC-SHA256 全部对上标准向量"

Write-Host "=== 2. 带口令嵌入（库接口）==="
$null = .\make_test_image.exe big.bmp 1024 768
$msg = "AIGC|steg-trace|2025-11-05|sha=9f2c7a1e|diff-v2.3"
[System.IO.File]::WriteAllText("$root\msg_m4.txt", $msg, (New-Object System.Text.UTF8Encoding($false)))
$null = .\wm_demo.exe embed big.bmp k_wm.bmp msg_m4.txt "team-secret-2025" dct
Check ($LASTEXITCODE -eq 0) "wm_embed_file() 带口令嵌入成功"

Write-Host "=== 3. 三种口令场景 ==="
$null = .\wm_demo.exe verify k_wm.bmp "team-secret-2025" 2>&1
Check ($LASTEXITCODE -eq 0) "正确口令 → 验证通过"
$bad = .\wm_demo.exe verify k_wm.bmp "wrong-key" 2>&1 | Out-String
Check ($LASTEXITCODE -ne 0 -and $bad -match '口令不正确') "错误口令 → 拒绝（密钥标签验证失败）"
$nokey = .\wm_demo.exe verify k_wm.bmp 2>&1 | Out-String
Check ($LASTEXITCODE -ne 0 -and $nokey -match '需要提供口令') "不给口令 → 明确提示需要密钥"

Write-Host "=== 4. 篡改必须被判失败 ==="
$null = .\wm.exe tamper -i k_wm.bmp -o k_tamp.bmp -r 10,10,900,650 2>&1
$tv = .\wm_demo.exe verify k_tamp.bmp "team-secret-2025" 2>&1 | Out-String
Check ($LASTEXITCODE -ne 0 -and $tv -match '不一致') "涂改后 → 校验失败（TAMPERED）"

Write-Host "=== 5. 不带口令（只防损坏）==="
$null = .\wm_demo.exe embed big.bmp plain_wm.bmp msg_m4.txt
$pv = .\wm_demo.exe verify plain_wm.bmp 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $pv -match '否（只能防损坏') "无口令水印可验证，但明确标注不防伪造"

Write-Host "=== 6. 提取明文（库接口）==="
$ex = .\wm_demo.exe extract k_wm.bmp "team-secret-2025" 2>&1 | Out-String
Check ($ex -match [regex]::Escape($msg)) "wm_extract_file() 取回原文"

Write-Host "=== 7. CLI 仍然可用（23 项自检 + 三套验收）==="
$null = .\wm.exe selftest
Check ($LASTEXITCODE -eq 0) "wm selftest 全通过"

Write-Host ""
if ($script:fail -eq 0) { Write-Host "SUMMARY: ALL PASSED" -ForegroundColor Green; exit 0 }
else { Write-Host "SUMMARY: $($script:fail) FAILED" -ForegroundColor Red; exit 1 }
