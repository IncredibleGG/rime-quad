#!/usr/bin/env bash
#
# collect_charset_guard.sh — 把「字集守門」的資料放進一份 shared 資料目錄
#
# 使用者選了「簡體」就不該看到繁體字，選了「繁體」也不該看到簡體字。
# RIME 的簡繁開關做不到這件事 —— 它是 opencc 的**字形轉換**，不是**字集篩選**，
# 而 luna_pinyin 的詞庫收了整個 CJK 基本區 + 擴展 A/B（粵語字、日本新字体、
# 部首、異體字都在裡面），那些字沒有簡繁對應，轉幾次都還在。
#
# 兩件事，順序不能反：
#   1. 補充轉換表：轉得掉的先轉掉。轉掉**不損失候選**，濾掉會。
#   2. 字集守門（lua_filter）：轉不掉的才濾。
# 判準、產生方式與已知缺口見 scripts/gen_charset_data.py 的檔頭。
#
# ⚠ 這一層**做不到絕對純度**，四端的 UI 都不可以宣稱它做得到。
#   見 docs/settings-model.md §4.7。
#
# 為什麼獨立成一支而不是寫在 collect_data.sh 裡：collect_data.sh 會
# `rm -rf core/data/shared` 再重建，而各條線的 worktree 常常把那個目錄
# symlink 到別人的。要「只補這幾個檔」的時候得有一個不會砍掉別人東西的入口。
#
# 用法：
#   scripts/collect_charset_guard.sh                 # 對 core/data/shared
#   scripts/collect_charset_guard.sh <shared 目錄>   # 對指定目錄
#
set -euo pipefail

# ⛔ **唯讀出口。** `scripts/verify_script_readonly.sh` 會把每一支腳本的
#   `--help` 跑一遍 —— 這一支從前沒有,於是 `--help` 會被當成一般啟動,
#   一路跑下去(編譯／連網／推檔)。說明不得有任何副作用。
case "${1:-}" in
  -h|--help)
    sed -n '2,/^set -[eu]/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_SHARED="${1:-$ROOT/core/data/shared}"
LUA_SRC="$ROOT/core/data/lua"
OCC_SRC="$ROOT/core/data/opencc"
CHARSET_PATCH="$ROOT/core/data/schemas/luminakey_charset.custom.yaml"
# 模糊音那一段。它是**接在** luna_pinyin.custom.yaml 尾巴的續集，
# 不是獨立的方案 patch —— 理由見該檔檔頭。
FUZZY_PATCH="$ROOT/core/data/schemas/luminakey_fuzzy_pinyin.yaml"

die()  { echo "錯誤: $*" >&2; exit 1; }
note() { echo "  $*"; }

[ -d "$OUT_SHARED" ] || die "$OUT_SHARED 不存在"

# 方案 patch 掛上去之後，底下每一個檔案都變成**必要**的。
# 少了任何一個的下場不是「守門沒生效」，是 lua 模組載入失敗 ——
# 實測那時 librime 會把整段候選變成**空的**，Android 接著把拼音字母上屏。
SRC_FILES=(
  "$LUA_SRC/luminakey_charset.lua"
  "$LUA_SRC/luminakey_charset_hans.lua"
  "$LUA_SRC/luminakey_charset_hant.lua"
  "$OCC_SRC/luminakey_t2s.json"
  "$OCC_SRC/luminakey_t2s_extra.txt"
  "$OCC_SRC/luminakey_t2tw.json"
  "$OCC_SRC/luminakey_t2tw_extra.txt"
  "$CHARSET_PATCH"
  "$FUZZY_PATCH"
)
for f in "${SRC_FILES[@]}"; do
  [ -f "$f" ] || die "缺少 $f"
done

# librime-lua 的 require 只看 <資料目錄>/lua/<名字>.lua，放在根目錄它找不到。
# （實測：module 'luminakey_charset' not found，然後整段候選變成空的。）
mkdir -p "$OUT_SHARED/lua" "$OUT_SHARED/opencc"
cp "$LUA_SRC"/luminakey_charset*.lua "$OUT_SHARED/lua/"
cp "$OCC_SRC"/luminakey_*.json "$OCC_SRC"/luminakey_*.txt "$OUT_SHARED/opencc/"

# 一份 patch 複製成三份。librime 會自動把 `xxx.custom.yaml` 套在
# `xxx.schema.yaml` 上，所以不必動上游的方案檔（日後更新才不會衝突）；
# `luna_pinyin_tw` 與 `bopomofo_tw` 靠 `__include:` 繼承母方案，一併吃到。
for s in luna_pinyin bopomofo t9_pinyin; do
  cp "$CHARSET_PATCH" "$OUT_SHARED/$s.custom.yaml"
done

# ── 模糊音：**只**接在 luna_pinyin 上 ────────────────────────────────────
# 理由（注音沒有拼音 algebra、九宮格已經折疊過）與選了哪幾組，
# 全寫在被接上去的那個檔案的檔頭裡。
#
# 用 append 而不是另開一個檔案：librime 一個方案只讀得到一份
# `<schema>.custom.yaml`，第二份不會被讀到 —— 那會是一個「檔案在、
# 規則沒生效」而且完全不報錯的坑。
cat "$FUZZY_PATCH" >> "$OUT_SHARED/luna_pinyin.custom.yaml"
note "模糊音：6 組接在 luna_pinyin.custom.yaml（luna_pinyin_tw 靠 __include 繼承）"

# ── 放完再驗一次。這裡驗的是**產物**，不是來源。 ──────────────────────────
for s in luna_pinyin bopomofo t9_pinyin; do
  [ -f "$OUT_SHARED/$s.custom.yaml" ] || die "缺少 $OUT_SHARED/$s.custom.yaml"
done
for f in lua/luminakey_charset.lua lua/luminakey_charset_hans.lua \
         lua/luminakey_charset_hant.lua \
         opencc/luminakey_t2s.json opencc/luminakey_t2s_extra.txt \
         opencc/luminakey_t2tw.json opencc/luminakey_t2tw_extra.txt; do
  [ -f "$OUT_SHARED/$f" ] || die "缺少 $OUT_SHARED/$f（會讓候選變空）"
done
# patch 指到的 opencc 設定檔必須真的存在，而且設定檔指到的詞典也要在。
# 這一條抓的是「改了檔名忘了改另一邊」——那種錯只有在裝置上才會爆。
for cfg in luminakey_t2s.json luminakey_t2tw.json; do
  while read -r want; do
    [ -f "$OUT_SHARED/opencc/$want" ] || die "$cfg 指到 $want，但 opencc/ 裡沒有"
  done < <(grep -oE '"file"[[:space:]]*:[[:space:]]*"[^"]+"' "$OUT_SHARED/opencc/$cfg" \
           | sed 's/.*"\([^"]*\)"$/\1/')
done
# ⚠ 驗的是**產物**：接上去了沒。少了這一條，cat 失敗（例如來源被改名）
#   之後 luna_pinyin.custom.yaml 仍然存在、字集守門仍然生效，
#   而模糊音靜靜地沒有接上 —— 那正是這個專案反覆吃虧的形狀。
grep -q '"speller/algebra"' "$OUT_SHARED/luna_pinyin.custom.yaml" \
  || die "模糊音沒有接上 luna_pinyin.custom.yaml"
for s in bopomofo t9_pinyin; do
  ! grep -q '"speller/algebra"' "$OUT_SHARED/$s.custom.yaml" \
    || die "模糊音接到了 $s —— 那一份不該有（見 luminakey_fuzzy_pinyin.yaml 檔頭）"
done

note "字集守門：lua/ 3 個檔、opencc 4 個檔、3 份方案 patch"
