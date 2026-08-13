#!/usr/bin/env bash
#
# verify_backup_roundtrip.sh — 「把全部存成一個文件 / 從文件恢復」真的走得通嗎
#
# ═══════════════════════════════════════════════════════════════════════════
#  為什麼需要這一支
# ═══════════════════════════════════════════════════════════════════════════
#
# 匯出/匯入是使用者換手機時唯一的救生索(`android:allowBackup` 是關的,
# 見 docs/backup-format.md §0),而它**在這支腳本之前從來沒有人跑過一次
# 完整往返**。單元測試守得住格式與安全檢查,守不住這一條:
#
#     使用者剛學到的詞,匯出的時候還在 librime 的**記憶體**裡。
#
# `Memory::OnCommit` 在上屏之後開一個交易才寫入,而那個交易住在
# `leveldb::WriteBatch`,要等 `FinishSession()` 或 `~UserDictionary` 才落地。
# 也就是說,直接複製 `*.userdb/` 目錄拿到的是**上一輪**的詞庫 —— 能開、能用、
# 大小差不多,只是少了最近的學習成果,而且沒有任何錯誤訊息。
# 那正是備份功能最不該有的失敗:使用者三個月後才發現,而且無從查起。
#
# ── 這支腳本怎麼確保自己不是在自我欺騙 ─────────────────────────────────
#
#   1. **學習用的 session 刻意不銷毀**(harness 的 `learn` op 把它存起來)。
#      session 一銷毀交易就落地了 —— 那時候就算匯出端完全沒有 flush,
#      測試一樣會綠,而缺陷還在。
#   1b. **教完之後、匯出之前不再打任何字。** 同一個理由的另一半:
#      `UserDictionary::Query` 開頭就 `FinishSession()`,所以**一次查詢**
#      也會替所有人提交。第一版把「確認它學到了」排在匯出之前,結果
#      把 flushEngine() 整支停掉、往返仍然全綠 —— 已實測。
#   2. **反向控制組**:`pm clear` 之後、匯入之前,先確認那幾個詞**真的不見了**
#      (候選回到原本的順序)。少了這一步,「匯入之後詞還在」可能只是因為
#      根本沒清乾淨 —— 一個永遠會綠的測試。
#   3. 每一步都斷言,失敗就停,並印出 logcat 現場。
#
# ── 這支跑的是 debug 建置,而使用者拿到的是 release ──────────────────────
#
# 2026-08-10 起發布的是 release,而驅動這支腳本的
# `<套件名>.devtools.BackupHarnessReceiver`(見上面的 $RECEIVER)住在 `src/debug/` ——
# 它**不在** release 裡,而且**不可以**為了讓這支好跑而搬進 `src/main`:
# 那是一個 exported 的廣播入口,一條 `am broadcast` 就能叫 app 把整份詞庫
# 匯出到指定路徑。留在 release 裡等於留一個後門。
#
# 那這支腳本的綠燈憑什麼延伸到使用者手上?憑兩件被**釘住**的事實:
#   1. 它驗的產品程式碼(BackupController / BackupFormat / RimeCore.flushEngine)
#      住在 src/main,兩個變體共用,而 release 沒有開 R8(isMinifyEnabled=false),
#      所以那段程式碼不會被改寫;
#   2. `scripts/release_check.sh` 第 3c 關**每一輪都問一次**:
#        · release 的 manifest 與 dex 裡沒有 BackupHarnessReceiver;
#        · release 的 dex 裡有 BackupController(= 這支驗的東西真的有出貨);
#        · debug 兩者皆有(正控:少了它,偵測方法壞掉時會安靜地全綠)。
#
# 換句話說:harness 只在 debug、被驗的程式碼在兩邊都在 —— 這兩句都是斷言,
# 不是慣例。少了第 3c 關,這一段就只是一個好聽的說法。
#
# ⚠ **沒有被這支涵蓋的**:SAF 的檔案選擇器(`ACTION_CREATE_DOCUMENT` /
#   `ACTION_OPEN_DOCUMENT` 的對話框)。harness 直接給 `BackupController`
#   一個 `file:` Uri;`ContentResolver` 對 file scheme 走的是同一組
#   `openOutputStream`/`openInputStream`,但**選檔器的互動沒有被驗到**。
#
# 用法:
#   ./scripts/verify_backup_roundtrip.sh [--apk <path>] [--no-install]
#
# 環境變數:
#   RIME_SERIAL     裝置序號(沒指定而且不只一台在線就中止 —— port 不是身分)
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# 產品識別碼的唯一來源。
# shellcheck source=lib/product.sh
. "$HERE/lib/product.sh"
# ⛔ 裝置選擇的唯一入口。沒有預設 port —— 這台機器上長期有三到四台在跑,
#   而 `adb devices` 以 port 升冪列出,「預設 5554」與「抓第一台」都會
#   落在同一台**別人的**機器上,然後 pm clear 它。
# shellcheck source=lib/device.sh
. "$HERE/lib/device.sh"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ADB="$SDK/platform-tools/adb"
SERIAL=""
PKG="$RS_ANDROID_APP_ID"
RECEIVER="$PKG/$PKG.devtools.BackupHarnessReceiver"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
OUT_DIR="$ROOT/build/verify-backup"
INSTALL=1

while [ $# -gt 0 ]; do
  case "$1" in
    --apk)        APK="$2"; shift 2 ;;
    --no-install) INSTALL=0; shift ;;
    --out)        OUT_DIR="$2"; shift 2 ;;
    --serial) SERIAL="$2"; shift 2 ;;
    -h|--help) sed -n '2,/^set -[eu]/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知參數:$1" >&2; exit 2 ;;
  esac
done

mkdir -p "$OUT_DIR"
# ⛔ `--serial` 也要算「指名」。閘從前只看環境變數,於是這一行帶 `--serial`
#   就必死(RC=2,訊息說「是自動選來的」而那台正是命令列指名的)。
#   `rs_select_device` 把來源(flag / env / auto)一起記下來給閘看。
rs_select_device "$ADB" "$SERIAL" || exit 2
SERIAL="$RS_SERIAL"
adbs() { "$ADB" -s "$SERIAL" "$@"; }

FAILED=0
step() { echo; echo "=== $* ==="; }
pass() { echo "  [PASS] $*"; }
info() { echo "  [INFO] $*"; }
fail() {
  echo "  [FAIL] $*" >&2
  adbs logcat -d > "$OUT_DIR/logcat-fail.txt" 2>/dev/null || true
  echo "  [INFO] logcat 現場:$OUT_DIR/logcat-fail.txt" >&2
  exit 1
}

# ── harness ───────────────────────────────────────────────────────────────
# 一次 broadcast + 等它印出 `done op=…`。回傳所有 BACKUPRT 行。
harness() {
  local op="$1"; shift
  adbs logcat -c >/dev/null 2>&1 || true
  adbs shell am broadcast -n "$RECEIVER" --es op "$op" "$@" >/dev/null 2>&1
  local i out
  for i in $(seq 1 120); do
    out="$(adbs logcat -d -s BACKUPRT 2>/dev/null || true)"
    case "$out" in *"done op=$op"*) break;; esac
    sleep 2
  done
  printf '%s\n' "$out"
}

# 候選第一名。$1=方案 $2=按鍵
top1() {
  harness probe --es schema "$1" --es keys "$2" --ei top 5 \
    | sed -n 's/.*cand\[0\]=//p' | head -1
}

SCHEMA="luna_pinyin_tw"

# 要教的三個詞:按鍵、挑第幾個候選(0 起算)。
# 刻意都不挑第一個 —— 挑第一個的話「學到了」與「本來就是第一個」分不出來。
LEARN_KEYS=(kaifang guojia nihao)
LEARN_PICK=(1 1 2)

# ═════════════════════════════════════════════════════════════════════════
step "0. 準備"
[ -x "$ADB" ] || fail "找不到 adb:$ADB"
adbs get-state >/dev/null 2>&1 || fail "$SERIAL 不在線上(先跑 scripts/emu.sh start)"
# ⚠ **從乾淨的資料開始。** `adb install -r` 會保留資料,於是「還沒教之前」
# 的基準會是**上一次跑這支腳本學到的詞**,而第 6 步的反向控制組就會誤判成
# 「沒清乾淨」。實際踩到過一次:基準拿到的是上一輪的 開房/郭嘉/逆號。
#
# ⚠ 閘搬到**安裝之前**:`install -r` 蓋掉的也是這一台上別條線的 app,
#   而 `verify_device_hygiene.sh` 的規則 C 現在是**逐呼叫點**的 ——
#   閘寫在下面那個 `fi` 之後,護不到 `fi` 裡的 `adbs install`。
rs_assert_destructive_ok "$ADB" "$SERIAL" "install -r $PKG、pm clear $PKG" || exit 2

if [ "$INSTALL" -eq 1 ]; then
  [ -f "$APK" ] || fail "找不到 APK:$APK"

  # ⚠ 先問這份 APK 裡到底有沒有 harness,再裝。
  #   沒有的話(最可能的原因:有人把 release 那一份傳進來了),原本的下場是
  #   `am broadcast` 送出去沒人接 → harness() 空轉 120 次 → 240 秒之後以
  #   「librime 一直沒有 READY」失敗。那句話指著引擎,而真正的原因是
  #   「這個變體本來就沒有那個 receiver」—— 完全不同的兩件事。
  #   release 沒有 harness 是**正確的**(見檔頭),所以這裡要說得出這件事。
  find_aapt2() {
    local sdk d
    for sdk in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "$HOME/Android/Sdk"; do
      [ -n "$sdk" ] && [ -d "$sdk/build-tools" ] || continue
      for d in $(ls -1 "$sdk/build-tools" 2>/dev/null | sort -Vr); do
        [ -x "$sdk/build-tools/$d/aapt2" ] && { printf '%s' "$sdk/build-tools/$d/aapt2"; return 0; }
      done
    done
    return 1
  }
  AAPT2="$(find_aapt2 || true)"
  if [ -n "$AAPT2" ]; then
    # grep -c(不是 -q):-q 一命中就結束,上游 aapt2 拿到 SIGPIPE,
    # 在 pipefail 之下整條管線被判失敗。本專案在四支腳本裡撞過同一件事。
    N_RECV="$("$AAPT2" dump xmltree --file AndroidManifest.xml "$APK" 2>/dev/null \
                | grep -c "$PKG.devtools.BackupHarnessReceiver" || true)"
    if [ "${N_RECV:-0}" -eq 0 ]; then
      fail "$(basename "$APK") 的 manifest 裡沒有 $RECEIVER。
這多半是 release 建置 —— 而 release **本來就不該**有 harness(它是一個
exported 的廣播入口,見本檔檔頭)。這支腳本要的是 debug 那一份:
  cd android && ./gradlew assembleDebug
  ./scripts/verify_backup_roundtrip.sh --apk android/app/build/outputs/apk/debug/app-debug.apk
「release 沒有 harness、debug 有」這件事本身由 release_check.sh 第 3c 關驗。"
    fi
    pass "APK 裡有 harness receiver（manifest 命中 $N_RECV 處）"
  else
    # 找不到 aapt2 時**不要**默默跳過。跳過的下場就是上面那個 240 秒的
    # 假失敗,而且沒有任何一行字提到過真正的原因。
    info "⚠ 找不到 aapt2，沒能事先確認這份 APK 有 harness。它若是 release，下面會以「librime 一直沒有 READY」失敗，而真正的原因是這個。"
  fi

  adbs install -r "$APK" >/dev/null 2>&1 || fail "安裝失敗(簽章不符?先 adb uninstall $PKG)"
  pass "已安裝 $(basename "$APK")"
fi
adbs shell pm clear "$PKG" >/dev/null 2>&1 || true

# 開著設定畫面把行程釘住 —— 學習用的 session 要跨好幾次 broadcast 活著,
# 而一個沒有任何元件在前景的行程隨時可能被系統收掉。
adbs shell am start -n "$PKG/.MainActivity" >/dev/null 2>&1 || true
sleep 2

wait_ready() {
  local i out
  for i in $(seq 1 40); do
    out="$(harness state)"
    case "$out" in *"ready=true"*) return 0;; esac
    sleep 3
  done
  return 1
}
wait_ready || fail "librime 一直沒有 READY"
pass "librime 就緒"

# ═════════════════════════════════════════════════════════════════════════
step "1. 記下「還沒教之前」的候選第一名"
BASELINE=()
for i in "${!LEARN_KEYS[@]}"; do
  t="$(top1 "$SCHEMA" "${LEARN_KEYS[$i]}")"
  [ -n "$t" ] || fail "${LEARN_KEYS[$i]} 打不出任何候選 —— 引擎沒在工作,後面全部沒有意義"
  BASELINE+=("$t")
  info "${LEARN_KEYS[$i]} → $t"
done
pass "基準已記下"

# ═════════════════════════════════════════════════════════════════════════
step "2. 教它三個詞(挑非第一名的候選上屏)"
LEARNED=()
for i in "${!LEARN_KEYS[@]}"; do
  out="$(harness learn --es schema "$SCHEMA" --es keys "${LEARN_KEYS[$i]}" --ei pick "${LEARN_PICK[$i]}")"
  w="$(printf '%s' "$out" | sed -n 's/.*committed=\([^ ]*\).*/\1/p' | head -1)"
  [ -n "$w" ] || fail "${LEARN_KEYS[$i]} 沒有上屏任何字:$out"
  [ "$w" != "${BASELINE[$i]}" ] \
    || fail "挑到的還是原本的第一名($w),這樣分不出「學到了」與「本來就是」"
  LEARNED+=("$w")
  info "${LEARN_KEYS[$i]} → $w(原本是 ${BASELINE[$i]})"
done
pass "已上屏三個詞。⚠ 學習用的 session 還活著,交易還沒落地"

# ═════════════════════════════════════════════════════════════════════════
# ⚠ **這一步必須緊接在「教」後面,中間不可以再打任何字。**
#
# 實測(2026-08-09)踩到過:原本這裡先跑一次「確認它學到了」的探針,結果
# 那次探針本身就把待落地的交易寫下去了 —— librime 的 `UserDictionary::Query`
# 會先 `FinishSession()`,而 `db_pool_` 讓同一本詞典在行程內只有一個 `Db`,
# 所以**任何**一次查詢都等於替所有人提交。於是把 flushEngine() 整支停掉,
# 往返仍然全綠:一個永遠不會紅的驗證。
#
# 「確認它學到了」改到匯出**之後**才做(第 5 步)。
step "4. 匯出(此刻交易還掛在記憶體裡)"
# 相對路徑:harness 會把它落在 getExternalFilesDir() 底下,並印出絕對路徑。
# 不在這裡寫死 /sdcard/Android/data/<pkg>/files —— 那個目錄要等 app 自己
# 呼叫過 getExternalFilesDir() 才存在。
OUT_EXPORT="$(harness export --es path roundtrip.zip)"
REMOTE_ZIP="$(printf '%s' "$OUT_EXPORT" | sed -n 's/.*I BACKUPRT: file=//p' | head -1)"
[ -n "$REMOTE_ZIP" ] || fail "harness 沒有回報寫到哪裡:$OUT_EXPORT"
printf '%s\n' "$OUT_EXPORT" > "$OUT_DIR/export.log"
case "$OUT_EXPORT" in *"result=ok"*) ;; *) fail "匯出失敗:$OUT_EXPORT";; esac
BYTES="$(printf '%s' "$OUT_EXPORT" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p' | head -1)"
[ "${BYTES:-0}" -gt 1000 ] || fail "匯出的檔案只有 ${BYTES:-0} bytes"
pass "匯出成功,$BYTES bytes"

# manifest 的 flushed 就是「這份詞庫證明得了自己是完整的嗎」。
case "$OUT_EXPORT" in
  *'"flushed": true'*|*'"flushed":true'*) pass "manifest 的 flushed=true(匯出端證明得了它 flush 過)" ;;
  *) info "⚠ manifest 沒有 flushed=true —— 詞庫可能少一截,見 docs/backup-format.md §3.1" ;;
esac

adbs pull "$REMOTE_ZIP" "$OUT_DIR/roundtrip.zip" >/dev/null 2>&1 \
  || fail "把備份拉回主機失敗"
pass "已拉回 $OUT_DIR/roundtrip.zip"

# ═════════════════════════════════════════════════════════════════════════
step "5. 確認它真的學到了(候選第一名換人)"
# 排在匯出之後,理由見第 4 步的註解:這一步本身會讓交易落地。
for i in "${!LEARN_KEYS[@]}"; do
  t="$(top1 "$SCHEMA" "${LEARN_KEYS[$i]}")"
  [ "$t" = "${LEARNED[$i]}" ] \
    || fail "${LEARN_KEYS[$i]} 的第一名是 $t,預期 ${LEARNED[$i]} —— 引擎根本沒學到,後面驗的是空氣"
done
pass "三個詞都變成第一名了"

# ═════════════════════════════════════════════════════════════════════════
step "6. 清掉 app 資料(模擬換手機/重裝)"
adbs shell pm clear "$PKG" >/dev/null 2>&1 || fail "pm clear 失敗"
adbs shell am start -n "$PKG/.MainActivity" >/dev/null 2>&1 || true
sleep 3
wait_ready || fail "清掉之後 librime 一直沒有 READY"
pass "資料已清空,librime 重新部署完成"

# ═════════════════════════════════════════════════════════════════════════
step "7. 反向控制組:那幾個詞現在**必須**不見了"
# 少了這一步,第 9 步的「詞還在」可能只是因為根本沒清乾淨 —— 一個永遠會綠的測試。
for i in "${!LEARN_KEYS[@]}"; do
  t="$(top1 "$SCHEMA" "${LEARN_KEYS[$i]}")"
  [ "$t" = "${BASELINE[$i]}" ] \
    || fail "${LEARN_KEYS[$i]} 清空後的第一名是 $t,預期回到 ${BASELINE[$i]} —— 沒清乾淨,這個驗證不成立"
done
pass "候選回到原本的順序 —— 資料確實清掉了"

# ═════════════════════════════════════════════════════════════════════════
step "8. 匯入"
adbs push "$OUT_DIR/roundtrip.zip" "$REMOTE_ZIP" >/dev/null 2>&1 || fail "把備份推回裝置失敗"
OUT_IMPORT="$(harness import --es path roundtrip.zip)"
printf '%s\n' "$OUT_IMPORT" > "$OUT_DIR/import.log"
case "$OUT_IMPORT" in *"result=ok"*) ;; *) fail "匯入失敗:$OUT_IMPORT";; esac
pass "匯入回報成功"
printf '%s\n' "$OUT_IMPORT" | sed -n 's/.*I BACKUPRT: note=/  [NOTE] /p'

# ═════════════════════════════════════════════════════════════════════════
step "9. 那幾個詞真的還在候選裡嗎"
for i in "${!LEARN_KEYS[@]}"; do
  t="$(top1 "$SCHEMA" "${LEARN_KEYS[$i]}")"
  if [ "$t" = "${LEARNED[$i]}" ]; then
    pass "${LEARN_KEYS[$i]} → $t(學到的詞回來了)"
  else
    FAILED=1
    echo "  [FAIL] ${LEARN_KEYS[$i]} 的第一名是 $t,預期 ${LEARNED[$i]} —— 使用者的詞沒有跟著備份回來" >&2
  fi
done

echo
if [ "$FAILED" -eq 0 ]; then
  echo "============================================"
  echo " 往返完整:匯出 → 清空 → 匯入,學到的詞都還在"
  echo " artifact: $OUT_DIR"
  echo "============================================"
else
  adbs logcat -d > "$OUT_DIR/logcat-fail.txt" 2>/dev/null || true
  echo "驗證失敗。artifact 在:$OUT_DIR" >&2
  exit 1
fi
