#!/usr/bin/env bash
#
# windows/build.sh — 建置 Windows 端的核心層
#
# ── 這支腳本的範圍 ───────────────────────────────────────────────
#
# 只做「不經 UI 的核心層」:librime、它的 5 個靜態相依、四端共用的門面
# core/src/rime_shell.cc,以及直接驅動它們的 tools/rime_console.cc。
#
# **不做** TSF DLL、不做 COM、不做候選窗。核心層沒綠之前做 UI,出問題分不出
# 是引擎、資料、還是 UI 的錯 —— 這個專案已經吃過這個虧(見 docs/handoff-windows.md §8)。
#
# ── 為什麼腳本是 bash,編譯器卻是 MSVC ───────────────────────────
#
# 工具鏈是 MSVC(cl.exe);腳本用 bash,在 runner 內建的 Git Bash 下執行。
# 理由很現實:唯一的 Windows 建置管道是 GitHub Actions,一輪十幾分鐘。
# bash 至少能在別台機器上先 `bash -n` 掃過語法,.bat 連這個都做不到。
# 這不影響產物 —— 編譯器仍然是 MSVC,TSF 實務上無法用 mingw 交叉編譯。
#
# ── 用法(Git Bash)─────────────────────────────────────────────
#
#   windows/build.sh              # deps + console
#   windows/build.sh deps         # 只建 librime 與 5 個相依
#   windows/build.sh console      # 只建 rime_console.exe(需先有 deps)
#   windows/build.sh --clean      # 先清掉中間產物再建
#
# 產出:
#   third_party/build/windows-<arch>/prefix/{include,lib,share}
#   third_party/build/windows-<arch>/console/bin/rime_console.exe
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TP="${ROOT}/third_party"

# ---------------------------------------------------------------- 釘住的版本
# librime 必須與 Android 用的是同一個 commit。四端共用同一顆引擎是這個專案的
# 前提;不同 commit 代表「同一份 schema 在兩端可能編出不同結果」,那就沒得談了。
# 這個值必須與 third_party/prebuilt/manifest.json 的 librime.commit 一致。
LIBRIME_REPO="${LIBRIME_REPO:-https://github.com/rime/librime.git}"
LIBRIME_COMMIT="${LIBRIME_COMMIT:-1d0df6e40cdcac17a986adc65e4668ae84ae0ada}"
# 與 scripts/build_native.sh 同版本。librime 只用 header-only 的部分,
# 上游 install-boost.bat 也只做 `b2 headers`,不需要編任何 Boost 二進位。
BOOST_VERSION="${BOOST_VERSION:-1.89.0}"

# 只做 x64。arm64 是下一輪 —— 先讓一個架構真的打得出字,再談第二個。
ARCH="${ARCH:-x64}"
case "${ARCH}" in
  x64) ;;
  *) die "本輪只支援 x64(收到 ${ARCH})。arm64 是下一個里程碑。" ;;
esac

# 產生器用 Ninja,不用 Visual Studio 產生器。
#
# 這不是為了快(雖然對 librime 那幾百個編譯單元差很多)。VS 產生器的名字裡帶著
# VS 的版本號("Visual Studio 17 2022"),於是 **CMake 版本** 與 **runner 上的 VS
# 版本** 被綁在一起:CMake 3.31 不認得 VS 2026,而 GitHub 的 windows-latest 已經
# 是 windows-2025-vs2026 的映像。第一版就是這樣掛掉的。
#
# Ninja 把這兩件事解耦:編譯器由 vcvars 設好的環境決定,CMake 只管產生規則。
# 上游 librime 自己的 Windows CI 也正是 Ninja + MSVC(見其 windows-build.yml
# 的 `set CMAKE_GENERATOR=Ninja`),所以這是已知會綠的組合。

BUILD_ROOT="${TP}/build/windows-${ARCH}"
PREFIX="${BUILD_ROOT}/prefix"
LIBRIME_SRC="${TP}/librime"
DEPS_SRC="${LIBRIME_SRC}/deps"
BOOST_ROOT_DIR="${RIME_BOOST_ROOT:-${TP}/boost}"
PATCH_DIR="${ROOT}/patches"

# ---------------------------------------------------------------- 參數
DO_CLEAN=0
DO_DEPS=0
DO_CONSOLE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --clean)  DO_CLEAN=1 ;;
    deps)     DO_DEPS=1 ;;
    console)  DO_CONSOLE=1 ;;
    -h|--help) sed -n '2,32p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "未知參數: $1" ;;
  esac
  shift
done
if [ "${DO_DEPS}" -eq 0 ] && [ "${DO_CONSOLE}" -eq 0 ]; then
  DO_DEPS=1; DO_CONSOLE=1
fi

# ---------------------------------------------------------------- 前置檢查
command -v cygpath >/dev/null 2>&1 \
  || die "找不到 cygpath。這支腳本必須在 Windows 的 Git Bash / MSYS2 下執行。"

# cygpath -m 給的是 CMake 吃得下的 Windows 路徑(正斜線),例如 D:/a/rime/rime。
w() { cygpath -m "$1"; }

# MSVC 的 /I 與 /LIBPATH 在 CMAKE_CXX_FLAGS 這種「一整串」的旗標裡沒有可靠的
# 引號行為,路徑一有空白就會被拆開,而且錯誤訊息完全看不出原因。寧可在這裡
# 明確擋掉。
case "${ROOT}" in
  *" "*) die "倉庫路徑含空白:${ROOT}
  opencc 需要以 /I 與 /LIBPATH 注入路徑,含空白會被靜默拆開。請移到無空白的路徑。" ;;
esac

CMAKE="${CMAKE:-cmake}"
command -v "${CMAKE}" >/dev/null 2>&1 || die "找不到 cmake"
# 不寫成 `cmake --version | head -1 | awk ...`:head 讀完就關掉 pipe,cmake 可能
# 收到 SIGPIPE 而以 141 結束,配上 pipefail 就是間歇性失敗 —— 而間歇性失敗
# 比明確的失敗難查得多。scripts/build_native.sh 的註解也踩過同一個坑。
CMAKE_VER="$("${CMAKE}" --version)"
CMAKE_VER="${CMAKE_VER%%$'\n'*}"   # 第一行:"cmake version 3.31.6"
CMAKE_VER="${CMAKE_VER##* }"       # 最後一個空白之後:"3.31.6"
case "${CMAKE_VER}" in
  3.*) ;;
  *) die "偵測到 cmake ${CMAKE_VER};必須使用 CMake 3.x。
  CMake 4 移除了 FindBoost 模組,而 librime 以 find_package(Boost) 找 header-only 的
  Boost,4.x 會直接找不到 —— 而且因為非 LINUX 分支不帶 REQUIRED,它不會報錯,
  只會在編譯時噴一整片找不到 boost 標頭。
  (scripts/build_native.sh 為了同一個原因釘住 Android SDK 的 cmake 3.22.1。)
  CI 已固定安裝 3.31.x;本機請設 CMAKE=<3.x 的 cmake 路徑>。" ;;
esac

NPROC="$(nproc 2>/dev/null || echo 4)"

# ---------------------------------------------------------------- MSVC 環境
# Ninja 需要 cl.exe / link.exe / rc.exe 與 INCLUDE、LIB 都在環境裡 —— 那正是
# vcvars64.bat 做的事。已經在 Developer Command Prompt 裡的話這一段直接跳過。
#
# 不寫死 VS 路徑:用 vswhere 問。runner 的映像換 VS 版本時(這已經發生過一次,
# 見上面產生器那段),這裡不需要改。
setup_msvc_env() {
  if command -v cl.exe >/dev/null 2>&1; then
    log "MSVC 環境已就緒: $(command -v cl.exe)"
    return
  fi
  local vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
  [ -x "${vswhere}" ] || die "找不到 vswhere:${vswhere}
  這台機器上沒有 Visual Studio Installer,無法定位 MSVC。"
  local vsdir
  vsdir="$("${vswhere}" -latest -products '*' \
             -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
             -property installationPath | tr -d '\r')"
  [ -n "${vsdir}" ] || die "vswhere 找不到含 C++ 工具集(VC.Tools.x86.x64)的 Visual Studio"
  local vcvars="$(cygpath -u "${vsdir}")/VC/Auxiliary/Build/vcvars64.bat"
  [ -f "${vcvars}" ] || die "找不到 ${vcvars}"
  log "載入 MSVC 環境: ${vcvars}"

  # 透過暫存 .bat 取環境,而不是 `cmd //c "call ... && set"`:
  # cmd 對 /c 後面那一整串的引號處理很難預測,而這裡錯了只會得到一堆
  # 「找不到標頭」的謎樣錯誤。落成檔案就沒有引號問題。
  mkdir -p "${BUILD_ROOT}"
  local bat="${BUILD_ROOT}/_vcvars_dump.bat"
  {
    printf '@echo off\r\n'
    printf 'call "%s" >nul\r\n' "$(cygpath -w "${vcvars}")"
    printf 'set\r\n'
  } > "${bat}"

  local dump
  dump="$(cmd //c "$(cygpath -w "${bat}")")" || die "vcvars64.bat 執行失敗"

  local line
  while IFS= read -r line; do
    line="${line%$'\r'}"
    case "${line}" in
      # cl.exe 直接讀 INCLUDE / LIB / LIBPATH,而且要的是 Windows 形式的路徑,
      # 所以原樣輸出,不可以經過 cygpath。
      INCLUDE=*|LIB=*|LIBPATH=*|VSINSTALLDIR=*|VCINSTALLDIR=*|VCToolsInstallDir=*|\
      WindowsSdkDir=*|WindowsSDKVersion=*|WindowsSdkVerBinPath=*|UCRTVersion=*)
        export "${line}" ;;
      # PATH 反過來:bash 這一側要 POSIX 形式,否則 command -v 找不到東西。
      PATH=*|Path=*)
        export PATH="$(cygpath -p "${line#*=}")" ;;
    esac
  done <<< "${dump}"

  command -v cl.exe >/dev/null 2>&1 \
    || die "載入 vcvars 之後仍然找不到 cl.exe;環境沒有正確帶進來。"
  log "cl.exe = $(command -v cl.exe)"
}

# 上游 librime 的 Windows CI 用 `pip install ninja`。這裡先看 PATH,再看 VS 自帶的
# 那一份,都沒有才停 —— 不自動安裝,免得建置腳本偷偷改動機器狀態。
resolve_ninja() {
  if [ -n "${NINJA:-}" ]; then return; fi
  if command -v ninja >/dev/null 2>&1; then
    NINJA="$(command -v ninja)"
    return
  fi
  local vs_ninja
  vs_ninja="$(cygpath -u "${VSINSTALLDIR:-}")Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
  if [ -n "${VSINSTALLDIR:-}" ] && [ -f "${vs_ninja}" ]; then
    NINJA="${vs_ninja}"
    return
  fi
  die "找不到 ninja。CI 上由 workflow 以 pip 安裝;本機請自行安裝或設 NINJA=<路徑>。"
}

setup_msvc_env
resolve_ninja

# CMake 靠 CC / CXX 選編譯器。不設的話,Git Bash 的 PATH 裡常有 mingw 的 gcc,
# CMake 會挑到它,然後在連結 MSVC 編出來的靜態庫時以難懂的方式失敗。
export CC=cl
export CXX=cl

log "ROOT       = ${ROOT}"
log "ARCH       = ${ARCH}"
log "cmake      = ${CMAKE} (${CMAKE_VER})"
log "ninja      = ${NINJA}"
log "librime    = ${LIBRIME_COMMIT:0:8}"
log "Boost      = ${BOOST_VERSION}"
log "prefix     = ${PREFIX}"

if [ "${DO_CLEAN}" -eq 1 ]; then
  log "清除 ${BUILD_ROOT}"
  rm -rf "${BUILD_ROOT}"
fi
mkdir -p "${BUILD_ROOT}" "${PREFIX}"

# ---------------------------------------------------------------- librime 原始碼
# third_party/librime 不在版控裡(見 .gitignore),CI 上必須自己取。
# 淺層取回**釘住的 commit**,不是 --depth 1 抓 HEAD —— 那樣上游一動就不可重現。
fetch_librime() {
  if [ -d "${LIBRIME_SRC}/.git" ]; then
    local cur
    cur="$(git -C "${LIBRIME_SRC}" rev-parse HEAD 2>/dev/null || echo none)"
    if [ "${cur}" = "${LIBRIME_COMMIT}" ]; then
      log "librime 已在 ${LIBRIME_COMMIT:0:8}"
    else
      log "librime 目前在 ${cur:0:8},切到 ${LIBRIME_COMMIT:0:8}"
      git -C "${LIBRIME_SRC}" fetch -q --depth 1 origin "${LIBRIME_COMMIT}"
      git -C "${LIBRIME_SRC}" checkout -q --detach "${LIBRIME_COMMIT}"
    fi
  else
    log "取得 librime @ ${LIBRIME_COMMIT:0:8}"
    rm -rf "${LIBRIME_SRC}"
    mkdir -p "${LIBRIME_SRC}"
    git -C "${LIBRIME_SRC}" init -q
    git -C "${LIBRIME_SRC}" remote add origin "${LIBRIME_REPO}"
    git -C "${LIBRIME_SRC}" fetch -q --depth 1 origin "${LIBRIME_COMMIT}" \
      || die "抓取 librime ${LIBRIME_COMMIT} 失敗"
    git -C "${LIBRIME_SRC}" checkout -q --detach FETCH_HEAD
  fi

  # 只取要用的 5 個 submodule。googletest 不取:BUILD_TEST=OFF,取了只是白等。
  # 這裡刻意不加 --depth 1:submodule 的釘住 commit 常常不在預設分支的淺層裡,
  # 加了會間歇性失敗,而間歇性失敗比多下載幾十 MB 貴得多。
  log "取得 librime 的 5 個相依 submodule"
  git -C "${LIBRIME_SRC}" submodule update --init -- \
    deps/glog deps/yaml-cpp deps/leveldb deps/marisa-trie deps/opencc \
    || die "submodule 取得失敗"

  local sm
  sm="$(git -C "${LIBRIME_SRC}" submodule status deps/glog deps/yaml-cpp \
        deps/leveldb deps/marisa-trie deps/opencc)"
  printf '%s\n' "${sm}" | sed 's/^/    /'
}

# ---------------------------------------------------------------- patches
# 命名規則同 scripts/build_native.sh:"<相對路徑>@<說明>.patch"。
# 目標目錄不存在的 patch **不會被靜默略過** —— 一律印出來並說明原因。
# 「測試/步驟自己安靜地跳過」是這個專案抓過最多次的失敗模式。
apply_patches() {
  shopt -s nullglob
  local patches=("${PATCH_DIR}"/*.patch)
  shopt -u nullglob
  [ ${#patches[@]} -eq 0 ] && { log "無 patch"; return; }

  local p base rel target
  for p in "${patches[@]}"; do
    base="$(basename "${p}" .patch)"
    rel="${base%%@*}"
    rel="${rel//__//}"
    case "${rel}" in
      librime)     target="${LIBRIME_SRC}" ;;
      librime-lua) target="${TP}/librime-lua" ;;
      *)           target="${LIBRIME_SRC}/${rel}" ;;
    esac

    if [ ! -d "${target}" ]; then
      warn "略過 $(basename "${p}"):目標 ${target} 不存在"
      warn "  (Windows 這一輪不編 librime-lua,見下方 check_lua_sandbox 的說明)"
      continue
    fi
    if git -C "${target}" apply --reverse --check "${p}" >/dev/null 2>&1; then
      log "patch 已套用,略過: $(basename "${p}")"
    else
      log "套用 patch: $(basename "${p}") -> ${target}"
      git -C "${target}" apply "${p}" || die "套用 patch 失敗: ${p}"
    fi
  done
}

# ---------------------------------------------------------------- lua 沙盒守門
#
# 本輪不編 librime-lua:里程碑的範圍是「librime 與 5 個依賴」,外掛不在其中。
# 但這是一個**真實的缺口**,不是可以忘記的細節,所以這裡同時做兩件事:
#
#   1. 明白說出 Windows 目前沒有 lua —— 倚賴 lua_translator / lua_filter 的
#      第三方方案在 Windows 上會「部署成功但一個候選都沒有」,那是最難察覺的
#      失敗模式(scripts/build_native.sh 的 verify_lua() 也在講同一件事)。
#   2. 放一道會**擋下建置**的檢查:只要 plugins/lua 出現了,
#      patches/librime-lua@sandbox.patch 就必須同時到位。
#      沒套沙盒等於第三方方案能在使用者機器上執行任意程式碼
#      (os.execute / io.popen / package.loadlib,見 docs/handoff-windows.md §6)。
#      不接受「先編起來、沙盒之後補」。
check_lua_sandbox() {
  local plugin_dir="${LIBRIME_SRC}/plugins/lua"
  if [ ! -e "${plugin_dir}" ]; then
    warn "librime-lua 未編入(本輪範圍外)。"
    warn "  → Windows 端目前沒有 lua_translator / lua_filter,倚賴它們的第三方方案"
    warn "    會部署成功但沒有任何候選。補上時必須連同 patches/librime-lua@sandbox.patch。"
    return
  fi
  local lua_src="${TP}/librime-lua"
  [ -d "${lua_src}" ] || die "plugins/lua 存在但找不到 ${lua_src},無法確認沙盒是否已套用。"
  git -C "${lua_src}" apply --reverse --check \
      "${PATCH_DIR}/librime-lua@sandbox.patch" >/dev/null 2>&1 \
    || die "plugins/lua 已掛上,但 patches/librime-lua@sandbox.patch **沒有套用**。
  未沙盒化的 librime-lua 會在模組初始化時就 dofile 使用者目錄下的 rime.lua ——
  等於「下載第三方方案即執行任意程式碼」。拒絕建置。"
  log "librime-lua 沙盒 patch 已套用"
}

# ---------------------------------------------------------------- Boost(headers)
ensure_boost() {
  if [ -f "${BOOST_ROOT_DIR}/boost/version.hpp" ]; then
    log "Boost headers 已就緒: ${BOOST_ROOT_DIR}"
    return
  fi
  local underscored="boost_${BOOST_VERSION//./_}"
  local tarball="${TP}/${underscored}.tar.gz"
  local url="https://archives.boost.io/release/${BOOST_VERSION}/source/${underscored}.tar.gz"
  mkdir -p "${TP}"
  if [ ! -f "${tarball}" ]; then
    log "下載 Boost ${BOOST_VERSION}"
    curl -fSL --retry 3 -o "${tarball}.part" "${url}"
    mv "${tarball}.part" "${tarball}"
  fi
  # **只解出 header 樹**。完整 Boost 原始碼有數十萬個小檔,在 NTFS 上解壓要十幾
  # 分鐘,而 librime 只用 header-only 的部分(上游 install-boost.bat 同樣只做
  # `b2 headers`,不編任何二進位)。這一個決定省掉大半的 CI 時間。
  log "解壓 Boost headers(只取 ${underscored}/boost)"
  rm -rf "${TP}/${underscored}" "${BOOST_ROOT_DIR}"
  tar xzf "${tarball}" -C "${TP}" "${underscored}/boost"
  mv "${TP}/${underscored}" "${BOOST_ROOT_DIR}"
  [ -f "${BOOST_ROOT_DIR}/boost/version.hpp" ] || die "Boost 解壓失敗"
}

# ---------------------------------------------------------------- cmake 共用參數
#
# /MT(靜態 CRT)必須貫穿 librime、5 個相依、以及最後的 rime_console —— 只要有一個
# 用了 /MD 就是 LNK2038,而且訊息指向的檔案通常不是真正出錯的那一個。
# 兩道保險並存是刻意的:
#   · CMAKE_MSVC_RUNTIME_LIBRARY 只在 CMP0091=NEW 時生效,而 leveldb(3.9)、
#     opencc(3.12)宣告的 cmake_minimum_required 太舊,政策是 OLD;
#   · 那些專案就靠 librime 自己的 c/cxx_flag_overrides.cmake 把 /MT 塞進
#     CMAKE_*_FLAGS_RELEASE_INIT。
# 上游的 build.bat 也是兩者都傳,照抄。
common_args() {
  COMMON=(
    -G Ninja
    "-DCMAKE_MAKE_PROGRAM=$(w "${NINJA}")"
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_USER_MAKE_RULES_OVERRIDE=$(w "${LIBRIME_SRC}/cmake/c_flag_overrides.cmake")"
    "-DCMAKE_USER_MAKE_RULES_OVERRIDE_CXX=$(w "${LIBRIME_SRC}/cmake/cxx_flag_overrides.cmake")"
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
    "-DCMAKE_INSTALL_PREFIX=$(w "${PREFIX}")"
    "-DCMAKE_PREFIX_PATH=$(w "${PREFIX}")"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_TESTING=OFF
  )
}

build_one() {
  # $1 = 名稱, $2 = 原始碼目錄, 其餘 = 額外 cmake 參數
  local name="$1" src="$2"; shift 2
  local bdir="${BUILD_ROOT}/${name}"
  log "[${name}] configure"
  "${CMAKE}" -S "$(w "${src}")" -B "$(w "${bdir}")" "${COMMON[@]}" "$@" \
    > "${bdir}.configure.log" 2>&1 \
    || { tail -80 "${bdir}.configure.log"; die "${name} configure 失敗"; }
  log "[${name}] build"
  # Ninja 是單組態產生器,組態由 CMAKE_BUILD_TYPE 決定,不傳 --config。
  "${CMAKE}" --build "$(w "${bdir}")" --parallel "${NPROC}" \
    > "${bdir}.build.log" 2>&1 \
    || { tail -120 "${bdir}.build.log"; die "${name} build 失敗"; }
  log "[${name}] install"
  "${CMAKE}" --install "$(w "${bdir}")" \
    > "${bdir}.install.log" 2>&1 \
    || { tail -80 "${bdir}.install.log"; die "${name} install 失敗"; }
}

build_deps() {
  # 原始碼的取得與 patch 只有這條路徑需要。console 那條只吃 prefix 裡的
  # 標頭與 .lib —— CI 上 prefix 是快取回來的,不該為了編一支執行檔再 clone
  # 一次 librime、再抓一次 Boost。
  fetch_librime
  apply_patches
  check_lua_sandbox
  ensure_boost

  common_args

  build_one glog "${DEPS_SRC}/glog" \
    -DWITH_GFLAGS=OFF -DWITH_GTEST=OFF -DWITH_PKGCONFIG=OFF -DWITH_UNWIND=OFF

  build_one yaml-cpp "${DEPS_SRC}/yaml-cpp" \
    -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_CPP_BUILD_TOOLS=OFF \
    -DYAML_CPP_BUILD_TESTS=OFF -DYAML_BUILD_SHARED_LIBS=OFF \
    -DYAML_CPP_INSTALL=ON -DYAML_MSVC_SHARED_RT=OFF

  build_one leveldb "${DEPS_SRC}/leveldb" \
    -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF -DLEVELDB_INSTALL=ON

  build_one marisa "${DEPS_SRC}/marisa-trie" \
    -DENABLE_TOOLS=OFF -DENABLE_NATIVE_CODE=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON

  # opencc:USE_SYSTEM_MARISA=ON,理由與 scripts/build_native.sh 相同 ——
  # 不開的話 opencc 會編**自己那份** marisa 並把 marisa.lib 一起裝進同一個
  # prefix,覆蓋掉 deps/marisa-trie 裝的那一份。兩份剛好都是 0.3.1,所以出不了
  # 事,但「誰覆蓋誰」取決於安裝順序,那是會靜默漂移的東西。一份 marisa 就好,
  # 而且是與 Android 同一份。
  #
  # 代價:上游在 USE_SYSTEM_MARISA=ON 時只做 find_library 確認存在,並沒有把
  # include / lib 路徑接到 target 上(它假設 marisa 裝在系統目錄,Windows 沒有
  # 這種東西),所以要自己補:
  #   · include 用 CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES,不用 CMAKE_CXX_FLAGS。
  #     後者是 cache 變數,從命令列給值會**蓋掉** CMake 自己的預設
  #     (/DWIN32 /D_WINDOWS /GR /EHsc),opencc 少了 /EHsc 會整片炸在例外處理上。
  #   · lib 用 /LIBPATH。只有 opencc 的 tools(opencc_dict)需要它 —— 靜態庫本身
  #     不連結。而我們需要那些 tools:Windows 上 host 就是 target,剛編出來的
  #     opencc_dict 跑得起來,.ocd2 詞典可以直接產生。Android 那邊得另外編一份
  #     host opencc,正是因為交叉編譯做不到這件事。
  # 兩者都不耐路徑含空白 —— 腳本開頭已經擋掉。
  build_one opencc "${DEPS_SRC}/opencc" \
    -DUSE_SYSTEM_MARISA=ON \
    -DENABLE_GTEST=OFF -DENABLE_BENCHMARK=OFF \
    -DBUILD_PYTHON=OFF -DBUILD_DOCUMENTATION=OFF \
    -DBUILD_OPENCC_JIEBA_PLUGIN=OFF \
    -DOPENCC_ENABLE_INSTALL=ON \
    "-DCMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES=$(w "${PREFIX}")/include" \
    "-DCMAKE_EXE_LINKER_FLAGS=/machine:x64 /LIBPATH:$(w "${PREFIX}")/lib"

  # librime。與 scripts/build_native.sh 給 Android 的選項逐項對齊,差別只有:
  #   · 沒有 NDK toolchain / ANDROID_* ;
  #   · BUILD_MERGED_PLUGINS 留 ON 但 plugins/ 是空的(本輪不編 lua)。
  # BUILD_SHARED_LIBS=OFF 同時讓上游跳過 tools/ 與 test/ 兩個子目錄。
  build_one librime "${LIBRIME_SRC}" \
    -DBUILD_STATIC=ON \
    -DBUILD_TEST=OFF \
    -DBUILD_SAMPLE=OFF \
    -DBUILD_DATA=OFF \
    -DBUILD_SEPARATE_LIBS=OFF \
    -DBUILD_MERGED_PLUGINS=ON \
    -DENABLE_EXTERNAL_PLUGINS=OFF \
    -DENABLE_LOGGING=ON \
    -DENABLE_THREADING=ON \
    -DENABLE_TIMESTAMP=ON \
    -DINSTALL_PRIVATE_HEADERS=ON \
    -DBoost_NO_BOOST_CMAKE=ON \
    "-DBOOST_ROOT=$(w "${BOOST_ROOT_DIR}")" \
    "-DBoost_INCLUDE_DIR=$(w "${BOOST_ROOT_DIR}")"

  verify_prefix
  stage_opencc_dicts
}

# ---------------------------------------------------------------- 產物檢查
# 「install 沒報錯」不等於「東西真的在」。逐一點名,缺一個就停。
verify_prefix() {
  local missing=0 f
  for f in \
    "${PREFIX}/lib/librime.lib" \
    "${PREFIX}/lib/opencc.lib" \
    "${PREFIX}/lib/marisa.lib" \
    "${PREFIX}/lib/leveldb.lib" \
    "${PREFIX}/include/rime_api.h" \
    "${PREFIX}/share/opencc/t2s.json"
  do
    if [ -e "${f}" ]; then
      printf '    ✓ %s\n' "${f#"${PREFIX}"/}"
    else
      printf '    !! 缺少 %s\n' "${f#"${PREFIX}"/}" >&2
      missing=1
    fi
  done
  # glog 與 yaml-cpp 的 .lib 檔名各版本不同(glog/glogd、yaml-cpp/libyaml-cppmt),
  # 不寫死,只確認找得到。真正的挑選交給 windows/CMakeLists.txt 的 find_library。
  local g y
  g="$(ls "${PREFIX}"/lib/glog*.lib 2>/dev/null | head -1 || true)"
  y="$(ls "${PREFIX}"/lib/*yaml-cpp*.lib 2>/dev/null | head -1 || true)"
  if [ -n "${g}" ]; then printf '    ✓ lib/%s\n' "$(basename "${g}")"
  else echo "    !! 缺少 glog 的 .lib" >&2; missing=1; fi
  if [ -n "${y}" ]; then printf '    ✓ lib/%s\n' "$(basename "${y}")"
  else echo "    !! 缺少 yaml-cpp 的 .lib" >&2; missing=1; fi

  local n
  # `|| true`:沒有任何 .ocd2 時 ls 會非零結束,配上 pipefail 會讓整支腳本
  # 在「印出診斷訊息之前」就死掉,反而看不到真正的原因。
  n="$( (ls "${PREFIX}"/share/opencc/*.ocd2 2>/dev/null || true) | wc -l | tr -d ' ')"
  printf '    opencc 詞典: %s 個 .ocd2\n' "${n}"
  [ "${n}" -gt 0 ] || { echo "    !! 沒有產生任何 .ocd2,簡繁與臺灣字形轉換會失效" >&2; missing=1; }

  [ "${missing}" -eq 0 ] || die "prefix 不完整,見上。"
  log "prefix 檢查通過"
}

# scripts/collect_data.sh 到 third_party/build/host-opencc-install/share/opencc
# 取 .ocd2。那個路徑名是為 Android 取的:交叉編譯時得另外編一份 **host** opencc。
# Windows 上 host 就是 target,上面剛裝好的那份就是 host 版,原樣擺過去即可。
# (collect_data.sh 是四端共用的,不為了 Windows 改它。)
stage_opencc_dicts() {
  local dest="${TP}/build/host-opencc-install/share/opencc"
  rm -rf "${dest}"
  mkdir -p "${dest}"
  cp "${PREFIX}"/share/opencc/*.ocd2 "${dest}/"
  cp "${PREFIX}"/share/opencc/*.json "${dest}/"
  log "opencc 詞典已擺到 collect_data.sh 預期的位置: ${dest}"
}

# ---------------------------------------------------------------- console
build_console() {
  local bdir="${BUILD_ROOT}/console"
  log "[console] configure"
  "${CMAKE}" -S "$(w "${SCRIPT_DIR}")" -B "$(w "${bdir}")" \
    -G Ninja \
    "-DCMAKE_MAKE_PROGRAM=$(w "${NINJA}")" \
    "-DCMAKE_BUILD_TYPE=Release" \
    "-DRIME_PREFIX=$(w "${PREFIX}")" \
    > "${bdir}.configure.log" 2>&1 \
    || { tail -80 "${bdir}.configure.log"; die "console configure 失敗"; }
  log "[console] build"
  "${CMAKE}" --build "$(w "${bdir}")" --parallel "${NPROC}" \
    > "${bdir}.build.log" 2>&1 \
    || { tail -120 "${bdir}.build.log"; die "console build 失敗"; }

  local exe="${bdir}/bin/rime_console.exe"
  [ -f "${exe}" ] || die "沒有產生 ${exe}"
  log "console 完成: ${exe} ($(stat -c%s "${exe}" 2>/dev/null || echo ?) bytes)"
}

# ---------------------------------------------------------------- main
if [ "${DO_DEPS}" -eq 1 ]; then build_deps; fi
if [ "${DO_CONSOLE}" -eq 1 ]; then build_console; fi

log "全部完成 ✓"
