#!/usr/bin/env bash
#
# fetch_librime.sh — 取得 librime 原始碼(commit 已釘死)與指定的相依 submodule
#
# ── 為什麼需要這支腳本 ──────────────────────────────────────────
#
# `third_party/librime/` 在 .gitignore 裡(那是上游的原始碼,不進本倉庫)。
# 結果是:**兩支 workflow 在 CI 上從來沒有成功過,而且沒有人發現**:
#
#   · build.yml    在「建 host 版 opencc 以產生詞典」直接對
#                  third_party/librime/deps/opencc 下 cmake ——
#                  CMake Error: The source directory ... does not exist.
#   · native.yml   跑 scripts/build_native.sh,那支在前置檢查就 die
#                  (「librime 原始碼不在 …」)。它是手動/每月觸發,所以更久沒人看。
#
# 兩支的根因是同一個:**沒有人負責把 librime 取回來**。以前那件事是「開發機上
# 剛好有」,而那不是一個步驟,是一個巧合。
#
# ── 為什麼 commit 釘在這裡,而不是各自寫一份 ────────────────────
#
# librime 的版本決定了使用者打出來的字。四端共用同一顆引擎是這個專案的前提;
# 兩個地方各寫一個 commit,遲早會漂移,而漂移的症狀是「同一份 schema 在兩端
# 編出不同結果」—— 那種問題沒人查得動。所以這裡是**唯一的釘住點**,
# windows/build.sh 與 CI 都從這裡拿。
#
# 底下還有一道交叉檢查:third_party/prebuilt/manifest.json 記錄的是「Android
# 那幾個 .a 是用哪個 commit 編出來的」。它與這裡不一致,就代表 Android 出貨的
# 引擎和其他端從原始碼建的引擎不是同一顆。那必須是一個明確的錯誤。
#
# ── 用法 ─────────────────────────────────────────────────────────
#
#   scripts/fetch_librime.sh                 # librime + 全部 5 個相依
#   scripts/fetch_librime.sh opencc          # 只要 opencc(APK 建置只需要這個)
#   scripts/fetch_librime.sh glog yaml-cpp   # 指定子集
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBRIME_SRC="${ROOT}/third_party/librime"

# ---------------------------------------------------------------- 釘住的版本
#
# 換這個值 = 換掉四端使用者拿到的輸入引擎。要換的時候:
#   1. 改這裡;
#   2. 重跑 scripts/build_native.sh 重建 Android 的 .a(它會一併更新
#      third_party/prebuilt/manifest.json,底下的交叉檢查才會再度相符);
#   3. 在 commit message 裡說明換了什麼、為什麼。
LIBRIME_REPO="${LIBRIME_REPO:-https://github.com/rime/librime.git}"
LIBRIME_COMMIT="${LIBRIME_COMMIT:-1d0df6e40cdcac17a986adc65e4668ae84ae0ada}"

# googletest 不在預設清單裡:所有建置路徑都是 BUILD_TEST=OFF,取了只是白等。
ALL_SUBMODULES=(glog yaml-cpp leveldb marisa-trie opencc)

SUBMODULES=("$@")
[ ${#SUBMODULES[@]} -eq 0 ] && SUBMODULES=("${ALL_SUBMODULES[@]}")

for s in "${SUBMODULES[@]}"; do
  case " ${ALL_SUBMODULES[*]} " in
    *" ${s} "*) ;;
    *) die "不認得的 submodule:${s}(可用:${ALL_SUBMODULES[*]})" ;;
  esac
done

# ------------------------------------------------- 與 prebuilt manifest 對帳
# manifest.json 是 scripts/build_native.sh 產生的,記錄 Android 那幾個 .a
# 實際是用哪個 commit 編的。這裡不相符 = 兩端跑的是不同的引擎。
check_manifest() {
  local manifest="${ROOT}/third_party/prebuilt/manifest.json"
  [ -f "${manifest}" ] || { log "沒有 manifest.json,略過對帳"; return; }
  # 用 awk 而不是 python3:這支腳本也會被 windows/build.sh 在 Windows 的
  # Git Bash 底下呼叫,那裡不保證有 python3。
  # 取的是**頂層 "librime" 物件裡**的 commit —— manifest 底下 plugins.lua 也有一個
  # commit 欄位,直接 grep '"commit"' 會抓錯。
  local recorded
  recorded="$(awk '
    /"librime"[[:space:]]*:/ { inblock = 1 }
    inblock && /"commit"[[:space:]]*:/ {
      match($0, /[0-9a-f]{40}/)
      if (RSTART > 0) { print substr($0, RSTART, RLENGTH); exit }
    }
  ' "${manifest}")"
  if [ -z "${recorded}" ]; then
    log "manifest.json 讀不到 librime.commit,略過對帳"
    return
  fi
  if [ "${recorded}" != "${LIBRIME_COMMIT}" ]; then
    die "librime commit 不一致:
  本腳本釘住            : ${LIBRIME_COMMIT}
  prebuilt/manifest.json: ${recorded}

  manifest.json 記的是 Android 出貨的 .a 實際用哪個 commit 編的。兩者不同,
  代表 Android 使用者拿到的引擎與其他端從原始碼建的引擎不是同一顆 ——
  同一份 schema 可能在兩端編出不同結果,而那種問題沒人查得動。

  要換 librime:改本腳本的 LIBRIME_COMMIT,重跑 scripts/build_native.sh
  重建 .a(它會更新 manifest.json),兩邊才會再度相符。"
  fi
  log "與 prebuilt/manifest.json 對帳相符:${LIBRIME_COMMIT:0:8}"
}

# ---------------------------------------------------------------- 取得原始碼
# 淺層取回**釘住的 commit**,不是 --depth 1 抓 HEAD —— 那樣上游一動就不可重現。
fetch_source() {
  if [ -d "${LIBRIME_SRC}/.git" ]; then
    local cur
    cur="$(git -C "${LIBRIME_SRC}" rev-parse HEAD 2>/dev/null || echo none)"
    if [ "${cur}" = "${LIBRIME_COMMIT}" ]; then
      log "librime 已在 ${LIBRIME_COMMIT:0:8}"
      return
    fi
    log "librime 目前在 ${cur:0:8},切到 ${LIBRIME_COMMIT:0:8}"
    git -C "${LIBRIME_SRC}" fetch -q --depth 1 origin "${LIBRIME_COMMIT}" \
      || die "抓取 librime ${LIBRIME_COMMIT} 失敗"
    git -C "${LIBRIME_SRC}" checkout -q --detach "${LIBRIME_COMMIT}"
    return
  fi

  log "取得 librime @ ${LIBRIME_COMMIT:0:8}"
  rm -rf "${LIBRIME_SRC}"
  mkdir -p "${LIBRIME_SRC}"
  git -C "${LIBRIME_SRC}" init -q
  git -C "${LIBRIME_SRC}" remote add origin "${LIBRIME_REPO}"
  git -C "${LIBRIME_SRC}" fetch -q --depth 1 origin "${LIBRIME_COMMIT}" \
    || die "抓取 librime ${LIBRIME_COMMIT} 失敗"
  git -C "${LIBRIME_SRC}" checkout -q --detach FETCH_HEAD
}

# ---------------------------------------------------------------- submodule
# 刻意不加 --depth 1:submodule 釘住的 commit 常常不在預設分支的淺層裡,
# 加了會間歇性失敗,而間歇性失敗比多下載幾十 MB 貴得多。
fetch_submodules() {
  local paths=()
  local s
  for s in "${SUBMODULES[@]}"; do paths+=("deps/${s}"); done
  log "取得 submodule:${SUBMODULES[*]}"
  git -C "${LIBRIME_SRC}" submodule update --init -- "${paths[@]}" \
    || die "submodule 取得失敗"
  git -C "${LIBRIME_SRC}" submodule status -- "${paths[@]}" | sed 's/^/    /'
}

# ---------------------------------------------------------------- 驗證
# 「git 沒報錯」不等於「東西在」。逐一點名。
verify() {
  [ -f "${LIBRIME_SRC}/CMakeLists.txt" ] || die "librime 原始碼不完整:缺少 CMakeLists.txt"
  local s missing=0
  for s in "${SUBMODULES[@]}"; do
    if [ -f "${LIBRIME_SRC}/deps/${s}/CMakeLists.txt" ]; then
      printf '    ✓ deps/%s\n' "${s}"
    else
      printf '    !! deps/%s 是空的(submodule 沒取到)\n' "${s}" >&2
      missing=1
    fi
  done
  [ "${missing}" -eq 0 ] || die "submodule 不完整,見上。"
  local head
  head="$(git -C "${LIBRIME_SRC}" rev-parse HEAD)"
  [ "${head}" = "${LIBRIME_COMMIT}" ] \
    || die "取回來的 HEAD 是 ${head},不是釘住的 ${LIBRIME_COMMIT}"
  log "librime ${LIBRIME_COMMIT:0:8} 就緒:${LIBRIME_SRC}"
}

check_manifest
fetch_source
fetch_submodules
verify
