#!/usr/bin/env bash
#
# 交叉編譯 librime 及其相依函式庫到 Android。
#
# 用法:
#   scripts/build_native.sh                 # 預設編譯 arm64-v8a x86_64
#   scripts/build_native.sh arm64-v8a       # 只編譯指定 ABI
#   scripts/build_native.sh --clean x86_64  # 先清掉中間產物再編譯
#   scripts/build_native.sh --no-lua        # 不編 librime-lua 外掛(對照用)
#
# 產出:
#   third_party/build/<abi>/       中間產物(不進 git)
#   third_party/prebuilt/<abi>/include, lib
#   third_party/prebuilt/manifest.json
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RIME_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

THIRD_PARTY="${RIME_ROOT}/third_party"
LIBRIME_SRC="${THIRD_PARTY}/librime"
DEPS_SRC="${LIBRIME_SRC}/deps"
BUILD_ROOT="${THIRD_PARTY}/build"
PREBUILT_ROOT="${THIRD_PARTY}/prebuilt"
PATCH_DIR="${RIME_ROOT}/patches"

# ---------------------------------------------------------------- 工具鏈設定
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-${HOME}/Android/Sdk}}"
NDK_VERSION="${NDK_VERSION:-27.2.12479018}"
ANDROID_NDK="${ANDROID_NDK:-${ANDROID_SDK_ROOT}/ndk/${NDK_VERSION}}"
# 必須使用 SDK 內附的 cmake 3.22.1:CMake 4.x 已移除 FindBoost,且對舊的
# cmake_minimum_required 下限更嚴,會讓數個相依函式庫直接設定失敗。
SDK_CMAKE_DIR="${SDK_CMAKE_DIR:-${ANDROID_SDK_ROOT}/cmake/3.22.1}"
CMAKE_BIN="${CMAKE_BIN:-${SDK_CMAKE_DIR}/bin/cmake}"
NINJA_BIN="${NINJA_BIN:-${SDK_CMAKE_DIR}/bin/ninja}"
ANDROID_API="${ANDROID_API:-21}"
ANDROID_STL_VALUE="${ANDROID_STL_VALUE:-c++_static}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"

BOOST_VERSION="${BOOST_VERSION:-1.89.0}"
BOOST_ROOT_DIR="${THIRD_PARTY}/boost"

# ---------------------------------------------------------------- librime-lua
# 現代方案(雾凇拼音 rime-ice、萬象 moran、openfly、keydo、五筆86 極點…)幾乎
# 都靠 lua_translator / lua_filter。缺這個外掛時「部署會回報 SUCCESS,但引擎少
# 了 translator,按下去沒有任何候選」—— 是最難察覺的失敗模式,所以一定要編進來。
#
# 編法:librime 的 plugin 機制 = 把外掛原始碼放進 <librime>/plugins/<name>/,
# 由 plugins/CMakeLists.txt 的 file(GLOB) 自動撿到並 add_subdirectory()。
# 搭配 BUILD_MERGED_PLUGINS=ON,外掛的 object 檔會被直接併進 librime.a,
# 且 CMake 會把 RIME_EXTRA_MODULES=(lua) 定義進 rime_api.cc,讓 RimeSetup()
# 自動 rime_require_module_lua() —— 這正是「外掛真的有註冊」的關鍵。
# ENABLE_EXTERNAL_PLUGINS 仍然維持 OFF(boost::dll 動態載入在 Android 無意義)。
#
# Lua 直譯器:不找系統套件(交叉編譯到 Android 根本找不到),改用上游自己維護的
# `thirdparty` 分支所附的 Lua 5.4.8 原始碼 —— librime-lua 的 CMakeLists 只要看到
# thirdparty/lua5.4/lua.h 就會走 in-tree 分支,把 Lua 一起編進外掛的 object 檔。
LIBRIME_LUA_SRC="${THIRD_PARTY}/librime-lua"
LIBRIME_LUA_REPO="${LIBRIME_LUA_REPO:-https://github.com/hchunhui/librime-lua.git}"
# 釘住 commit,避免上游改動讓建置不可重現。
LIBRIME_LUA_COMMIT="${LIBRIME_LUA_COMMIT:-ec52e48ea18f11af37717a01c337f853215cf70b}"
# 同一個 repo 的 `thirdparty` 分支只放 Lua 原始碼(lua5.3/ 與 lua5.4/),
# 對應上游的 action-install.sh。lua5.4 = Lua 5.4.8。
LIBRIME_LUA_TP_COMMIT="${LIBRIME_LUA_TP_COMMIT:-fa40fadd8af1e5b1fbd55703ccbd54476956d74c}"
LUA_VERSION="5.4.8"
ENABLE_LUA="${ENABLE_LUA:-1}"

DEFAULT_ABIS=(arm64-v8a x86_64)

# ---------------------------------------------------------------- 參數解析
DO_CLEAN=0
# NDK 的 cmake toolchain 預設會加 -g,未剝離的 librime.a 高達 ~118 MiB。
# 匯出到 prebuilt/ 時預設剝掉 DWARF(保留所有連結需要的符號),
# 想保留除錯資訊請加 --keep-debug 或設 KEEP_DEBUG=1。
KEEP_DEBUG="${KEEP_DEBUG:-0}"
ABIS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean) DO_CLEAN=1 ;;
    --keep-debug) KEEP_DEBUG=1 ;;
    --no-lua) ENABLE_LUA=0 ;;
    --api) shift; ANDROID_API="$1" ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    -*) die "未知選項: $1" ;;
    *) ABIS+=("$1") ;;
  esac
  shift
done
[[ ${#ABIS[@]} -eq 0 ]] && ABIS=("${DEFAULT_ABIS[@]}")

for abi in "${ABIS[@]}"; do
  case "$abi" in
    armeabi-v7a|arm64-v8a|x86|x86_64) ;;
    *) die "不支援的 ABI: $abi" ;;
  esac
done

# ---------------------------------------------------------------- 前置檢查
[[ -x "${CMAKE_BIN}" ]]  || die "找不到 SDK 內附 cmake: ${CMAKE_BIN}"
[[ -x "${NINJA_BIN}" ]]  || die "找不到 SDK 內附 ninja: ${NINJA_BIN}"
[[ -d "${ANDROID_NDK}" ]] || die "找不到 NDK: ${ANDROID_NDK}"
TOOLCHAIN_FILE="${ANDROID_NDK}/build/cmake/android.toolchain.cmake"
[[ -f "${TOOLCHAIN_FILE}" ]] || die "找不到 NDK toolchain file: ${TOOLCHAIN_FILE}"
[[ -f "${LIBRIME_SRC}/CMakeLists.txt" ]] || die "librime 原始碼不在 ${LIBRIME_SRC}"

CMAKE_VER="$("${CMAKE_BIN}" --version | head -1 | awk '{print $3}')"
case "${CMAKE_VER}" in
  3.*) ;;
  *) die "偵測到 cmake ${CMAKE_VER};本腳本必須使用 CMake 3.x(SDK 內附 3.22.1)" ;;
esac

LLVM_BIN="${ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin"
[[ -d "${LLVM_BIN}" ]] || LLVM_BIN="$(echo "${ANDROID_NDK}"/toolchains/llvm/prebuilt/*/bin)"
READELF="${LLVM_BIN}/llvm-readelf"
CLANGXX="${LLVM_BIN}/clang++"
LLVM_AR="${LLVM_BIN}/llvm-ar"
LLVM_STRIP="${LLVM_BIN}/llvm-strip"
LLVM_NM="${LLVM_BIN}/llvm-nm"

# ---------------------------------------------------------------- Boost headers
ensure_boost() {
  if [[ -f "${BOOST_ROOT_DIR}/boost/version.hpp" ]]; then
    log "Boost headers 已就緒: ${BOOST_ROOT_DIR}"
    return
  fi
  local underscored="${BOOST_VERSION//./_}"
  local tarball="${THIRD_PARTY}/boost_${underscored}.tar.gz"
  local url="https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${underscored}.tar.gz"
  mkdir -p "${THIRD_PARTY}"
  if [[ ! -f "${tarball}" ]]; then
    log "下載 Boost ${BOOST_VERSION} …"
    curl -fSL --retry 3 -o "${tarball}.part" "${url}"
    mv "${tarball}.part" "${tarball}"
  fi
  log "解壓 Boost ${BOOST_VERSION} …"
  rm -rf "${THIRD_PARTY}/boost_${underscored}" "${BOOST_ROOT_DIR}"
  tar xzf "${tarball}" -C "${THIRD_PARTY}"
  mv "${THIRD_PARTY}/boost_${underscored}" "${BOOST_ROOT_DIR}"
  [[ -f "${BOOST_ROOT_DIR}/boost/version.hpp" ]] || die "Boost 解壓失敗"
}

# ---------------------------------------------------------------- librime-lua
# 淺層取回釘住的 commit(不是 --depth 1 抓 HEAD —— 那樣上游一動就不可重現)。
fetch_pinned() {
  local dir="$1" url="$2" commit="$3" name="$4"
  if [[ -d "${dir}/.git" ]]; then
    local cur; cur="$(git -C "${dir}" rev-parse HEAD 2>/dev/null || echo)"
    if [[ "${cur}" == "${commit}" ]]; then
      log "${name} 已在 ${commit:0:8}"
      return
    fi
    log "${name} 目前在 ${cur:0:8},切到 ${commit:0:8}"
  else
    log "取得 ${name} @ ${commit:0:8} …"
    rm -rf "${dir}"
    mkdir -p "${dir}"
    git -C "${dir}" init -q
    git -C "${dir}" remote add origin "${url}"
  fi
  git -C "${dir}" fetch -q --depth 1 origin "${commit}" \
    || die "抓取 ${name} 的 ${commit} 失敗"
  git -C "${dir}" checkout -q --detach FETCH_HEAD || die "checkout ${name} 失敗"
}

ensure_librime_lua() {
  local plugin_dir="${LIBRIME_SRC}/plugins/lua"
  if [[ "${ENABLE_LUA}" != "1" ]]; then
    log "ENABLE_LUA=0,移除 librime 的 lua 外掛目錄"
    rm -rf "${plugin_dir}"
    return
  fi

  fetch_pinned "${LIBRIME_LUA_SRC}" "${LIBRIME_LUA_REPO}" \
               "${LIBRIME_LUA_COMMIT}" "librime-lua"
  # Lua 原始碼(上游 action-install.sh 的做法:同 repo 的 thirdparty 分支)
  fetch_pinned "${LIBRIME_LUA_SRC}/thirdparty" "${LIBRIME_LUA_REPO}" \
               "${LIBRIME_LUA_TP_COMMIT}" "librime-lua/thirdparty(Lua ${LUA_VERSION})"
  [[ -f "${LIBRIME_LUA_SRC}/thirdparty/lua5.4/lua.h" ]] \
    || die "Lua 原始碼不在 ${LIBRIME_LUA_SRC}/thirdparty/lua5.4"
  # 掛進 librime 的 plugins/。用真實目錄的 symlink:CMake 的
  # file(GLOB)+IS_DIRECTORY 認得 symlink,add_subdirectory 也吃得下。
  mkdir -p "${LIBRIME_SRC}/plugins"
  if [[ -L "${plugin_dir}" ]]; then
    [[ "$(readlink -f "${plugin_dir}")" == "$(readlink -f "${LIBRIME_LUA_SRC}")" ]] \
      || { rm -f "${plugin_dir}"; ln -s "${LIBRIME_LUA_SRC}" "${plugin_dir}"; }
  elif [[ -e "${plugin_dir}" ]]; then
    die "${plugin_dir} 已存在且不是 symlink,請手動處理"
  else
    ln -s "${LIBRIME_LUA_SRC}" "${plugin_dir}"
  fi
  log "librime-lua 已掛在 ${plugin_dir} -> ${LIBRIME_LUA_SRC}"
}

# ---------------------------------------------------------------- patches
# patches/<name>.patch 會被套用到對應的 submodule。命名規則是
# "<submodule 相對路徑>@<說明>.patch",路徑裡的 / 寫成 __,
# 例如 deps__opencc@skip-tools.patch -> 套用於 third_party/librime/deps/opencc。
#
# 兩個特例(名字直接對到頂層目錄,不走 librime/ 底下的相對路徑):
#   librime@...      -> third_party/librime
#   librime-lua@...  -> third_party/librime-lua
# librime-lua 之所以要特例:它實際是以 symlink 掛在
# third_party/librime/plugins/lua,寫成 plugins__lua@... 也能用,但那樣
# patch 檔名就綁死在 symlink 的名字上,改名就靜默失效。直接指名來源目錄。
apply_patches() {
  shopt -s nullglob
  local patches=("${PATCH_DIR}"/*.patch)
  shopt -u nullglob
  if [[ ${#patches[@]} -eq 0 ]]; then
    log "無 patch 需要套用"
    return
  fi
  local p
  for p in "${patches[@]}"; do
    local base target rel
    base="$(basename "${p}" .patch)"
    rel="${base%%@*}"
    rel="${rel//__//}"
    if [[ "${rel}" == "librime" ]]; then
      target="${LIBRIME_SRC}"
    elif [[ "${rel}" == "librime-lua" ]]; then
      target="${LIBRIME_LUA_SRC}"
    else
      target="${LIBRIME_SRC}/${rel}"
    fi
    [[ -d "${target}" ]] || die "patch ${p} 的目標目錄不存在: ${target}"
    if git -C "${target}" apply --reverse --check "${p}" >/dev/null 2>&1; then
      log "patch 已套用,略過: $(basename "${p}")"
    else
      log "套用 patch: $(basename "${p}") -> ${target}"
      git -C "${target}" apply "${p}" || die "套用 patch 失敗: ${p}"
    fi
  done
}

# ---------------------------------------------------------------- cmake 共用參數
common_cmake_args() {
  local abi="$1" prefix="$2"
  COMMON_ARGS=(
    -G Ninja
    "-DCMAKE_MAKE_PROGRAM=${NINJA_BIN}"
    "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}"
    "-DANDROID_ABI=${abi}"
    "-DANDROID_PLATFORM=android-${ANDROID_API}"
    "-DANDROID_STL=${ANDROID_STL_VALUE}"
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DCMAKE_INSTALL_PREFIX=${prefix}"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DCMAKE_FIND_ROOT_PATH=${prefix};${BOOST_ROOT_DIR}"
    "-DCMAKE_PREFIX_PATH=${prefix}"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_TESTING=OFF
  )
}

configure_and_install() {
  # $1 = 名稱, $2 = 原始碼目錄, $3 = build 目錄, 其餘 = 額外 cmake 參數
  local name="$1" src="$2" bdir="$3"; shift 3
  log "[${ABI}] 設定 ${name}"
  "${CMAKE_BIN}" -S "${src}" -B "${bdir}" "${COMMON_ARGS[@]}" "$@" \
    > "${bdir}.configure.log" 2>&1 || { tail -60 "${bdir}.configure.log"; die "${name} configure 失敗"; }
  log "[${ABI}] 編譯並安裝 ${name}"
  "${CMAKE_BIN}" --build "${bdir}" --target install -j "${JOBS}" \
    > "${bdir}.build.log" 2>&1 || { tail -60 "${bdir}.build.log"; die "${name} build 失敗"; }
}

# ---------------------------------------------------------------- 單一 ABI 建置
build_abi() {
  ABI="$1"
  local bdir="${BUILD_ROOT}/${ABI}"
  local prefix="${bdir}/prefix"

  if [[ ${DO_CLEAN} -eq 1 ]]; then
    log "[${ABI}] 清除 ${bdir}"
    rm -rf "${bdir}"
  fi
  mkdir -p "${bdir}" "${prefix}"

  common_cmake_args "${ABI}" "${prefix}"

  # ---- glog(關掉 gflags / gtest / 測試)
  configure_and_install glog "${DEPS_SRC}/glog" "${bdir}/glog" \
    -DWITH_GFLAGS=OFF -DWITH_GTEST=OFF -DWITH_PKGCONFIG=OFF -DWITH_UNWIND=OFF

  # ---- yaml-cpp(關掉測試 / 工具 / contrib)
  configure_and_install yaml-cpp "${DEPS_SRC}/yaml-cpp" "${bdir}/yaml-cpp" \
    -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_CPP_BUILD_TOOLS=OFF \
    -DYAML_CPP_BUILD_TESTS=OFF -DYAML_BUILD_SHARED_LIBS=OFF -DYAML_CPP_INSTALL=ON

  # ---- leveldb(關掉 benchmark / tests)
  configure_and_install leveldb "${DEPS_SRC}/leveldb" "${bdir}/leveldb" \
    -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF -DLEVELDB_INSTALL=ON

  # ---- marisa-trie(關掉 tools / 測試)
  configure_and_install marisa "${DEPS_SRC}/marisa-trie" "${bdir}/marisa-trie" \
    -DENABLE_TOOLS=OFF -DENABLE_NATIVE_CODE=OFF

  # ---- opencc
  # opencc 的 data/ 子目錄有一個 ALL target,會執行剛編出來的 opencc_dict
  # 去產生 .ocd2 詞典 —— 交叉編譯時那是 Android 執行檔,host 跑不起來。
  # 因此這裡只 build `libopencc` 這個 target,並手動安裝標頭與 .a,
  # 完全避開 data/ 與 src/tools/。
  # 同時用 USE_SYSTEM_MARISA=ON 改用上面剛裝好的 marisa,避免 opencc 內附的
  # marisa 副本產生第二份 libmarisa.a。
  local opencc_build="${bdir}/opencc"
  log "[${ABI}] 設定 opencc"
  "${CMAKE_BIN}" -S "${DEPS_SRC}/opencc" -B "${opencc_build}" "${COMMON_ARGS[@]}" \
    -DOPENCC_ENABLE_INSTALL=OFF \
    -DENABLE_GTEST=OFF -DENABLE_BENCHMARK=OFF \
    -DBUILD_PYTHON=OFF -DBUILD_DOCUMENTATION=OFF \
    -DBUILD_OPENCC_JIEBA_PLUGIN=OFF \
    -DUSE_SYSTEM_MARISA=ON \
    "-DCMAKE_CXX_FLAGS=-I${prefix}/include" \
    > "${opencc_build}.configure.log" 2>&1 \
    || { tail -60 "${opencc_build}.configure.log"; die "opencc configure 失敗"; }
  log "[${ABI}] 編譯 opencc(僅 libopencc target)"
  "${CMAKE_BIN}" --build "${opencc_build}" --target libopencc -j "${JOBS}" \
    > "${opencc_build}.build.log" 2>&1 \
    || { tail -60 "${opencc_build}.build.log"; die "opencc build 失敗"; }
  mkdir -p "${prefix}/include/opencc/plugin" "${prefix}/lib"
  cp -f "${opencc_build}/src/libopencc.a" "${prefix}/lib/"
  cp -f "${DEPS_SRC}"/opencc/src/*.hpp "${prefix}/include/opencc/"
  cp -f "${DEPS_SRC}/opencc/src/opencc.h" "${prefix}/include/opencc/"
  cp -f "${opencc_build}/src/Opencc_Export.h" "${prefix}/include/opencc/"
  cp -f "${DEPS_SRC}/opencc/src/plugin/OpenCCPlugin.h" "${prefix}/include/opencc/plugin/"

  # ---- librime
  # 注意:CMAKE_SYSTEM_NAME=Android 時 CMake 的 LINUX 為假,librime 會走
  # find_package(Boost 1.77.0) 這條(不帶 COMPONENTS)= header-only Boost,
  # 因此不需要編譯任何 Boost 二進位。千萬別誤觸發 LINUX 分支。
  # BUILD_MERGED_PLUGINS=ON 是 lua 生效的關鍵:
  #   1. 外掛的 object 檔(含 Lua 5.4 直譯器)直接併進 librime.a,不另外產生
  #      .so/.a,所以連結順序不變;
  #   2. 上層 CMakeLists 會把 rime_plugins_modules 展開成
  #      RIME_EXTRA_MODULES=(lua) 定義給 rime_api.cc,RimeSetup() 就會呼叫
  #      rime_require_module_lua(),外掛才真的被註冊進 Registry。
  #      設成 OFF 的話外掛會編成獨立 target 且沒人 require,等於白編。
  configure_and_install librime "${LIBRIME_SRC}" "${bdir}/librime" \
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
    "-DBOOST_ROOT=${BOOST_ROOT_DIR}" \
    "-DBoost_INCLUDE_DIR=${BOOST_ROOT_DIR}"

  # ---- 匯出到 prebuilt/
  local out="${PREBUILT_ROOT}/${ABI}"
  rm -rf "${out}"
  mkdir -p "${out}/include" "${out}/lib"
  cp -a "${prefix}/include/." "${out}/include/"
  local a
  for a in "${prefix}"/lib/*.a; do
    cp -f "${a}" "${out}/lib/"
    if [[ "${KEEP_DEBUG}" != "1" ]]; then
      "${LLVM_STRIP}" --strip-debug "${out}/lib/$(basename "${a}")"
    fi
  done
  log "[${ABI}] 匯出至 ${out}(strip-debug=$([[ ${KEEP_DEBUG} == 1 ]] && echo no || echo yes))"

  verify_arch "${ABI}" "${out}"
  verify_lua "${ABI}" "${out}" "${bdir}/librime.configure.log"
}

# ---------------------------------------------------------------- lua 驗證
# 「編得起來」不等於「外掛有註冊」。這裡在 .a 的層級把三件事一次驗掉:
#   1. cmake 真的看到 plugin(configure log 的 "Found plugin");
#   2. librime.a 裡有 rime_require_module_lua 這個「定義」(T,不是 U);
#   3. rime_api.cc.o 裡有對它的「引用」—— 這是 RIME_EXTRA_MODULES 有生效的證明。
# 少了第 3 點就是典型的假成功:符號在庫裡但沒人叫它,RimeSetup() 不會註冊
# lua_translator,方案部署照樣 SUCCESS 卻打不出字。
verify_lua() {
  local abi="$1" out="$2" conflog="$3"
  if [[ "${ENABLE_LUA}" != "1" ]]; then
    warn "[${abi}] ENABLE_LUA=0,略過 lua 驗證"
    return
  fi
  grep -q "Found plugin: .*plugins/lua" "${conflog}" \
    || die "[${abi}] cmake 沒有看到 lua 外掛(configure log 無 'Found plugin')"
  grep -q "rime_plugins_modules: .*lua" "${conflog}" \
    || die "[${abi}] cmake 沒把 lua 列入 rime_plugins_modules,RIME_EXTRA_MODULES 不會生效"

  # rime_require_module_lua 是在 C++ 裡宣告/定義的(rime_api.h 的
  # RIME_REGISTER_MODULE 巨集在 extern "C" 區塊外),所以符號是 mangled 的
  # _Z23rime_require_module_luav —— 直接找未修飾名字會什麼都找不到。
  # 注意:llvm-nm 的輸出動輒數十萬行,直接 `nm | grep -q` 會讓 grep 提早關掉
  # pipe,nm 收到 SIGPIPE 而非零退出,配上 set -o pipefail 整條就被判成失敗。
  # 這個坑 verify_arch() 的註解也提過,所以這裡一律先落檔再查。
  local a="${out}/lib/librime.a" sym="_Z23rime_require_module_luav"
  local syms="${BUILD_ROOT}/${abi}/.librime.nm.txt"
  mkdir -p "$(dirname "${syms}")"
  "${LLVM_NM}" "${a}" > "${syms}" 2>/dev/null || true
  grep -q " T ${sym}$" "${syms}" \
    || die "[${abi}] librime.a 沒有 rime_require_module_lua 的定義"
  grep -q "^ *U ${sym}$" "${syms}" \
    || die "[${abi}] 沒有任何目的檔引用 rime_require_module_lua ——
    代表 RIME_EXTRA_MODULES 沒被定義進 rime_api.cc,RimeSetup() 不會註冊 lua_*
    元件。這正是「部署 SUCCESS 但打不出字」的成因,絕不可放行。"
  # Lua 直譯器本體也要在(不是只有 glue code)
  grep -q " T lua_pcallk$" "${syms}" \
    || die "[${abi}] librime.a 裡沒有 Lua 直譯器符號(lua_pcallk)"
  rm -f "${syms}"
  log "[${abi}] lua 驗證通過:外掛已註冊 + Lua ${LUA_VERSION} 直譯器已編入 librime.a"
}

# ---------------------------------------------------------------- 架構驗證
verify_arch() {
  local abi="$1" out="$2" expect
  case "${abi}" in
    arm64-v8a) expect="AArch64" ;;
    x86_64)    expect="X86-64" ;;
    armeabi-v7a) expect="ARM" ;;
    x86)       expect="386" ;;
  esac
  local tmp a
  tmp="${BUILD_ROOT}/${abi}/.verify"
  rm -rf "${tmp}"; mkdir -p "${tmp}"
  for a in "${out}"/lib/*.a; do
    # 直接對 .a 跑 readelf 會把每個 member 都印一遍(librime.a 有數千個),
    # 所以抽出第一個 member 再檢查。也避免 pipefail + SIGPIPE 讓腳本靜默中止。
    local members first machine
    members="$("${LLVM_AR}" t "${a}")" || die "無法列出 ${a} 的成員"
    first="${members%%$'\n'*}"
    [[ -n "${first}" ]] || die "${a} 是空的 archive"
    "${LLVM_AR}" p "${a}" "${first}" > "${tmp}/member.o" || die "無法抽出 ${a} 的 ${first}"
    machine="$("${READELF}" -h "${tmp}/member.o" | awk -F': +' '/Machine:/ {m=$2} END {print m}')"
    [[ -n "${machine}" ]] || die "無法讀取 ${a} 的 ELF header"
    case "${machine}" in
      *"${expect}"*) ;;
      *) die "$(basename "${a}") 架構錯誤:預期 ${expect},實得 ${machine}" ;;
    esac
    log "[${abi}]   $(basename "${a}"): ${machine}"
  done
  rm -rf "${tmp}"
  log "[${abi}] 架構驗證通過 (${expect})"
}

# ---------------------------------------------------------------- 連結順序
# 靜態庫的順序會影響連結成敗:被依賴者要排在依賴者之後。
#   librime -> opencc, glog, yaml-cpp, leveldb, marisa
#   opencc  -> marisa
link_order() {
  printf '%s\n' librime.a libopencc.a libmarisa.a libleveldb.a libyaml-cpp.a libglog.a
}
system_libs() { printf '%s\n' log m; }

# ---------------------------------------------------------------- 煙霧測試
smoke_test() {
  local abi="$1"
  local out="${PREBUILT_ROOT}/${abi}"
  local work="${BUILD_ROOT}/${abi}/smoke"
  local target api="${ANDROID_API}"
  case "${abi}" in
    arm64-v8a)   target="aarch64-linux-android" ;;
    x86_64)      target="x86_64-linux-android" ;;
    armeabi-v7a) target="armv7a-linux-androideabi" ;;
    x86)         target="i686-linux-android" ;;
  esac
  mkdir -p "${work}"

  local libs=()
  local l
  while read -r l; do libs+=("${out}/lib/${l}"); done < <(link_order)
  local syslibs=()
  while read -r l; do syslibs+=("-l${l}"); done < <(system_libs)

  local common=(
    --target="${target}${api}"
    -std=c++17 -fPIC -O2
    "-I${out}/include"
    "-I${RIME_ROOT}/core/include"
    -stdlib=libc++
    -static-libstdc++
  )

  log "[${abi}] 煙霧測試:連結成 executable"
  "${CLANGXX}" "${common[@]}" \
    "${SCRIPT_DIR}/smoke_test.cc" -o "${work}/rime_smoke" \
    "${libs[@]}" "${syslibs[@]}" \
    > "${work}/exe.log" 2>&1 || { cat "${work}/exe.log"; die "[${abi}] 煙霧測試(exe)連結失敗"; }

  # .so 這一關是真正的關卡:--whole-archive 會把 librime.a 裡「所有」目的檔
  # 都拉進來(包括 lua 外掛與 Lua 直譯器),再配 --no-undefined 要求零 undefined。
  # 一起編 core/src/rime_shell.cc,連 app 實際用的門面層一併驗掉。
  log "[${abi}] 煙霧測試:連結成 shared library(含 rime_shell.cc,--no-undefined)"
  "${CLANGXX}" "${common[@]}" -shared \
    "${SCRIPT_DIR}/smoke_test.cc" "${RIME_ROOT}/core/src/rime_shell.cc" \
    -o "${work}/librime_jni_smoke.so" \
    -Wl,--no-undefined \
    -Wl,--whole-archive "${out}/lib/librime.a" -Wl,--no-whole-archive \
    "${libs[@]:1}" "${syslibs[@]}" \
    > "${work}/so.log" 2>&1 || { cat "${work}/so.log"; die "[${abi}] 煙霧測試(so)連結失敗"; }

  # --no-undefined 已經保證「連得起來就沒有解不掉的符號」,剩下的 UND 動態符號
  # 一律來自 DT_NEEDED 的系統庫(libc/libm/libdl/liblog)。這裡再明著確認一次:
  # 不可以有任何屬於「我們自己這幾包」的符號留在 UND —— 那代表某個 .a 沒連進來。
  local undef
  undef="$("${READELF}" --dyn-syms "${work}/librime_jni_smoke.so" \
           | awk '$7=="UND" && $8!="" {print $8}' \
           | grep -E '^(rime_|Rime|lua_|luaL_|luaopen_|opencc_|marisa|leveldb|YAML|google::|yaml)' \
           || true)"
  if [[ -n "${undef}" ]]; then
    printf '%s\n' "${undef}" | sed 's/^/    /'
    die "[${abi}] .so 仍有屬於本專案相依的 undefined symbol(見上)"
  fi
  local nneed
  nneed="$("${READELF}" -d "${work}/librime_jni_smoke.so" \
           | awk -F'[][]' '/NEEDED/{printf "%s ", $2}')"
  log "[${abi}] .so 零 undefined symbol(DT_NEEDED: ${nneed})"

  local m
  m="$("${READELF}" -h "${work}/rime_smoke" | awk -F': +' '/Machine:/ {v=$2} END {print v}')"
  log "[${abi}] 煙霧測試通過 (exe machine=${m}, so=$(du -h "${work}/librime_jni_smoke.so" | cut -f1))"
}

# ---------------------------------------------------------------- manifest
write_manifest() {
  log "產生 manifest.json"
  RIME_ROOT="${RIME_ROOT}" \
  PREBUILT_ROOT="${PREBUILT_ROOT}" \
  LIBRIME_SRC="${LIBRIME_SRC}" \
  BOOST_ROOT_DIR="${BOOST_ROOT_DIR}" \
  BOOST_VERSION="${BOOST_VERSION}" \
  NDK_VERSION="${NDK_VERSION}" \
  ANDROID_NDK="${ANDROID_NDK}" \
  ANDROID_API="${ANDROID_API}" \
  ANDROID_STL_VALUE="${ANDROID_STL_VALUE}" \
  BUILD_TYPE="${BUILD_TYPE}" \
  CMAKE_VER="${CMAKE_VER}" \
  KEEP_DEBUG="${KEEP_DEBUG}" \
  ENABLE_LUA="${ENABLE_LUA}" \
  LIBRIME_LUA_SRC="${LIBRIME_LUA_SRC}" \
  LIBRIME_LUA_REPO="${LIBRIME_LUA_REPO}" \
  LIBRIME_LUA_COMMIT="${LIBRIME_LUA_COMMIT}" \
  LIBRIME_LUA_TP_COMMIT="${LIBRIME_LUA_TP_COMMIT}" \
  LUA_VERSION="${LUA_VERSION}" \
  LINK_ORDER="$(link_order | tr '\n' ' ')" \
  SYSTEM_LIBS="$(system_libs | tr '\n' ' ')" \
  ABIS="${ABIS[*]}" \
  python3 "${SCRIPT_DIR}/gen_manifest.py"
}

# ---------------------------------------------------------------- main
log "RIME_ROOT      = ${RIME_ROOT}"
log "NDK            = ${ANDROID_NDK}"
log "cmake          = ${CMAKE_BIN} (${CMAKE_VER})"
log "API level      = ${ANDROID_API}"
log "STL            = ${ANDROID_STL_VALUE}"
log "ABIs           = ${ABIS[*]}"
log "librime-lua    = $([[ ${ENABLE_LUA} == 1 ]] && echo "ON (${LIBRIME_LUA_COMMIT:0:8}, Lua ${LUA_VERSION})" || echo OFF)"

ensure_boost
ensure_librime_lua
apply_patches
mkdir -p "${BUILD_ROOT}" "${PREBUILT_ROOT}"

for abi in "${ABIS[@]}"; do
  build_abi "${abi}"
  smoke_test "${abi}"
done

write_manifest
log "全部完成 ✓"
