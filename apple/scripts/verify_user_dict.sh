#!/usr/bin/env bash
#
# verify_user_dict.sh — 證明「自己加的詞」那份 TSV 真的會改變輸出
#
# ── 為什麼單元測試不夠 ──────────────────────────────────────────────────
# 單元測試能證明我們**寫出了**格式正確的 custom_phrase.txt 與掛載用的
# <schema>.custom.yaml,但證明不了 librime **讀得懂**它們。
# 「檔案寫對了、格式其實沒生效」正是這個專案一再撞到的那一類缺陷:
# 畫面完全正常、自動化全過,而使用者加的詞永遠不會出現。
#
# 所以這一關走的是真的 librime:寫一個現實中不存在的詞,部署,打它的編碼,
# 斷言它真的出現在候選裡。docs/settings-model.md §5 的格式規範由這一關兜底。
#
# ── 2026-08-09:這一關紅了一輪,而紅的原因跟大家猜的都不一樣 ────────────
# 曾經的註解寫著「編碼必須是這個方案打得出來的音節」,並且把 `zzq zzq`
# 換成 `zhuang zhuang zhuang` —— **那個診斷是錯的,所以換完還是紅的。**
# 真正的原因是**空格**:
#
#   librime 的 TableTranslator::Query() 把使用者按出來的**原始字串**
#   (`zhuangzhuangzhuang`)直接拿去查 stabledb,而 stabledb 的鍵是
#   table_db.cc 組出來的 `編碼 + " \t" + 詞`。兩邊要逐字元相等才算命中。
#   編碼欄裡有空格 → 永遠不會命中 → 沒有候選、**也沒有任何錯誤訊息**。
#
# 順帶推翻的三個猜測(都實測過):`engine/translators/@next` 這個 patch
# **有**生效;`db_class: stabledb` **不**需要先編譯成二進位;
# `dictionary: ""` 的 table_translator **可以**沒有 prism。
#
# ── 這一關驗四件事,少一件都會讓它變成一張空頭支票 ──────────────────────
#   1. 沒加詞時打不出來        —— 少了它,「詞本來就在」也會綠
#   2. 內建方案 + 只覆寫參數    —— 我們對內建方案實際寫出去的那一種掛載檔
#   3. 自帶翻譯器的方案         —— 對市集方案的那一種掛載檔(多一行 @next)
#   4. **空白分隔的編碼必須紅** —— 反向測試。少了它,這一關證明不了
#      自己對「編碼格式」敏感;它可能只是在驗「檔案讀得到」而已。
#
# ⚠ 這一關會**動 core/data/user**(那是 CI 上的暫時目錄),跑完會還原。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONSOLE="${ROOT}/apple/build/rime_console"
SHARED="${ROOT}/core/data/shared"
USER_DIR="${ROOT}/core/data/user"
SCHEMA="luna_pinyin_tw"

WORD="測試詞彙鑰匙"
# ⚠ 編碼 = 使用者按出來的那一串鍵,原樣。**不要加空格。**
CODE="zhuangzhuangzhuang"
KEYS="zhuangzhuangzhuang"
# 被推翻的舊寫法。第 4 步用它當反向測試,必須查不到。
BAD_CODE="zhuang zhuang zhuang"

[ -x "${CONSOLE}" ] || { echo "找不到 ${CONSOLE} —— 先跑 apple/scripts/build_macos.sh" >&2; exit 2; }
[ -d "${SHARED}" ] || { echo "找不到 ${SHARED}" >&2; exit 2; }

mkdir -p "${USER_DIR}"
PHRASES="${USER_DIR}/custom_phrase.txt"
MOUNT="${USER_DIR}/${SCHEMA}.custom.yaml"
BUILT="${USER_DIR}/build/${SCHEMA}.schema.yaml"
BACKUP="$(mktemp -d)"
[ -f "${PHRASES}" ] && cp "${PHRASES}" "${BACKUP}/phrases"
[ -f "${MOUNT}" ] && cp "${MOUNT}" "${BACKUP}/mount"

restore() {
  if [ -f "${BACKUP}/phrases" ]; then cp "${BACKUP}/phrases" "${PHRASES}"; else rm -f "${PHRASES}"; fi
  if [ -f "${BACKUP}/mount" ]; then cp "${BACKUP}/mount" "${MOUNT}"; else rm -f "${MOUNT}"; fi
  rm -rf "${BACKUP}"
}
trap restore EXIT INT TERM

fail=0
OUT=""

write_phrases() {  # $1 = 編碼
  {
    printf '# LuminaKey 使用者詞庫 / user phrases — format 1\n'
    printf '%s\t%s\t99\n' "${WORD}" "$1"
  } > "${PHRASES}"
}

# 內建方案用的掛載檔:方案自己已經有 table_translator@custom_phrase,
# 我們只覆寫參數。**多接一個的話,沒有 uniquifier 的方案會出現兩筆一樣的候選。**
write_mount_params_only() {
  cat > "${MOUNT}" <<'YAML'
# luminakey-managed: custom_phrase v1
patch:
  custom_phrase:
    dictionary: ""
    user_dict: custom_phrase
    db_class: stabledb
    enable_completion: false
    enable_sentence: false
    initial_quality: 99
YAML
}

# 沒有自帶翻譯器的方案(市集下載的多半是這種)用的掛載檔。
write_mount_with_translator() {
  cat > "${MOUNT}" <<'YAML'
# luminakey-managed: custom_phrase v1
patch:
  "engine/translators/@next": table_translator@custom_phrase
  custom_phrase:
    dictionary: ""
    user_dict: custom_phrase
    db_class: stabledb
    enable_completion: false
    enable_sentence: false
    initial_quality: 99
YAML
}

# 回傳 0 = 那個詞出現在候選裡(或被上屏)。
# ⚠ 每次都先砍掉 build/:librime 只在設定檔變動時才重編,
#   沿用上一格留下來的編譯結果會讓後面幾格驗到的是**前一格的方案**。
run_once() {
  local rc
  rm -rf "${USER_DIR}/build"
  OUT="$("${CONSOLE}" "${SHARED}" "${USER_DIR}" "${KEYS}" 1 "${SCHEMA}" 2>&1)"
  rc=$?
  if [ "${rc}" -ne 0 ]; then
    printf '%s\n' "${OUT}" | grep -vE '^[WIE][0-9]{8} ' >&2
    echo "  !! rime_console 結束碼 ${rc}" >&2
    return 2
  fi
  # 候選行的格式是 `        <label>. <text><comment>`(rime_console.cc:65);
  # 上屏行是 `>>> COMMIT: "<text>"`。兩者任一出現都算數 ——
  # 我們要證明的是「librime 讀到了這個詞」,不是「它排第幾」。
  printf '%s\n' "${OUT}" | grep -qF "${WORD}"
}

diagnose() {
  echo "   --- 編譯後的方案裡有沒有 custom_phrase ---" >&2
  if [ -f "${BUILT}" ]; then
    grep -n "custom_phrase" "${BUILT}" | head -12 | sed 's/^/      /' >&2 \
      || echo "      (一個都沒有 —— 掛載的 patch 整段沒有被套用)" >&2
  else
    echo "      (找不到 ${BUILT} —— 方案根本沒編出來)" >&2
  fi
  echo "   --- rime_console 的輸出(去掉 glog)---" >&2
  printf '%s\n' "${OUT}" | grep -vE '^[WIE][0-9]{8} ' | tail -30 >&2
}

# ── 1/4 先證明它現在打不出來 ───────────────────────────────────
# 少了這一步,這一關在「那個詞本來就在詞典裡」時會假性通過,
# 而我們什麼都沒驗到。
echo "=== 1/4 沒有加過詞時,${KEYS} 不該出現「${WORD}」==="
rm -f "${PHRASES}" "${MOUNT}"
if run_once; then
  echo "!! 還沒加詞就看得到「${WORD}」—— 這一關驗不到東西,換一個更冷僻的詞。" >&2
  exit 1
fi
echo "  ✓ 如預期沒有出現"

# ── 2/4 內建方案:只覆寫參數的掛載檔 ────────────────────────────
echo "=== 2/4 內建方案(只覆寫參數的掛載檔):必須出現「${WORD}」==="
write_phrases "${CODE}"
write_mount_params_only
if run_once; then
  echo "  ✓ 生效:${KEYS} → 「${WORD}」"
  printf '%s\n' "${OUT}" | grep -F "${WORD}" | head -2 | sed 's/^/      /'
else
  echo "!! 加了詞卻看不到。" >&2
  echo "   檢查:欄位分隔是不是真的 TAB;" >&2
  echo "         編碼欄有沒有混進空格(**這是上一次紅掉的原因**);" >&2
  echo "         user_dict 是不是 custom_phrase(不含副檔名);" >&2
  echo "         db_class 是不是 stabledb(userdb 讀的是 LevelDB,不是文字檔)。" >&2
  diagnose
  fail=1
fi

# ── 2b 這一種掛載檔不可以讓翻譯器變成兩個 ───────────────────────
# 兩個翻譯器讀同一份檔案 = 同一個詞在選字窗裡出現兩次。
# 內建的兩個方案剛好有 uniquifier 會把它收掉,所以這件事看不見 ——
# 而市集下載的方案沒有人保證有 uniquifier。這裡直接數編譯後的結果。
echo "=== 2b 編譯後的方案裡,custom_phrase 翻譯器必須剛好一個 ==="
if [ -f "${BUILT}" ]; then
  N="$(grep -c 'table_translator@custom_phrase' "${BUILT}")"
  if [ "${N}" -eq 1 ]; then
    echo "  ✓ 剛好一個"
  else
    echo "!! 有 ${N} 個 table_translator@custom_phrase(應該剛好 1 個)。" >&2
    echo "   ${SCHEMA} 自己就帶了一個,掛載檔不該再接一個 ——" >&2
    echo "   沒有 uniquifier 的方案會因此在選字窗裡出現兩筆一樣的詞。" >&2
    echo "   見 UserPhrases.schemaAlreadyMountsPhrases。" >&2
    fail=1
  fi
else
  echo "!! 找不到 ${BUILT},這一項驗不到 —— 當成失敗,不是當成通過。" >&2
  fail=1
fi

# ── 3/4 沒有自帶翻譯器的方案:掛載檔要多接一行 ──────────────────
echo "=== 3/4 需要自己接翻譯器的掛載檔:也必須出現「${WORD}」==="
write_mount_with_translator
if run_once; then
  echo "  ✓ 生效"
else
  echo "!! 接上翻譯器的那一種掛載檔失效了 —— 市集下載的方案會打不出使用者的詞。" >&2
  diagnose
  fail=1
fi

# ── 4/4 反向測試:空白分隔的編碼**必須**查不到 ──────────────────
# 這一步在證明「這一關對編碼格式是敏感的」。少了它,前面幾步全綠也可能
# 只代表「檔案讀得到」,而不代表我們寫的編碼格式是對的 ——
# 那正是這一關上一輪紅掉、而註解卻歸咎於別的原因的那個處境。
echo "=== 4/4 反向測試:編碼寫成「${BAD_CODE}」時必須查不到 ==="
write_phrases "${BAD_CODE}"
write_mount_params_only
if run_once; then
  echo "!! 空白分隔的編碼竟然也查得到 —— 那表示前面三步證明不了編碼格式,"  >&2
  echo "   這一關已經不是在驗它宣稱在驗的東西了。先修這一關,再信它。" >&2
  fail=1
else
  echo "  ✓ 如預期查不到(所以前面三步的綠是有意義的)"
fi

if [ "${fail}" -ne 0 ]; then
  echo "" >&2
  echo "使用者加的詞沒有真的生效。docs/settings-model.md §5 與 librime 對不上。" >&2
  exit 1
fi
echo ""
echo "全部通過:使用者加的詞真的會改變輸出。"
exit 0
