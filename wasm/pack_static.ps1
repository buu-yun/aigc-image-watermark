# 把 wasm 以 base64 内嵌进 HTML，产出单文件站点 docs/index.html
#   - 单文件 = 双击就能跑（file:// 也能加载 wasm，因为不走 fetch）
#   - 整个文件就是可上传的"整站"，丢给任何静态托管都能用
param(
    [string]$Template = "wasm\index.template.html",
    [string]$Wasm     = "wasm\wm.wasm",
    [string]$Out      = "docs\index.html"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

if (-not (Test-Path $Wasm)) { throw "找不到 $Wasm，请先跑 wasm\build_wasm.bat" }

$tpl = [System.IO.File]::ReadAllText((Join-Path $root $Template), [System.Text.Encoding]::UTF8)
$b64 = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes((Join-Path $root $Wasm)))
$html = $tpl.Replace('__WM_WASM_B64__', $b64)

if ($html -match '__WM_WASM_B64__') { throw '占位符替换失败' }

$outPath = Join-Path $root $Out
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outPath) | Out-Null
[System.IO.File]::WriteAllText($outPath, $html, (New-Object System.Text.UTF8Encoding($false)))

# GitHub Pages 用 Jekyll 处理 /docs 时可能出幺蛾子，放个 .nojekyll 直接当静态文件发
Set-Content -Path (Join-Path (Split-Path -Parent $outPath) '.nojekyll') -Value '' -NoNewline

"生成 $Out : $([math]::Round((Get-Item $outPath).Length / 1KB, 1)) KB（wasm 已内嵌，单文件）"
