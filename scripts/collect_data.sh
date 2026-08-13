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

# ── 一頁幾個候選 ───────────────────────────────────────────────────────────
#
# 上游 rime-prelude 寫的是 `menu/page_size: 5`，那是**桌面版的預設，從來沒有
# 為這個產品重新決定過**。使用者的回報是「候選詞只有三個」，三層量下來：
#
#   引擎（rime_console，繁體，打 nihao）              5 個
#   畫面（emulator-5558，1080×2400 @420dpi，直式）
#       預設字級          完整畫得出 3 個，第 4 個只畫得出半個字
#       font_scale 1.30   3 個
#       九宮格            3 個（每一項都掛著讀音，項寬幾乎翻倍）
#   橫式                  5 個全上，右邊還空著一大半
#
# 為什麼是 9（不是抄競品，理由三條）：
#
#   1. ⚠ **這一條原本寫的是「這個產品自己早就說是 9 —— 設定頁的『一次顯示
#      幾個字』是 3/5/7/9/不限，9 是使用者選得到的最大值」。那句話已經撤掉，
#      因為它同時是假的、而且本來就不該當理由。**
#
#      · **假的。** 批 1（G07）量到 7 / 9 / 不限三檔按了沒反應（引擎那時
#        真的只給 5），把檔位砍成了 3 / 4 / 5。兩批併起來之後，「設定頁最大
#        是 9」不成立 —— 最大是 5。
#      · **不該當理由。** 設定頁的檔位**現在是從這一行產生的**：
#        page_size → generateEnginePageSize（android/app/build.gradle.kts）
#        → PrefLevels.ENGINE_PAGE_SIZE → 檔位清單。
#        拿它回頭證明它自己是繞一圈的循環論證，兩邊會一起漂而沒有人發現。
#
#      撐住 9 的是底下的 2 與 3，那兩條都與設定頁無關。
#   2. **9 是「每一個候選都還叫得出名字」的上界。** 候選列每一項都印著序號，
#      無障礙也照著念（「候選字第 3 個」）。librime 預設的 select_keys 是
#      `1234567890` 十個字；第 11 個之後 core/src/rime_shell.cc 的 pick_label
#      退回 `index+1`，畫面上會出現「…0、11、12」這種數不下去的序號。
#      10 是硬上界，9 是最後一個乾淨的值。
#   3. **9 是量出來畫得完的量。** 直式一列畫得下 3–4 個，其餘走 §8.6.6 的
#      展開面板（2–3 欄 × 3 列，一屏掃得完）；橫式一列就放得下。
#
# ⚠ **為什麼改 shared/default.yaml 而不是 user/default.custom.yaml。**
#   user/ 那一份在升級時是「只補不覆蓋」（android RimeRuntime 的註解寫得很
#   清楚，schema_list 還要靠 BuiltinMigration 逐項補）—— 把 page_size 放在
#   那裡，**只有全新安裝的人拿得到**，舊使用者永遠停在 5，而且沒有任何徵狀。
#   shared/ 是唯讀、隨 APK 摘要整份汰換的，改在這裡才會真的到使用者手上。
#
# ⚠ 這是本腳本唯一一處**改寫上游檔案內容**的地方，所以它必須「改不到就爆」：
#   上游哪天把這個鍵改名或改值，底下的比對會失敗、整個 collect 停住，
#   而不是靜靜地留下 5 —— 那正是「畫面看起來一切正常」的那一類缺陷。
if ! grep -qE '^  page_size: 5$' "$OUT_SHARED/default.yaml"; then
  die "rime-prelude 的 default.yaml 裡找不到 '  page_size: 5'。
上游可能改了鍵名或預設值。確認之後再更新這一段 —— 不要直接拿掉這道檢查,
拿掉的下場是候選數靜靜地退回上游的桌面預設。"
fi
# ⚠ 不用 sed -i：GNU sed 的 -i 後綴是選用的，BSD/macOS sed 的**是必要的**。
#   sed -i 's/…/' file 在 macOS 上會把指令碼當成備份後綴、把檔名當成指令碼，
#   於是一個檔案運算元都不剩、改讀 stdin，死在：
#       sed: -I or -i may not be used with stdin
#   四端共用的腳本不能只在 Linux 上對 —— 這一條是 macOS 車道實際紅
#   出來的（b1/cand 兩批在合進 ship 之前從沒有上過 macOS 車道）。
#   寫暫存檔再換過去：兩種 sed 都吃得下，而且不必為 -i '' 分平台。
_page_size_tmp="$OUT_SHARED/default.yaml.tmp.$$"
sed 's/^  page_size: 5$/  page_size: 9/' "$OUT_SHARED/default.yaml" > "$_page_size_tmp"
mv "$_page_size_tmp" "$OUT_SHARED/default.yaml"
grep -qE '^  page_size: 9$' "$OUT_SHARED/default.yaml" || die "page_size 沒有改成 9"
note "menu/page_size: 5 → 9（上游是桌面預設；9 = 序號還叫得出名字的上界，且量得出畫得完）"

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

echo "=== 6.5 字集守門（本專案自撰）==="
# 選了「簡體」就不該看到繁體字，選了「繁體」也不該看到簡體字。
# RIME 的簡繁開關做不到 —— 那是 opencc 的**字形轉換**，不是**字集篩選**。
# 這一步放兩樣東西進去：補充轉換表（轉得掉的先轉掉，不損失候選）與
# 字集守門的 lua_filter（轉不掉的才濾）。細節在被呼叫的那一支裡。
"$ROOT/scripts/collect_charset_guard.sh" "$OUT_SHARED"

echo "=== 7. 使用者初始配置 ==="
# rime-prelude 的 default.yaml 列了 cangjie5 / quick5 / stroke / terra_pinyin 等
# 我們沒有全部提供的方案。與其改動上游檔案（日後更新會衝突），
# 用 RIME 慣用的 default.custom.yaml 做 patch，只列實際提供的方案。
#
# ── 兩份，共用同一段本文 ───────────────────────────────────────────────
#   default.custom.yaml         四端都用（桌面裝的就是這一份）
#   default.custom.mobile.yaml  行動端用：同一段 **加上**只有觸控鍵盤才成立的
#                               那幾條（見下面的 MOBILE_ONLY）
#
# 為什麼要兩份而不是一份加旗標：`default.custom.yaml` 是 librime 的固定檔名，
# 一個資料目錄只讀得到一份。而 core/data 是四端共用的，桌面拿到的必須是
# 沒有行動端那幾條的版本。
#
# 為什麼不是兩個獨立檔案各寫一次：schema_list 抄成兩份必定腐爛（改了一邊
# 忘了另一邊，症狀是「某一端的方案清單少一個」，而且只有裝上去才看得到）。
# 這裡本文只寫一次，行動端那一份是「本文 + 追加」。
COMMON_PATCH=$(cat <<'YAML'
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
)

# 只有行動端成立的那幾條。**縮排要與上面的 patch: 底下對齊。**
MOBILE_ONLY=$(cat <<'YAML'

  # ── 行動端不套 paging_with_comma_period ──────────────────────────────
  #
  # 上游 default.yaml 的 key_binder 綁了
  #   { when: paging,   accept: comma,  send: Page_Up }
  #   { when: has_menu, accept: period, send: Page_Down }
  #
  # 在**實體鍵盤**上那是個好慣例：手不用離開主排就能翻頁，而逗號句號
  # 隨時可以先上屏再打。在**觸控鍵盤**上它是缺陷：底列那顆「。」是使用者
  # 特地伸手去點的一顆標點鍵，點下去卻換了一頁候選 —— 他要的那個句號
  # 一個都沒有出來，而且畫面上看不出發生了什麼事。
  #
  # 實測（emulator-5558，luna_pinyin_tw，rime_console 打 `nihao.`）：
  #   組字後 preedit="nihao"，候選 5 個 (page 1)  ← 翻頁了，沒有句號
  #
  # 桌面端保留上游行為（它們裝的是 default.custom.yaml，沒有這一段）。
  #
  # ⚠ 這裡重寫整份 key_binder/bindings 的 __patch 清單，而不是「移除某幾條」：
  #   librime 的 key_binder 沒有「取消綁定」的語法，同一顆鍵綁兩次是先到先贏，
  #   所以唯一乾淨的作法是把清單重列一次、少列那一項。
  #   代價是上游日後在 default.yaml 的 key_binder 加新項目時這裡要跟著加 ——
  #   `scripts/verify_yaml_no_dup_keys.py` 之外沒有東西會提醒，記在這裡。
  key_binder/bindings:
    __patch:
      - key_bindings:/emacs_editing
      - key_bindings:/paging_with_minus_equal
      - key_bindings:/numbered_mode_switch
YAML
)

printf '%s\n' "$COMMON_PATCH" > "$OUT_USER/default.custom.yaml"
printf '%s\n%s\n' "$COMMON_PATCH" "$MOBILE_ONLY" > "$OUT_USER/default.custom.mobile.yaml"
note "default.custom.yaml（schema_list 限縮為實際打包的四個方案）"
note "default.custom.mobile.yaml（同上 ＋ 行動端不翻頁的逗號句號）"

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
