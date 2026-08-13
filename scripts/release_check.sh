#!/usr/bin/env bash
#
# release_check.sh — 發布前的驗證關卡
#
# 為什麼要有這支:
#   先前差點把一份「其他 agent 正在改到一半」的 APK 發給使用者，靠臨時想到
#   加防呆才擋下。而更早之前，我把「腳本說 PASS」當成「使用者拿到的東西能用」，
#   結果使用者裝上真機才發現：鍵盤被拉伸、設定頁根本是開發用的診斷畫面。
#
#   所以發布前要跑的不是「有沒有編出來」，而是**這份 APK 是不是真的能用**。
#   每一項都必須是可觀察的事實，不是「應該沒問題」。
#
# 用法:
#   ./release_check.sh              # 全部跑一遍
#   ./release_check.sh --skip-emu   # 只跑不需要模擬器的部分(快車道)
#   ./release_check.sh --emu-only   # 只跑需要模擬器的部分(慢車道)
#   ./release_check.sh --strict     # 略過一律視為失敗(CI 用)
#   ./release_check.sh --apk <path>       # 驗指定的 **release** APK,不自己建
#   ./release_check.sh --debug-apk <path> # 模擬器驗證腳本用的那一份
#
# ── 為什麼這支腳本同時盯著兩份 APK(2026-08-10 起)────────────────────────
# 使用者拿到的是 **release**;模擬器上那幾支驗證腳本跑的是 **debug**。
# 這不是偷懶,是它們需要 debug 才有的東西:`run-as` 讀資料目錄、
# `src/debug` 底下的 BackupHarnessReceiver 驅動匯出/匯入往返。
#
# 但**發布關卡不可以只驗 debug 那一份** —— 那正是 2026-08-10 之前的狀態:
# 每一關都綠,而綠的是一份使用者永遠不會拿到的 APK。所以分工是:
#
#   驗 release(使用者拿到的)  第 3c、4、6c 關
#   驗 debug (驗證腳本用的)   第 6、6b 關,以及 verify_* 那幾支
#
# 而第 3c 關同時盯住這條分界線本身:release 不是 debuggable、不含 harness,
# debug 兩者皆是。後半句是**正控** —— 少了它,偵測方法自己壞掉的那天
# (aapt2 換一版少印一行、dex 字串換了寫法)這一關會安靜地永遠說「乾淨」。
#
# ── 為什麼要拆成快慢兩條車道 ──────────────────────────────────────────
# 模擬器在 CI 上要開機、要冷啟動部署,一輪十幾分鐘;單元測試與 APK 內容
# 檢查兩分鐘就跑完。全部綁在一起的結果是「太慢所以只在發版前跑」,
# 那等於沒有。所以拆開:快的每次 push 跑,慢的推上 main 或手動觸發時跑。
#
# **拆開不等於拿掉。** 兩條車道加起來必須仍是全部的關卡(數字見檔尾的下界):
#   --skip-emu → 第 0–4 關(含 3b、3c)  --emu-only → 第 5–7 關
# 而且 --emu-only 會在結尾明白列出它**沒有**驗的那幾關,免得有人拿慢車道
# 的綠燈當成「發布關卡通過」。發布必須兩條都綠(見 .github/workflows/build.yml
# 的 publish job 的 needs:)。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 產品識別碼一律從這裡來,不在腳本裡寫死。理由見 scripts/lib/product.env 檔頭。
# shellcheck source=lib/product.sh
. "$ROOT/scripts/lib/product.sh"
# shellcheck source=lib/device.sh
. "$ROOT/scripts/lib/device.sh"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$SDK/platform-tools/adb"
# $APK = **使用者拿到的那一份**。所有「這份東西能不能發」的判斷都問它。
APK="$ROOT/android/app/build/outputs/apk/release/app-release.apk"
# $APK_DEBUG = 模擬器上那幾支驗證腳本用的那一份(它們需要 run-as 與
# src/debug 的 harness)。它**不會**被發布,但第 3c 關要拿它當正控。
APK_DEBUG="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
IME_ID="$RS_ANDROID_IME_ID"
PKG="$RS_ANDROID_APP_ID"
SKIP_EMU=0
EMU_ONLY=0
STRICT=0
OUT="$ROOT/build/release-check"

while [ $# -gt 0 ]; do
  case "$1" in
    --skip-emu)  SKIP_EMU=1; shift ;;
    --emu-only)  EMU_ONLY=1; shift ;;
    --strict)    STRICT=1; shift ;;
    --apk)       APK="$2"; shift 2 ;;
    --debug-apk) APK_DEBUG="$2"; shift 2 ;;
    -h|--help)   sed -n '2,46p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done
if [ "$SKIP_EMU" -eq 1 ] && [ "$EMU_ONLY" -eq 1 ]; then
  echo "--skip-emu 與 --emu-only 同時指定,那就什麼都不會驗到。拒絕。" >&2
  exit 2
fi

mkdir -p "$OUT"
# 每一輪重寫。留著上一輪的內容 = 缺口修好了報告上還印著它。
: > "$OUT/known-gaps.md"

# build-tools 的版本不寫死。本機是 35.0.0，CI runner 上不保證 ——
# 寫死的結果是這支腳本在別的機器上第一步就死，而「只有那一台機器驗得了」
# 正是現在要拆掉的東西。
AAPT2=""
for _d in $(ls -1 "$SDK/build-tools" 2>/dev/null | sort -Vr); do
  [ -x "$SDK/build-tools/$_d/aapt2" ] && { AAPT2="$SDK/build-tools/$_d/aapt2"; break; }
done
[ -n "$AAPT2" ] || { echo "找不到 aapt2（$SDK/build-tools/*/aapt2）" >&2; exit 1; }

PASS=0; FAIL=0; SKIP=0
SKIPPED=""
ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }
# 略過**不是**通過。這支腳本曾經因為升級測試的步驟順序寫反而把那一關判成
# 「略過」,然後在結尾報「失敗 0 項」—— 一片全綠,而真正該驗的東西一次都沒驗。
# 所以略過要自己有一個計數,而且要在結尾逐項列出來,逼人看見。
#
# --strict(CI 用)把略過直接算成失敗。在 CI 上「略過」比在人手上更危險:
# 沒有人會去讀那一段文字,綠燈就是綠燈。要嘛驗,要嘛紅。
skip() {
  if [ "$STRICT" -eq 1 ]; then
    echo "  [FAIL] $* ←(--strict:略過不算通過)" >&2; FAIL=$((FAIL+1))
  else
    echo "  [SKIP] $*"; SKIP=$((SKIP+1)); SKIPPED="$SKIPPED
    · $*"
  fi
}

# ── 第三種結果:[WARN] ────────────────────────────────────────────────
#
# 有一類關卡說的是實話,而那句實話是「我們還沒做」——「這一版沒有語言模型」
# 就是。它不是這一次發布搞壞的東西,是一件本來就還沒做完的事。
#
# 判成失敗的話整條車道會停:release_check.sh 在 build.yml 的 `fast` job 裡,
# 而 `publish` 的 needs: 掛著 fast —— 每一次 push 都紅、任何一版都發不出去,
# 直到那件事做完。判成通過的話,就回到這支腳本存在的原因:假綠燈。
#
# 所以 [WARN] 是第三種結果,而且**警告自己要被守著**:只有登記在
# scripts/lib/known_gaps.tsv(有工單、有到期日)的缺口才准是警告,
# 過期就真的紅,沒登記的一律紅。見底下的 gap()。
#
# ⚠ 為什麼 --strict 不把 WARN 當成失敗(它把 SKIP 當成失敗)。
#   兩者不是同一件事:SKIP 是「這一關**沒有驗**」,在 CI 上等於沒有訊號;
#   WARN 是「驗過了,結論是我們還沒做,而且那件事有名字、有人、有期限」。
#   把後者也判紅就是回到卡死車道那條路。
WARN=0
WARNED=""
warn() {
  echo "  [WARN] $*"
  WARN=$((WARN+1))
  WARNED="$WARNED
    · $*"
  printf '%s\n\n' "$*" >> "$OUT/known-gaps.md"
}

# 已知缺口清冊。格式與規則見該檔檔頭。
GAPS_FILE="$ROOT/scripts/lib/known_gaps.tsv"
GAPS_USED=""

# gap <id> <一句話>
#
# 「這件事我們還沒做」。登記過而且沒過期 → [WARN];否則 → [FAIL]。
gap() {
  local id="$1" msg="$2" line until ticket today
  GAPS_USED="$GAPS_USED $id"
  line="$(awk -F'\t' -v want="$id" '$1==want{print;exit}' "$GAPS_FILE" 2>/dev/null || true)"
  if [ -z "$line" ]; then
    bad "$msg
    —— 而 scripts/lib/known_gaps.tsv 裡沒有登記 '$id'。
       已知缺口必須先登記(工單 + 到期日)才准只是警告,否則它就是一個
       沒有人負責、也不會過期的綠燈。"
    return
  fi
  until="$(printf '%s' "$line" | cut -f2)"
  ticket="$(printf '%s' "$line" | cut -f3)"
  case "$until" in
    [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
    *) bad "known_gaps.tsv 的 '$id' 到期日不是 YYYY-MM-DD:$until"; return ;;
  esac
  today="$(date -u +%F)"
  if [ "$today" ">" "$until" ]; then
    bad "$msg
    —— 而且 '$id' 的豁免在 $until 就過期了(工單 $ticket)。
       要嘛把它做完,要嘛改 known_gaps.tsv 的到期日並在 commit 訊息裡說為什麼。
       不改而讓它一直是警告,就是把「還沒做」變成永久狀態。"
    return
  fi
  warn "$msg
      (已知缺口 '$id',工單 $ticket,豁免到 $until 為止 —— 過期之後這一關會真的紅)"
}

CURRENT_STEP=""
step() { CURRENT_STEP="$*"; echo; echo "=== $* ==="; }

# ── 腳本自己猝死時要說得出來 ────────────────────────────────
#
# ⚠ `set -e` + `pipefail` 之下,任何一個沒被 `|| true` 兜住的非零結束都會讓
#   這支腳本**當場消失** —— 連 [FAIL] 與最後那三行統計都不印。實際發生過:
#   第 6c 關的 vc_of 是一條管線,release/ 底下有一個讀不出版本號的 .apk,
#   於是整支腳本從那一行起就不存在了,而呼叫端看到的只有一個非零結束碼。
#   **那與「某一項驗證失敗」在結果上長得一模一樣,而原因完全不同。**
#
#   所以掛一個 EXIT 陷阱:不管怎麼結束,統計一定印得出來,而且非預期的
#   結束要**指名它是非預期的**,不要偽裝成一次正常的失敗。
#   (陷阱正常返回時不會改動結束碼。)
FINISHED=0
on_abrupt_exit() {
  local rc=$?
  [ "$FINISHED" -eq 1 ] && return 0
  echo
  echo "============================================"
  echo " ⚠ 這支腳本在跑完之前就結束了(結束碼 $rc)"
  echo "   在此之前:通過 $PASS 項,失敗 $FAIL 項,略過 $SKIP 項"
  echo "   最後開始的那一關:${CURRENT_STEP:-(還沒進到任何一關)}"
  echo "============================================"
  {
    echo "這**不是**「某一項驗證失敗」。set -e 之下,任何沒被兜住的非零結束"
    echo "都會讓本腳本從那一行起消失 —— 上面那一關的最後幾行輸出就是現場。"
    echo "把它讀成關卡失敗會讓所有人去查錯的地方(這件事已經發生過一次)。"
  } >&2
  return 0
}
trap on_abrupt_exit EXIT

if [ "$EMU_ONLY" -eq 1 ]; then
  echo
  echo "※ --emu-only:這一輪**只**跑第 5–7 關(需要模擬器的部分)。"
  echo "  沒有跑到的是:離線稽核、工作區狀態、單元測試、建置、APK 內容、"
  echo "  以及第 3c 關(release 不是 debuggable、不含 harness)。"
  echo "  那幾關由 --skip-emu 那一條車道負責。**這一輪的綠燈不等於發布關卡通過。**"
  echo "  要發的那一份(第 6c 關驗它):$APK"
  echo "  驗證腳本用的那一份(第 6、6b 關):$APK_DEBUG"
  [ -f "$APK" ] || { echo "  找不到 release APK,升級關卡無從驗起。" >&2; exit 1; }
  # 少了 debug 那一份不是「少驗一點」,是第 6、6b 關整段沒得跑 ——
  # 而那兩關失敗時的訊息會是「輸入法打不出字」,完全指錯方向。
  [ -f "$APK_DEBUG" ] || { echo "  找不到 debug APK,第 6/6b 關無從驗起。" >&2; exit 1; }
fi

if [ "$EMU_ONLY" -eq 0 ]; then

step "0. 離線稽核（產品定位・原始碼層）"
# 放在第 1 關之前,而且是**不需要模擬器**的一關 —— 它擋的不是「會不會壞」,
# 是「我們對使用者講的話還算不算數」。無審查、離線為預設、經得起審計,
# 這些是這個 app 存在的理由;功能壞了可以下一版補,定位破了補不回來。
#
# ⚠ 但這一關跑在**建 APK 之前**(assembleDebug 在第 3 關),所以
#   audit_offline.sh 裡四段只看得到產物的檢查必然落空:.so 的動態符號、
#   APK 實際打進去的 allowBackup、dex 的傳遞相依、dex 粗篩。
#   而它 SKIP>0 仍然 exit 0,這裡原本又把訊息縮成「全數通過(N 項)」——
#   於是「使用者手上那份 APK 裡有沒有第二個出口」從來沒有在快車道上被問過。
#   **這一關現在只宣稱原始碼層**,產物層由第 3b 關負責,而那一關帶 --strict。
#
# 檢查項目與每一項的理由見 scripts/audit_offline.sh 的檔頭。
if "$ROOT/scripts/audit_offline.sh" > "$OUT/audit-offline.log" 2>&1; then
  # ⚠ `grep -c` 一個都沒數到時**回 1**,而 `set -e` 之下,
  #   `x="$(grep -c … )"` 這種賦值會讓整支腳本當場消失 ——
  #   而且是在「一項都沒略過」也就是**一切正常**的時候消失。
  #   `|| true` 不會多印一行:`grep -c` 自己已經印了 0。
  #   (寫成 `|| echo 0` 才會得到 "0\n0",那是這個專案踩過的另一個坑。)
  #   CI 上第 0 關跑在建 APK 之前,略過數必然 >0,所以那裡永遠看不到這件事;
  #   本機把 APK 建起來之後就必然踩到。**只在成功時發作的猝死最難查。**
  _AP="$(grep -c '\[PASS\]' "$OUT/audit-offline.log" || true)"
  _AS="$(grep -c '\[SKIP\]' "$OUT/audit-offline.log" || true)"
  # 略過的項數要說出來。藏起來的話,這一行的綠燈會被當成「全部驗過了」。
  ok "離線稽核（原始碼層）通過 $_AP 項，另有 $_AS 項要等 APK 建好（見第 3b 關）"
  # 同一個理由:不可以寫成 `[ "$_AS" -gt 0 ] && grep …`,那個 [ ] 回 1 一樣會猝死。
  if [ "$_AS" -gt 0 ]; then
    grep '\[SKIP\]' "$OUT/audit-offline.log" | sed 's/^/    /'
  fi
else
  bad "離線稽核未通過 —— 這份建置不符合本專案對外的承諾，不要發布"
  grep -A2 '\[FAIL\]' "$OUT/audit-offline.log" | head -20 >&2
fi

step "1. 工作區狀態"
cd "$ROOT"
if git diff --quiet && git diff --cached --quiet; then
  ok "工作區乾淨，APK 對得上 commit $(git rev-parse --short HEAD)"
else
  bad "工作區有未提交變更，這份 APK 對應不到任何 commit"
  git status --short | head -10
fi

step "2. 單元測試"
# ⚠ 兩個地方以前會讓這一關**一項都沒跑就報 PASS**:
#
#   1. Gradle 判 UP-TO-DATE。發布檢查跑在剛建置完之後,任務多半是最新的,
#      於是 `:app:testDebugUnitTest` 直接跳過,gradle 回 0,這裡就 ok。
#      任務選項 `--rerun`(要放在任務名**後面**,放前面 gradle 會當成未知的
#      全域選項而印出說明並失敗)讓那一個任務一定重跑,相依不受影響。
#   2. 數量從 log 抓。log 裡沒有「N tests」時原本填 `?` 然後照樣 ok ——
#      **數不出來就不該說通過**。改成讀測試結果的 XML,而且 0 項就算失敗。
RESULTS="$ROOT/android/app/build/test-results/testDebugUnitTest"
rm -rf "$RESULTS"
if (cd android && nohup ./gradlew --console=plain :app:testDebugUnitTest --rerun \
      > "$OUT/unittest.log" 2>&1); then
  N="$(python3 - "$RESULTS" <<'PYEOF'
import glob, os, sys, xml.etree.ElementTree as ET
total = fails = skipped = 0
for x in glob.glob(os.path.join(sys.argv[1], "*.xml")):
    r = ET.parse(x).getroot()
    total += int(r.get("tests") or 0)
    fails += int(r.get("failures") or 0) + int(r.get("errors") or 0)
    skipped += int(r.get("skipped") or 0)
print("%d %d %d" % (total, fails, skipped))
PYEOF
)" || N="0 0 0"
  set -- $N
  if [ "${1:-0}" -eq 0 ]; then
    bad "單元測試一項都沒跑 —— gradle 判 UP-TO-DATE 或結果檔不見了，這不是通過"
  elif [ "${2:-0}" -gt 0 ]; then
    bad "單元測試有 $2 項失敗（共 $1 項），見 $OUT/unittest.log"
  elif [ "${3:-0}" -gt 0 ]; then
    bad "單元測試有 $3 項被略過（共 $1 項）—— 略過的測試守不住任何東西"
  else
    ok "單元測試通過（$1 項，0 略過）"
  fi
else
  bad "單元測試失敗，見 $OUT/unittest.log"
  grep -E "FAILED|AssertionError" "$OUT/unittest.log" | head -10 || true
fi

step "3. 建置 APK（release 與 debug 兩份）"
# 兩份都要建。release 是使用者拿到的;debug 是模擬器上那幾支驗證腳本用的。
# 只建其中一份的話,另一條路線會在幾關之後以「找不到檔案」失敗,
# 而那個訊息看起來像環境壞了。
if (cd android && nohup ./gradlew --console=plain assembleRelease assembleDebug \
      > "$OUT/build.log" 2>&1); then
  if [ -f "$APK" ] && [ -f "$APK_DEBUG" ]; then
    ok "assembleRelease $(stat -c%s "$APK") bytes / assembleDebug $(stat -c%s "$APK_DEBUG") bytes"
  else
    # gradle 回 0 但檔案不在 —— 最常見的原因是沒有簽章設定,於是 AGP 產出的是
    # app-release-**unsigned**.apk。那份東西誰也裝不上,而「建置成功」會蓋掉它。
    bad "gradle 回 0,但預期的 APK 不在:$( [ -f "$APK" ] || echo "$APK ")$( [ -f "$APK_DEBUG" ] || echo "$APK_DEBUG")"
    ls -l "$ROOT/android/app/build/outputs/apk"/*/ 2>/dev/null | sed 's/^/    /' || true
  fi
else
  bad "建置失敗，見 $OUT/build.log"; tail -20 "$OUT/build.log"
fi

step "3b. 離線稽核（產品定位・產物層）"
# 第 0 關跑在 assembleDebug 之前,所以那四段只看得到產物的檢查一定是空的。
# 這一關在 APK 建好之後再跑一次,而且**一律帶 --strict**(略過算失敗)——
# 「這一輪沒有掃過傳遞相依」在 CI 上沒有人會去讀,綠燈就是綠燈。
# --strict 不是從呼叫端傳下來的:呼叫端沒帶 --strict 也不表示產物層可以不驗,
# 那是這一關存在的理由。(上游原始碼沒抓下來造成的略過除外,見 audit_offline.sh
# 的 skipped_upstream —— 沙盒那一項由同一條車道的 verify_lua_sandbox.sh 真的驗。)
if [ -f "$APK" ]; then
  if "$ROOT/scripts/audit_offline.sh" --strict > "$OUT/audit-offline-apk.log" 2>&1; then
    # ⚠ 綠燈不夠。要問的是「產物層那四段**跑了幾段**」——
    #   落空的時候輸出裡少的那幾行,和一切正常長得一模一樣,
    #   而這正是這一輪要修的東西,不能修完又留一個同型的洞。
    _ART="$(sed -n 's/^ 產物層檢查:\([0-9]*\/[0-9]*\) .*/\1/p' "$OUT/audit-offline-apk.log")"
    if [ "$_ART" = "4/4" ]; then
      ok "離線稽核（含產物層）通過 $(grep -c '\[PASS\]' "$OUT/audit-offline-apk.log") 項，產物層 $_ART 都真的跑了"
    else
      bad "離線稽核沒紅，但產物層只跑了「${_ART:-讀不到}」段 —— 這不是通過，是沒驗到"
      grep -E '\[SKIP\]|產物層檢查' "$OUT/audit-offline-apk.log" | head -10 >&2
    fi
  else
    bad "離線稽核在**產物層**沒過 —— 原始碼寫對不等於打包出來是對的"
    grep -A2 '\[FAIL\]' "$OUT/audit-offline-apk.log" | head -25 >&2
  fi
else
  # 走到這裡代表第 3 關沒建出 APK。那已經是 FAIL 了,但這一關要自己說一句,
  # 否則「產物層沒驗」會被第 3 關的紅燈蓋過去而沒有人記得補驗。
  skip "APK 不在，產物層的離線稽核（.so 符號 / APK 的 allowBackup / dex）沒有跑"
fi
# ⚠ **已知缺口(2026-08-10 明著留下,不是漏看)**:audit_offline.sh 的產物層
#   仍然優先掃 app-debug.apk(它自己 :291 與 :536 寫死了那個順序),而使用者
#   拿到的是 release。debug 是**未經 R8 縮減、而且多了 src/debug** 的一份,
#   所以「誰碰得到網路」這一項掃它是**更寬**的網(dex_network_refs.py 釘死的
#   清單也是在它身上量的);但「APK 實際打進去的 allowBackup」這一項掃的
#   就確實是錯的那一份。改掉要連同重新釘 dex 清單一起做,而那會動到
#   verify_audit_offline.sh 的 16 條植入 —— 不在這一輪的範圍。

step "3c. 要發的那一份不是 debuggable，也不含開發用的 harness"
# ═════════════════════════════════════════════════════════════════════════
# 這一關擋的是 2026-08-10 之前一直在發生的事:發給使用者的是 debug 建置。
# 後果不是「開發者方便」——
#   · debuggable=true → 任何拿得到 adb(或把裝置解鎖)的人都能
#     `run-as <套件名>` 把詞庫與輸入歷史整包讀走,並對輸入法進程
#     掛除錯器。而輸入法看得到使用者打的每一個字。
#   · src/debug 的 BackupHarnessReceiver 是一個 exported 的廣播入口,
#     一條 `am broadcast` 就能叫 app 匯出整份詞庫到指定路徑。留在 release
#     裡等於留一個後門 —— 所以它**不可以**為了讓往返測試好跑而搬進 main。
#
# ── 為什麼要同時驗 debug 那一份(正控)────────────────────────────────
# 只問「release 乾不乾淨」的話,偵測方法自己壞掉的那天(aapt2 換一版不再印
# application-debuggable、dex 裡的類別描述子換了寫法、unzip 抓不到 classes*.dex)
# 這一關會安靜地永遠說「乾淨」,而輸出和一切正常長得一模一樣。
# 所以每一項都成對:release 必須沒有,debug 必須有。debug 那半邊紅了,
# 代表**這一關本身**壞了,不是產品壞了 —— 訊息要這樣寫。
# ═════════════════════════════════════════════════════════════════════════
if [ -f "$APK" ] && [ -f "$APK_DEBUG" ]; then
  # 這三個字串都從 product.env 推導,不寫死套件名。
  HARNESS_CLASS="$RS_ANDROID_APP_ID.devtools.BackupHarnessReceiver"
  HARNESS_DEX="L$RS_ANDROID_PKG_PATH/devtools/BackupHarnessReceiver;"
  # 往返測試真正在驗的產品程式碼(它住在 src/main,兩個變體都有)。
  SHIPPED_DEX="L$RS_ANDROID_PKG_PATH/store/BackupController;"

  # dex 裡有沒有這個類別。**不要用 `unzip -p … | grep -q`** —— grep -q 一命中
  # 就結束,unzip 還在寫 → SIGPIPE → 在 pipefail 之下整條管線被判失敗。
  # grep -c 會讀到 EOF,沒有這個問題(本檔第 4 關的註解記過同一件事)。
  dex_has() {   # $1 apk  $2 類別描述子 → 印出命中行數
    unzip -p "$1" 'classes*.dex' 2>/dev/null | grep -ac -- "$2" || true
  }
  mani_has() {  # $1 apk  $2 字串 → 印出命中行數
    "$AAPT2" dump xmltree --file AndroidManifest.xml "$1" 2>/dev/null \
      | grep -c -- "$2" || true
  }

  # ── (1) debuggable ──────────────────────────────────────────────────
  R_BADG="$("$AAPT2" dump badging "$APK" 2>/dev/null || true)"
  D_BADG="$("$AAPT2" dump badging "$APK_DEBUG" 2>/dev/null || true)"
  R_DBG="$(printf '%s\n' "$R_BADG" | grep -c '^application-debuggable' || true)"
  D_DBG="$(printf '%s\n' "$D_BADG" | grep -c '^application-debuggable' || true)"
  R_DBG_X="$(mani_has "$APK" 'android:debuggable')"
  D_DBG_X="$(mani_has "$APK_DEBUG" 'android:debuggable')"

  if [ "$D_DBG" -eq 0 ] || [ "$D_DBG_X" -eq 0 ]; then
    bad "正控倒了:debug 建置**不是** debuggable(badging $D_DBG 處、manifest $D_DBG_X 處)。這代表偵測方法失效,不是產品變好了 —— 這一關現在什麼都保證不了"
  elif [ "$R_DBG" -ne 0 ] || [ "$R_DBG_X" -ne 0 ]; then
    bad "release 建置是 debuggable(badging $R_DBG 處、manifest $R_DBG_X 處)—— 任何拿得到 adb 的人都能 run-as 讀走使用者的詞庫與輸入歷史。檢查 android/app/build.gradle.kts 的 buildTypes.release"
  else
    ok "release 不是 debuggable，而 debug 是（正控成立，兩個判讀來源都同意）"
  fi

  # ── (2) harness receiver ────────────────────────────────────────────
  R_H_M="$(mani_has "$APK" "$HARNESS_CLASS")"
  D_H_M="$(mani_has "$APK_DEBUG" "$HARNESS_CLASS")"
  R_H_D="$(dex_has "$APK" "$HARNESS_DEX")"
  D_H_D="$(dex_has "$APK_DEBUG" "$HARNESS_DEX")"

  if [ "$D_H_M" -eq 0 ] || [ "$D_H_D" -eq 0 ]; then
    bad "正控倒了:debug 建置裡找不到 $HARNESS_CLASS(manifest $D_H_M 處、dex $D_H_D 處)。要嘛 harness 被刪了(那 verify_backup_roundtrip.sh 也跑不了),要嘛這一關的偵測方式已經失效"
  elif [ "$R_H_M" -ne 0 ] || [ "$R_H_D" -ne 0 ]; then
    bad "release 建置裡有 $HARNESS_CLASS(manifest $R_H_M 處、dex $R_H_D 處)—— 那是一個 exported 的廣播入口,一條 am broadcast 就能把使用者整份詞庫匯出到指定路徑。它必須留在 src/debug/"
  else
    ok "harness receiver 只在 debug 裡（release 的 manifest 與 dex 都沒有）"
  fi

  # ── (3) 往返測試驗的那段程式碼真的有出貨 ────────────────────────────
  # verify_backup_roundtrip.sh 跑的是 debug 建置(它需要 harness 驅動)。
  # 那個結論要能延伸到使用者手上,前提是「被驗的那段程式碼在 release 裡也在」。
  # 這一項就是把那個前提從「大家都這麼相信」變成一句可觀察的斷言。
  R_SHIP="$(dex_has "$APK" "$SHIPPED_DEX")"
  if [ "$R_SHIP" -gt 0 ]; then
    ok "release 的 dex 裡有 $SHIPPED_DEX —— 匯出/匯入往返驗的是真的會出貨的程式碼"
  else
    bad "release 的 dex 裡找不到 $SHIPPED_DEX —— 那 verify_backup_roundtrip.sh 在 debug 上的綠燈就延伸不到使用者手上"
  fi
else
  skip "兩份 APK 沒有都在，debuggable 與 harness 的對照檢查沒有跑"
fi

step "4. APK 內容（驗的是 release，也就是使用者拿到的那一份）"
# ⚠ 這一關以前驗的是 debug。ABI、IME 宣告、隨附的 yaml 三件事在兩個變體上
#   確實一樣,但那是**碰巧**一樣 —— 只要有人在 release 加一條 manifest 規則、
#   或讓某個 assets 只進 debug,這一關的綠燈就會替一份沒被看過的 APK 背書。
if [ -f "$APK" ]; then
  # ⚠ 先把清單抓進變數，不要用 `unzip -l | grep -q`。
  #   grep -q 一找到就結束，unzip 還在寫 → SIGPIPE → 在 `set -o pipefail`
  #   之下整條管線被判定失敗，變成假警報。輸出小時管線緩衝區塞得下不會發生，
  #   大時才會 —— 這種偶發性的假失敗最難查。
  LIST="$(unzip -l "$APK")"
  # ── ABI 與 IME 宣告:判斷搬到 scripts/apk_ime_shape.py ────────────────
  #
  # ⚠ 2026-08-10 之前這五關是用「這個字串有沒有出現在檔案裡」判斷的,
  #   兩個洞都用**真的 APK** 實測證實過(見 fix3-install 的 commit 訊息):
  #
  #     · ABI 只問 `lib/<abi>/` 底下有沒有東西。但相依函式庫
  #       (androidx.graphics.path、datastore_shared_counter)本來就在每個
  #       ABI 底下各放一個 .so —— 把 lib/x86_64/librime_jni.so 從發布出去的
  #       那份 APK 裡刪掉,舊寫法「含 x86_64（模擬器用）」照樣是綠的。
  #       而那份 APK 在 x86_64 裝置上就是「裝得起來、載不到引擎」。
  #
  #     · IME 三項只問三個字串在不在**整份 manifest** 裡,不問它們掛在
  #       哪個元素上。把 <service> 整個拿掉、改掛一個帶著同樣三個字串的
  #       <receiver>,重新建一份 release APK:系統眼裡它已經不是輸入法
  #       (aapt2 連 provides-component:'ime' 都不印了),而舊寫法三項全綠。
  #
  #   這正是本專案反覆吃虧的那一種:`grep 名字` 掃整個檔案 = 名字在別處
  #   出現一次就永遠綠。改成純函式 + 結構化判讀,並保留舊寫法當對照組 ——
  #   apk_ime_shape.py --self-test 的 16 條裡有兩條就是在斷言
  #   「舊寫法在這個變異上真的說綠」,少了它,新檢查嚴在哪裡沒有人驗得到。
  #
  # 這裡刻意逐項轉成 ok/bad(五項),不是整包一個綠燈:關卡數是本腳本結尾
  # 那道下界唯一的依據,合併成一項會讓「少驗了四件事」不留任何痕跡。
  SHAPE_OUT="$(python3 "$ROOT/scripts/apk_ime_shape.py" --apk "$APK" --aapt2 "$AAPT2" 2>&1 || true)"
  SHAPE_N="$(printf '%s\n' "$SHAPE_OUT" | grep -cE '^(PASS|FAIL): ' || true)"
  if [ "${SHAPE_N:-0}" -ne 5 ]; then
    bad "apk_ime_shape.py 只吐了 ${SHAPE_N:-0} 行判讀(預期 5)—— 是它自己壞了,不是這份 APK 沒問題:
$SHAPE_OUT"
  else
    while IFS= read -r _line; do
      case "$_line" in
        "PASS: "*) ok "${_line#PASS: }" ;;
        "FAIL: "*) bad "${_line#FAIL: }" ;;
      esac
    done <<EOF_SHAPE
$SHAPE_OUT
EOF_SHAPE
  fi

  # 隨附資料：沒有 schema 就是一個打不出字的輸入法
  has() { case "$2" in *"$1"*) return 0 ;; *) return 1 ;; esac; }
  N_YAML="$(printf '%s\n' "$LIST" | grep -c 'assets/rime/.*\.yaml' || true)"
  [ "$N_YAML" -gt 0 ] && ok "隨附 $N_YAML 份 yaml" || bad "assets 裡沒有任何 yaml"

  # ── 語言模型:兩級,而且兩級的分界線是「有沒有人宣稱做了」 ──────────────
  #
  # 這一關以前是**永遠綠**的:`has "essay" "$LIST" && ok "含語言模型"` 問的是
  # 「APK 的檔案清單裡有沒有出現字串 essay」,而 assets/rime/shared/essay.txt
  # 一定在裡面 —— 從第一天到現在沒有紅過一次。而且它綠得**不對**:essay.txt 是
  # 詞典編譯期的靜態詞頻表(`〇\t981` 這種一行一詞的字頻),不是語言模型。
  # 真正的語言模型是 librime-octagram 的 `.gram` 檔,而本專案(2026-08-13 實測):
  #
  #   · nm -C third_party/prebuilt/<abi>/lib/librime.a | grep -c octagram → 0
  #     (x86_64 與 arm64-v8a 都是 0,octagram 根本沒有被編進去)
  #   · core/data/shared/grammar.yaml 不存在,任何 .gram 也不存在
  #   · 方案裡的 `__patch: - grammar:/hant?` 結尾那個 `?` 是「找不到就靜默略過」
  #
  # 三個環節做了零個,而關卡說做完了。
  #
  # ── 為什麼不是直接判紅 ────────────────────────────────────────────────
  # 因為那會**卡死整條發布車道**:這支腳本在 build.yml 的 `fast` job 裡(每次
  # push 都跑),而 `publish` 的 needs: 掛著 fast。判紅 = 在有人把
  # librime-octagram 編進去、並隨附幾十 MB 的 .gram 之前,一版都發不出去。
  # 那不是「說實話」,那是把一件還沒排到的工作變成停機。
  #
  # 所以拆成兩級,分界線是**有沒有人宣稱做了**:
  #
  #   · 沒有語言模型,也沒有任何地方宣稱有 → [WARN],登記在 known_gaps.tsv,
  #     有工單、有到期日,過期就真的紅。發布報告(build/release-check/
  #     known-gaps.md)會原樣帶著這句話。
  #   · **宣稱有、實際沒有** → [FAIL]。那是壞掉,不是還沒做:方案硬性依賴
  #     grammar 卻缺檔,librime 部署時會出錯;repo 裡有 .gram 卻沒進 APK,
  #     那是打包漏了。兩種都會讓使用者拿到一份跑不起來或悄悄退化的東西。
  N_GRAM="$(printf '%s\n' "$LIST" | grep -cE 'assets/rime/.*(\.gram$|grammar\.yaml$)' || true)"
  if [ "${N_GRAM:-0}" -gt 0 ]; then
    ok "含語言模型($N_GRAM 個 .gram / grammar.yaml)"
  else
    # (a) 方案宣告了**硬性**的 grammar 依賴(結尾沒有 `?` = 找不到就出錯)
    CLAIM_HARD="$(grep -rn -- '- grammar:' "$ROOT/core/data" 2>/dev/null | grep -v '?[[:space:]]*$' || true)"
    # (b) repo 裡真的有語言模型檔,卻沒有被打包進去 = 打包漏了
    CLAIM_ONDISK="$(find "$ROOT/core/data" \( -name '*.gram' -o -name 'grammar.yaml' \) 2>/dev/null | head -5 || true)"
    if [ -n "$CLAIM_HARD" ] || [ -n "$CLAIM_ONDISK" ]; then
      bad "宣稱有語言模型,APK 裡卻一個 .gram / grammar.yaml 都沒有:
${CLAIM_HARD:+
      · 方案宣告了硬性的 grammar 依賴(結尾少了 '?',缺檔時 librime 會出錯):
$CLAIM_HARD}${CLAIM_ONDISK:+
      · repo 裡有語言模型檔,卻沒有被打包進 APK(打包漏了):
$CLAIM_ONDISK}"
    else
      gap "language_model" "這一版沒有語言模型:APK 裡沒有任何 .gram 或 grammar.yaml。
      長句組字只能靠詞頻表,enable_sentence 的效果會比有語言模型時差很多。
      (essay.txt 不算 —— 那是詞典編譯期的靜態詞頻表)"
    fi
  fi
  # 詞頻表本身仍然是必要的(少了它候選排序會整個亂掉),只是它的名字不叫
  # 語言模型。這一條是新增的正控,不是把舊斷言改個字。
  has "essay.txt" "$LIST" && ok "含詞頻表 essay.txt" || bad "缺詞頻表 essay.txt"
else
  bad "找不到 APK"
fi

# ── 4b. 已知缺口清單自己要被守 ─────────────────────────────────────────────
#
# 一份「可以是警告」的清單,如果沒有人檢查它,就是一份永久豁免名單。
# 這一關反過來問兩件事:
#
#   · 清單上的每一條,這一輪**真的有關卡用到它**嗎?沒有用到 = 那件事已經
#     做完了(關卡轉綠),豁免卻還留著 —— 判紅,逼人把它刪掉。
#   · 格式壞掉(欄位不足)也判紅。一行讀不出到期日的豁免等於無限期豁免。
#
# 過期那一條不在這裡,在 gap() 裡:過期的當場就是 [FAIL]。
step "4b. 已知缺口清單(scripts/lib/known_gaps.tsv)"
if [ ! -f "$GAPS_FILE" ]; then
  bad "找不到 $GAPS_FILE —— 沒有這份清單,gap() 就沒有任何一條可以是警告"
else
  GAP_PROBLEMS=""
  GAP_N=0
  while IFS= read -r _l; do
    case "$_l" in ''|'#'*) continue ;; esac
    GAP_N=$((GAP_N+1))
    _id="$(printf '%s' "$_l" | cut -f1)"
    _until="$(printf '%s' "$_l" | cut -f2)"
    _ticket="$(printf '%s' "$_l" | cut -f3)"
    _why="$(printf '%s' "$_l" | cut -f4)"
    if [ -z "$_until" ] || [ -z "$_ticket" ] || [ -z "$_why" ]; then
      GAP_PROBLEMS="$GAP_PROBLEMS
      · '$_id' 欄位不全(要 id/到期日/工單/說明,以 TAB 分隔)"
      continue
    fi
    case " $GAPS_USED " in
      *" $_id "*) ;;
      *) GAP_PROBLEMS="$GAP_PROBLEMS
      · '$_id' 登記著,但這一輪沒有任何一關用到它 —— 那件事做完了嗎?
        做完了就把這一行刪掉;豁免留著不會有人再看第二眼。" ;;
    esac
  done < "$GAPS_FILE"
  if [ -n "$GAP_PROBLEMS" ]; then
    bad "已知缺口清單有問題:$GAP_PROBLEMS"
  else
    ok "已知缺口 $GAP_N 條,每一條都有工單與到期日,而且這一輪都真的被用到"
  fi
fi

fi   # EMU_ONLY == 0

[ "$SKIP_EMU" -eq 1 ] && { step "略過模擬器驗證（--skip-emu）"; }

if [ "$SKIP_EMU" -eq 0 ]; then
  # ⛔ 「抓第一台」= 永遠是 emulator-5554,而這一關會 uninstall。
  #
  # ⛔ **`SER` 是空字串的時候,一個 `adb -s "$SER"` 都不可以送出去。**
  #    `rs_pick_serial` 失敗有三種:0 台在線、多台在線不猜、**指名的那一台不在線**。
  #    三種都會落到 `SER=""`,而實測(2026-08-14,建置機上三台在線):
  #
  #        adb -s "" get-state     → error: more than one device/emulator
  #        adb -s nosuch get-state → error: device 'nosuch' not found
  #
  #    兩句話不一樣 —— `adb` 把**空字串當成「沒有指定」**。於是「指名的那一台
  #    不在線 ＋ 場上剛好一台」時,底下每一個 `adb -s "$SER"` 都會靜靜地打在
  #    那一台上,包含 `uninstall` ——**解除安裝別條線的 app**。
  #    (改動前是安全的:舊碼在挑不到時保留使用者指名的序號,adb 會回
  #     「device not found」而不是換一台。)
  SER="$(rs_pick_serial "$ADB")" || SER=""

  step "5. 核心層對照基準（不經 UI）"
  # `--skip-push` 只有在裝置上已經有那 13MB 資料時才成立。在本機重跑時它省下
  # 一分鐘；在 CI 上模擬器每次都是新的，硬帶 --skip-push 會讓這一關以
  # 「找不到資料」失敗，而那個失敗看起來像核心壞了。所以先問裝置。
  PUSH_ARG="--skip-push"
  if [ -z "$SER" ] || ! "$ADB" -s "$SER" shell "[ -d /data/local/tmp/rime/shared ]" >/dev/null 2>&1; then
    PUSH_ARG=""
    echo "  [INFO] 裝置上還沒有 librime 資料，這一輪會推送（約 13MB）"
  fi
  if nohup env RIME_SERIAL="$SER" "$ROOT/scripts/run_console_test.sh" $PUSH_ARG > "$OUT/console.log" 2>&1; then
    ok "拼音與注音在核心層都能上屏"
  else
    bad "核心層測試失敗 —— 問題在 librime 或資料，不在 UI。見 $OUT/console.log"
  fi

  step "5b. 清空 app 資料（驗證全新安裝的體驗）"
  # 發布驗證不可以信任裝置上殘留的狀態。實際發生過：某條開發線在測試時
  # 把裝置上的 default.custom.yaml 改成把九宮格排第一，於是這台機器的預設
  # 方案不再是拼音，驗證因此失敗——但那份 APK 本身是好的。
  #
  # 反過來說，帶著舊資料驗證也會漏掉真正的問題：使用者拿到的是全新安裝，
  # 首次要解壓資料、跑一次完整部署。那條路徑才是該驗的。
  if [ -n "$SER" ]; then
    # 用 uninstall 而不是 pm clear。pm clear 只清資料，app 仍在，
    # 於是裝置上殘留的 versionCode 會擋下較低版本的安裝 —— 實際發生過：
    # 測試用的建置把 versionCode 調高到 26090100，正式建置 26080714 因此
    # 被 Android 當成降版拒絕，關卡失敗但產品沒問題。
    # ⚠ `uninstall` 是破壞性的,而 `SER` 可能是**自動選來的那一台**
    #   (`rs_pick_serial` 在只有一台在線時會自動選)。閘沒過就跳過這一關,
    #   而不是把別條線的 app 移掉 —— 與下面 `else` 那一支同一個處置。
    #   由 `scripts/verify_device_hygiene.sh` 規則 C 守著。
    if ! rs_assert_destructive_ok "$ADB" "$SER" "uninstall $PKG"; then
      skip "未明著指定 RIME_SERIAL(或 AVD 對不上),沒有移除舊安裝 —— 驗的不是全新安裝的體驗"
    elif "$ADB" -s "$SER" uninstall $PKG >/dev/null 2>&1; then
      ok "已移除舊安裝，接下來驗的是全新安裝的路徑"
    else
      echo "  [INFO] 無既有安裝可移除，繼續"
    fi
  else
    skip "未指定 RIME_SERIAL，沒有清空 app 資料 —— 驗的不是全新安裝的體驗"
  fi

  step "5c. 輸入測試靶（dev.rime.imetest）"
  # 第 6 關與第 6b 關都往 dev.rime.imetest 的輸入框打字。在本機它早就裝著了，
  # 所以這件事一直沒有人管；CI 上每一輪都是全新的模擬器，少了它第 6 關會以
  # 「無法啟動 dev.rime.imetest」失敗——而那個訊息看起來像輸入法壞了。
  # 這支腳本不該假設裝置上殘留著什麼，該自己把前置條件備齊。
  TESTAPP="$ROOT/build/imetest/rime-imetest.apk"
  if [ -z "$SER" ]; then
    # `adb -s ""` = 沒有指定 = 場上唯一那一台。裝測試靶到別條線的模擬器上
    # 是看不見的污染,而且下面第 6 關會在那台上打字。
    skip "沒有選定裝置,測試靶沒有裝(第 6/6b 關無處打字)"
  elif "$ADB" -s "$SER" shell pm list packages 2>/dev/null | grep -q "dev.rime.imetest"; then
    ok "測試靶已在裝置上"
  else
    [ -f "$TESTAPP" ] || "$ROOT/scripts/build_testapp.sh" > "$OUT/testapp.log" 2>&1 || true
    if [ -f "$TESTAPP" ] && "$ADB" -s "$SER" install -r "$TESTAPP" >/dev/null 2>&1; then
      ok "已安裝測試靶 $(basename "$TESTAPP")"
    else
      bad "裝不上輸入測試靶，第 6 關無處打字。見 $OUT/testapp.log"
    fi
  fi

  step "6. 真正的輸入驗證（走實體按鍵路徑；用 debug 建置）"
  # 注意：verify_ime.sh 用的 input text 走 commitText，會繞過組字，
  # 即使 librime 沒載入也會通過。這裡一定要用 verify_rime_compose.sh。
  #
  # 為什麼這一關用 debug 而不是 release:子腳本要讀 logcat 的部署狀態、
  # 必要時要 run-as 撈現場。組字這條路徑上兩個變體跑的是同一份 src/main
  # (release 沒開 R8),而「真的會出貨的那份裝得起來、打得出字」由第 6c 關
  # 用 release 回答 —— 它會把 release 蓋裝上去、確認資料還在。
  for c in "nihao:1:你好:拼音"; do
    KEYS="${c%%:*}"; r="${c#*:}"; SEL="${r%%:*}"; r="${r#*:}"; EXP="${r%%:*}"; NAME="${r##*:}"
    if "$ROOT/scripts/verify_rime_compose.sh" --ime "$IME_ID" --apk "$APK_DEBUG" \
         --keys "$KEYS" --select "$SEL" --expect "$EXP" \
         --ready-log "phase . READY" --out "$OUT/verify-$KEYS" > "$OUT/verify-$KEYS.log" 2>&1; then
      ok "$NAME：$KEYS → $EXP"
    else
      # ⚠ 這裡**不要替失敗定調**。
      #   「$KEYS 沒有打出 $EXP」聽起來像組字壞了,於是所有人都去查引擎與詞庫。
      #   CI run 31310612204 就是這樣:真正的原因是測試靶的視窗沒拿到焦點,
      #   連一個鍵都還沒送出去。子腳本已經把原因寫在它的 [FAIL] 那幾行了,
      #   這裡原樣端出來就好,不要自己再編一個看起來合理的說法。
      bad "$NAME($KEYS → $EXP)沒有通過。子腳本說:"
      grep -E "^[[:space:]]*\[FAIL\]" -A 6 "$OUT/verify-$KEYS.log" | head -12 >&2 || true
      echo "         完整紀錄:$OUT/verify-$KEYS.log" >&2
    fi
  done

  step "6b. 按住不會弄壞鍵盤（tap 驗不到的那一類）"
  # `adb shell input tap` 的 down/up 幾乎沒有間隔。「按下之後按鍵永久變灰」
  # 那個缺陷用 tap 一次都重現不出來，要按住 100ms 以上——它就是這樣漏到
  # 使用者手上的。所以這一關送的是 `input swipe`（起訖同點）。
  # 判定與自我驗證（在畫面上植入一塊一顆鍵大小的灰塊，要求比對器報紅）
  # 見 scripts/verify_longpress.sh 的檔頭。
  if RIME_SERIAL="$SER" "$ROOT/scripts/verify_longpress.sh" \
       --ime "$IME_ID" --out "$OUT/longpress" > "$OUT/longpress.log" 2>&1; then
    ok "按住 12 次之後鍵盤外觀復原，且仍打得出字"
  else
    # 同上:不要替失敗定調。「按住之後鍵盤壞了」只是這一關**可能**的結論之一,
    # 而它也可能根本沒走到按住那一步(例如測試靶沒有焦點)。
    bad "按住那一關沒有通過。子腳本說:"
    grep -E "^[[:space:]]*\[FAIL\]" -A 6 "$OUT/longpress.log" | head -12 >&2 || true
    grep -E "差異|SystemExit|Error" "$OUT/longpress.log" | head -4 >&2 || true
    echo "         完整紀錄:$OUT/longpress.log" >&2
  fi

  step "6c. 升級路徑（覆蓋安裝，不解除安裝；蓋上去的是 release）"
  # ⚠ 這一關驗的是 **release**,也就是使用者真的會裝下去的那一份。
  #   2026-08-10 之前它驗的是 debug —— 於是「舊版能不能被新版蓋掉、蓋完資料
  #   還在不在」這件事,從來沒有在使用者實際會拿到的那個檔案上問過。
  #   兩個變體的簽章與 versionCode 雖然一樣,但 manifest 合併、變體專屬的
  #   資源與 source set 都可能讓它們的安裝行為分岔。
  #
  # 最近兩個真 bug 都出在這條路徑上：新增的內建方案進不到舊使用者、
  # 以及降版被拒。全新安裝永遠測不到它們，而真實使用者絕大多數是升級。
  # 挑「前一版」要挑得對，否則會拿測試產物來比。條件有三：
  #   1. 不是 -dirty（那是開發途中的建置，不是發布過的版本）
  #   2. versionCode 嚴格小於新版（測試建置常把版號調高，拿它比會誤判成降版）
  #   3. 取符合條件者中最新的一個
  #
  # ⚠ CI 上「前一版」刻意放的是**線上正在服役的 rime-latest.apk**（由
  #   .github/workflows/build.yml 下載進 release/）。那一份是在別的機器上、
  #   用同一把金鑰簽的。所以這一關同時是「CI 的簽章真的和本機同一條鏈嗎」
  #   的決定性驗收：裝得上去 = 使用者升得上來；裝不上去 = 使用者只能移除重裝。
  # ⚠ **這兩支不可以做成管線。**
  #   舊版是 `"$AAPT2" dump badging | grep -oE ... | head -1 | tr -dc '0-9'`,
  #   而 `set -e` + `pipefail` 之下:release/ 底下只要有一個 .apk 讀不出
  #   版本號(aapt2 失敗、或 grep 沒命中就以 1 結束),整條管線就是失敗 ——
  #   而它的結果直接拿去做變數指派,於是**整支腳本在這一行當場消失,
  #   連 [FAIL] 與最後的統計都不印**。呼叫端(CI、publish job)只看得到一個
  #   非零結束碼,看不到「哪一關失敗、通過幾項」,而那看起來像關卡失敗。
  #
  #   「讀不出版本號」是正常情形,不是這支腳本該死掉的理由。正確的行為是
  #   回空字串,由呼叫端決定怎麼辦(下面那個迴圈本來就會跳過空的)。
  #   所以這裡一律用 bash 內建的字串運算,一條管線都不用。
  vc_of() {
    local out rest vc=""
    out="$("$AAPT2" dump badging "$1" 2>/dev/null)" || out=""
    rest="${out#*versionCode=\'}"
    [ "$rest" != "$out" ] && vc="${rest%%\'*}"
    printf '%s' "${vc//[^0-9]/}"
  }
  NEW_VC="$(vc_of "$APK")"
  PREV=""
  for cand in $(ls -t "$ROOT"/release/*.apk 2>/dev/null); do
    case "$cand" in *-dirty.apk) continue ;; esac
    [ "$(basename "$cand")" = "$(basename "$APK")" ] && continue
    CVC="$(vc_of "$cand")"
    [ -n "$CVC" ] && [ -n "$NEW_VC" ] && [ "$CVC" -lt "$NEW_VC" ] || continue
    PREV="$cand"; break
  done
  # 「前一版」的套件名可能跟現在不一樣 —— 改 applicationId 等於換一個 app。
  # 那種時候 `run-as <新套件>` 在舊版身上看不到任何檔案,這一關必然失敗,
  # 而失敗訊息會是「舊版跑了 180s 還沒種出資料」—— 完全指錯方向。
  # 同上:沒有管線,讀不出來回空字串。
  pkg_of() {
    local out rest
    out="$("$AAPT2" dump badging "$1" 2>/dev/null)" || out=""
    rest="${out#*package: name=\'}"
    [ "$rest" = "$out" ] && return 0
    printf '%s' "${rest%%\'*}"
  }
  PREV_PKG=""
  [ -n "$PREV" ] && PREV_PKG="$(pkg_of "$PREV")"
  if [ -z "$SER" ]; then
    # ⛔ 這一關會 uninstall 現行版、裝上前一版。`adb -s ""` 等於「沒有指定」,
    #    場上剛好一台時它會打在那一台上 —— 別條線的 app 就這樣被移掉。
    #    沒有選定裝置時唯一誠實的答案是「沒驗到」(--strict 下就是失敗)。
    skip "沒有選定裝置（rs_pick_serial 沒過），升級路徑完全沒有驗到"
  elif [ -z "$PREV" ]; then
    skip "release/ 下沒有前一版，升級路徑完全沒有驗到"
  elif [ -n "$PREV_PKG" ] && [ "$PREV_PKG" != "$PKG" ]; then
    # ⚠ 這一段刻意**不是**把關卡關掉。沒有明文宣告就當場失敗 ——
    #   applicationId 無聲地變了,是最貴的那種缺陷:所有使用者都升不上去。
    if [ "$PREV_PKG" = "${RS_ANDROID_APP_ID_PREVIOUS:-}" ]; then
      echo "  前一版:$(basename "$PREV")(套件 $PREV_PKG)"
      echo "  [INFO] 這是一次**已宣告**的套件識別碼變更:$PREV_PKG -> $PKG"
      echo "  [INFO] 理由:${RS_ANDROID_APP_ID_CHANGE_REASON:-(未填)}"
      # 換套件之後「升級」在定義上不存在,所以改驗這一次真正該成立的事:
      # 新舊兩個 app 並存 —— 也就是使用者會看到兩個,必須自己移除舊的。
      # 這件事要被關卡驗到,而不是靠人記得在發布說明裡講。
      # 逐呼叫點過閘(verify_device_hygiene.sh 規則 C):裝一個**別的套件**
      # 上去、待會再把它移掉,兩件都是破壞性的。
      if ! rs_assert_destructive_ok "$ADB" "$SER" "install -r $PREV_PKG、uninstall $PREV_PKG"; then
        skip "沒過裝置閘,已宣告的套件變更沒有驗到共存行為"
      elif "$ADB" -s "$SER" install -r "$PREV" >/dev/null 2>&1; then
        PKGS="$("$ADB" -s "$SER" shell pm list packages 2>/dev/null | tr -d "\r")"
        if printf "%s\n" "$PKGS" | grep -qx "package:$PREV_PKG" \
           && printf "%s\n" "$PKGS" | grep -qx "package:$PKG"; then
          ok "已宣告的套件變更:新舊兩個 app 並存(使用者必須自行移除舊的那一個)"
        else
          bad "宣告的套件變更下,新舊兩個套件沒有同時存在 —— 與預期不符"
        fi
        rs_assert_destructive_ok "$ADB" "$SER" "uninstall $PREV_PKG(收尾)" \
          && "$ADB" -s "$SER" uninstall "$PREV_PKG" >/dev/null 2>&1 || true
      else
        bad "宣告的舊套件裝不上去,無法驗證共存行為"
      fi
      echo "  [!] 使用者動作:必須先解除安裝 $PREV_PKG,再安裝 $PKG。詞典與設定不會轉移。"
    else
      bad "前一版的套件是 $PREV_PKG,與現在的 $PKG 不同,而 product.env 沒有宣告這次變更 —— applicationId 被無聲地改掉了,所有使用者都升不上去"
    fi
  else
    echo "  前一版:$(basename "$PREV")"
    # 宣告過期偵測:線上版本的套件名已經等於現在的了,那份一次性宣告該刪掉,
    # 否則下一次真的無聲改套件時,會被誤當成「已宣告」而放行。
    if [ -n "${RS_ANDROID_APP_ID_PREVIOUS:-}" ]; then
      echo "  [INFO] product.env 仍留著 ANDROID_APP_ID_PREVIOUS=$RS_ANDROID_APP_ID_PREVIOUS,"
      echo "         而線上版本的套件名已經是 $PKG —— 那份一次性宣告可以刪掉了。"
    fi
    # 先移除，才能裝回舊簽章的版本 —— 第 6 步已經裝上新版了，
    # 直接 install -r 舊版會因為簽章不同（或降版）而失敗，
    # 於是升級測試被「略過」而不是被執行。那正好放過了要驗的東西。
    # 逐呼叫點過閘(verify_device_hygiene.sh 規則 C):移除現行版與裝回前一版
    # 都是破壞性的,而且都會落在 `adb -s "$SER"` 上。第 5b 關那一處過了閘,
    # **不代表這裡也過了** —— 那個 if 早在 150 行前就 `fi` 掉了。
    if ! rs_assert_destructive_ok "$ADB" "$SER" "uninstall $PKG、install 前一版"; then
      skip "沒過裝置閘,升級路徑沒有驗到"
    elif { "$ADB" -s "$SER" uninstall $PKG >/dev/null 2>&1 || true; } &&
         "$ADB" -s "$SER" install "$PREV" >/dev/null 2>&1; then
      # 讓它跑一次，把舊版的 user 資料種下去。
      # 固定 sleep 25 不夠：首次啟動要解壓 13MB 並編譯方案，模擬器上常超過一分鐘，
      # 而「資料還沒種下去就升級」會讓下面的保留檢查驗到一個空目錄——空的比對
      # 空的永遠會過，於是這一關變成裝飾品。所以改成等到真的有資料為止。
      "$ADB" -s "$SER" shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true

      # ── 為什麼這裡不再用 run-as ────────────────────────────────────────
      # 這一關現在蓋上去的是 **release**,而 release 不是 debuggable ——
      # `run-as $PKG` 會直接回 `package not debuggable` 並失敗。
      # 那正是這一輪要的結果:使用者手上那份不該讓任何人讀得到資料目錄。
      # (實測 2026-08-10,emulator-5554,即使 ro.debuggable=1:
      #    release → run-as: package not debuggable: <套件名>
      #    debug   → cache code_cache files shared_prefs)
      #
      # 所以要看「使用者的東西還在不在」,只剩 root shell 一條路,而模擬器
      # (userdebug)給得起。
      #
      # ⚠ 給不起 root 就**失敗**,不是略過。這一關存在的理由就是回答
      #   「升級後詞庫還在嗎」;看不見資料目錄時唯一誠實的答案是「沒驗到」,
      #   而這支腳本對「沒驗到」的態度寫在檔頭的 skip() 註解裡 ——
      #   曾經有一關被判成略過然後在結尾報「失敗 0 項」,一片全綠。
      DATA_DIR="/data/data/$PKG"
      ROOT_OK=0
      _R="$("$ADB" -s "$SER" root 2>&1 || true)"
      case "$_R" in
        *"already running as root"*) ;;
        *) "$ADB" -s "$SER" wait-for-device >/dev/null 2>&1 || true; sleep 2 ;;
      esac
      [ "$("$ADB" -s "$SER" shell id -u 2>/dev/null | tr -d '\r\n')" = "0" ] && ROOT_OK=1
      if [ "$ROOT_OK" -eq 0 ]; then
        bad "拿不到 root shell（adb root 說:${_R:-(沒有輸出)}）—— release 不是 debuggable，沒有 root 就看不見 $DATA_DIR，「升級後資料還在嗎」這一關驗不了。這不是通過"
      fi
      # 一律走 root。不做「debuggable 就用 run-as、否則用 root」的分流 ——
      # 兩條路的關卡會各自腐爛,而先壞掉的那條不會有人發現。
      seeded() {
        [ "$ROOT_OK" -eq 1 ] || return 0
        "$ADB" -s "$SER" shell "find $DATA_DIR -type f 2>/dev/null | sort" 2>/dev/null | tr -d '\r'
      }
      BEFORE=""
      for i in $(seq 1 180); do
        BEFORE="$(seeded)"
        [ "$(printf '%s\n' "$BEFORE" | grep -c .)" -ge 20 ] && break
        sleep 1
      done
      NB="$(printf '%s\n' "$BEFORE" | grep -c . || true)"
      printf '%s\n' "$BEFORE" > "$OUT/upgrade-files-before.txt"
      if [ "${NB:-0}" -lt 20 ]; then
        bad "舊版跑了 180s 還沒種出資料（只有 $NB 個檔）——「資料保留」這一關驗不了空目錄"
      else
        echo "  舊版已種下 $NB 個檔案"
        # 種一個哨兵。使用者真正在意的是「我的東西還在嗎」，而那件事只有
        # 「升級前放進去的東西升級後還讀得到」能證明。
        MARKER_TEXT="rime-upgrade-marker-$$"
        MARKER_PATH="$DATA_DIR/files/.rime_upgrade_marker"
        "$ADB" -s "$SER" shell "mkdir -p $DATA_DIR/files && printf %s '$MARKER_TEXT' > $MARKER_PATH" >/dev/null 2>&1 || true
        CHK="$("$ADB" -s "$SER" shell "cat $MARKER_PATH 2>/dev/null" 2>/dev/null | tr -d '\r\n')"
        # 種不進去 = 這一關的**證據來源**壞了。它必須紅,而且不可以被下面
        # 「升級後哨兵還在嗎」的比對掩蓋 —— 空的比對空的永遠會過。
        [ "$CHK" = "$MARKER_TEXT" ] || bad "種不進哨兵檔（讀回 '$CHK'，路徑 $MARKER_PATH）——資料保留這一關驗不了"
        if "$ADB" -s "$SER" install -r "$APK" > "$OUT/upgrade.log" 2>&1; then
          ok "舊版可被新版覆蓋安裝（簽章相容、versionCode 未降版）"
          NEWVC="$("$ADB" -s "$SER" shell dumpsys package $PKG 2>/dev/null | grep -oE 'versionCode=[0-9]+' | head -1)"
          echo "       安裝後：$NEWVC"
          # 「裝得上去」還不夠，要問資料在不在。簽章不同時 Android 會拒絕，
          # 但別的原因（例如 app 自己在升級路徑上把目錄砍了重建）一樣會讓
          # 使用者失去自訂詞庫，而那個失敗只有這裡問得出來。
          AFTER="$(seeded)"
          printf '%s\n' "$AFTER" > "$OUT/upgrade-files-after.txt"
          NA="$(printf '%s\n' "$AFTER" | grep -c . || true)"
          # 不逐檔比對：librime 的 userdb 是 LevelDB，重開時 MANIFEST/LOG 會換檔名，
          # 逐檔比會為了正常的輪替而報紅——那種假警報最後一定會被關掉，
          # 於是連真的資料遺失也一起關掉了。
          # 改問兩件不會誤判的事：我們自己種的哨兵還在嗎，資料量有沒有塌掉。
          MARKER="$("$ADB" -s "$SER" shell "cat $MARKER_PATH 2>/dev/null" 2>/dev/null | tr -d '\r\n')"
          if [ "$MARKER" != "$MARKER_TEXT" ]; then
            bad "升級後哨兵檔不見了（讀到 '$MARKER'）——使用者的自訂詞庫與設定會跟著消失"
          elif [ "${NA:-0}" -lt $((NB / 2)) ]; then
            bad "升級後檔案數從 $NB 掉到 $NA —— 資料目錄被清掉了大半"
          else
            ok "升級後資料保留（哨兵檔還在，檔案數 $NB → $NA）"
          fi
          # 蓋上去的是 release,所以裝完之後 run-as 必須**失敗**。
          # 這一句不是額外的裝飾:上面每一項用的都是 root shell,
          # 而 root shell 讀得到資料目錄這件事,和「這份 APK 是不是 debuggable」
          # 完全無關 —— 少了這一句,把 release 悄悄改成 debuggable 之後
          # 第 6c 關會一路全綠。第 3c 關驗的是檔案,這一句驗的是**裝上去之後**。
          #
          # ⚠ 這一行的兩個坑,兩個都實際踩過(2026-08-10 本機):
          #   1. run-as 被擋下時 adb 回非零 → `X="$(…)"` 的結束碼就是非零 →
          #      `set -e` 當場中止整支腳本。症狀是日誌停在上一行、沒有結尾
          #      的統計、退出碼 1 —— 看起來像「後面那幾關不存在」。
          #      **這一關期待的正是失敗**,所以結束碼一定要吃掉。
          #   2. 收尾不可以用 `head`:它讀夠了就關管線,上游拿到 SIGPIPE,
          #      在 pipefail 之下整條變非零(本檔第 4 關的註解記過同一件事)。
          #      用 tr 把換行壓成空白,它會讀到 EOF。
          RA_OUT=""
          RA_OUT="$("$ADB" -s "$SER" shell "run-as $PKG ls" 2>&1 | tr -d '\r' | tr '\n' ' ')" || true
          case "$RA_OUT" in
            *"not debuggable"*)
              ok "裝上去之後 run-as 被系統擋下（$RA_OUT）" ;;
            "")
              bad "run-as 一個字都沒有回 —— 這一句什麼都沒問到,不是通過" ;;
            *)
              bad "裝上去的這一份可以被 run-as 讀出資料目錄，回應是「$RA_OUT」—— 使用者的詞庫與輸入歷史對任何拿得到 adb 的人是敞開的。這份 APK 不該發" ;;
          esac
        else
          bad "覆蓋安裝失敗 —— 現有使用者將無法升級，只能解除安裝重裝並失去詞典與設定"
          head -5 "$OUT/upgrade.log" >&2
        fi
      fi
      # 把裝置還原成非 root。後面還有別的腳本要跑（verify_syllables、
      # verify_backup_roundtrip），讓它們拿到的是預設狀態而不是這一關的殘留。
      [ "$ROOT_OK" -eq 1 ] && { "$ADB" -s "$SER" unroot >/dev/null 2>&1 || true; sleep 2; }
    else
      skip "前一版裝不上（簽章不同或降版），升級路徑沒有驗到"
    fi
  fi

  step "7. 截圖存證"
  # 這一項不自動判定 —— 「醜」與「拉伸」沒辦法寫成斷言，只能靠人看。
  # 但至少要有一張圖存下來，讓發布前有機會看一眼。
  if [ -f "$OUT/verify-nihao/01-composing.png" ]; then
    ok "已產生鍵盤截圖：$OUT/verify-nihao/01-composing.png"
    echo "       ⚠ 這張圖需要人工過目。視覺問題無法自動判定。"
  else
    bad "沒有產生截圖"
  fi
fi

FINISHED=1   # 走到這裡代表每一關都跑完了 —— 上面的 EXIT 陷阱不必再說話。

# ── 關卡數的下界 ────────────────────────────────────────────────────────
#
# ⚠ 這一段不是潔癖,是實測出來的洞。覆核者把整個 step 3c(release 建置不得是
#   debuggable)從這支腳本裡刪掉,對一份 **debuggable 的 APK** 跑
#   `--skip-emu --strict`,它印的是:
#
#       通過 12 項，失敗 0 項，略過 0 項
#       可以發布：./scripts/publish_apk.sh
#
#   一整關消失,而輸出跟一切正常長得一模一樣。這正是這個專案反覆吃虧的
#   「測試在該紅的時候安靜地不跑」,只是這次不跑的是**整關**。
#
#   publish_apk.sh 早就有 `SC_N -ne 36` 這種保護,這支沒有。補上。
#
#   數字是下界不是等號:新增關卡不該逼人改這裡,刪掉關卡才要。
#   刪關卡是合法的(例如某一關被更好的取代),但**必須是有人動手把下界改小**,
#   而不是刪完之後沒有任何東西提起這件事。
#   數字是**量出來的**,不是估的(2026-08-10,乾淨工作區):
#     --skip-emu  15 關   --emu-only  4 關   合計 19 關
#   第一版我把 --skip-emu 訂在 11,結果把 3c 整關刪掉之後總數 12 > 11,
#   下界沒有咬到 —— 一個訂得太鬆的下界跟沒有下界是一樣的東西。
# ── 下界 ────────────────────────────────────────────────────────────────
#
# ⚠ 這幾個數字**必須跟著實跑的關卡數走**。覆核抓到過一次:快車道實跑 16 關
#   而下界還寫著 15 —— 剛好空出一格,可以有人刪掉一整關而這道防線不會響。
#   改關卡的人請當場重跑一次對應的車道,把印出來的「通過+失敗+警告+略過」
#   填回來,不要用估的。
#
#   2026-08-13 三條車道都實跑過一次(emulator-5558):
#     --skip-emu --strict  通過 16、失敗 0、警告 1、略過 0  = 17 關
#     --emu-only           通過  6、失敗 0、警告 0、略過 1  =  7 關
#     完整                 通過 22、失敗 0、警告 1、略過 1  = 24 關 = 17 + 7 ✓
MIN_EMU_ONLY=7       # 第 5–7 關(含 5b/5c/6b/6c)。原本寫 4,比實跑少 3 關。
MIN_SKIP_EMU=17      # 第 0–4 關(含 3b/3c/4b)。原本寫 15,比實跑少 1 關。
# 完整車道 = 兩條車道相加,不另外手寫一個數字(手寫的第三個數字正是上面那個坑)。
MIN_FULL=$((MIN_SKIP_EMU + MIN_EMU_ONLY))

_min=$MIN_FULL
_lane="完整"
if [ "$EMU_ONLY" -eq 1 ]; then _min=$MIN_EMU_ONLY; _lane="--emu-only"
elif [ "$SKIP_EMU" -eq 1 ]; then _min=$MIN_SKIP_EMU; _lane="--skip-emu"
fi
# ⚠ WARN 必須算進來。不算的話,把一關從 [FAIL] 改成 [WARN] 會讓總數 -1,
#   於是「下界」這道防線自己被那個改動打了一個洞。
_ran=$((PASS + FAIL + SKIP + WARN))
if [ "$_ran" -lt "$_min" ]; then
  echo >&2
  echo "這一輪($_lane)只跑了 $_ran 關,少於下界 $_min 關。" >&2
  echo "有關卡被刪掉或整段沒有執行到 —— 而那不會讓任何一項變紅,只會讓總數變小。" >&2
  echo "確定要刪關卡的話,把 release_check.sh 裡的下界一起改小,並在 commit 訊息裡說為什麼。" >&2
  FAIL=$((FAIL+1))
fi

echo
echo "============================================"
echo " 通過 $PASS 項，失敗 $FAIL 項，警告 $WARN 項，略過 $SKIP 項（下界 $_min，$_lane）"
echo "============================================"
if [ "$SKIP" -gt 0 ]; then
  echo "以下這幾關沒有驗到,發布前自己決定能不能接受:$SKIPPED"
  echo
fi
if [ "$WARN" -gt 0 ]; then
  echo "以下是**已知缺口** —— 驗過了,結論是我們還沒做。每一條都登記在"
  echo "scripts/lib/known_gaps.tsv,有工單與到期日,過期之後這幾關會真的紅:$WARNED"
  echo
  echo "  這幾句話已經寫進 $OUT/known-gaps.md —— 那份是**發布報告**的一部分,"
  echo "  請把它帶進這一版的發布說明裡。"
  echo "  (刻意**不**自動塞進 version.json 的 notes:那是唯一會被舊版 app 顯示給"
  echo "   使用者看的自由文字,寫什麼是產品決定,不該由一支關卡腳本代勞。)"
  echo
fi
if [ "$FAIL" -gt 0 ]; then
  echo "驗證未通過，不要發布。" >&2
  exit 1
fi
if [ "$EMU_ONLY" -eq 1 ]; then
  echo "模擬器那幾關通過。**這不是完整的發布關卡** —— 第 0–4 關（離線稽核、"
  echo "工作區、單元測試、建置、APK 內容）這一輪沒有跑，由 --skip-emu 那一條負責。"
elif [ "$SKIP_EMU" -eq 1 ]; then
  echo "不需要模擬器的那幾關通過。**這不是完整的發布關卡** —— 第 5–7 關（核心層、"
  echo "輸入驗證、按住回歸、升級路徑）這一輪沒有跑，由 --emu-only 那一條負責。"
elif [ "$SKIP" -gt 0 ]; then
  echo "可以發布，但上面那 $SKIP 關是沒驗過的：./scripts/publish_apk.sh"
else
  echo "可以發布：./scripts/publish_apk.sh"
fi
