#!/usr/bin/env bash
#
# build_macos.sh — 在 macOS 上編譯 librime 及其依賴為靜態庫，並編譯 rime_console
#
# 與 Android 端的 scripts/build_native.sh 對等，差異：
#   - 不走 NDK 交叉編譯，直接用系統 clang
#   - 不產出 prebuilt/（macOS 靜態庫只在 CI 用，不入版控）
#   - 兼顧 CMake 3.x 與 4.x（4.x 移除了 FindBoost）
#
# 先決條件：
#   - macOS + Xcode command line tools（提供 clang、cmake、git）
#   - 網路（取得 librime、Boost、librime-lua 原始碼）
#
# 用法:
#   apple/scripts/build_macos.sh              # 編譯全部
#   apple/scripts/build_macos.sh --clean      # 清掉中間產物再編
#
# 產出:
#   apple/build/prefix/                       依賴與 librime 的 install prefix
#   apple/build/rime_console                  端到端測試執行檔
#   third_party/build/host-opencc-install/    host opencc（供 collect_data.sh 產生 .ocd2）
#
set -euo pipefail

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RIME_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

THIRD_PARTY="${RIME_ROOT}/third_party"
LIBRIME_SRC="${THIRD_PARTY}/librime"
DEPS_SRC="${LIBRIME_SRC}/deps"
BUILD_ROOT="${RIME_ROOT}/apple/build"
PREFIX="${BUILD_ROOT}/prefix"
PATCH_DIR="${RIME_ROOT}/patches"

CMAKE_BIN="${CMAKE_BIN:-cmake}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

# ── librime ── 釘死 commit，與 Android 端 prebuilt/manifest.json 一致
LIBRIME_REPO="https://github.com/rime/librime.git"
LIBRIME_COMMIT="1d0df6e40cdcac17a986adc65e4668ae84ae0ada"

# ── Boost (header-only) ── 與 Android 端一致
BOOST_VERSION="${BOOST_VERSION:-1.89.0}"
BOOST_ROOT_DIR="${THIRD_PARTY}/boost"

# ── librime-lua ── 與 Android 端 build_native.sh 一致
LIBRIME_LUA_SRC="${THIRD_PARTY}/librime-lua"
LIBRIME_LUA_REPO="https://github.com/hchunhui/librime-lua.git"
LIBRIME_LUA_COMMIT="ec52e48ea18f11af37717a01c337f853215cf70b"
LIBRIME_LUA_TP_COMMIT="fa40fadd8af1e5b1fbd55703ccbd54476956d74c"
LUA_VERSION="5.4.8"

# ---------------------------------------------------------------- 參數解析
DO_CLEAN=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean) DO_CLEAN=1 ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "未知選項: $1" ;;
  esac
  shift
done

if [[ ${DO_CLEAN} -eq 1 ]]; then
  log "清除 ${BUILD_ROOT}"
  rm -rf "${BUILD_ROOT}"
fi
mkdir -p "${BUILD_ROOT}" "${PREFIX}"

# ---------------------------------------------------------------- 前置檢查
CMAKE_VER="$("${CMAKE_BIN}" --version | head -1 | awk '{print $3}')"
CMAKE_MAJOR="${CMAKE_VER%%.*}"
log "CMake: ${CMAKE_BIN} (${CMAKE_VER})"
command -v clang++ >/dev/null 2>&1 || die "找不到 clang++"

# ---------------------------------------------------------------- 取得 librime
ensure_librime() {
  if [[ -f "${LIBRIME_SRC}/CMakeLists.txt" ]]; then
    local cur
    cur="$(git -C "${LIBRIME_SRC}" rev-parse HEAD 2>/dev/null || echo none)"
    if [[ "${cur}" == "${LIBRIME_COMMIT}" ]]; then
      log "librime 已在 ${LIBRIME_COMMIT:0:8}"
      # 確保子模組已初始化
      git -C "${LIBRIME_SRC}" submodule update --init --depth 1 \
        deps/glog deps/yaml-cpp deps/leveldb deps/marisa-trie deps/opencc \
        2>/dev/null || true
      return
    fi
    log "librime 在 ${cur:0:8}，需要 ${LIBRIME_COMMIT:0:8}，重新取得"
    rm -rf "${LIBRIME_SRC}"
  fi

  log "取得 librime @ ${LIBRIME_COMMIT:0:8} …"
  mkdir -p "${LIBRIME_SRC}"
  git -C "${LIBRIME_SRC}" init -q
  git -C "${LIBRIME_SRC}" remote add origin "${LIBRIME_REPO}"
  git -C "${LIBRIME_SRC}" fetch -q --depth 1 origin "${LIBRIME_COMMIT}" \
    || die "抓取 librime 失敗"
  git -C "${LIBRIME_SRC}" checkout -q FETCH_HEAD

  log "初始化 librime 子模組（deps）…"
  git -C "${LIBRIME_SRC}" submodule update --init --depth 1 \
    deps/glog deps/yaml-cpp deps/leveldb deps/marisa-trie deps/opencc

  for dep in glog yaml-cpp leveldb marisa-trie opencc; do
    [[ -f "${DEPS_SRC}/${dep}/CMakeLists.txt" ]] \
      || die "子模組 deps/${dep} 初始化失敗"
  done
}

# ---------------------------------------------------------------- Boost headers
ensure_boost() {
  if [[ -f "${BOOST_ROOT_DIR}/boost/version.hpp" ]]; then
    log "Boost headers 已就緒"
    return
  fi
  local underscored="${BOOST_VERSION//./_}"
  local tarball="${THIRD_PARTY}/boost_${underscored}.tar.gz"
  local url="https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${underscored}.tar.gz"
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
fetch_pinned() {
  local dir="$1" url="$2" commit="$3" name="$4"
  if [[ -d "${dir}/.git" ]]; then
    local cur; cur="$(git -C "${dir}" rev-parse HEAD 2>/dev/null || echo)"
    if [[ "${cur}" == "${commit}" ]]; then
      log "${name} 已在 ${commit:0:8}"
      return
    fi
  else
    rm -rf "${dir}"
    mkdir -p "${dir}"
    git -C "${dir}" init -q
    git -C "${dir}" remote add origin "${url}"
  fi
  log "取得 ${name} @ ${commit:0:8} …"
  git -C "${dir}" fetch -q --depth 1 origin "${commit}" \
    || die "抓取 ${name} 失敗"
  git -C "${dir}" checkout -q --detach FETCH_HEAD || die "checkout ${name} 失敗"
}

ensure_librime_lua() {
  local plugin_dir="${LIBRIME_SRC}/plugins/lua"

  fetch_pinned "${LIBRIME_LUA_SRC}" "${LIBRIME_LUA_REPO}" \
               "${LIBRIME_LUA_COMMIT}" "librime-lua"
  fetch_pinned "${LIBRIME_LUA_SRC}/thirdparty" "${LIBRIME_LUA_REPO}" \
               "${LIBRIME_LUA_TP_COMMIT}" "librime-lua/thirdparty(Lua ${LUA_VERSION})"
  [[ -f "${LIBRIME_LUA_SRC}/thirdparty/lua5.4/lua.h" ]] \
    || die "Lua 原始碼不在 ${LIBRIME_LUA_SRC}/thirdparty/lua5.4"

  mkdir -p "${LIBRIME_SRC}/plugins"
  rm -f "${plugin_dir}"
  ln -s "${LIBRIME_LUA_SRC}" "${plugin_dir}"
  log "librime-lua 已掛在 ${plugin_dir}"
}

# ---------------------------------------------------------------- patches
apply_patches() {
  shopt -s nullglob
  local patches=("${PATCH_DIR}"/*.patch)
  shopt -u nullglob
  if [[ ${#patches[@]} -eq 0 ]]; then
    log "無 patch"
    return
  fi
  for p in "${patches[@]}"; do
    local base target rel
    base="$(basename "${p}" .patch)"
    rel="${base%%@*}"
    rel="${rel//__//}"
    case "${rel}" in
      librime)     target="${LIBRIME_SRC}" ;;
      librime-lua) target="${LIBRIME_LUA_SRC}" ;;
      *)           target="${LIBRIME_SRC}/${rel}" ;;
    esac
    [[ -d "${target}" ]] || die "patch 目標不存在: ${target}"
    if git -C "${target}" apply --reverse --check "${p}" >/dev/null 2>&1; then
      log "patch 已套用，略過: $(basename "${p}")"
    else
      log "套用 patch: $(basename "${p}")"
      git -C "${target}" apply "${p}" || die "套用 patch 失敗: ${p}"
    fi
  done
}

# ---------------------------------------------------------------- FindBoost 墊片
# CMake 4.x 移除了內建的 FindBoost 模組。librime 用 find_package(Boost 1.77.0)
# 查找 header-only Boost，在 cmake 4 下會因為找不到 BoostConfig.cmake 而失敗。
# 這個墊片只在 cmake 4+ 才注入（見 build_librime）；cmake 3.x 用系統內建即可。
create_findboost_shim() {
  SHIM_DIR="${BUILD_ROOT}/cmake-shim"
  mkdir -p "${SHIM_DIR}"
  cat > "${SHIM_DIR}/FindBoost.cmake" << 'SHIMEOF'
# Minimal FindBoost for header-only Boost (cmake 4 compat).
if(DEFINED BOOST_ROOT AND EXISTS "${BOOST_ROOT}/boost/version.hpp")
  file(STRINGS "${BOOST_ROOT}/boost/version.hpp" _bv
       REGEX "^#define BOOST_VERSION ")
  string(REGEX MATCH "[0-9]+" _bvi "${_bv}")
  if(_bvi)
    math(EXPR Boost_VERSION_MAJOR "${_bvi} / 100000")
    math(EXPR Boost_VERSION_MINOR "${_bvi} / 100 % 1000")
    math(EXPR Boost_VERSION_PATCH "${_bvi} % 100")
    set(Boost_VERSION "${Boost_VERSION_MAJOR}.${Boost_VERSION_MINOR}.${Boost_VERSION_PATCH}")
    set(Boost_VERSION_STRING "${Boost_VERSION}")
  endif()
  set(Boost_FOUND TRUE)
  set(Boost_INCLUDE_DIRS "${BOOST_ROOT}")
  set(Boost_INCLUDE_DIR "${BOOST_ROOT}")
  set(Boost_LIBRARY_DIRS "")
  set(Boost_LIBRARIES "")
  message(STATUS "Found Boost ${Boost_VERSION} (shim): ${BOOST_ROOT}")
else()
  set(Boost_FOUND FALSE)
  if(Boost_FIND_REQUIRED)
    message(FATAL_ERROR "FindBoost shim: BOOST_ROOT not set or headers missing")
  endif()
endif()
SHIMEOF
}

# ---------------------------------------------------------------- cmake 共用
common_cmake_args() {
  COMMON_ARGS=(
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DCMAKE_INSTALL_PREFIX=${PREFIX}"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DCMAKE_PREFIX_PATH=${PREFIX}"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_TESTING=OFF
  )
}

configure_and_install() {
  local name="$1" src="$2" bdir="$3"; shift 3
  log "設定 ${name}"
  "${CMAKE_BIN}" -S "${src}" -B "${bdir}" "${COMMON_ARGS[@]}" "$@" \
    > "${bdir}.configure.log" 2>&1 \
    || { tail -80 "${bdir}.configure.log"; die "${name} configure 失敗"; }
  log "編譯並安裝 ${name}"
  "${CMAKE_BIN}" --build "${bdir}" --target install -j "${JOBS}" \
    > "${bdir}.build.log" 2>&1 \
    || { tail -80 "${bdir}.build.log"; die "${name} build 失敗"; }
}

# ---------------------------------------------------------------- 建置依賴
build_deps() {
  common_cmake_args

  configure_and_install glog "${DEPS_SRC}/glog" "${BUILD_ROOT}/glog" \
    -DWITH_GFLAGS=OFF -DWITH_GTEST=OFF -DWITH_PKGCONFIG=OFF -DWITH_UNWIND=OFF

  configure_and_install yaml-cpp "${DEPS_SRC}/yaml-cpp" "${BUILD_ROOT}/yaml-cpp" \
    -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_CPP_BUILD_TOOLS=OFF \
    -DYAML_CPP_BUILD_TESTS=OFF -DYAML_BUILD_SHARED_LIBS=OFF -DYAML_CPP_INSTALL=ON

  configure_and_install leveldb "${DEPS_SRC}/leveldb" "${BUILD_ROOT}/leveldb" \
    -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF -DLEVELDB_INSTALL=ON

  configure_and_install marisa "${DEPS_SRC}/marisa-trie" "${BUILD_ROOT}/marisa-trie" \
    -DENABLE_TOOLS=OFF

  # opencc — 只建 libopencc，不建 data target
  # data target 由 host opencc（下方）另外處理。
  # 此處也可以全建，因為 macOS 不像 Android 有交叉編譯的問題；
  # 但保持與 Android 一致的分離，日後更容易互相參照。
  local opencc_build="${BUILD_ROOT}/opencc"
  log "設定 opencc"
  "${CMAKE_BIN}" -S "${DEPS_SRC}/opencc" -B "${opencc_build}" "${COMMON_ARGS[@]}" \
    -DOPENCC_ENABLE_INSTALL=OFF \
    -DENABLE_GTEST=OFF -DENABLE_BENCHMARK=OFF \
    -DBUILD_PYTHON=OFF -DBUILD_DOCUMENTATION=OFF \
    -DBUILD_OPENCC_JIEBA_PLUGIN=OFF \
    -DUSE_SYSTEM_MARISA=ON \
    "-DCMAKE_CXX_FLAGS=-I${PREFIX}/include" \
    > "${opencc_build}.configure.log" 2>&1 \
    || { tail -80 "${opencc_build}.configure.log"; die "opencc configure 失敗"; }
  log "編譯 opencc（僅 libopencc）"
  "${CMAKE_BIN}" --build "${opencc_build}" --target libopencc -j "${JOBS}" \
    > "${opencc_build}.build.log" 2>&1 \
    || { tail -80 "${opencc_build}.build.log"; die "opencc build 失敗"; }
  mkdir -p "${PREFIX}/include/opencc/plugin" "${PREFIX}/lib"
  cp -f "${opencc_build}/src/libopencc.a" "${PREFIX}/lib/"
  cp -f "${DEPS_SRC}"/opencc/src/*.hpp "${PREFIX}/include/opencc/"
  cp -f "${DEPS_SRC}/opencc/src/opencc.h" "${PREFIX}/include/opencc/"
  cp -f "${opencc_build}/src/Opencc_Export.h" "${PREFIX}/include/opencc/"
  cp -f "${DEPS_SRC}/opencc/src/plugin/OpenCCPlugin.h" "${PREFIX}/include/opencc/plugin/"
}

# ---------------------------------------------------------------- librime
build_librime() {
  common_cmake_args
  local bdir="${BUILD_ROOT}/librime"
  local cmake_module_path="${LIBRIME_SRC}/cmake"

  # CMake 4.x 需要 FindBoost 墊片
  if [[ "${CMAKE_MAJOR}" -ge 4 ]]; then
    cmake_module_path="${SHIM_DIR};${cmake_module_path}"
  fi

  configure_and_install librime "${LIBRIME_SRC}" "${bdir}" \
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
    "-DBoost_INCLUDE_DIR=${BOOST_ROOT_DIR}" \
    "-DCMAKE_MODULE_PATH=${cmake_module_path}"
}

# ---------------------------------------------------------------- lua 驗證
# 「編得起來」不等於「外掛有註冊」。驗三件事：
#   1. cmake configure 真的看到 lua plugin
#   2. librime.a 裡有 rime_require_module_lua 的定義（T）
#   3. 有目的檔引用它（U）—— 代表 RIME_EXTRA_MODULES 有生效
# macOS 的 nm 符號帶底線前綴（_Z... → __Z...），用模式匹配。
verify_lua() {
  local a="${PREFIX}/lib/librime.a"
  local conflog="${BUILD_ROOT}/librime.configure.log"

  grep -q "Found plugin: .*plugins/lua" "${conflog}" \
    || die "cmake 沒有看到 lua 外掛"
  grep -q "rime_plugins_modules: .*lua" "${conflog}" \
    || die "cmake 沒把 lua 列入 rime_plugins_modules"

  local syms="${BUILD_ROOT}/.librime.nm.txt"
  nm "${a}" > "${syms}" 2>/dev/null || true
  grep -q "T.*rime_require_module_lua" "${syms}" \
    || die "librime.a 沒有 rime_require_module_lua 的定義"
  grep -q "U.*rime_require_module_lua" "${syms}" \
    || die "無 object 引用 rime_require_module_lua — RIME_EXTRA_MODULES 沒生效"
  grep -q "T.*lua_pcallk" "${syms}" \
    || die "librime.a 缺 Lua 直譯器符號(lua_pcallk)"
  rm -f "${syms}"
  log "lua 驗證通過：外掛已註冊 + Lua ${LUA_VERSION} 已編入"
}

# ---------------------------------------------------------------- host opencc
# 產生 .ocd2 詞典供 scripts/collect_data.sh 使用。
# 路徑 third_party/build/host-opencc-install 是 collect_data.sh 寫死的。
build_host_opencc() {
  local install_dir="${THIRD_PARTY}/build/host-opencc-install"
  if ls "${install_dir}/share/opencc/"*.ocd2 >/dev/null 2>&1; then
    log "host opencc 已就緒"
    return
  fi
  log "建置 host opencc（產生 .ocd2 詞典）"
  local bdir="${THIRD_PARTY}/build/host-opencc"
  # 這個路徑由 collect_data.sh 寫死，位於 third_party/build/ 底下。
  # Android 端的 build_native.sh 會建立該目錄，macOS 這條路徑不經過它，
  # 所以必須自己建 —— 否則底下的 > "${bdir}.configure.log" 會直接失敗。
  mkdir -p "$(dirname "${bdir}")"
  "${CMAKE_BIN}" -S "${DEPS_SRC}/opencc" -B "${bdir}" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_GTEST=OFF -DENABLE_BENCHMARK=OFF -DBUILD_PYTHON=OFF \
    -DBUILD_DOCUMENTATION=OFF -DOPENCC_ENABLE_INSTALL=ON \
    "-DCMAKE_INSTALL_PREFIX=${install_dir}" \
    > "${bdir}.configure.log" 2>&1 \
    || { tail -80 "${bdir}.configure.log"; die "host opencc configure 失敗"; }
  "${CMAKE_BIN}" --build "${bdir}" -j "${JOBS}" \
    > "${bdir}.build.log" 2>&1 \
    || { tail -80 "${bdir}.build.log"; die "host opencc build 失敗"; }
  "${CMAKE_BIN}" --install "${bdir}" \
    > "${bdir}.install.log" 2>&1 \
    || { tail -80 "${bdir}.install.log"; die "host opencc install 失敗"; }
  ls "${install_dir}/share/opencc/"*.ocd2 >/dev/null 2>&1 \
    || die "host opencc 安裝後缺少 .ocd2"
}

# ---------------------------------------------------------------- rime_console
build_rime_console() {
  log "編譯 rime_console"
  local out="${BUILD_ROOT}/rime_console"

  # 連結順序：被依賴者排後面（與 Android 端 manifest.json 一致）
  local libs=(
    "${PREFIX}/lib/librime.a"
    "${PREFIX}/lib/libopencc.a"
    "${PREFIX}/lib/libmarisa.a"
    "${PREFIX}/lib/libleveldb.a"
    "${PREFIX}/lib/libyaml-cpp.a"
    "${PREFIX}/lib/libglog.a"
  )
  for a in "${libs[@]}"; do
    [[ -f "${a}" ]] || die "缺少 ${a}"
  done

  clang++ -std=c++17 -O2 \
    -I "${PREFIX}/include" \
    -I "${RIME_ROOT}/core/include" \
    "${RIME_ROOT}/tools/rime_console.cc" \
    "${RIME_ROOT}/core/src/rime_shell.cc" \
    "${libs[@]}" \
    -lpthread \
    -o "${out}" 2>&1 \
    || die "rime_console 編譯失敗"

  [[ -x "${out}" ]] || die "rime_console 不可執行"
  log "rime_console: $(du -h "${out}" | cut -f1)"
}

# ---------------------------------------------------------------- main
log "RIME_ROOT  = ${RIME_ROOT}"
log "BUILD_ROOT = ${BUILD_ROOT}"
log "Jobs       = ${JOBS}"

ensure_librime
ensure_boost
ensure_librime_lua
apply_patches
create_findboost_shim

build_deps
build_librime
verify_lua
build_host_opencc
build_rime_console

log "全部完成 ✓"
