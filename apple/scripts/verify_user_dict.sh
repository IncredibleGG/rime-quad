#!/usr/bin/env bash
#
# verify_user_dict.sh — 證明「我的詞庫」那份 TSV 真的會改變輸出
#
# ── 為什麼單元測試不夠 ──────────────────────────────────────────────────
# 單元測試能證明我們**寫出了**格式正確的 custom_phrase.txt 與掛載用的
# <schema>.custom.yaml,但證明不了 librime **讀得懂**它們。
# 「檔案寫對了、格式其實沒生效」正是這個專案一再撞到的那一類缺陷:
# 畫面完全正常、自動化全過,而使用者加的詞永遠不會出現。
#
# 所以這一關走的是真的 librime:寫一個現實中不存在的詞,部署,打它的編碼,
# 斷言它真的上屏。docs/settings-model.md §5 的格式規範由這一關兜底。
#
# ⚠ 這一關會**動 core/data/user**(那是 CI 上的暫時目錄),跑完會還原。
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONSOLE="${ROOT}/apple/build/rime_console"
SHARED="${ROOT}/core/data/shared"
USER_DIR="${ROOT}/core/data/user"
SCHEMA="luna_pinyin_tw"

# 刻意選一個**不可能**出現在任何詞庫裡的詞與編碼。
# 用真實的詞會讓這一關在詞庫剛好也有那個詞時假性通過。
WORD="測試詞彙鑰匙"
CODE="zzq zzq"
KEYS="zzqzzq"

[ -x "${CONSOLE}" ] || { echo "找不到 ${CONSOLE} —— 先跑 apple/scripts/build_macos.sh" >&2; exit 2; }
[ -d "${SHARED}" ] || { echo "找不到 ${SHARED}" >&2; exit 2; }

mkdir -p "${USER_DIR}"
PHRASES="${USER_DIR}/custom_phrase.txt"
MOUNT="${USER_DIR}/${SCHEMA}.custom.yaml"
BACKUP="$(mktemp -d)"
[ -f "${PHRASES}" ] && cp "${PHRASES}" "${BACKUP}/phrases"
[ -f "${MOUNT}" ] && cp "${MOUNT}" "${BACKUP}/mount"

restore() {
  if [ -f "${BACKUP}/phrases" ]; then cp "${BACKUP}/phrases" "${PHRASES}"; else rm -f "${PHRASES}"; fi
  if [ -f "${BACKUP}/mount" ]; then cp "${BACKUP}/mount" "${MOUNT}"; else rm -f "${MOUNT}"; fi
  rm -rf "${BACKUP}"
}
trap restore EXIT INT TERM

run_once() {
  local out rc
  out="$("${CONSOLE}" "${SHARED}" "${USER_DIR}" "${KEYS}" 1 "${SCHEMA}" 2>&1)"
  rc=$?
  if [ "${rc}" -ne 0 ]; then
    printf '%s\n' "${out}" | grep -vE '^[WIE][0-9]{8} ' >&2
    echo "  !! rime_console 結束碼 ${rc}" >&2
    return 2
  fi
  printf '%s\n' "${out}" | grep -q "^>>> COMMIT: \"${WORD}\"$"
}

# ── 第一步:**先證明它現在打不出來** ────────────────────────────
# 少了這一步,這一關在「那個詞本來就在詞庫裡」時會假性通過,
# 而我們什麼都沒驗到。
echo "=== 1/2 沒有詞庫時,${KEYS} 不該打出「${WORD}」==="
rm -f "${PHRASES}" "${MOUNT}"
if run_once; then
  echo "!! 還沒加詞就打得出「${WORD}」—— 這一關驗不到東西,換一個更冷僻的詞。" >&2
  exit 1
fi
echo "  ✓ 如預期打不出來"

# ── 第二步:加詞之後必須打得出來 ────────────────────────────────
echo "=== 2/2 加進詞庫之後,${KEYS} 必須打出「${WORD}」==="
{
  printf '# RimeQuad 使用者詞庫 / user phrases — format 1\n'
  printf '%s\t%s\t99\n' "${WORD}" "${CODE}"
} > "${PHRASES}"

cat > "${MOUNT}" <<'YAML'
# rimequad-managed: custom_phrase v1
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

if run_once; then
  echo "  ✓ 使用者詞庫生效:${KEYS} → 「${WORD}」"
  exit 0
fi

echo "!! 加了詞卻打不出來 —— docs/settings-model.md §5 的格式與 librime 對不上。" >&2
echo "   檢查:custom_phrase.txt 的欄位分隔是不是真的 TAB;" >&2
echo "         <schema>.custom.yaml 的 user_dict 是不是 custom_phrase(不含副檔名);" >&2
echo "         db_class 是不是 stabledb(userdb 讀的是 LevelDB,不是文字檔)。" >&2
exit 1
