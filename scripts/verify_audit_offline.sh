#!/usr/bin/env bash
#
# verify_audit_offline.sh — 對 audit_offline.sh 做反向測試
#
# ═══════════════════════════════════════════════════════════════════════════
#  為什麼需要這一支
# ═══════════════════════════════════════════════════════════════════════════
#
# audit_offline.sh 是「離線為預設」這句話的守門人，而它整支都是 grep。
# 一支全綠的 grep 腳本有兩種可能：**真的沒有違規**，或**它其實什麼都沒在看**。
# 兩者印出來的字一模一樣。
#
# 這個專案已經被同一件事咬過三次：
#   · 發布關卡的升級測試步驟順序寫反，被判「略過」而報一片全綠
#   · LayoutEscapeTest 把佈局清單寫死四份，12 份裡有 8 份從沒被檢查
#   · 詞庫檢查用 grep -oP，在 BSD 上永遠印「都齊全」而完全沒在檢查
#
# 所以這裡把違規**真的植入一次**：複製一份最小的樹、動一個檔案、跑守門腳本，
# 要求它紅、而且紅在對的那一項。任何一條植入之後仍然全綠，就代表守門腳本
# 對那一類違規是瞎的 —— 本腳本會因此判失敗。
#
# 第 2 條是**正向對照**：只出現在註解裡的 java.net 不可以被判成違規。
# 一支永遠紅的守門腳本會被當成雜訊直接略過，那比沒有還糟。
#
# 用法：
#   scripts/verify_audit_offline.sh
#   scripts/verify_audit_offline.sh --keep     # 保留每個案例的樹（除錯用）
#
set -uo pipefail

# ⛔ **唯讀出口。** `scripts/verify_script_readonly.sh` 把每一支腳本的
#   `--help` 跑一遍,而且用 shim 檢查它有沒有碰外部工具。這一支從前沒有
#   `--help`,於是 `--help` 被當成一般啟動 —— 一路跑下去(建置／git／推檔)。
#   說明不得有任何副作用。
case "${1:-}" in
  -h|--help)
    sed -n '2,/^set -[eu]/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 產品識別碼的唯一來源,見 scripts/lib/product.env。植入的違規要用**真的**
# 套件路徑寫進那棵樹,不能自己編一個 —— 編錯了 audit_offline.sh 只會說
# 「找不到 NetworkGate.kt」,那是植入失敗,不是它抓到了違規。
# shellcheck source=lib/product.sh
. "$ROOT/scripts/lib/product.sh"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }
log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/$RS_PRODUCT_ID_ROOT-auditrev.XXXXXX")"
cleanup() { [ "$KEEP" -eq 1 ] || rm -rf "$TMP"; }
trap cleanup EXIT
[ "$KEEP" -eq 1 ] && echo "暫存目錄：$TMP"

# ── 準備一份「已套用 patch」的 librime-lua 原始碼 ────────────────────────
# ⚠ 不能直接抄 third_party/librime-lua 的工作區。那份的狀態取決於有沒有人
#   跑過 scripts/build_native.sh：在開發機上通常是套過 patch 的，在 CI 上
#   通常**沒有**（scripts/verify_lua_sandbox.sh 只把 patch 套到自己的暫存副本）。
#   抄到沒套 patch 的那份，基準會直接紅在「沙盒不完整」，而真正的原因是
#   「這棵樹本來就沒套」——一個看起來像安全問題的環境問題。
#   所以這裡自己從 HEAD 匯出乾淨的原始碼再套一次 patch，與環境無關。
LUA_SRC_DIR="$ROOT/third_party/librime-lua"
PATCHED_SRC=""
if [ -d "$LUA_SRC_DIR/.git" ]; then
  mkdir -p "$TMP/patched"
  if git -C "$LUA_SRC_DIR" archive HEAD src > "$TMP/lua-src.tar" 2>/dev/null &&
     tar -xf "$TMP/lua-src.tar" -C "$TMP/patched" 2>/dev/null &&
     ( cd "$TMP/patched" && git apply "$ROOT/patches/librime-lua@sandbox.patch" ) 2>/dev/null; then
    PATCHED_SRC="$TMP/patched"
  else
    echo "  [SKIP] patch 套不上乾淨的 librime-lua —— 沙盒那幾條植入這一輪不會跑" >&2
  fi
fi

# ── 一棵剛好夠 audit_offline.sh 讀完的樹 ─────────────────────────────────
# 只複製它真的會讀的東西。third_party/prebuilt 用 symlink（18MB 的 .a
# 複製 15 次太慢），要動它的那一條案例會自己換成真目錄。
make_tree() {   # make_tree <名字> -> 印出樹的路徑
  local name="$1"
  local T="$TMP/$name"
  mkdir -p "$T/scripts" "$T/android/app" "$T/android/gradle" \
           "$T/third_party/librime-lua/src/lib" "$T/core"
  cp "$ROOT/scripts/audit_offline.sh" "$T/scripts/"
  # audit_offline.sh 讀 scripts/lib/product.env 取識別碼與稽核樣式,少了它
  # 基準會紅在「設定載入不了」,而那個紅看起來會像被驗的東西壞了。
  cp -a "$ROOT/scripts/lib" "$T/scripts/lib"
  cp -a "$ROOT/android/app/src" "$T/android/app/src"
  cp "$ROOT/android/gradle/libs.versions.toml" "$T/android/gradle/" 2>/dev/null
  cp "$ROOT/android/app/build.gradle.kts" "$T/android/app/" 2>/dev/null
  cp "$ROOT/android/build.gradle.kts" "$T/android/" 2>/dev/null
  cp "$ROOT/android/settings.gradle.kts" "$T/android/" 2>/dev/null
  cp -a "$ROOT/core/src" "$T/core/src" 2>/dev/null
  cp -a "$ROOT/core/include" "$T/core/include" 2>/dev/null
  cp -a "$ROOT/patches" "$T/patches"
  if [ -n "$PATCHED_SRC" ]; then
    for f in src/lib/lua.cc src/modules.cc src/lua_gears.h; do
      cp "$PATCHED_SRC/$f" "$T/third_party/librime-lua/$f"
    done
  else
    # 沒有原始碼時整個目錄不要存在，audit_offline.sh 才會走它的 [SKIP] 分支
    # （而不是對著半棵樹得出「沙盒不見了」這種錯誤結論）。
    rm -rf "$T/third_party/librime-lua"
  fi
  ln -s "$ROOT/third_party/prebuilt" "$T/third_party/prebuilt"
  printf '%s' "$T"
}

run_audit() {   # run_audit <樹> -> 輸出存進 AUDIT_OUT，回傳 exit code
  AUDIT_OUT="$( "$1/scripts/audit_offline.sh" 2>&1 )"
  return $?
}

# ── 基準：沒有動任何東西的樹必須全綠 ─────────────────────────────────────
# 基準紅的話，底下每一條「它紅了」都可能只是因為基準本來就紅，
# 整份反向測試會變成沒有意義的全綠。
log "基準：未經修改的樹"
BASE="$(make_tree base)"
if run_audit "$BASE"; then
  ok "基準全綠（反向測試才有意義）"
else
  bad "基準就已經是紅的 —— 底下的結果全部不可採信"
  printf '%s\n' "$AUDIT_OUT" | grep -E '^\s*\[FAIL\]' | sed 's/^/         /' >&2
  echo; echo "先把基準修綠再跑這支。" >&2
  exit 1
fi

# ── 植入一條違規，要求它紅、而且紅在對的那一項 ───────────────────────────
CASE_N=0
expect_red() {   # expect_red <名稱> <期望出現在 FAIL 訊息裡的字串> <mutator 函式>
  CASE_N=$((CASE_N + 1))
  local name="$1" want="$2" fn="$3"
  local T; T="$(make_tree "case$CASE_N")"
  "$fn" "$T" || { bad "案例「$name」：植入失敗（mutator 自己錯了）"; return; }
  if run_audit "$T"; then
    bad "案例「$name」：植入違規之後守門腳本仍然全綠 —— 它對這一類是瞎的"
    return
  fi
  local fails
  fails="$(printf '%s\n' "$AUDIT_OUT" | grep -E '\[FAIL\]' || true)"
  case "$fails" in
    *"$want"*) ok "案例「$name」→ 紅在對的地方" ;;
    *) bad "案例「$name」：紅了，但不是因為這件事（找不到「$want」）"
       printf '%s\n' "$fails" | sed 's/^/         /' >&2 ;;
  esac
}

expect_green() {  # expect_green <名稱> <mutator 函式>
  CASE_N=$((CASE_N + 1))
  local name="$1" fn="$2"
  local T; T="$(make_tree "case$CASE_N")"
  "$fn" "$T" || { bad "案例「$name」：植入失敗"; return; }
  if run_audit "$T"; then
    ok "案例「$name」→ 仍然全綠（沒有誤報）"
  else
    bad "案例「$name」：誤報。一支動不動就紅的守門腳本會被當成雜訊略過"
    printf '%s\n' "$AUDIT_OUT" | grep -E '\[FAIL\]' | sed 's/^/         /' >&2
  fi
}

# 小工具：對檔案做一次「必須命中正好一次」的字串取代
sub_once() {   # sub_once <檔案> <舊> <新>
  python3 - "$1" "$2" "$3" <<'PY'
import io, sys
f, old, new = sys.argv[1:4]
s = io.open(f, encoding="utf-8").read()
if s.count(old) != 1:
    raise SystemExit("在 %s 裡命中 %d 次（需要正好 1 次）：%r" % (f, s.count(old), old))
io.open(f, "w", encoding="utf-8").write(s.replace(old, new))
PY
}

KT="android/app/src/main/java/$RS_ANDROID_PKG_PATH"
MANIFEST="android/app/src/main/AndroidManifest.xml"
NETSEC="android/app/src/main/res/xml/network_security_config.xml"

m_second_exit() {
  cat > "$1/$KT/store/Sneaky.kt" <<EOF
package $RS_ANDROID_APP_ID.store
import java.net.HttpURLConnection
import java.net.URL
object Sneaky {
    fun ping(u: String) = (URL(u).openConnection() as HttpURLConnection).responseCode
}
EOF
}
expect_red "第二個連網出口（store/Sneaky.kt）" "NetworkGate.kt 以外" m_second_exit

m_comment_only() {
  cat > "$1/$KT/store/NoteOnly.kt" <<EOF
package $RS_ANDROID_APP_ID.store
/**
 * 這個檔案沒有任何連網程式碼。註解裡提到 java.net.HttpURLConnection、
 * URL( 與 OkHttp 只是為了說明「我們刻意不用它們」。
 */
// val x = URL("https://example") 這一行是註解，不是程式碼
object NoteOnly { const val WHY = "見上面的說明" }
EOF
}
expect_green "只出現在註解裡的連網 token" m_comment_only

m_okhttp() {
  printf '\nokhttp = { group = "com.squareup.okhttp3", name = "okhttp", version = "4.12.0" }\n' \
    >> "$1/android/gradle/libs.versions.toml"
}
expect_red "相依裡加了 okhttp" "相依裡出現了會自己連網" m_okhttp

m_crashlytics() {
  printf '\nfirebase-crashlytics = { group = "com.google.firebase", name = "firebase-crashlytics" }\n' \
    >> "$1/android/gradle/libs.versions.toml"
}
expect_red "相依裡加了 crashlytics" "相依裡出現了會自己連網" m_crashlytics

m_webview() {
  cat > "$1/$KT/home/HelpPage.kt" <<EOF
package $RS_ANDROID_APP_ID.home
import android.webkit.WebView
import android.content.Context
fun helpView(c: Context) = WebView(c).apply { loadUrl("https://example/help") }
EOF
}
expect_red "為了說明頁塞了一個 WebView" "出現 WebView" m_webview

m_native_socket() {
  cat > "$1/core/src/telemetry.cc" <<'EOF'
#include <sys/socket.h>
extern "C" int rs_report_stats(const char* host) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  return fd;
}
EOF
}
expect_red "原生層偷開 socket" "原生層出現網路呼叫" m_native_socket

m_allowbackup() {
  sub_once "$1/$MANIFEST" 'android:allowBackup="false"' 'android:allowBackup="true"'
}
expect_red "allowBackup 改回 true" "allowBackup" m_allowbackup

m_cleartext() {
  sub_once "$1/$NETSEC" '<base-config' '<base-config cleartextTrafficPermitted="true"'
}
expect_red "network_security_config 開了明文" "明文例外" m_cleartext

# ⚠ 這一條植入的是**現在的**產品名。改名時若只改顯示名而忘了這裡,守門的樣式
#   會繼續擋舊名、放行新名 —— 稽核照樣全綠,而 UA 已經在對外自報家門了。
#   舊名另立一條:我們仍然是 RIME 前端,UA 裡出現 rime/quad 一樣會暴露身分。
m_ua() {
  sub_once "$1/$KT/net/NetworkGate.kt" \
    'private const val USER_AGENT = "Mozilla/5.0"' \
    "private const val USER_AGENT = \"$RS_PRODUCT_ID_ROOT-android/1.0\""
}
expect_red "User-Agent 自報家門(現在的產品名)" "自報家門" m_ua

m_ua_legacy() {
  sub_once "$1/$KT/net/NetworkGate.kt" \
    'private const val USER_AGENT = "Mozilla/5.0"' \
    "private const val USER_AGENT = \"$RS_LEGACY_ID_ROOT-android/1.0\""
}
expect_red "User-Agent 自報家門(舊產品名)" "自報家門" m_ua_legacy

m_perm() {
  # manifest 裡有三個 <uses-permission，所以不能用 sub_once（它要求正好一次）。
  # 插在第一個之前。
  python3 - "$1/$MANIFEST" <<'PYPERM' || return 1
import io, sys
f = sys.argv[1]
s = io.open(f, encoding='utf-8').read()
i = s.index('<uses-permission')
s = s[:i] + '<uses-permission android:name="android.permission.READ_CONTACTS" />' + chr(10) + '    ' + s[i:]
io.open(f, 'w', encoding='utf-8').write(s)
PYPERM
}
expect_red "多了一個 READ_CONTACTS 權限" "未列入白名單的權限" m_perm

m_no_patch() { rm -f "$1/patches/librime-lua@sandbox.patch"; }
expect_red "沙盒 patch 檔被刪掉" "沙盒的來源不見了" m_no_patch

# 底下四條都要動 librime-lua 的原始碼。取不到就**明說跳過**，
# 不要讓「沒跑」長得像「跑過而且過了」。
skip_lua_cases() {
  echo "  [SKIP] 取不到 librime-lua 原始碼 —— 沙盒的 4 條植入這一輪完全沒有跑"
  echo "         先跑：scripts/verify_lua_sandbox.sh（它會把原始碼抓下來）"
}

if [ -z "$PATCHED_SRC" ]; then skip_lua_cases; fi

m_sandbox_commented() {
  # 這一條就是當年真的抓到的漏洞：被註解掉的 `-- package.loadlib = nil`
  # 曾經被當成「還在」而放行。
  sub_once "$1/third_party/librime-lua/src/lib/lua.cc" \
    'package.loadlib = nil' '-- package.loadlib = nil'
}
[ -n "$PATCHED_SRC" ] && expect_red "沙盒被註解掉（不是刪掉）" "第一層(lua.cc)的允許清單" m_sandbox_commented

m_io_allowlist_gone() {
  sub_once "$1/third_party/librime-lua/src/lib/lua.cc" \
    'for k in pairs(io) do if not keep_io[k] then io[k] = nil end end' \
    '-- 有人把 io 的允許清單拿掉了'
}
[ -n "$PATCHED_SRC" ] && expect_red "第一層少了 io 允許清單" "第一層(lua.cc)的允許清單" m_io_allowlist_gone

m_order_swapped() {
  # 把 rime.lua 的執行搬到第二層沙盒之前 —— 順序錯了等於沒做，
  # 而且原始碼看起來每一段都還在。
  sub_once "$1/third_party/librime-lua/src/modules.cc" \
    '  types_init(L);' \
    '  types_init(L);
  luaL_dofile(L, user_file.c_str());   // 有人把它搬到沙盒之前了'
}
[ -n "$PATCHED_SRC" ] && expect_red "第二層被搬到 rime.lua 之後" "第二層沒有裝在 rime.lua 之前" m_order_swapped

m_hook_removed() {
  sub_once "$1/third_party/librime-lua/src/lua_gears.h" \
    "    ::$RS_LUA_SANDBOX_INIT();" \
    "    // ::$RS_LUA_SANDBOX_INIT();"
}
[ -n "$PATCHED_SRC" ] && expect_red "延後的掛勾被註解掉" "延後的掛勾" m_hook_removed

m_stale_lib() {
  # symlink 換成真目錄，塞一顆「沒有沙盒」的 librime.a：
  # 等同於「改了 patch 但沒有重跑 build_native.sh」。
  rm -f "$1/third_party/prebuilt"
  mkdir -p "$1/third_party/prebuilt/arm64-v8a/lib"
  printf 'this is not a real librime.a, and it has no sandbox in it\n' \
    > "$1/third_party/prebuilt/arm64-v8a/lib/librime.a"
}
expect_red "出貨的 librime.a 是套 patch 之前建的" "沒有**沙盒" m_stale_lib

# ── 第 10 項另外驗 ───────────────────────────────────────────────────────
# 第 10 項要一個已建置的 APK，走不了「複製一棵樹」那條路（24MB × 16 份）。
# 改成直接對 scripts/dex_network_refs.py 下手：餵它一個假的引用者，它必須紅。
# 這裡植入的是**輸入**而不是檢查器本身 —— 檢查器被改壞的情形由基準那一條擋。
log "第 10 項：產物層的引用者清單"
APK_REV="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
DEXREFS_REV="$ROOT/scripts/dex_network_refs.py"
if [ ! -f "$APK_REV" ]; then
  echo "  [SKIP] 沒有已建置的 APK —— **第 10 項這一輪完全沒有被反向測試**"
  echo "         先跑：cd android && ./gradlew :app:assembleDebug"
elif [ ! -f "$DEXREFS_REV" ]; then
  bad "找不到 scripts/dex_network_refs.py"
else
  if python3 "$DEXREFS_REV" "$APK_REV" >/dev/null 2>&1; then
    ok "基準：APK 的網路型別引用者與釘死的清單相符"
  else
    bad "基準：APK 的引用者清單本來就對不上 —— 底下那一條沒有意義"
  fi
  if python3 "$DEXREFS_REV" "$APK_REV" \
       --extra-referrer 'Lcom/evil/Tracker;:Ljava/net/HttpURLConnection;' >/dev/null 2>&1; then
    bad "植入「第二個碰得到 HttpURLConnection 的類別」之後仍然全綠"
  else
    ok "植入「第二個碰得到 HttpURLConnection 的類別」→ 紅了"
  fi
  if python3 "$DEXREFS_REV" "$APK_REV" \
       --extra-referrer 'Lcom/evil/Beacon;:Ljava/net/Socket;' >/dev/null 2>&1; then
    bad "植入「多一個碰得到 Socket 的類別」之後仍然全綠"
  else
    ok "植入「多一個碰得到 Socket 的類別」→ 紅了"
  fi
fi

echo
echo "============================================"
echo " 守門腳本反向測試：通過 $PASS 項，失敗 $FAIL 項（$CASE_N 條植入）"
echo "============================================"
[ "$FAIL" -gt 0 ] && { echo "audit_offline.sh 對上面那幾類違規是瞎的。" >&2; exit 1; }
echo "守門腳本該紅的時候會紅，該綠的時候會綠。"
