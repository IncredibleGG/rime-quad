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
