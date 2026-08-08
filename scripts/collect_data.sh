#!/usr/bin/env bash
#
# collect_data.sh — 組裝 librime 的執行期資料
#
# 交叉編譯只產出程式碼。librime 啟動時還需要兩類資料，缺任何一類輸入法都打不出字：
#
#   1. RIME schema（方案定義 + 詞庫 + 語言模型）
#   2. OpenCC 的 .ocd2 詞典 —— 簡繁轉換用。交叉編譯時 opencc 的 Dictionaries
#      target 會執行剛編出來的 Android 版 opencc_dict，在交叉編譯環境必炸，
#      所以那一步被刻意繞開了。.ocd2 必須另外用 host 版 opencc 產生，本腳本負責。
#
# 產出：
#   core/data/shared/   → 對應 RimeTraits.shared_data_dir（唯讀，打包進 APK assets）
#   core/data/user/     → 對應 RimeTraits.user_data_dir 的初始內容（首次啟動時複製，之後可寫）
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$ROOT/third_party"
DATA="$TP/rime-data"
OPENCC_INSTALL="$TP/build/host-opencc-install/share/opencc"
OUT_SHARED="$ROOT/core/data/shared"
OUT_USER="$ROOT/core/data/user"

die() { echo "錯誤: $*" >&2; exit 1; }
note() { echo "  $*"; }

# --------------------------------------------------------------- 前置檢查 ---
[ -d "$DATA" ] || die "找不到 $DATA。先執行 scripts/fetch_rime_data.sh"
[ -d "$OPENCC_INSTALL" ] || die "找不到 host 版 opencc 產物 $OPENCC_INSTALL。
先建置 host 版 opencc（交叉編譯的那一份不能用來產生 .ocd2）：
  cmake -S third_party/librime/deps/opencc -B third_party/build/host-opencc \\
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_GTEST=OFF \\
        -DENABLE_BENCHMARK=OFF -DBUILD_PYTHON=OFF -DOPENCC_ENABLE_INSTALL=ON \\
        -DCMAKE_INSTALL_PREFIX=\$PWD/third_party/build/host-opencc-install
  cmake --build third_party/build/host-opencc -j && cmake --install third_party/build/host-opencc"

rm -rf "$OUT_SHARED" "$OUT_USER"
mkdir -p "$OUT_SHARED/opencc" "$OUT_USER"

# 從 <repo> 複製指定檔案，缺檔即失敗（寧可早點炸，也不要出一個裝了打不出字的 APK）
take() {
  local repo="$1"; shift
  for f in "$@"; do
    [ -f "$DATA/$repo/$f" ] || die "$repo 缺少 $f"
    cp "$DATA/$repo/$f" "$OUT_SHARED/"
  done
  note "$repo: $*"
}

echo "=== 1. 基礎配置（rime-prelude）==="
take rime-prelude default.yaml key_bindings.yaml punctuation.yaml symbols.yaml

echo "=== 2. 語言模型（rime-essay）==="
take rime-essay essay.txt

echo "=== 3. 拼音方案（rime-luna-pinyin）==="
# luna_pinyin 的詞庫本身是繁體，簡體透過 opencc simplifier 產生；
# _tw 變體把字形標準設成臺灣。
take rime-luna-pinyin luna_pinyin.schema.yaml luna_pinyin_tw.schema.yaml \
                      luna_pinyin.dict.yaml pinyin.yaml

echo "=== 4. 注音方案（rime-bopomofo）==="
take rime-bopomofo bopomofo.schema.yaml bopomofo_tw.schema.yaml zhuyin.yaml

echo "=== 5. 注音的相依詞庫 ==="
# 注音方案的 translator 指定 dictionary: terra_pinyin，不是 luna_pinyin。
# 少了這份，部署 bopomofo 會失敗。
take rime-terra-pinyin terra_pinyin.schema.yaml terra_pinyin.dict.yaml
# 注音方案有一組以筆畫反查的 reverse_lookup translator。
take rime-stroke stroke.schema.yaml stroke.dict.yaml

echo "=== 5.5 九宮格拼音（本專案自撰，非上游）==="
# 九宮格是一套 RIME 方案，不是前端硬湊 —— Engine::ProcessKey 一次只吃一個
# keycode，「一鍵三字母」的模糊性只能由 speller 的 algebra 與 prism 承擔。
# 它共用 luna_pinyin 的詞典，只會多編一份 t9_pinyin.prism.bin。
T9="$ROOT/core/data/schemas/t9_pinyin.schema.yaml"
[ -f "$T9" ] || die "缺少 $T9"
cp "$T9" "$OUT_SHARED/"
note "t9_pinyin.schema.yaml（詞典共用 luna_pinyin，不需額外詞庫）"

echo "=== 6. OpenCC 詞典 ==="
cp "$OPENCC_INSTALL"/*.ocd2 "$OUT_SHARED/opencc/"
cp "$OPENCC_INSTALL"/*.json "$OUT_SHARED/opencc/"
note "$(ls "$OUT_SHARED/opencc"/*.ocd2 | wc -l) 個 .ocd2、$(ls "$OUT_SHARED/opencc"/*.json | wc -l) 個 .json"

echo "=== 7. 使用者初始配置 ==="
# rime-prelude 的 default.yaml 列了 cangjie5 / quick5 / stroke / terra_pinyin 等
# 我們沒有全部提供的方案。與其改動上游檔案（日後更新會衝突），
# 用 RIME 慣用的 default.custom.yaml 做 patch，只列實際提供的方案。
cat > "$OUT_USER/default.custom.yaml" <<'YAML'
# 由 scripts/collect_data.sh 產生。
#
# 上游 rime-prelude 的 default.yaml 列出的方案多於本專案實際打包的，
# 未打包的方案會在部署時報錯。這裡以 patch 覆寫 schema_list，
# 只保留確實有詞庫的方案 —— 這樣上游 default.yaml 可以原封不動地更新。
# t9_pinyin 是本專案自撰的九宮格方案，不在上游 default.yaml 裡，一併在此列入。
patch:
  schema_list:
    - schema: luna_pinyin_tw    # 拼音（臺灣字形）
    - schema: bopomofo_tw       # 注音（臺灣字形）
    - schema: luna_pinyin       # 拼音（原版）
    - schema: t9_pinyin         # 九宮格拼音（本專案自撰，共用 luna_pinyin 詞典）
YAML
note "default.custom.yaml（schema_list 限縮為實際打包的四個方案）"

# --------------------------------------------------------------- 完整性檢查 ---
echo
echo "=== 完整性檢查 ==="
# 每個 schema_list 裡的方案都必須有對應的 .schema.yaml
for s in luna_pinyin_tw bopomofo_tw luna_pinyin t9_pinyin; do
  [ -f "$OUT_SHARED/$s.schema.yaml" ] || die "schema_list 列了 $s 但沒有 $s.schema.yaml"
done
note "schema_list 的四個方案都有對應檔案"

# 每個 schema 引用的 dictionary 都必須有對應的 .dict.yaml
#
# ⚠ 抽取器不可以用 grep -oP。-P 與 \K 是 GNU/PCRE 專屬,BSD grep(macOS)
#   根本沒有 -P,會直接以 exit 2 拒絕。原本這行寫成
#       < <(grep -oP '...\K...' "$f" 2>/dev/null || true)
#   三層消音之後(2>/dev/null 吃掉錯誤訊息、|| true 吃掉結束碼、
#   process substitution 裡的失敗又不觸發 set -e),在 macOS 上迴圈跑零次、
#   missing 恆為 0,於是它**每一次都印「所有 schema 引用的詞庫都齊全」**,
#   不管詞庫在不在 —— 一個永遠不會發現問題的檢查。
#   (在真的 macOS 上以兩本刻意缺席的詞庫重現過。)
#   改用 awk:BSD 與 GNU 的行為一致,且不需要 PCRE。
missing=0
refs=0
for f in "$OUT_SHARED"/*.schema.yaml; do
  while read -r d; do
    [ -z "$d" ] && continue
    [ "$d" = '""' ] && continue
    d="${d//\"/}"
    [ -z "$d" ] && continue
    refs=$((refs + 1))
    if [ ! -f "$OUT_SHARED/$d.dict.yaml" ]; then
      echo "  [警告] $(basename "$f") 引用 dictionary: $d，但沒有 $d.dict.yaml"
      missing=$((missing + 1))
    fi
  done < <(awk '/^[[:space:]]*dictionary:[[:space:]]/ { print $2 }' "$f")
done

# 「一個引用都沒抽到」本身就不可能(隨附的 schema 必定引用詞庫)。
# 少了這一條,抽取器將來若再被改壞,這個檢查會再一次靜靜地全綠 ——
# 那正是上面那個 bug 之所以能存在這麼久的原因。
[ "$refs" -gt 0 ] || die "在所有 .schema.yaml 裡一個 dictionary: 都沒抽到 —— 抽取器壞了,不是資料乾淨"

# 注意:這裡刻意維持「只警告、不中斷」。未列入 schema_list 的方案
# (例如隨附但不啟用的 bopomofo/stroke)引用缺檔是容許的,見下方訊息。
# 真正該擋的是 schema_list 那四個方案,那由上面的 die 與各端的驗證負責。
[ "$missing" -eq 0 ] && note "所有 schema 引用的詞庫都齊全（檢查了 $refs 個引用）" \
                     || echo "  共 $missing 個引用缺檔（若屬於未列入 schema_list 的方案則可忽略）"

echo
echo "=== 完成 ==="
echo "shared: $(du -sh "$OUT_SHARED" | cut -f1)  ($(find "$OUT_SHARED" -type f | wc -l) 個檔案)"
echo "user  : $(du -sh "$OUT_USER" | cut -f1)"
echo
echo "下一步：把 core/data/shared 打包進 APK assets，首次啟動時解到 filesDir，"
echo "        並把 core/data/user 的內容複製到 user_data_dir（僅當該檔尚不存在時）。"
