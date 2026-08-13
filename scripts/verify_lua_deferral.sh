#!/usr/bin/env bash
#
# verify_lua_deferral.sh — 在**真的引擎**上驗兩件事
#
# ═══════════════════════════════════════════════════════════════════════════
#  這支和 verify_lua_sandbox.sh 的分工
# ═══════════════════════════════════════════════════════════════════════════
#
#   verify_lua_sandbox.sh   純 Lua 層：把兩段沙盒字串裝進真的 Lua 5.4.8，
#                           逐條驗語義。快、不需要裝置、含四個變異測試。
#   本檔                    整合層：用**實際出貨的 librime.a**（含 librime-lua
#                           與 patch）在模擬器上跑，驗兩件純 Lua 層驗不到的事：
#                             1. rime.lua 到底什麼時候被執行
#                             2. 沙盒在真的 lua_State 上真的裝起來了
#
# 純 Lua 層綠而這裡紅 = patch 的 C++ 那半邊有問題（順序、掛勾、fail-closed）。
#
# ═══════════════════════════════════════════════════════════════════════════
#  三個案例
# ═══════════════════════════════════════════════════════════════════════════
#
#   A  只部署（rime_console 的 "-" 模式）           → rime.lua **不可以**跑
#   B  選一個不含 lua 元件的方案（luna_pinyin_tw）  → rime.lua **不可以**跑
#      而且「你好」照樣打得出來（延後不能把引擎弄壞）
#   C  選一個含 lua_filter 的方案（luaprobe）        → rime.lua **必須**跑，
#      而且它在裡面看到的是被沙盒過的環境
#
# A 與 B 是這次改動的重點：上游在 RimeSetup 期間就跑 rime.lua，
# 「下載一個方案」等於「執行它的程式碼」。C 是反向測試 —— 若三個案例都
# 看不到 rime.lua 跑過，那可能只是探針壞了，不是延後成功。
#
# 用法：
#   scripts/verify_lua_deferral.sh                 # 用第一台模擬器
#   RIME_SERIAL=emulator-5554 scripts/verify_lua_deferral.sh
#   scripts/verify_lua_deferral.sh --skip-push     # 資料已在裝置上
#
set -uo pipefail

# ⛔ **唯讀出口。** `scripts/verify_script_readonly.sh` 會把每一支腳本的
#   `--help` 跑一遍 —— 這一支從前沒有,於是 `--help` 會被當成一般啟動,
#   一路跑下去(編譯／連網／推檔)。說明不得有任何副作用。
case "${1:-}" in
  -h|--help)
    sed -n '2,/^set -[eu]/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 產品識別碼、以及 patches/ 裡那組沙盒符號名的唯一來源,見 scripts/lib/product.env。
# shellcheck source=lib/product.sh
. "$ROOT/scripts/lib/product.sh"
SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ADB="$SDK/platform-tools/adb"
NDK="$SDK/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin"
ABI=x86_64
# 刻意不是 /data/local/tmp/rime —— 那是 run_console_test.sh 的目錄，
# 這台機器上常有好幾條線同時在跑，共用目錄等於互相踩。
DEV_DIR=/data/local/tmp/rime-sandbox-probe
FIX="$ROOT/scripts/lua_sandbox/device"
DATA_DIR="${RIME_DATA_DIR:-$ROOT/core/data}"

SKIP_PUSH=0
[ "${1:-}" = "--skip-push" ] && SKIP_PUSH=1

die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }
log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }

[ -d "$DATA_DIR/shared" ] || die "缺少 $DATA_DIR/shared（設 RIME_DATA_DIR= 或先跑 scripts/collect_data.sh）"
[ -f "$ROOT/third_party/prebuilt/$ABI/lib/librime.a" ] || \
  die "缺少 $ABI 的 librime.a —— 先跑 scripts/build_native.sh $ABI（它會套上 patches/）"
[ -x "$ADB" ] || die "找不到 adb: $ADB"

# librime.a 裡必須真的有第二層沙盒的字串。沒有的話底下全部白測 ——
# 很可能是 .a 是「套 patch 之前」建的。
if strings "$ROOT/third_party/prebuilt/$ABI/lib/librime.a" > /tmp/.rimestr.$$ 2>/dev/null; then
  if grep -q "$RS_LUA_SANDBOX_MARK" /tmp/.rimestr.$$; then
    ok "librime.a 裡有第二層沙盒（.a 是套過 patch 之後建的）"
  else
    bad "librime.a 裡找不到沙盒字串 —— 這份 .a 不是套 patch 之後建的，底下的結果沒有意義"
  fi
  rm -f /tmp/.rimestr.$$
fi

# ---------------------------------------------------------------- 編譯 ---
log "編 rime_console ($ABI)"
BIN="$ROOT/third_party/build/rime_console.$ABI"
mkdir -p "$(dirname "$BIN")"
L="$ROOT/third_party/prebuilt/$ABI/lib"
# 連結順序不可更動，見 third_party/prebuilt/manifest.json
"$NDK/clang++" --target=${ABI}-linux-android21 -std=c++17 -O2 \
  -I "$ROOT/core/include" -I "$ROOT/third_party/prebuilt/$ABI/include" \
  "$ROOT/tools/rime_console.cc" "$ROOT/core/src/rime_shell.cc" \
  -static-libstdc++ \
  "$L/librime.a" "$L/libopencc.a" "$L/libmarisa.a" \
  "$L/libleveldb.a" "$L/libyaml-cpp.a" "$L/libglog.a" \
  -llog -lm -o "$BIN" || die "編譯失敗"

# ---------------------------------------------------------------- 裝置 ---
# shellcheck source=lib/device.sh
. "$ROOT/scripts/lib/device.sh"
# ⛔ 「抓第一台」= 永遠是 emulator-5554。
SERIAL="$(rs_pick_serial "$ADB")" || die "沒有選定裝置。先起模擬器（scripts/emu.sh start）或設 RIME_SERIAL="
log "裝置 $SERIAL"

if [ "$SKIP_PUSH" -eq 0 ]; then
  # ⚠ `rm -rf /data/local/tmp/rime` 會把**別條線**正在用的那一份資料刪掉
  #   (run_console_test.sh 早就有這個閘,這一支漏了)。
  #   由 `scripts/verify_device_hygiene.sh` 規則 C 守著。
  rs_assert_destructive_ok "$ADB" "$SERIAL" "rm -rf $DEV_DIR" \
    || die "沒有明著指定 RIME_SERIAL(或 AVD 對不上),不推送資料"
  log "推送資料到 $DEV_DIR"
  "$ADB" -s "$SERIAL" shell rm -rf "$DEV_DIR" >/dev/null
  "$ADB" -s "$SERIAL" shell mkdir -p "$DEV_DIR" >/dev/null
  "$ADB" -s "$SERIAL" push "$DATA_DIR/shared" "$DEV_DIR/shared" >/dev/null || die "push shared 失敗"
  "$ADB" -s "$SERIAL" push "$DATA_DIR/user"   "$DEV_DIR/user"   >/dev/null || die "push user 失敗"
fi
"$ADB" -s "$SERIAL" push "$BIN" "$DEV_DIR/rime_console" >/dev/null || die "push 執行檔失敗"
"$ADB" -s "$SERIAL" shell chmod 755 "$DEV_DIR/rime_console" >/dev/null

# 探針用的三個檔案
"$ADB" -s "$SERIAL" shell mkdir -p "$DEV_DIR/user/lua" >/dev/null
"$ADB" -s "$SERIAL" push "$FIX/rime.lua"             "$DEV_DIR/user/rime.lua" >/dev/null
"$ADB" -s "$SERIAL" push "$FIX/probe_filter.lua"     "$DEV_DIR/user/lua/probe_filter.lua" >/dev/null
"$ADB" -s "$SERIAL" push "$FIX/luaprobe.schema.yaml" "$DEV_DIR/user/luaprobe.schema.yaml" >/dev/null

# luaprobe 要進 schema_list 才部署得到。
#
# ⚠ 這裡**不能**直接覆寫 default.custom.yaml。core/data/user 裡本來就有一份，
#   它的作用是把上游 rime-prelude 那張過長的 schema_list 換成「本專案真的有
#   詞庫的那幾個」。覆寫掉的話部署會因為 cangjie5 / quick5 找不到而整個
#   FAILURE —— 第一版就是這樣掛的，而症狀（deploy FAILURE）看起來完全不像
#   「是我把設定檔蓋掉了」。所以改成在既有內容後面**追加**一行。
CUSTOM_SRC="$DATA_DIR/user/default.custom.yaml"
[ -f "$CUSTOM_SRC" ] || die "找不到 $CUSTOM_SRC —— 追加 luaprobe 的前提不成立"
CUSTOM_TMP="$(mktemp)"
cat "$CUSTOM_SRC" > "$CUSTOM_TMP"
LAST_LINE="$(awk 'NF {last=$0} END {print last}' "$CUSTOM_TMP")"
case "$LAST_LINE" in
  "    - schema: "*) ;;
  *) die "default.custom.yaml 的最後一行不是 schema_list 的項目（是「$LAST_LINE」），
追加的做法不再成立。請改成真的 YAML 合併。" ;;
esac
printf '    - schema: luaprobe          # verify_lua_deferral.sh 加的探針方案\n' >> "$CUSTOM_TMP"
"$ADB" -s "$SERIAL" push "$CUSTOM_TMP" "$DEV_DIR/user/default.custom.yaml" >/dev/null
rm -f "$CUSTOM_TMP"

EVENTS="$DEV_DIR/user/probe-events.txt"
reset_events() { "$ADB" -s "$SERIAL" shell rm -f "$EVENTS" "$DEV_DIR/user/from-rime-lua.txt" >/dev/null; }
read_events()  { "$ADB" -s "$SERIAL" shell "cat $EVENTS 2>/dev/null" | tr -d '\r'; }

run_console() {  # run_console <keys> [select] [schema]
  "$ADB" -s "$SERIAL" shell "cd $DEV_DIR && ./rime_console $DEV_DIR/shared $DEV_DIR/user $*" 2>&1 | tr -d '\r'
}

# ═══════════════════════════════════════════════════════════════════════════
log "案例 A：只部署，不建立任何會話"
# ═══════════════════════════════════════════════════════════════════════════
reset_events
OUT_A="$(run_console -)"
EV_A="$(read_events)"
if printf '%s' "$OUT_A" | grep -q '\[deploy-only\] OK'; then
  ok "部署成功"
else
  bad "部署沒有成功，後面的結論不成立"
  printf '%s\n' "$OUT_A" | tail -20 | sed 's/^/         /' >&2
fi
if [ -z "$EV_A" ]; then
  ok "A：只部署時 rime.lua **沒有**被執行（上游會執行）"
else
  bad "A：只部署時 rime.lua 就跑了 —— 延後沒有生效"
  printf '%s\n' "$EV_A" | sed 's/^/         /' >&2
fi

# ═══════════════════════════════════════════════════════════════════════════
log "案例 B：選一個不含 lua 元件的方案"
# ═══════════════════════════════════════════════════════════════════════════
reset_events
OUT_B="$(run_console nihao 1 luna_pinyin_tw)"
EV_B="$(read_events)"
COMMIT_B="$(printf '%s\n' "$OUT_B" | grep '>>> COMMIT: ' | tail -1 | sed 's/.*>>> COMMIT: "\(.*\)".*/\1/')"
if [ "$COMMIT_B" = "你好" ]; then
  ok "B：luna_pinyin_tw 仍然打得出「你好」（延後沒有把引擎弄壞）"
else
  bad "B：luna_pinyin_tw 打出來的是「$COMMIT_B」，不是「你好」"
fi
if [ -z "$EV_B" ]; then
  ok "B：選了不用 lua 的方案，rime.lua **沒有**被執行"
else
  bad "B：選了不用 lua 的方案，rime.lua 還是跑了"
  printf '%s\n' "$EV_B" | sed 's/^/         /' >&2
fi

# ═══════════════════════════════════════════════════════════════════════════
log "案例 C：選一個含 lua_filter 的方案（反向測試 + 沙盒觀測）"
# ═══════════════════════════════════════════════════════════════════════════
reset_events
OUT_C="$(run_console nihao 1 luaprobe)"
EV_C="$(read_events)"
if [ -n "$EV_C" ]; then
  ok "C：方案真的用到 lua 時，rime.lua 有被執行（探針本身是活的）"
else
  bad "C：連含 lua_filter 的方案都沒有觸發 rime.lua —— 探針可能根本沒裝好，"\
      "A 與 B 的「沒跑」因此不能當成證據"
  printf '%s\n' "$OUT_C" | tail -25 | sed 's/^/         /' >&2
fi

expect_ev() {  # expect_ev <字串> <說明>
  case "$EV_C" in
    *"$1"*) ok "C：$2（$1）" ;;
    *)      bad "C：$2 —— 沒看到 $1" ;;
  esac
}
if [ -n "$EV_C" ]; then
  expect_ev "rime.lua-ran"          "rime.lua 執行了"
  expect_ev "probe_filter-required" "lua_filter 的模組被 require 了"
  expect_ev "os.execute=nil"        "os.execute 在真的引擎裡是 nil"
  expect_ev "io.popen=nil"          "io.popen 在真的引擎裡是 nil"
  expect_ev "package.loadlib=nil"   "package.loadlib 在真的引擎裡是 nil"
  expect_ev "os.remove=nil"         "os.remove 在真的引擎裡是 nil"
  expect_ev "io.tmpfile=nil"        "io.tmpfile 在真的引擎裡是 nil"
  expect_ev "debug.getupvalue=nil"  "debug.getupvalue 在真的引擎裡是 nil"
  expect_ev "read-outside=denied"   "讀資料目錄外面的檔案被拒絕"
  expect_ev "write-user=opened"     "寫使用者資料目錄仍然可以（雾凇要用）"
  expect_ev "marker=2/true"         "第二層沙盒真的裝起來了"
fi

echo
echo "============================================"
echo " 真引擎沙盒／延後執行：通過 $PASS 項，失敗 $FAIL 項"
echo "============================================"
[ "$FAIL" -gt 0 ] && exit 1
echo "延後與沙盒在實際出貨的引擎上成立。"
