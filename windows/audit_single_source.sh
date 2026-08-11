#!/usr/bin/env bash
#
# windows/audit_single_source.sh — 原始碼層面的「只能有一份」稽核
#
# 這個專案吃虧的形狀一向不是「程式碼寫錯」,是**同一件事在兩個地方各寫
# 一份,然後其中一份悄悄漂走**,而漂走的症狀在畫面上完全看不出來:
#
#   · 選項計畫:service/main.cc 的暖機與 pipe_server.cc 的 SESSION_NEW
#     各寫一份,上面掛著一句註解說「這一段必須逐字相同」。它漂了一個
#     `ascii_mode` → TakeSpareSession 的 SameOptions 連長度都不符 →
#     **每一個預熱好的備用 session 都被丟掉**,SESSION_NEW 的 300 毫秒
#     預算保護整個失效。畫面上什麼異狀都沒有,只是第一顆按鍵變慢。
#
# 一句註解不是守門。這支才是。
#
# ⚠ 判準刻意是**原始碼層面**的:windows/service/ 在 Ubuntu 上編不起來
#   (只有 GitHub Actions 的 windows-latest 編得動),所以任何要靠編譯或
#   執行才看得到的守門在開發時都等於不存在。
#
#   windows/audit_single_source.sh              # 稽核
#   windows/audit_single_source.sh --self-check # 反向:植入違規,必須紅
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DEFAULT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 在 $1 這個 repo 根底下跑一次稽核。回傳非零 = 有違規。
audit_root() {
  local root="$1"
  local bad=0
  local n

  # ── 規則 1:選項計畫只能有一份 ────────────────────────────────
  #
  # 建 session 的兩條路徑(暖機、SESSION_NEW)都必須走
  # common/schema_choice.cc 的 BuildOptionPlan,不得自己拼一份。
  n=$(cat "${root}/windows/service/main.cc" \
          "${root}/windows/service/pipe_server.cc" 2>/dev/null \
      | grep -c 'PlanVariant(')
  if [ "${n}" -ne 0 ]; then
    echo "!! main.cc / pipe_server.cc 裡有 ${n} 處直接呼叫 PlanVariant(" >&2
    echo "   選項計畫只能有一份 —— 走 BuildOptionPlan(),見它的註解。" >&2
    bad=1
  fi

  return "${bad}"
}

if [ "${1:-}" = "--self-check" ]; then
  # 反向測試:真的把違規植入一份**複本**,要求上面那一支抓得到。
  # 不接受只看綠燈 —— 這個專案有過「守門的東西沒有人守」。
  fail=0
  for plant in plan_variant; do
    tmp="$(mktemp -d)"
    mkdir -p "${tmp}/windows/service"
    cp "${ROOT_DEFAULT}/windows/service/main.cc" \
       "${ROOT_DEFAULT}/windows/service/pipe_server.cc" "${tmp}/windows/service/"
    case "${plant}" in
      plan_variant)
        echo '  auto oops = rimewin::PlanVariant(true, 0x0804);' \
          >> "${tmp}/windows/service/main.cc" ;;
    esac
    if audit_root "${tmp}" >/dev/null 2>&1; then
      echo "!! 植入了 ${plant},稽核卻以 0 結束 —— 它不會紅" >&2
      fail=1
    else
      echo "  ok   植入 ${plant} 之後稽核紅了"
    fi
    rm -rf "${tmp}"
  done
  # 現況必須是綠的(否則上面的「紅」證明不了任何事)。
  if ! audit_root "${ROOT_DEFAULT}"; then
    echo "!! 現況就是紅的 —— 反向測試不成立" >&2
    fail=1
  else
    echo "  ok   現況是綠的"
  fi
  exit "${fail}"
fi

audit_root "${ROOT_DEFAULT}"
