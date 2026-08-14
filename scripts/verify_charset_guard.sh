#!/usr/bin/env bash
#
# verify_charset_guard.sh — 字集守門的純函式測試（真的 Lua，不是模擬）
#
# ═══════════════════════════════════════════════════════════════════════════
#  為什麼要有這一支
# ═══════════════════════════════════════════════════════════════════════════
#
# `core/data/lua/luminakey_charset.lua` 是**出貨的程式碼**：它在每一台裝置上
# 決定每一個候選字要不要留下來。它壞掉的樣子是「候選變少」甚至「候選變空」，
# 而候選變空在 Android 上會直接把 preedit 上屏 —— 使用者打 fong 得到四個字母。
# 這種缺陷不會讓 build 紅、不會讓 log 叫，只會讓人覺得這個輸入法很爛。
#
# Kotlin 那一側的 `./gradlew test` 碰不到它（它不是 Kotlin），所以這裡自己
# 用 librime-lua 出貨的那一份 **Lua 5.4.8 原始碼**編一個直譯器，把
# `scripts/charset_guard/test.lua` 跑起來。
#
# ⚠ 2026-08-12 的教訓：這一層被從 main 撤回過一次，而當時的純函式測試**全是綠的**
#   —— 破口不在過濾邏輯，在「模組載不起來」與「func 中途 raise」這兩條路，
#   兩條的症狀都是候選變成 0。test.lua 第 5 節與變異 M6–M8 就是補這個。
#
# 最後跑八個**變異測試**：對過濾器各植入一個真缺陷，確認測試會紅。
# 不會紅的那一條代表測試根本沒在檢查它 —— 這支腳本會因此判自己失敗。
# （沒有做過這一步的檢查，一律當作沒有。）
#
# 用法：
#   scripts/verify_charset_guard.sh          # 全部
#   scripts/verify_charset_guard.sh --keep   # 保留暫存目錄
#   scripts/verify_charset_guard.sh --help   # 只印這一段就走（不碰網路、不碰檔案）
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── ⚠ 參數解析必須在**任何**網路／檔案動作之前 ───────────────────────────
#  這一段從前不存在（只有 `[ "${1:-}" = "--keep" ] && KEEP=1`），於是 `--help`
#  一路落到下面的「取得 librime-lua/thirdparty」，`mkdir -p` 建了目錄、
#  `git fetch` 去抓 Lua 原始碼。後果不是「說明多印了幾行」：
#
#    · `scripts/verify_script_readonly.sh` 的判準是「唯讀路徑不准碰外部工具」，
#      它把 `git` 換成會 exit 127 的 shim。於是本檔的 `--help` 在**任何
#      沒有 `third_party/librime-lua/thirdparty` 的地方**（新 clone、CI、
#      任何新開的 worktree）RC=1，而在建置機的 `/home/lc/rime` 上因為那個
#      gitignore 的目錄早就在，就一路綠燈。
#      → **那支守門的綠燈取決於機器歷史，不是程式碼。**
#    · 實測（2026-08-14，全新 worktree）：
#        [readonly-guard] 唯讀路徑不該呼叫 git   ×3
#        [error] 抓取 Lua 原始碼失敗   → RC=1
#
#  所以 `--help` 在這裡就結束，`ROOT` 之外一個變數都還沒展開、一個目錄都還
#  沒建。其餘參數也在這裡定案，別再散落到下面。
KEEP=0
case "${1:-}" in
  -h|--help)
    sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0 ;;
  --keep) KEEP=1 ;;
  "") ;;
  *) echo "未知參數：$1" >&2; exit 2 ;;
esac

DATA="$ROOT/core/data/lua"
TEST="$ROOT/scripts/charset_guard/test.lua"
GEN="$ROOT/scripts/gen_charset_data.py"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }
PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }

[ -f "$TEST" ] || die "找不到 $TEST"
[ -f "$DATA/luminakey_charset.lua" ] || die "找不到 $DATA/luminakey_charset.lua"

# ── Lua 原始碼。gitignore 的，CI 上要先取回來。 ────────────────────────────
LUA_SRC_DIR="${LIBRIME_LUA_DIR:-$ROOT/third_party/librime-lua}"
LIBRIME_LUA_REPO="${LIBRIME_LUA_REPO:-https://github.com/hchunhui/librime-lua.git}"
LIBRIME_LUA_TP_COMMIT="${LIBRIME_LUA_TP_COMMIT:-fa40fadd8af1e5b1fbd55703ccbd54476956d74c}"
if [ ! -f "$LUA_SRC_DIR/thirdparty/lua5.4/lua.h" ]; then
  log "取得 librime-lua/thirdparty（Lua 5.4）@ ${LIBRIME_LUA_TP_COMMIT:0:8}"
  mkdir -p "$LUA_SRC_DIR/thirdparty"
  if [ ! -d "$LUA_SRC_DIR/thirdparty/.git" ]; then
    git -C "$LUA_SRC_DIR/thirdparty" init -q
    git -C "$LUA_SRC_DIR/thirdparty" remote add origin "$LIBRIME_LUA_REPO"
  fi
  git -C "$LUA_SRC_DIR/thirdparty" fetch -q --depth 1 origin "$LIBRIME_LUA_TP_COMMIT" \
    || die "抓取 Lua 原始碼失敗"
  git -C "$LUA_SRC_DIR/thirdparty" checkout -q --detach FETCH_HEAD \
    || die "checkout Lua 原始碼失敗"
fi
LUA54="$LUA_SRC_DIR/thirdparty/lua5.4"
[ -f "$LUA54/lua.h" ] || die "Lua 原始碼不在 $LUA54"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/luminakey-charset.XXXXXX")"
cleanup() { [ "$KEEP" -eq 1 ] || rm -rf "$TMP"; }
trap cleanup EXIT
[ "$KEEP" -eq 1 ] && echo "暫存目錄：$TMP"

# ═══════════════════════════════════════════════════════════════════════════
log "1/4 編 Lua 5.4（librime-lua 出貨的那一份原始碼）"
# ═══════════════════════════════════════════════════════════════════════════
CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || die "找不到 C 編譯器（設 CC=）"
HOST="$ROOT/scripts/charset_guard/test_host.c"
[ -f "$HOST" ] || die "找不到 $HOST"
# librime-lua 打包的 lua5.4 只有函式庫，沒有上游的 lua.c，所以 main 自己給。
SRCS=()
for f in "$LUA54"/*.c; do
  case "$(basename "$f")" in
    lua.c|luac.c|onelua.c) continue ;;   # 它們自己有 main（這一份通常都沒有）
  esac
  SRCS+=("$f")
done
"$CC" -O1 -w -DLUA_USE_POSIX -I"$LUA54" "$HOST" "${SRCS[@]}" -lm -o "$TMP/lua" \
  || die "編 Lua 失敗"
LUA_VER="$(grep -E '^#define LUA_VERSION_(MAJOR|MINOR|RELEASE)[[:space:]]' "$LUA54/lua.h" | sed 's/.*"\(.*\)".*/\1/' | paste -sd. -)"
ok "Lua 直譯器編好了（${LUA_VER:-lua5.4}）"

# ═══════════════════════════════════════════════════════════════════════════
log "2/4 兩份漢字範圍表必須一致（Lua 一份、Python 一份）"
# ═══════════════════════════════════════════════════════════════════════════
# 過濾器與產生器各寫了一份 HAN_RANGES。它們漂開的下場是：產生器算出來的
# 「表外字」與過濾器實際攔下來的不是同一批，而兩邊各自都會全綠。
lua_ranges="$(sed -n '/^local HAN_RANGES/,/^}/p' "$DATA/luminakey_charset.lua" \
  | grep -oE '0x[0-9A-Fa-f]+' | tr 'a-f' 'A-F' | paste -sd, -)"
py_ranges="$(sed -n '/^HAN_RANGES = (/,/^)/p' "$GEN" \
  | grep -oE '0x[0-9A-Fa-f]+' | tr 'a-f' 'A-F' | paste -sd, -)"
if [ -n "$lua_ranges" ] && [ "$lua_ranges" = "$py_ranges" ]; then
  ok "漢字範圍一致（$lua_ranges）"
else
  bad "漢字範圍不一致：lua=[$lua_ranges] python=[$py_ranges]"
fi

# ═══════════════════════════════════════════════════════════════════════════
log "3/4 跑純函式測試"
# ═══════════════════════════════════════════════════════════════════════════
if "$TMP/lua" "$TEST" "$DATA"; then
  ok "純函式測試全過"
else
  bad "純函式測試有失敗"
fi

# ═══════════════════════════════════════════════════════════════════════════
log "4/4 變異測試：植入真缺陷，確認測試會紅"
# ═══════════════════════════════════════════════════════════════════════════
# 每一個變異都對應一個「真的會傷到使用者」的寫法。
#
# ⚠ 每個變異都必須**真的改到檔案**。perl 的 s/// 沒有配對到時是靜靜地成功的，
#   於是「跑了原始碼、原始碼是綠的」被讀成「變異沒被測出來」—— 而那是一句
#   假的指控，會讓人跑去改一份其實沒問題的測試。（實際踩過：M.run 重寫之後
#   M1 的樣式對不上，整支腳本開始說 M1 沒被抓到。）所以這裡比對前後的內容。
mutate() {
  local name="$1" desc="$2"; shift 2
  local dir="$TMP/mut-$name"
  rm -rf "$dir"; mkdir -p "$dir"
  cp "$DATA"/*.lua "$dir/"
  local before after
  before="$(cat "$dir"/*.lua | cksum)"
  "$@" "$dir" || { bad "變異 $name 套用失敗"; return; }
  after="$(cat "$dir"/*.lua | cksum)"
  if [ "$before" = "$after" ]; then
    bad "變異「$desc」根本沒有改到檔案 —— 樣式對不上了（實作變了?），這不是測試的問題"
    return
  fi
  if "$TMP/lua" "$TEST" "$dir" >/dev/null 2>&1; then
    bad "變異「$desc」沒有被測出來 —— 測試根本沒在檢查它"
  else
    ok "變異「$desc」被測出來了"
  fi
}

# M1：拿掉「整段被濾光就退回」。這正是使用者會打出 f-o-n-g 的那個缺陷。
m1() { perl -0pi -e 's/  if not emitted and not bailed then.*?\n  end\n/  if false then\n  end\n/s' "$1/luminakey_charset.lua"; }
# M2：忽略使用者的關閉開關。設定裡關了卻還在濾。
m2() { perl -0pi -e 's/if get\("luminakey_charset_off"\) then return nil end//' "$1/luminakey_charset.lua"; }
# M3：把非漢字也拿去比對。標點、字母、注音全部會被濾掉。
m3() { perl -0pi -e 's/if is_han\(cp\) and not set\[cp\] then return true end/if not set[cp] then return true end/' "$1/luminakey_charset.lua"; }
# M4：先把整段收完再吐。惰性沒了，長候選串會讓畫面停住。
m4() { perl -0pi -e 's/^function M\.run\(pull, emit, set\)$/function M.run(pull, emit, set)\n  local _all = {}\n  for _c in pull do _all[#_all+1] = _c end\n  local _i = 0\n  pull = function() _i = _i + 1; return _all[_i] end/m' "$1/luminakey_charset.lua"; }
# M5：兩份字集其實是同一份（複製貼上）。所有單點斷言都還會過。
m5() { cp "$1/luminakey_charset_hans.lua" "$1/luminakey_charset_hant.lua"; }
# ── M6–M8：**候選變成 0** 的那三條路。這一層被從 main 撤回過一次，
#    而 1–3 節那 43 條當時全是綠的 —— 破口不在過濾邏輯，在錯誤處理。
# M6：緩衝在出錯時不倒出來。使用者少掉的正是「已經拉進來、還沒吐出去」那幾個。
m6() { perl -0pi -e 's/    flush\(\)\s*-- 見上面的保證\n    error\(err, 0\)/    error(err, 0)/' "$1/luminakey_charset.lua"; }
# M7：func 不再吃掉錯誤，照上游那樣往外丟 —— librime 收到就把整段候選清空。
m7() { perl -0pi -e 's/  if pcall\(M\.run, pull, yield, set\) then return end/  M.run(pull, yield, set)\n  do return end/' "$1/luminakey_charset.lua"; }
# M8：決定字集那一步不做保護。引擎狀態問不到時整段候選消失。
m8() { perl -0pi -e 's/  local ok_set, set = pcall\(M\.choose_set, env\)\n  if not ok_set then set = nil end/  local set = M.choose_set(env)/' "$1/luminakey_charset.lua"; }

mutate m1 "濾到空不退回" m1
mutate m2 "無視使用者關掉守門" m2
mutate m3 "連標點與字母都拿去比對" m3
mutate m4 "過濾器不再惰性" m4
mutate m5 "兩份字集其實是同一份" m5
mutate m6 "出錯時緩衝裡的候選被吞掉" m6
mutate m7 "func 把錯誤丟給 librime（整段候選清空）" m7
mutate m8 "問不到開關就整段陣亡" m8

echo
echo "═══ 字集守門：$PASS 過、$FAIL 失敗 ═══"
[ "$FAIL" -eq 0 ] || exit 1
