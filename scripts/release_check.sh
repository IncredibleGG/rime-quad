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
#   ./release_check.sh --skip-emu   # 只跑不需要模擬器的部分
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ADB="$SDK/platform-tools/adb"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
IME_ID="org.rimequad.ime/.RimeInputMethodService"
SKIP_EMU=0
OUT="$ROOT/build/release-check"

[ "${1:-}" = "--skip-emu" ] && SKIP_EMU=1

mkdir -p "$OUT"
PASS=0; FAIL=0
ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }
step() { echo; echo "=== $* ==="; }

step "1. 工作區狀態"
cd "$ROOT"
if git diff --quiet && git diff --cached --quiet; then
  ok "工作區乾淨，APK 對得上 commit $(git rev-parse --short HEAD)"
else
  bad "工作區有未提交變更，這份 APK 對應不到任何 commit"
  git status --short | head -10
fi

step "2. 單元測試"
if (cd android && nohup ./gradlew --console=plain :app:testDebugUnitTest > "$OUT/unittest.log" 2>&1); then
  N="$(grep -oE '[0-9]+ tests' "$OUT/unittest.log" | tail -1 || echo '?')"
  ok "單元測試通過（$N）"
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
  X="$("$SDK/build-tools/35.0.0/aapt2" dump xmltree --file AndroidManifest.xml "$APK" 2>/dev/null || true)"
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

[ "$SKIP_EMU" -eq 1 ] && { step "略過模擬器驗證（--skip-emu）"; }

if [ "$SKIP_EMU" -eq 0 ]; then
  step "5. 核心層對照基準（不經 UI）"
  if nohup "$ROOT/scripts/run_console_test.sh" --skip-push > "$OUT/console.log" 2>&1; then
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
  SER="${RIME_SERIAL:-${ANDROID_SERIAL:-}}"
  if [ -n "$SER" ]; then
    # 用 uninstall 而不是 pm clear。pm clear 只清資料，app 仍在，
    # 於是裝置上殘留的 versionCode 會擋下較低版本的安裝 —— 實際發生過：
    # 測試用的建置把 versionCode 調高到 26090100，正式建置 26080714 因此
    # 被 Android 當成降版拒絕，關卡失敗但產品沒問題。
    "$ADB" -s "$SER" uninstall org.rimequad.ime >/dev/null 2>&1 \
      && ok "已移除舊安裝，接下來驗的是全新安裝的路徑" \
      || echo "  [INFO] 無既有安裝可移除，繼續"
  else
    echo "  [INFO] 未指定 RIME_SERIAL，略過清空；驗證結果可能受殘留狀態影響"
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

  step "6b. 升級路徑（覆蓋安裝，不解除安裝）"
  # 最近兩個真 bug 都出在這條路徑上：新增的內建方案進不到舊使用者、
  # 以及降版被拒。全新安裝永遠測不到它們，而真實使用者絕大多數是升級。
  # 挑「前一版」要挑得對，否則會拿測試產物來比。條件有三：
  #   1. 不是 -dirty（那是開發途中的建置，不是發布過的版本）
  #   2. versionCode 嚴格小於新版（測試建置常把版號調高，拿它比會誤判成降版）
  #   3. 取符合條件者中最新的一個
  vc_of() { "$SDK/build-tools/35.0.0/aapt2" dump badging "$1" 2>/dev/null \
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
  if [ -z "$PREV" ]; then
    echo "  [INFO] release/ 下沒有前一版可用來測升級，略過"
  else
    echo "  前一版：$(basename "$PREV")"
    if "$ADB" -s "$SER" install -r "$PREV" >/dev/null 2>&1; then
      # 讓它跑一次，把舊版的 user 資料種下去
      "$ADB" -s "$SER" shell monkey -p org.rimequad.ime -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true
      sleep 25
      if "$ADB" -s "$SER" install -r "$APK" > "$OUT/upgrade.log" 2>&1; then
        ok "舊版可被新版覆蓋安裝（簽章相容、versionCode 未降版）"
        NEWVC="$("$ADB" -s "$SER" shell dumpsys package org.rimequad.ime 2>/dev/null | grep -oE 'versionCode=[0-9]+' | head -1)"
        echo "       安裝後：$NEWVC"
      else
        bad "覆蓋安裝失敗 —— 現有使用者將無法升級，只能解除安裝重裝並失去詞典與設定"
        head -5 "$OUT/upgrade.log" >&2
      fi
    else
      echo "  [INFO] 前一版裝不上（可能簽章不同或降版），略過升級測試"
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
echo " 通過 $PASS 項，失敗 $FAIL 項"
echo "============================================"
if [ "$FAIL" -gt 0 ]; then
  echo "驗證未通過，不要發布。" >&2
  exit 1
fi
echo "可以發布：./scripts/publish_apk.sh"
