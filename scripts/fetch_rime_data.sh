#!/usr/bin/env bash
#
# fetch_rime_data.sh — 取得 RIME 的方案與詞庫原始資料
#
# `scripts/collect_data.sh` 需要這些原始資料才能組出 `core/data/`。
# 先前這一步是手動 clone 的，於是有兩個問題:
#
#   1. **CI 上不存在。** 換一台乾淨的機器就建不出能打字的 APK —— 建置會成功，
#      但產出的輸入法沒有任何方案。這種失敗不會被建置系統抓到。
#   2. **沒有釘版本。** clone 的是當下的 HEAD，上游改了我們的建置結果就跟著變，
#      而且沒有人會知道。輸入法的詞庫變動會直接改變使用者打出來的字。
#
# 所以這裡把 commit 釘死。要更新上游時，改下面的雜湊並在 commit message 裡
# 說明改了什麼、為什麼 —— 那是一次有意識的決定，不該是某天剛好 clone 的結果。
#
set -euo pipefail

# ⛔ **唯讀出口。** `scripts/verify_script_readonly.sh` 把每一支腳本的
#   `--help` 跑一遍,而且用 shim 檢查它有沒有碰外部工具。這一支從前沒有
#   `--help`,於是 `--help` 被當成一般啟動 —— 一路跑下去(建置／git／推檔)。
#   說明不得有任何副作用。
case "${1:-}" in
  -h|--help)
    sed -n '2,/^set -[eu]/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/third_party/rime-data"

# repo<TAB>commit
REPOS="
rime-prelude	082425ea0684bca36474415d4a0e8db9b016487e
rime-essay	e9b1a374a6ea015fca5bdd04318924b4483ac35a
rime-luna-pinyin	56b934b099dfbeab842320f13aa8b461a6ab3e42
rime-bopomofo	6085c9a38a4a728047862b33d67eee18aa86f3b9
rime-terra-pinyin	8a2c895ad7ee8e2b137d91be77f18f86b04d7fc9
rime-stroke	3a4b0f4013e2b4c14b1e80c92b1d4723eb65f39c
"

mkdir -p "$DEST"
cd "$DEST"

echo "=== 取得 RIME 方案資料（版本已釘住）==="
printf '%s\n' "$REPOS" | while IFS=$'\t' read -r repo sha; do
  [ -z "$repo" ] && continue
  if [ -d "$repo/.git" ]; then
    have="$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo none)"
    if [ "$have" = "$sha" ]; then
      echo "  $repo 已是 ${sha:0:8}"
      continue
    fi
    echo "  $repo 更新到 ${sha:0:8}（原為 ${have:0:8}）"
    git -C "$repo" fetch -q --depth 50 origin "$sha" 2>/dev/null \
      || git -C "$repo" fetch -q origin
  else
    echo "  $repo 取得中…"
    rm -rf "$repo"
    git init -q "$repo"
    git -C "$repo" remote add origin "https://github.com/rime/$repo.git"
    # 只抓需要的那個 commit，不抓整段歷史（essay 與 stroke 的詞庫都不小）
    git -C "$repo" fetch -q --depth 1 origin "$sha" 2>/dev/null \
      || git -C "$repo" fetch -q origin
  fi
  git -C "$repo" checkout -q "$sha" 2>/dev/null || {
    echo "  !! $repo 取不到 commit $sha —— 上游可能重寫了歷史" >&2
    exit 1
  }
done

echo
echo "=== 驗證 ==="
fail=0
printf '%s\n' "$REPOS" | while IFS=$'\t' read -r repo sha; do
  [ -z "$repo" ] && continue
  got="$(git -C "$repo" rev-parse HEAD)"
  if [ "$got" = "$sha" ]; then
    printf "  %-22s %s ✓\n" "$repo" "${sha:0:8}"
  else
    printf "  %-22s 期望 %s 實際 %s !!\n" "$repo" "${sha:0:8}" "${got:0:8}" >&2
    fail=1
  fi
done

echo
echo "下一步：scripts/collect_data.sh"
