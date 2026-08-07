#!/usr/bin/env bash
#
# 交叉編譯 librime 及其相依函式庫到 Android。
#
# 用法:
#   scripts/build_native.sh                 # 預設編譯 arm64-v8a x86_64
#   scripts/build_native.sh arm64-v8a       # 只編譯指定 ABI
#   scripts/build_native.sh --clean x86_64  # 先清掉中間產物再編譯
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

# ---------------------------------------------------------------- patches
# patches/<name>.patch 會被套用到對應的 submodule。目前無需任何 patch,
# 但保留機制:新增的 patch 檔請以 "<submodule 相對路徑>@<說明>.patch" 命名,
# 例如 deps__opencc@skip-tools.patch -> 套用於 third_party/librime/deps/opencc。
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
  configure_and_install librime "${LIBRIME_SRC}" "${bdir}/librime" \
    -DBUILD_STATIC=ON \
    -DBUILD_TEST=OFF \
    -DBUILD_SAMPLE=OFF \
    -DBUILD_DATA=OFF \
    -DBUILD_SEPARATE_LIBS=OFF \
    -DBUILD_MERGED_PLUGINS=OFF \
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
    -stdlib=libc++
    -static-libstdc++
  )

  log "[${abi}] 煙霧測試:連結成 executable"
  "${CLANGXX}" "${common[@]}" \
    "${SCRIPT_DIR}/smoke_test.cc" -o "${work}/rime_smoke" \
    "${libs[@]}" "${syslibs[@]}" \
    > "${work}/exe.log" 2>&1 || { cat "${work}/exe.log"; die "[${abi}] 煙霧測試(exe)連結失敗"; }

  log "[${abi}] 煙霧測試:連結成 shared library"
  "${CLANGXX}" "${common[@]}" -shared \
    "${SCRIPT_DIR}/smoke_test.cc" -o "${work}/librime_jni_smoke.so" \
    -Wl,--no-undefined \
    -Wl,--whole-archive "${out}/lib/librime.a" -Wl,--no-whole-archive \
    "${libs[@]:1}" "${syslibs[@]}" \
    > "${work}/so.log" 2>&1 || { cat "${work}/so.log"; die "[${abi}] 煙霧測試(so)連結失敗"; }

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

ensure_boost
apply_patches
mkdir -p "${BUILD_ROOT}" "${PREBUILT_ROOT}"

for abi in "${ABIS[@]}"; do
  build_abi "${abi}"
  smoke_test "${abi}"
done

write_manifest
log "全部完成 ✓"
