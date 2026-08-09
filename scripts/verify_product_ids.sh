#!/usr/bin/env bash
#
# verify_product_ids.sh — 守住「產品識別碼只有一份」這件事
#
# 為什麼需要它:
#
#   2026-08-09 把「RimeQuad / 四端」換成 LuminaKey 的時候,同一個 Android IME id
#   在九支腳本裡各寫死了一份。改名之所以痛,不是因為要改的地方多,是因為沒有人
#   知道到底有幾個地方 —— grep 找得到的只是**字面寫出來的那些**,拼接出來的
#   (`$PKG/.RimeInputMethodService`)、格式化出來的、當成路徑一段用掉的
#   都找不到。改完之後最典型的下場是「顯示名改了、識別碼沒改」,而那種半套
#   在編譯期、單元測試、發布關卡全部是綠的,只有使用者裝上去才發現。
#
#   所以這一關做三件事:
#     1. shell 與 python 兩邊的讀取器對同一份 product.env 必須得出**逐字相同**
#        的結果(組合值是兩邊各推一次的,這一項就是在盯那個「各推一次」)。
#     2. 兩個讀取器必須真的在讀檔案。改了 product.env 而輸出不變 = 有人在
#        讀取器裡也寫死了一份。
#     3. scripts/ 底下(scripts/lib/ 以外)的 .sh 與 .py 不准再出現識別碼字面值;
#        core/ docs/ tools/ README 不准殘留舊名。
#
#   ⚠ 每一項都自帶**反向測試**:先植入一個一定要被抓到的違規,確認這一關真的
#   會紅,再跑真的檢查。沒有反向測試的守門腳本,在它自己壞掉的那天會安靜地
#   全綠 —— 這個專案已經吃過好幾次那種虧(見 docs/coordination.md §3)。
#
# 用法: ./scripts/verify_product_ids.sh
# 離開碼: 0 全過;1 有項目失敗。

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

PASS=0; FAIL=0
ok()  { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*" >&2; FAIL=$((FAIL+1)); }
step(){ echo; echo "=== $* ==="; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/product-ids.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# ─────────────────────────────────────────────────────────────────────────────
step "1. shell 與 python 的讀取器輸出一致"

bash "$HERE/lib/product.sh" --dump > "$TMP/sh.txt" 2>"$TMP/sh.err"
SH_RC=$?
python3 "$HERE/lib/product.py" --dump > "$TMP/py.txt" 2>"$TMP/py.err"
PY_RC=$?

if [ "$SH_RC" -ne 0 ]; then
  bad "product.sh --dump 失敗"; sed 's/^/         /' "$TMP/sh.err" >&2
elif [ ! -s "$TMP/sh.txt" ]; then
  bad "product.sh --dump 沒有輸出 —— 空的輸出和 python 的空輸出會互相比對成功"
elif [ "$PY_RC" -ne 0 ]; then
  bad "product.py --dump 失敗"; sed 's/^/         /' "$TMP/py.err" >&2
elif [ ! -s "$TMP/py.txt" ]; then
  bad "product.py --dump 沒有輸出"
elif diff -u "$TMP/sh.txt" "$TMP/py.txt" > "$TMP/diff.txt"; then
  ok "兩邊各 $(grep -c . "$TMP/sh.txt") 個值,逐字相同"
else
  bad "shell 與 python 讀出來的產品識別不一致 —— 兩邊各有一套名字正是這次要根除的"
  sed 's/^/         /' "$TMP/diff.txt" >&2
fi

# ─────────────────────────────────────────────────────────────────────────────
step "2. 讀取器真的在讀 product.env(反向測試)"
# 一個把值寫死在自己身上的讀取器,前一項會完美通過。

mkdir -p "$TMP/lib"
cp "$HERE/lib/product.env" "$HERE/lib/product.sh" "$HERE/lib/product.py" "$TMP/lib/"
SENTINEL="org.sentinel.notaproduct"
python3 - "$TMP/lib/product.env" "$SENTINEL" <<'PY'
import io, sys
f, sentinel = sys.argv[1], sys.argv[2]
s = io.open(f, encoding="utf-8").read()
old = [l for l in s.splitlines() if l.startswith("ANDROID_APP_ID=")]
if len(old) != 1:
    raise SystemExit("product.env 裡 ANDROID_APP_ID= 出現 %d 次(需要正好 1 次)" % len(old))
io.open(f, "w", encoding="utf-8").write(s.replace(old[0], "ANDROID_APP_ID=" + sentinel))
PY
if [ $? -ne 0 ]; then
  bad "植入哨兵值失敗,第 2 項沒有驗到"
else
  M_SH="$(bash "$TMP/lib/product.sh" --dump 2>/dev/null | grep -c "$SENTINEL")"
  M_PY="$(python3 "$TMP/lib/product.py" --dump 2>/dev/null | grep -c "$SENTINEL")"
  # 改一個 ANDROID_APP_ID,推導值(IME id、套件路徑、NetworkGate 類別名與路徑)
  # 應該跟著變,所以命中數必須 > 1 —— 只變一行代表推導那幾行是寫死的。
  if [ "${M_SH:-0}" -gt 1 ] && [ "${M_PY:-0}" -gt 1 ]; then
    ok "改 product.env 後兩邊都跟著變(shell $M_SH 處、python $M_PY 處)"
  else
    bad "改了 product.env 而讀取器輸出沒跟著變(shell $M_SH 處、python $M_PY 處)—— 有人在讀取器裡也寫死了一份"
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# 識別碼的字面樣式。scripts/lib/ 以外的腳本不准出現這些字。
HARDCODE_RE='org\.(luminakey|rimequad)|(luminakey|rimequad)-(store|layouts|backup|update|current-keyboard)|kRimeQuad|rimequad_lua_|rimequad-path-sandbox|__rimequad_sandbox|LuminaKey|RimeQuad'

# scripts/ 底下的 .sh 與 .py。兩個排除:
#   scripts/lib/            —— 值的合法住址
#   本檔                    —— 規則本身要把那些字寫出來才能拿去比對
# (scripts/lua_sandbox/ 的 .lua 與 .c 不在掃描範圍:它們裡面的 RIMEQUAD_* 全域
#  與 __rimequad_sandbox 標記是 patches/ 那顆沙盒定義的,見 product.env。)
scan_scripts() {   # scan_scripts <root>
  local root="$1"
  [ -d "$root/scripts" ] || return 0
  find "$root/scripts" -type f \( -name '*.sh' -o -name '*.py' \) \
       -not -path "$root/scripts/lib/*" \
       -not -name 'verify_product_ids.sh' -print0 2>/dev/null \
    | xargs -0 -r grep -nE "$HARDCODE_RE" 2>/dev/null \
    | sed "s|^$root/||"
}

step "3. scripts/ 底下沒有寫死的識別碼"
# 先確認這個掃描器抓得到東西 —— 一個永遠回空的掃描器會讓這一關恆綠。
mkdir -p "$TMP/plant/scripts"
printf 'IME_ID="org.luminakey.ime/.RimeInputMethodService"\n' > "$TMP/plant/scripts/planted.sh"
printf 'PREFS = "rimequad-store.json"\n' > "$TMP/plant/scripts/planted.py"
N_PLANT="$(scan_scripts "$TMP/plant" | grep -c . || true)"
if [ "${N_PLANT:-0}" -lt 2 ]; then
  bad "掃描器抓不到植入的違規($N_PLANT/2)—— 這一關本身壞了,不是通過"
else
  HITS="$(scan_scripts "$ROOT")"
  if [ -z "$HITS" ]; then
    ok "掃描器對植入的違規報紅($N_PLANT 處),而真的腳本裡一處都沒有"
  else
    bad "這些腳本仍然把識別碼寫死(請改讀 scripts/lib/product.sh 或 product.py):"
    printf '%s\n' "$HITS" | sed 's/^/         /' >&2
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# 舊名殘留。改名最典型的失敗是「改了一半」,而一半的那一半通常在文件裡。
LEGACY_RE='rimequad|RimeQuad|RIMEQUAD'
# 刻意保留舊名的地方:
#   product.env            —— 保留舊名的**唯一**合法住址,每一條都寫了理由
#   decisions/product-name.md —— 決策紀錄,舊值是紀錄的一部分
#   coordination.md        —— §5 是各端當時寫下的紀錄,規矩是只加不刪
#
# 另外:規範裡「讀取端要認得舊名」這種相容條款**必須**寫出舊名才有用,所以
# 放行**同一行**寫了「舊名」的句子。這不是漏洞:忘了改的地方不會自稱舊名,
# 而要繞過它就得明著打上那兩個字 —— 那正好是我們想逼人做的那個動作。
LEGACY_LABEL='舊名'
scan_legacy() {   # scan_legacy <root>
  local root="$1"
  grep -rnE "$LEGACY_RE" \
       "$root/core" "$root/docs" "$root/tools" "$root/README.md" \
       "$root/.github/workflows/build.yml" 2>/dev/null \
    | grep -v '^'"$root"'/docs/decisions/product-name\.md:' \
    | grep -v '^'"$root"'/docs/coordination\.md:' \
    | grep -vF "$LEGACY_LABEL" \
    | sed "s|^$root/||"
}

step "4. core/ docs/ tools/ README 沒有殘留舊產品名"
mkdir -p "$TMP/legacy/docs" "$TMP/legacy/core" "$TMP/legacy/tools" "$TMP/legacy/.github/workflows"
: > "$TMP/legacy/README.md"
: > "$TMP/legacy/.github/workflows/build.yml"
printf '使用者資料在 ~/Library/Application Support/RimeQuad。\n' > "$TMP/legacy/docs/planted.md"
N_LEG="$(scan_legacy "$TMP/legacy" | grep -c . || true)"
if [ "${N_LEG:-0}" -lt 1 ]; then
  bad "舊名掃描器抓不到植入的 RimeQuad —— 這一關本身壞了"
else
  LEG="$(scan_legacy "$ROOT")"
  if [ -z "$LEG" ]; then
    ok "掃描器對植入的舊名報紅,而 core/ docs/ tools/ README 裡一處都沒有"
  else
    bad "還有地方叫舊名字(改名改一半就是這樣開始的):"
    printf '%s\n' "$LEG" | sed 's/^/         /' >&2
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
step "5. 稽核用的 UA 樣式擋得住新名字"
# 改名之後最容易漏的一條:守門的樣式還在擋舊名,新名字大搖大擺地走出去。
# shellcheck disable=SC1091
. "$HERE/lib/product.sh"
UA_MISS=""
for probe in "$RS_PRODUCT_ID_ROOT" "$RS_LEGACY_ID_ROOT"; do
  printf '%s' "$probe-android/1.0" | grep -qiE "$RS_SELF_ID_UA_PATTERN" \
    || UA_MISS="$UA_MISS $probe"
done
if [ -z "$UA_MISS" ]; then
  ok "SELF_ID_UA_PATTERN 抓得到新舊兩個名字的 User-Agent"
else
  bad "SELF_ID_UA_PATTERN 漏掉:$UA_MISS —— 用這個名字自報家門的 UA 會通過稽核"
fi
# 反向對照:一個不含產品名的 UA 不可以被判成自報家門,否則第 7 項會永遠紅,
# 然後被當成雜訊關掉。
if printf '%s' "Mozilla/5.0" | grep -qiE "$RS_SELF_ID_UA_PATTERN"; then
  bad "SELF_ID_UA_PATTERN 把 Mozilla/5.0 也判成自報家門 —— 樣式太寬"
else
  ok "不含產品名的 UA 不會被誤判"
fi

echo
echo "============================================"
echo " 通過 $PASS 項,失敗 $FAIL 項"
echo "============================================"
[ "$FAIL" -eq 0 ] || exit 1
