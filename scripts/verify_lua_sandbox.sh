#!/usr/bin/env bash
#
# verify_lua_sandbox.sh — 實際跑一遍，證明 Lua 沙盒真的擋得住
#
# ═══════════════════════════════════════════════════════════════════════════
#  為什麼要有這一支
# ═══════════════════════════════════════════════════════════════════════════
#
# scripts/audit_offline.sh 第 8 項是用 grep 檢查「沙盒 patch 還在、而且裝在
# luaL_openlibs 之後」。那擋得住「有人升級 librime-lua 忘了重套 patch」，
# 但擋不住「patch 還在，只是它不再擋得住任何東西」——
# 讀 patch 檔說「看起來有擋」不算數。
#
# 這支腳本用**真的 Lua 5.4.8 直譯器**（librime-lua 出貨的那一份原始碼）
# 把兩層沙盒照 librime-lua 的順序裝起來，然後真的去 os.execute、真的去
# io.popen、真的去 io.open 資料目錄外面的檔案，比對結果。
#
#   stage 0  什麼都不裝（上游行為）  ← 反向測試：這些事在這裡必須**成功**
#   stage 1  src/lib/lua.cc 的第一層
#   stage 2  再加 src/modules.cc 的第二層（路徑收斂）
#
# 沙盒字串不是抄過來的：先把 patches/librime-lua@sandbox.patch 套到乾淨的
# 原始碼上，再從**套完的檔案**裡把兩段 R"SANDBOX(...)" 抽出來。patch 改了
# 而探針沒跟上，這裡就會紅。
#
# 最後跑四個**變異測試**：對沙盒各植入一個真違規，確認探針會紅。
# 不會紅的那一條，代表探針根本沒在檢查它 —— 這支腳本會因此判自己失敗。
#
# 用法：
#   scripts/verify_lua_sandbox.sh            # 全部
#   scripts/verify_lua_sandbox.sh --keep     # 保留暫存目錄（除錯用）
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HERE="$ROOT/scripts/lua_sandbox"
LUA_SRC_DIR="$ROOT/third_party/librime-lua"
PATCH="$ROOT/patches/librime-lua@sandbox.patch"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }
PASS=0; FAIL=0
ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }

[ -f "$PATCH" ] || die "找不到 $PATCH"
[ -f "$HERE/probe_host.c" ] || die "找不到 $HERE/probe_host.c"
[ -f "$HERE/probe.lua" ]    || die "找不到 $HERE/probe.lua"

# ── librime-lua 原始碼。gitignore 的，CI 上要先取回來。 ────────────────────
# 這裡自己抓而不是叫人先跑 build_native.sh：那支要 NDK，而本腳本只需要
# 一份 Lua 原始碼與被 patch 的兩個 .cc，在任何有 cc 的機器上都跑得起來。
LIBRIME_LUA_REPO="${LIBRIME_LUA_REPO:-https://github.com/hchunhui/librime-lua.git}"
LIBRIME_LUA_COMMIT="${LIBRIME_LUA_COMMIT:-ec52e48ea18f11af37717a01c337f853215cf70b}"
LIBRIME_LUA_TP_COMMIT="${LIBRIME_LUA_TP_COMMIT:-fa40fadd8af1e5b1fbd55703ccbd54476956d74c}"

fetch_pinned() {
  local dir="$1" commit="$2" name="$3"
  if [ -d "$dir/.git" ]; then
    local cur; cur="$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo)"
    [ "$cur" = "$commit" ] && return 0
  else
    rm -rf "$dir"; mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$LIBRIME_LUA_REPO"
  fi
  log "取得 $name @ ${commit:0:8}"
  git -C "$dir" fetch -q --depth 1 origin "$commit" || die "抓取 $name 失敗"
  git -C "$dir" checkout -q --detach FETCH_HEAD || die "checkout $name 失敗"
}

if [ ! -f "$LUA_SRC_DIR/src/lib/lua.cc" ]; then
  fetch_pinned "$LUA_SRC_DIR" "$LIBRIME_LUA_COMMIT" "librime-lua"
fi
if [ ! -f "$LUA_SRC_DIR/thirdparty/lua5.4/lua.h" ]; then
  fetch_pinned "$LUA_SRC_DIR/thirdparty" "$LIBRIME_LUA_TP_COMMIT" "librime-lua/thirdparty"
fi
LUA54="$LUA_SRC_DIR/thirdparty/lua5.4"
[ -f "$LUA54/lua.h" ] || die "Lua 原始碼不在 $LUA54"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/rimequad-luasandbox.XXXXXX")"
cleanup() { [ "$KEEP" -eq 1 ] || rm -rf "$TMP"; }
trap cleanup EXIT
[ "$KEEP" -eq 1 ] && echo "暫存目錄：$TMP"

# ═══════════════════════════════════════════════════════════════════════════
log "1/5 從 patch 套用後的原始碼抽出兩段沙盒"
# ═══════════════════════════════════════════════════════════════════════════
# 用 git archive 取**乾淨**的 HEAD（工作區可能已經被 build_native.sh 套過
# patch，直接讀會讀到已套用的版本，那就不是在驗這一份 patch 了）。
mkdir -p "$TMP/pristine"
git -C "$LUA_SRC_DIR" archive HEAD src > "$TMP/src.tar" \
  || die "git archive 失敗（$LUA_SRC_DIR 不是 git 檢出？）"
tar -xf "$TMP/src.tar" -C "$TMP/pristine" || die "解開原始碼失敗"
grep -q 'kRimeQuadSandbox' "$TMP/pristine/src/lib/lua.cc" \
  && die "乾淨的原始碼裡就有沙盒？取到的不是上游版本"
( cd "$TMP/pristine" && git apply "$PATCH" ) \
  || die "patch 套不上乾淨的 librime-lua ${LIBRIME_LUA_COMMIT:0:8}"
ok "patch 套用成功"

python3 - "$TMP" <<'PY' || die "抽取沙盒字串失敗"
import io, re, sys
tmp = sys.argv[1]
def grab(path, name):
    s = io.open(path, encoding="utf-8").read()
    m = re.search(name + r'\[\] = R"SANDBOX\((.*?)\)SANDBOX";', s, re.S)
    if not m:
        raise SystemExit("在 %s 找不到 %s 的 R\"SANDBOX(...)\"" % (path, name))
    body = m.group(1)
    if len(body) < 200:
        raise SystemExit("%s 抽出來只有 %d 位元組，太短，八成抽錯了" % (name, len(body)))
    return body
s1 = grab(tmp + "/pristine/src/lib/lua.cc", "kRimeQuadSandbox")
s2 = grab(tmp + "/pristine/src/modules.cc", "kRimeQuadPathSandbox")
io.open(tmp + "/stage1.lua", "w", encoding="utf-8").write(s1)
io.open(tmp + "/stage2.lua", "w", encoding="utf-8").write(s2)
print("  第一層 %d 位元組、第二層 %d 位元組" % (len(s1), len(s2)))
PY
ok "抽出兩段沙盒"

# 順序：第二層必須在 rime.lua 之前。這一條 grep 得出來，就在這裡一起驗。
MOD="$TMP/pristine/src/modules.cc"
L_SBOX="$(grep -n 'kRimeQuadPathSandbox,' "$MOD" | head -1 | cut -d: -f1)"
L_RIME="$(grep -n 'luaL_dofile(L, user_file' "$MOD" | head -1 | cut -d: -f1)"
if [ -n "$L_SBOX" ] && [ -n "$L_RIME" ] && [ "$L_SBOX" -lt "$L_RIME" ]; then
  ok "第二層裝在 rime.lua 之前（第 $L_SBOX -> $L_RIME 行）"
else
  bad "第二層沒有裝在 rime.lua 之前 —— 順序錯了等於沒做"
fi

# ═══════════════════════════════════════════════════════════════════════════
log "2/5 編 Lua 5.4.8（librime-lua 出貨的那一份）與探針宿主"
# ═══════════════════════════════════════════════════════════════════════════
CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || die "找不到 C 編譯器（設 CC=）"
mkdir -p "$TMP/obj"
SRCS=""
for f in "$LUA54"/*.c; do
  b="$(basename "$f")"
  case "$b" in lua.c|luac.c|onelua.c) continue ;; esac
  SRCS="$SRCS $f"
done
[ -n "$SRCS" ] || die "$LUA54 裡沒有 .c"
for f in $SRCS; do
  "$CC" -O2 -std=gnu99 -DLUA_USE_LINUX -I"$LUA54" -c "$f" \
        -o "$TMP/obj/$(basename "$f" .c).o" 2>>"$TMP/cc.log" &
done
wait
NOBJ="$(ls "$TMP/obj"/*.o 2>/dev/null | wc -l)"
[ "$NOBJ" -ge 30 ] || { cat "$TMP/cc.log" >&2; die "Lua 只編出 $NOBJ 個 .o，太少"; }
"$CC" -O2 -I"$LUA54" "$HERE/probe_host.c" "$TMP/obj"/*.o -lm -ldl \
      -o "$TMP/probe_host" 2>>"$TMP/cc.log" || { cat "$TMP/cc.log" >&2; die "連結探針失敗"; }
# 不用 grep|head|tr 串管線取版本：pipefail 配 head 會因 SIGPIPE 誤判失敗，
# 這個專案已經被同一件事咬過兩次。整段讀進變數再比對。
LUA_H="$(cat "$LUA54/lua.h")"
LUA_VER="$(printf '%s' "$LUA_H" | sed -n 's/^#define LUA_VERSION_RELEASE\t*"\([0-9]*\)".*/\1/p')"
ok "Lua 5.4.${LUA_VER:-?} 與探針宿主編好了（$NOBJ 個 .o）"

# ═══════════════════════════════════════════════════════════════════════════
log "3/5 佈置測試用的資料目錄"
# ═══════════════════════════════════════════════════════════════════════════
FIX="$TMP/fix"
U="$FIX/rime/user"; S="$FIX/rime/shared"; O="$FIX/outside"
mkdir -p "$U/lua/pkg" "$U/lua/cold_word_drop" "$S/lua" "$O" "$TMP/cwd"
printf 'return "mymod-ok"\n'      > "$U/lua/mymod.lua"
printf 'return "sub-ok"\n'        > "$U/lua/pkg/sub.lua"
printf 'return "sharedmod-ok"\n'  > "$S/lua/sharedmod.lua"
printf 'zrm\n'                    > "$U/lua/zrmdb.txt"
printf 'secret\n'                 > "$O/secret.txt"
printf 'return "evil-ran"\n'      > "$O/evil.lua"
# $U/../../outside 必須真的指到 $O —— 穿越測試靠這個。
[ -f "$U/../../outside/secret.txt" ] || die "測試佈置錯了：穿越路徑指不到外面"
ok "資料目錄就緒（user / shared / outside）"

run_stage() {   # run_stage <stage> <stage1.lua> <stage2.lua> -> exit code
  ( cd "$TMP/cwd" && "$TMP/probe_host" "$1" "$2" "$3" "$U" "$S" "$O" "$HERE/probe.lua" )
}

# ═══════════════════════════════════════════════════════════════════════════
log "4/5 跑三個階段"
# ═══════════════════════════════════════════════════════════════════════════
for st in 0 1 2; do
  OUT="$(run_stage "$st" "$TMP/stage1.lua" "$TMP/stage2.lua" 2>&1)"; RC=$?
  printf '%s\n' "$OUT" | sed 's/^/    /'
  if [ "$RC" -eq 0 ]; then
    ok "stage $st 全數符合期望"
  else
    bad "stage $st 有 $RC 條不符（見上）"
  fi
done

# ═══════════════════════════════════════════════════════════════════════════
log "5/5 變異測試：植入真違規，確認探針會紅"
# ═══════════════════════════════════════════════════════════════════════════
# 「該紅的時候會不會安靜地不紅」是這個專案反覆踩到的坑。每一條變異都對應
# 一個真的曾經或可能發生的錯誤：升級 Lua 後允許清單漏一項、把路徑檢查改成
# 恆真、把搜尋器換回上游版本。
mutate() {   # mutate <which:1|2> <sed-free python replace> <old> <new> -> 檔案路徑
  # 一行 local 裡不能引用同一行剛宣告的變數：bash 在執行 local 之前就把
  # 整行的 $which 展開了（set -u 之下直接爆「unbound variable」）。
  local which="$1"
  local old="$2"
  local new="$3"
  local out="$TMP/mut.$which.lua"
  python3 - "$TMP/stage$which.lua" "$out" "$old" "$new" <<'PY' || return 1
import io, sys
src, dst, old, new = sys.argv[1:5]
s = io.open(src, encoding="utf-8").read()
if s.count(old) != 1:
    raise SystemExit("變異目標在沙盒裡出現 %d 次（需要正好 1 次）：%r" % (s.count(old), old))
io.open(dst, "w", encoding="utf-8").write(s.replace(old, new))
PY
  printf '%s' "$out"
}

expect_red() {  # expect_red <名稱> <stage> <stage1檔> <stage2檔>
  local name="$1" st="$2" f1="$3" f2="$4"
  local out rc
  out="$( ( cd "$TMP/cwd" && "$TMP/probe_host" "$st" "$f1" "$f2" "$U" "$S" "$O" "$HERE/probe.lua" ) 2>&1 )"
  rc=$?
  if [ "$rc" -ne 0 ]; then
    ok "變異「$name」→ stage $st 紅了（$rc 條）"
  else
    bad "變異「$name」→ stage $st 仍然全綠：探針根本沒在檢查這件事"
    printf '%s\n' "$out" | sed 's/^/         /' >&2
  fi
}

M1="$(mutate 1 'for k in pairs(io) do if not keep_io[k] then io[k] = nil end end' '-- 變異：拿掉 io 允許清單')" \
  || bad "變異 1 植入失敗"
[ -n "${M1:-}" ] && expect_red "第一層漏掉 io 允許清單（io.popen 活著）" 1 "$M1" "$TMP/stage2.lua"

M2="$(mutate 1 'for k in pairs(debug) do if not keep_debug[k] then debug[k] = nil end end' '-- 變異：拿掉 debug 允許清單')" \
  || bad "變異 2 植入失敗"
[ -n "${M2:-}" ] && expect_red "第一層漏掉 debug 允許清單（getupvalue 挖得到 io.open）" 2 "$M2" "$TMP/stage2.lua"

M3="$(mutate 2 'local ok, err = check(path, is_write(mode))' 'local ok, err = true, nil')" \
  || bad "變異 3 植入失敗"
[ -n "${M3:-}" ] && expect_red "第二層的路徑檢查恆真" 2 "$TMP/stage1.lua" "$M3"

M4="$(mutate 2 'package.searchers[2] = function(name)' 'local _unused_searcher = function(name)')" \
  || bad "變異 4 植入失敗"
[ -n "${M4:-}" ] && expect_red "第二層沒有換掉 require 的搜尋器" 2 "$TMP/stage1.lua" "$M4"

# ═══════════════════════════════════════════════════════════════════════════
echo
echo "============================================"
echo " Lua 沙盒探針：通過 $PASS 項，失敗 $FAIL 項"
echo "============================================"
[ "$FAIL" -gt 0 ] && { echo "沙盒現在擋不住它宣稱擋得住的東西。修好再發布。" >&2; exit 1; }
echo "沙盒站得住。"
