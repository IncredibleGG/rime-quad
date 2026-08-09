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
#   ./release_check.sh --apk <path> # 驗指定的 APK,不自己建
#
# ── 為什麼要拆成快慢兩條車道 ──────────────────────────────────────────
# 模擬器在 CI 上要開機、要冷啟動部署,一輪十幾分鐘;單元測試與 APK 內容
# 檢查兩分鐘就跑完。全部綁在一起的結果是「太慢所以只在發版前跑」,
# 那等於沒有。所以拆開:快的每次 push 跑,慢的推上 main 或手動觸發時跑。
#
# **拆開不等於拿掉。** 兩條車道加起來必須仍是原本那 16 項:
#   --skip-emu → 第 0–4 關      --emu-only → 第 5–7 關
# 而且 --emu-only 會在結尾明白列出它**沒有**驗的那幾關,免得有人拿慢車道
# 的綠燈當成「發布關卡通過」。發布必須兩條都綠(見 .github/workflows/build.yml
# 的 publish job 的 needs:)。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 產品識別碼一律從這裡來,不在腳本裡寫死。理由見 scripts/lib/product.env 檔頭。
# shellcheck source=lib/product.sh
. "$ROOT/scripts/lib/product.sh"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$SDK/platform-tools/adb"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
IME_ID="$RS_ANDROID_IME_ID"
PKG="$RS_ANDROID_APP_ID"
SKIP_EMU=0
EMU_ONLY=0
STRICT=0
OUT="$ROOT/build/release-check"

while [ $# -gt 0 ]; do
  case "$1" in
    --skip-emu) SKIP_EMU=1; shift ;;
    --emu-only) EMU_ONLY=1; shift ;;
    --strict)   STRICT=1; shift ;;
    --apk)      APK="$2"; shift 2 ;;
    -h|--help)  sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done
if [ "$SKIP_EMU" -eq 1 ] && [ "$EMU_ONLY" -eq 1 ]; then
  echo "--skip-emu 與 --emu-only 同時指定,那就什麼都不會驗到。拒絕。" >&2
  exit 2
fi

mkdir -p "$OUT"

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
step() { echo; echo "=== $* ==="; }

if [ "$EMU_ONLY" -eq 1 ]; then
  echo
  echo "※ --emu-only:這一輪**只**跑第 5–7 關(需要模擬器的部分)。"
  echo "  沒有跑到的是:離線稽核、工作區狀態、單元測試、建置、APK 內容。"
  echo "  那幾關由 --skip-emu 那一條車道負責。**這一輪的綠燈不等於發布關卡通過。**"
  echo "  待驗的 APK:$APK"
  [ -f "$APK" ] || { echo "  找不到那個 APK,無從驗起。" >&2; exit 1; }
fi

if [ "$EMU_ONLY" -eq 0 ]; then

step "0. 離線稽核（產品定位）"
# 放在第 1 關之前,而且是**不需要模擬器**的一關 —— 它擋的不是「會不會壞」,
# 是「我們對使用者講的話還算不算數」。無審查、離線為預設、經得起審計,
# 這些是這個 app 存在的理由;功能壞了可以下一版補,定位破了補不回來。
#
# 檢查項目與每一項的理由見 scripts/audit_offline.sh 的檔頭。
if "$ROOT/scripts/audit_offline.sh" > "$OUT/audit-offline.log" 2>&1; then
  ok "離線稽核全數通過（$(grep -c '\[PASS\]' "$OUT/audit-offline.log") 項）"
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

step "3. 建置 APK"
if (cd android && nohup ./gradlew --console=plain assembleDebug > "$OUT/build.log" 2>&1); then
  ok "assembleDebug 成功（$(stat -c%s "$APK") bytes）"
else
  bad "建置失敗，見 $OUT/build.log"; tail -20 "$OUT/build.log"
fi

step "4. APK 內容"
if [ -f "$APK" ]; then
  # ⚠ 先把清單抓進變數，不要用 `unzip -l | grep -q`。
  #   grep -q 一找到就結束，unzip 還在寫 → SIGPIPE → 在 `set -o pipefail`
  #   之下整條管線被判定失敗，變成假警報。輸出小時管線緩衝區塞得下不會發生，
  #   大時才會 —— 這種偶發性的假失敗最難查。
  LIST="$(unzip -l "$APK")"
  ABIS="$(printf '%s\n' "$LIST" | grep -oE 'lib/[a-z0-9_-]+/' | sort -u | tr '\n' ' ')"
  case "$ABIS" in
    *arm64-v8a*) ok "含 arm64-v8a（真機用的就是這個）" ;;
    *) bad "缺 arm64-v8a，真機裝不起來" ;;
  esac
  case "$ABIS" in *x86_64*) ok "含 x86_64（模擬器用）" ;; *) bad "缺 x86_64" ;; esac

  # IME 宣告缺任一項，輸入法會裝得起來但系統看不見
  X="$("$AAPT2" dump xmltree --file AndroidManifest.xml "$APK" 2>/dev/null || true)"
  has() { case "$2" in *"$1"*) return 0 ;; *) return 1 ;; esac; }
  has "BIND_INPUT_METHOD" "$X"        && ok "有 BIND_INPUT_METHOD 權限"   || bad "缺 BIND_INPUT_METHOD"
  has "android.view.InputMethod" "$X" && ok "有 InputMethod intent-filter" || bad "缺 intent-filter"
  has "android.view.im" "$X"          && ok "有 android.view.im meta-data" || bad "缺 meta-data"

  # 隨附資料：沒有 schema 就是一個打不出字的輸入法
  N_YAML="$(printf '%s\n' "$LIST" | grep -c 'assets/rime/.*\.yaml' || true)"
  [ "$N_YAML" -gt 0 ] && ok "隨附 $N_YAML 份 yaml" || bad "assets 裡沒有任何 yaml"
  has "essay" "$LIST" && ok "含語言模型" || bad "缺語言模型"
else
  bad "找不到 APK"
fi

fi   # EMU_ONLY == 0

[ "$SKIP_EMU" -eq 1 ] && { step "略過模擬器驗證（--skip-emu）"; }

if [ "$SKIP_EMU" -eq 0 ]; then
  SER="${RIME_SERIAL:-${ANDROID_SERIAL:-}}"
  if [ -z "$SER" ]; then
    SER="$("$ADB" devices | awk '/emulator-[0-9]+\tdevice/{print $1; exit}')"
  fi

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
    "$ADB" -s "$SER" uninstall $PKG >/dev/null 2>&1 \
      && ok "已移除舊安裝，接下來驗的是全新安裝的路徑" \
      || echo "  [INFO] 無既有安裝可移除，繼續"
  else
    skip "未指定 RIME_SERIAL，沒有清空 app 資料 —— 驗的不是全新安裝的體驗"
  fi

  step "5c. 輸入測試靶（dev.rime.imetest）"
  # 第 6 關與第 6b 關都往 dev.rime.imetest 的輸入框打字。在本機它早就裝著了，
  # 所以這件事一直沒有人管；CI 上每一輪都是全新的模擬器，少了它第 6 關會以
  # 「無法啟動 dev.rime.imetest」失敗——而那個訊息看起來像輸入法壞了。
  # 這支腳本不該假設裝置上殘留著什麼，該自己把前置條件備齊。
  TESTAPP="$ROOT/build/imetest/rime-imetest.apk"
  if "$ADB" -s "$SER" shell pm list packages 2>/dev/null | grep -q "dev.rime.imetest"; then
    ok "測試靶已在裝置上"
  else
    [ -f "$TESTAPP" ] || "$ROOT/scripts/build_testapp.sh" > "$OUT/testapp.log" 2>&1 || true
    if [ -f "$TESTAPP" ] && "$ADB" -s "$SER" install -r "$TESTAPP" >/dev/null 2>&1; then
      ok "已安裝測試靶 $(basename "$TESTAPP")"
    else
      bad "裝不上輸入測試靶，第 6 關無處打字。見 $OUT/testapp.log"
    fi
  fi

  step "6. 真正的輸入驗證（走實體按鍵路徑）"
  # 注意：verify_ime.sh 用的 input text 走 commitText，會繞過組字，
  # 即使 librime 沒載入也會通過。這裡一定要用 verify_rime_compose.sh。
  for c in "nihao:1:你好:拼音"; do
    KEYS="${c%%:*}"; r="${c#*:}"; SEL="${r%%:*}"; r="${r#*:}"; EXP="${r%%:*}"; NAME="${r##*:}"
    if "$ROOT/scripts/verify_rime_compose.sh" --ime "$IME_ID" --apk "$APK" \
         --keys "$KEYS" --select "$SEL" --expect "$EXP" \
         --ready-log "phase . READY" --out "$OUT/verify-$KEYS" > "$OUT/verify-$KEYS.log" 2>&1; then
      ok "$NAME：$KEYS → $EXP"
    else
      bad "$NAME：$KEYS 沒有打出 $EXP，見 $OUT/verify-$KEYS.log"
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
    bad "按住之後鍵盤壞了或畫面沒有復原，見 $OUT/longpress.log"
    grep -E "\[FAIL\]|差異|SystemExit|Error" "$OUT/longpress.log" | head -10 >&2 || true
  fi

  step "6c. 升級路徑（覆蓋安裝，不解除安裝）"
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
  vc_of() { "$AAPT2" dump badging "$1" 2>/dev/null \
              | grep -oE "versionCode='[0-9]+'" | head -1 | tr -dc '0-9'; }
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
  pkg_of() { "$AAPT2" dump badging "$1" 2>/dev/null \
               | grep -oE "package: name='[^']+'" | head -1 | cut -d"'" -f2; }
  PREV_PKG=""
  [ -n "$PREV" ] && PREV_PKG="$(pkg_of "$PREV")"
  if [ -z "$PREV" ]; then
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
      if "$ADB" -s "$SER" install -r "$PREV" >/dev/null 2>&1; then
        PKGS="$("$ADB" -s "$SER" shell pm list packages 2>/dev/null | tr -d "\r")"
        if printf "%s\n" "$PKGS" | grep -qx "package:$PREV_PKG" \
           && printf "%s\n" "$PKGS" | grep -qx "package:$PKG"; then
          ok "已宣告的套件變更:新舊兩個 app 並存(使用者必須自行移除舊的那一個)"
        else
          bad "宣告的套件變更下,新舊兩個套件沒有同時存在 —— 與預期不符"
        fi
        "$ADB" -s "$SER" uninstall "$PREV_PKG" >/dev/null 2>&1 || true
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
    "$ADB" -s "$SER" uninstall $PKG >/dev/null 2>&1 || true
    if "$ADB" -s "$SER" install "$PREV" >/dev/null 2>&1; then
      # 讓它跑一次，把舊版的 user 資料種下去。
      # 固定 sleep 25 不夠：首次啟動要解壓 13MB 並編譯方案，模擬器上常超過一分鐘，
      # 而「資料還沒種下去就升級」會讓下面的保留檢查驗到一個空目錄——空的比對
      # 空的永遠會過，於是這一關變成裝飾品。所以改成等到真的有資料為止。
      "$ADB" -s "$SER" shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true
      # debug 建置是 debuggable，所以不需要 root 就能用 run-as 看自己的資料目錄。
      seeded() { "$ADB" -s "$SER" shell "run-as $PKG find . -type f 2>/dev/null | sort" 2>/dev/null | tr -d '\r'; }
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
        "$ADB" -s "$SER" shell "run-as $PKG sh -c 'mkdir -p files && printf %s \"$MARKER_TEXT\" > files/.rime_upgrade_marker'" >/dev/null 2>&1 || true
        CHK="$("$ADB" -s "$SER" shell "run-as $PKG cat files/.rime_upgrade_marker 2>/dev/null" 2>/dev/null | tr -d '\r\n')"
        [ "$CHK" = "$MARKER_TEXT" ] || bad "種不進哨兵檔（run-as 讀回 '$CHK'）——資料保留這一關驗不了"
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
          MARKER="$("$ADB" -s "$SER" shell "run-as $PKG cat files/.rime_upgrade_marker 2>/dev/null" 2>/dev/null | tr -d '\r\n')"
          if [ "$MARKER" != "$MARKER_TEXT" ]; then
            bad "升級後哨兵檔不見了（讀到 '$MARKER'）——使用者的自訂詞庫與設定會跟著消失"
          elif [ "${NA:-0}" -lt $((NB / 2)) ]; then
            bad "升級後檔案數從 $NB 掉到 $NA —— 資料目錄被清掉了大半"
          else
            ok "升級後資料保留（哨兵檔還在，檔案數 $NB → $NA）"
          fi
        else
          bad "覆蓋安裝失敗 —— 現有使用者將無法升級，只能解除安裝重裝並失去詞典與設定"
          head -5 "$OUT/upgrade.log" >&2
        fi
      fi
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

echo
echo "============================================"
echo " 通過 $PASS 項，失敗 $FAIL 項，略過 $SKIP 項"
echo "============================================"
if [ "$SKIP" -gt 0 ]; then
  echo "以下這幾關沒有驗到,發布前自己決定能不能接受:$SKIPPED"
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
