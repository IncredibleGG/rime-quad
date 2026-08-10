# verify_manifest.ps1 — 建出來的執行檔真的內嵌了 comctl32 v6 的資訊清單嗎
#
# ⚠ 為什麼需要它:沒有這個相依,InitCommonControlsEx 拿到的是 comctl32 **v5**
#   —— Windows 95/2000 那一代的控制項。立體灰按鈕、鑿進去的清單外框、
#   非佈景的核取方塊與捲軸,全部是它畫的,而且**自繪救不了**
#   (那幾顆是 comctl32/user32 自己畫的,WM_DRAWITEM 拿不到)。
#
#   使用者的原話:「你這個設定感覺就是 win95 的方案」。他看到的就是這件事。
#
# ⚠ 這一關有**正負兩個對照組**,不是只問「有沒有」:
#     · rime_service.exe / rime_tsf_host.exe / rime_tests.exe → **必須有**
#     · rime_probe.exe                                        → **必須沒有**
#   負對照是關鍵:少了它,這支腳本在「每一個 exe 都碰巧含有那串字」的情況下
#   也會全綠,而那正是這個專案反覆吃虧的假綠形狀。rime_probe 是命令列工具、
#   沒有視窗,刻意不掛 —— 所以它是一個天然的負對照,不是為了測試而造的假貨。

param([string]$BinDir = "build/windows/Release")

$ErrorActionPreference = "Stop"
$needle = "Microsoft.Windows.Common-Controls"

function Test-Manifest([string]$exe) {
    if (-not (Test-Path $exe)) { return $null }   # 檔案不在 = 說不出答案,不是「沒有」
    $bytes = [System.IO.File]::ReadAllBytes($exe)
    # 資訊清單是以 UTF-8 內嵌的資源。逐位元組找,不做編碼轉換 ——
    # Get-Content 在 Windows 上會依行程碼頁解讀,而這是二進位檔。
    $pat = [System.Text.Encoding]::UTF8.GetBytes($needle)
    $limit = $bytes.Length - $pat.Length
    for ($i = 0; $i -le $limit; $i++) {
        $hit = $true
        for ($j = 0; $j -lt $pat.Length; $j++) {
            if ($bytes[$i + $j] -ne $pat[$j]) { $hit = $false; break }
        }
        if ($hit) { return $true }
    }
    return $false
}

$must    = @("rime_service.exe", "rime_tsf_host.exe", "rime_tests.exe")
$mustNot = @("rime_probe.exe")

$bad = 0
$checked = 0

foreach ($n in $must) {
    $r = Test-Manifest (Join-Path $BinDir $n)
    if ($null -eq $r) { Write-Host "  [SKIP] $n 不在 $BinDir(這一輪沒建它)"; continue }
    $checked++
    if ($r) { Write-Host "  [PASS] $n 內嵌了 comctl32 v6 的資訊清單" }
    else {
        Write-Host "  [FAIL] $n **沒有** comctl32 v6 —— 它會拿到 v5,外觀退回 Windows 95 那一代" -ForegroundColor Red
        $bad++
    }
}

foreach ($n in $mustNot) {
    $r = Test-Manifest (Join-Path $BinDir $n)
    if ($null -eq $r) { Write-Host "  [SKIP] $n 不在 $BinDir"; continue }
    $checked++
    if (-not $r) { Write-Host "  [PASS] $n 沒有資訊清單(負對照:它是命令列工具,本來就不該有)" }
    else {
        Write-Host "  [FAIL] $n 竟然也有 —— 負對照倒了,這支腳本的判準沒有鑑別力" -ForegroundColor Red
        $bad++
    }
}

# ⚠ §2-G:掃描範圍為空必須是**紅**,不是「零個違規」。
if ($checked -lt 2) {
    Write-Host "  [FAIL] 只檢查到 $checked 個執行檔 —— 路徑錯了,這一關等於沒跑" -ForegroundColor Red
    exit 1
}

if ($bad -gt 0) { Write-Host "資訊清單檢查未通過($bad 項)" -ForegroundColor Red; exit 1 }
Write-Host "資訊清單檢查通過(檢查了 $checked 個執行檔,含 1 個負對照)"
