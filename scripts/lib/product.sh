# shellcheck shell=bash
#
# product.sh — 把 product.env 讀進 shell,並推導組合值。
#
#   . "$(dirname "${BASH_SOURCE[0]}")/lib/product.sh"
#   echo "$RS_ANDROID_IME_ID"
#
# 檔案裡的每個 KEY 會變成 RS_<KEY>。前綴是刻意的:腳本裡看到 RS_ 就知道
# 這個值來自 scripts/lib/product.env,不是就地寫死的。
#
# ⚠ 不用 `source product.env`。值裡有空格(「LuminaKey 輸入法」)、有反斜線
#   (%APPDATA%\LuminaKey)、有 | (UA 樣式),交給 shell 去 eval 一次就是一次
#   注入機會,而且 python 那邊沒辦法照抄同一套規則。逐行切第一個 `=` 兩邊
#   各自實作十行,反而是唯一能保證兩邊一致的做法。

_rs_product_load() {
  local env_file="${1:?}" line key val
  [ -f "$env_file" ] || { echo "product.sh: 找不到 $env_file" >&2; return 1; }
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      ''|'#'*) continue ;;
    esac
    case "$line" in
      *=*) ;;
      *) echo "product.sh: 這一行沒有 '=':$line" >&2; return 1 ;;
    esac
    key="${line%%=*}"
    val="${line#*=}"
    case "$key" in
      *[!A-Z0-9_]*|'') echo "product.sh: 不合法的鍵名:$key" >&2; return 1 ;;
    esac
    printf -v "RS_$key" '%s' "$val"
    export "RS_$key"
  done < "$env_file"
}

if ! _rs_product_load "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/product.env"; then
  # 讀不到就要停。半套的 RS_ 變數會讓下游腳本拿著空字串去跑 `ime set ""`,
  # 那種失敗看起來像裝置壞了,不像設定檔壞了。
  if [ "${BASH_SOURCE[0]}" = "${0}" ]; then exit 1; else return 1; fi
fi
unset -f _rs_product_load

# ── 推導值 ────────────────────────────────────────────────────────────────
# 只從上面那些原子事實推,不另外寫死。product.py 有一份逐字相同的推導,
# scripts/verify_product_ids.sh 每次都會比對兩邊的輸出。

# 系統認得的輸入法 id。`ime enable` / `ime set` / settings 裡存的都是它。
RS_ANDROID_IME_ID="${RS_ANDROID_APP_ID}/${RS_ANDROID_IME_SERVICE}"
# 套件名的目錄形式:org.luminakey.ime -> org/luminakey/ime
RS_ANDROID_PKG_PATH="${RS_ANDROID_APP_ID//./\/}"
# 唯一的連網出口。稽核腳本與 dex 檢查都問同一個類別。
RS_NETWORK_GATE_CLASS="${RS_ANDROID_APP_ID}.net.NetworkGate"
RS_NETWORK_GATE_REL="main/java/${RS_ANDROID_PKG_PATH}/net/NetworkGate.kt"
# 2026-08-16:RS_MACOS_TIS_HANT / RS_MACOS_TIS_HANS(TISInputSourceID)隨 macOS 端
# 退場 —— MACOS_BUNDLE_ID 已從 product.env 移除,留著會推出兩個空字串。
# product.py 的同一組推導同時刪除,否則 verify_product_ids.sh 的兩邊比對會紅。
# R2 的公開位址字首(bucket 內只准動 R2_PREFIX 底下)。
RS_R2_REMOTE="r2:tgapk/${RS_R2_PREFIX}"

export RS_ANDROID_IME_ID RS_ANDROID_PKG_PATH RS_NETWORK_GATE_CLASS \
       RS_NETWORK_GATE_REL RS_R2_REMOTE

# --dump:給 verify_product_ids.sh 拿去和 python 那邊逐字比對。
# 直接執行本檔(而不是 source)時才有作用。
if [ "${BASH_SOURCE[0]}" = "${0}" ] && [ "${1:-}" = "--dump" ]; then
  for _k in $(compgen -v | grep '^RS_' | sort); do
    printf '%s=%s\n' "$_k" "${!_k}"
  done
fi
