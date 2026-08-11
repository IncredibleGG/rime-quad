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
#   · rs_select_schema:engine.cc 裡本來有四個裸呼叫點,而 librime 每次
#     載入方案都會跑 ConcreteEngine::InitializeOptions() 把 switches 重設回
#     方案宣告的值。四個裡有三個之後沒有重套簡繁 —— 使用者從那一橫的
#     方案選單換一次方案,他選的簡體就被洗掉,而畫面上那一格還畫著舊的。
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

  # ── 規則 2:rs_select_schema 只能有一個裸呼叫點 ────────────────
  #
  # 那一個必須在 Engine::SelectAndApply 裡。「載入方案」與「套用簡繁」
  # 是一個不可分割的動作,不能靠人記得 —— 靠人記得的版本已經漏了三次。
  #
  # ⚠ 只數真的呼叫(帶左括號),註解裡提到函式名不算。
  n=$(grep -c 'rs_select_schema(' "${root}/windows/service/engine.cc")
  if [ "${n}" -ne 1 ]; then
    echo "!! engine.cc 裡有 ${n} 處 rs_select_schema( —— 只能有 1 處" >&2
    echo "   那一處必須在 SelectAndApply 裡。換方案會把 switches 重設回" >&2
    echo "   方案宣告的值,之後不重套簡繁就等於把使用者的設定洗掉。" >&2
    bad=1
  fi
  if ! grep -q 'bool Engine::SelectAndApply(' "${root}/windows/service/engine.cc"; then
    echo "!! engine.cc 裡找不到 Engine::SelectAndApply —— 規則 2 沒有東西可守" >&2
    bad=1
  fi

  # ── 規則 3:換方案之前一定要重讀設定檔裡的簡繁偏好 ──────────────
  #
  # 守的是 648c02c ——「換方案洗掉簡繁」。規則 2 守的是「換完方案要重套」,
  # 這一條守的是**重套時拿的是哪一份偏好**:`Engine::variant_pref_` 是設定的
  # 複本,設定檔在服務跑著的時候被別人改掉(設定視窗有一顆「用記事本開啟
  # 設定檔」)它就過期了,而拿過期那一份重套 = 把使用者剛選的簡繁洗掉。
  #
  # ⚠ 為什麼這一條非要在原始碼層面守不可:648c02c 唯一的守門是
  #   windows/verify_installer.sh §6g 案例二,而那支只有 Windows 跑得動。
  #   覆核實跑證明:把 pipe_server.cc 那一行刪掉,三支守門全綠 ——
  #   也就是說那一輪唯一改變使用者看得到的行為的修法,在開發機上
  #   沒有任何東西攔得住它被刪掉。
  #
  #   判斷本身已經抽成 common/schema_choice.cc 的
  #   PickVariantPrefForSchemaSwitch(),tests/test_schema_choice.cc 驗得到;
  #   純函式驗不到的是**有沒有人呼叫它**,那一格就是這一條。
  #
  # ⚠ 只看 kSelectSchema 那一個 case 的區塊,而且**先把註解行濾掉** ——
  #   註解裡提到函式名不算(與規則 2 同一個判準)。
  local blk
  blk="$(awk '{ if (f == 1 && $0 ~ /case Op::/) exit;
                if (f == 1) print;
                if ($0 ~ /case Op::kSelectSchema:/) { f = 1; print } }' \
         "${root}/windows/service/pipe_server.cc" \
       | grep -v '^[[:space:]]*//')"
  if [ -z "${blk}" ]; then
    echo "!! pipe_server.cc 裡找不到 case Op::kSelectSchema —— 規則 3 沒有東西可守" >&2
    bad=1
  else
    local n_pick n_set n_sel
    n_pick=$(printf '%s\n' "${blk}" | grep -n 'PickVariantPrefForSchemaSwitch(' \
             | head -1 | cut -d: -f1)
    n_set=$(printf '%s\n' "${blk}" | grep -n 'engine_->SetVariantPref(' \
            | head -1 | cut -d: -f1)
    n_sel=$(printf '%s\n' "${blk}" | grep -n 'engine_->SelectSchema(' \
            | head -1 | cut -d: -f1)
    if [ -z "${n_pick}" ]; then
      echo "!! pipe_server.cc 的 kSelectSchema 沒有呼叫 PickVariantPrefForSchemaSwitch(" >&2
      echo "   換方案時會拿引擎手上那份**過期的**簡繁偏好去重套 ——" >&2
      echo "   使用者剛在設定檔裡選的簡繁被洗掉,而狀態列那一格還畫著新的。" >&2
      echo "   見 common/schema_choice.h 的 PickVariantPrefForSchemaSwitch。" >&2
      bad=1
    fi
    if [ -z "${n_set}" ]; then
      echo "!! pipe_server.cc 的 kSelectSchema 算了偏好卻沒有 engine_->SetVariantPref(" >&2
      echo "   算出來沒有交給引擎 = 沒算。" >&2
      bad=1
    fi
    if [ -z "${n_sel}" ]; then
      echo "!! pipe_server.cc 的 kSelectSchema 裡沒有 engine_->SelectSchema( —— 規則 3 沒有東西可守" >&2
      bad=1
    fi
    if [ -n "${n_pick}" ] && [ -n "${n_sel}" ] && [ "${n_pick}" -gt "${n_sel}" ]; then
      echo "!! pipe_server.cc 的 kSelectSchema 先換方案才重讀偏好 —— 順序反了" >&2
      echo "   順序就是這一條的全部意義:SelectAndApply 在換方案的**當下**" >&2
      echo "   就會拿 variant_pref_ 重套,晚一步更新等於沒更新。" >&2
      bad=1
    fi
  fi

  return "${bad}"
}

if [ "${1:-}" = "--self-check" ]; then
  # 反向測試:真的把違規植入一份**複本**,要求上面那一支抓得到。
  # 不接受只看綠燈 —— 這個專案有過「守門的東西沒有人守」。
  fail=0
  for plant in plan_variant bare_select_schema no_select_and_apply \
               no_variant_reread variant_reread_too_late; do
    tmp="$(mktemp -d)"
    mkdir -p "${tmp}/windows/service"
    cp "${ROOT_DEFAULT}/windows/service/main.cc" \
       "${ROOT_DEFAULT}/windows/service/pipe_server.cc" \
       "${ROOT_DEFAULT}/windows/service/engine.cc" "${tmp}/windows/service/"
    case "${plant}" in
      plan_variant)
        echo '  auto oops = rimewin::PlanVariant(true, 0x0804);' \
          >> "${tmp}/windows/service/main.cc" ;;
      bare_select_schema)
        echo '  rs_select_schema(sess, "luna_pinyin");' \
          >> "${tmp}/windows/service/engine.cc" ;;
      no_select_and_apply)
        sed -i 's/^bool Engine::SelectAndApply(/bool Engine::SelectAndApplyX(/' \
          "${tmp}/windows/service/engine.cc" ;;
      # 這一個就是覆核實際做過的那個植入:把重讀設定檔那一行刪掉。
      # 在規則 3 存在之前,做完這件事三支守門全綠。
      no_variant_reread)
        sed -i '/PickVariantPrefForSchemaSwitch(/d' \
          "${tmp}/windows/service/pipe_server.cc" ;;
      # 順序反了:先換方案、才重讀偏好。SelectAndApply 在換方案的當下就
      # 拿 variant_pref_ 重套了,所以晚一步 = 沒更新,而它看起來完全正確。
      variant_reread_too_late)
        sed -i 's/^\([[:space:]]*\)const VariantPrefPick pick = PickVariantPrefForSchemaSwitch(/\1Result r0 = engine_->SelectSchema(sc.session, sc.schema_id);\n\1const VariantPrefPick pick = PickVariantPrefForSchemaSwitch(/' \
          "${tmp}/windows/service/pipe_server.cc" ;;
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
