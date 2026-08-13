# shellcheck shell=bash
#
# ocr.sh — 找得到 tesseract,而且找得到的話連 runtime 環境一起備好。
#
# ── 為什麼需要這一支 ────────────────────────────────────────────────────
# `verify_syllables.sh` 這一關**整輪沒有跑過**:它掃描階段綠,然後停在
# 「找不到 tesseract」(exit 2)—— 而 `exit 2` 與 `exit 0` 在只看「有沒有紅字」
# 的人眼裡沒有差別。上一輪改的正是它守的那幾份九宮格佈局之一,等於消歧欄
# **無人看守**。
#
# 建置機上裝不了系統套件(`sudo -n true` 失敗,需要互動式密碼),但**不需要
# root 也裝得起來**:`apt-get download` 以呼叫者的身分把 .deb 下載到 cwd,
# `dpkg-deb -x` 解到自己的目錄,不碰系統任何一個檔案。
#
#     mkdir -p ~/.local/tess/debs && cd ~/.local/tess/debs
#     apt-get download libleptonica6 libtesseract5 \
#                      tesseract-ocr tesseract-ocr-eng tesseract-ocr-osd
#     for d in *.deb; do dpkg-deb -x "$d" ~/.local/tess/root; done
#
# 解出來的 tesseract 需要三件事才跑得動,而**要求呼叫者記得設三個環境變數
# 等於這一關會再一次「因為缺工具而沒跑」** —— 所以這裡自己找、自己設。
#
# ⚠ 找不到仍然**中止**,不跳過。跳過的關卡與綠燈長得一模一樣,那正是這一輪
#   在修的東西。

# 找 tesseract。成功時把路徑放進 **RS_TESSERACT**,並 export 好
# LD_LIBRARY_PATH / TESSDATA_PREFIX。找不到回 1(訊息在 stderr,指向安裝
# 而不是指向產品)。
#
# ⚠ **回傳走變數,不走 stdout。** 寫成 `T="$(rs_find_tesseract)"` 的話,
#   函式跑在命令替換的子行程裡,`export LD_LIBRARY_PATH` 一出子行程就沒了 ——
#   結果是「找得到執行檔、跑起來卻缺 libleptonica」。這一支第一次接上
#   verify_candbar.sh 就踩到:訊息印的是「沒有 tesseract」。
rs_find_tesseract() {
  local cand

  if [ -n "${RIME_TESSERACT:-}" ] && [ -x "${RIME_TESSERACT}" ]; then
    cand="$RIME_TESSERACT"
  elif cand="$(command -v tesseract 2>/dev/null)" && [ -n "$cand" ]; then
    :
  elif [ -x "$HOME/.local/tess/root/usr/bin/tesseract" ]; then
    cand="$HOME/.local/tess/root/usr/bin/tesseract"
  else
    {
      echo "找不到 tesseract。這台機器沒有 passwordless sudo,但**不需要 root** 也裝得起來:"
      echo "    mkdir -p ~/.local/tess/debs && cd ~/.local/tess/debs"
      echo "    apt-get download libleptonica6 libtesseract5 tesseract-ocr tesseract-ocr-eng tesseract-ocr-osd"
      echo "    for d in *.deb; do dpkg-deb -x \"\$d\" ~/.local/tess/root; done"
      echo "  裝好之後這一支會自己找到它(也可以用 RIME_TESSERACT 指定)。"
    } >&2
    return 1
  fi

  # 解包安裝的那一份不在 ld 的搜尋路徑上,而 tessdata 也不在預設位置 ——
  # 兩者任何一個缺席,tesseract 都會以一句看起來像產品壞掉的訊息失敗。
  local root
  # `<prefix>/bin/tesseract` → root = `<prefix>`(解包安裝時是 `~/.local/tess/root/usr`)。
  root="$(cd "$(dirname "$cand")/.." 2>/dev/null && pwd)"
  if [ -d "$root/lib/x86_64-linux-gnu" ]; then
    export LD_LIBRARY_PATH="$root/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  fi
  if [ -z "${TESSDATA_PREFIX:-}" ]; then
    local td
    for td in "$root/share/tesseract-ocr/5/tessdata" "$root/share/tessdata" \
              /usr/share/tesseract-ocr/5/tessdata; do
      [ -d "$td" ] && { export TESSDATA_PREFIX="$td"; break; }
    done
  fi

  # ⚠ 自證:找得到執行檔不等於跑得動(缺 libleptonica、缺 tessdata 都會在
  #   這裡當場現形,而不是在第 3 關以「消歧欄上讀不到 ni」的形式現形)。
  if ! "$cand" --version >/dev/null 2>&1; then
    echo "找到 $cand 但它跑不起來(LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-<空>})" >&2
    "$cand" --version >&2 2>&1 | head -3
    return 1
  fi
  RS_TESSERACT="$cand"
  return 0
}
