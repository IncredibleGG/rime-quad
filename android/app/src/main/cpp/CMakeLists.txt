cmake_minimum_required(VERSION 3.22.1)
project(rime_jni C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# app/src/main/cpp -> main -> src -> app -> android -> <repo root>
get_filename_component(RIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
message(STATUS "[rime] repo root = ${RIME_ROOT}")

set(RIME_SHELL_INCLUDE "${RIME_ROOT}/core/include")
set(RIME_SHELL_IMPL    "${RIME_ROOT}/core/src/rime_shell.cc")
set(PREBUILT_DIR       "${RIME_ROOT}/third_party/prebuilt/${ANDROID_ABI}")
set(PREBUILT_LIB_DIR   "${PREBUILT_DIR}/lib")
set(PREBUILT_INC_DIR   "${PREBUILT_DIR}/include")
set(PREBUILT_MANIFEST  "${RIME_ROOT}/third_party/prebuilt/manifest.json")

if(NOT DEFINED RIME_JNI_CLASS OR RIME_JNI_CLASS STREQUAL "")
  set(RIME_JNI_CLASS "org/luminakey/ime/core/RimeCore")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# 真 librime 還是 stub？
#
# 兩個條件都成立才走真的：
#   * third_party/prebuilt/<abi>/lib/librime.a 存在（交叉編譯那條線的產出）
#   * core/src/rime_shell.cc 存在（門面實作那條線的產出）
# 否則編 rime_shell_stub.cc，讓 APK 今天就能跑起來。
# 依賴補齊後只要重跑 gradle（--rerun-tasks 或 clean）就會自動切換。
# ─────────────────────────────────────────────────────────────────────────────
set(RIME_USE_REAL OFF)
if(EXISTS "${PREBUILT_LIB_DIR}/librime.a" AND EXISTS "${RIME_SHELL_IMPL}")
  set(RIME_USE_REAL ON)
endif()

set(RIME_SOURCES jni_bridge.cc)

if(RIME_USE_REAL)
  message(STATUS "[rime] ==> 連結真實 librime (${ANDROID_ABI})")
  list(APPEND RIME_SOURCES "${RIME_SHELL_IMPL}")
else()
  message(WARNING
    "[rime] ==> STUB 模式 (${ANDROID_ABI})。"
    " 缺 ${PREBUILT_LIB_DIR}/librime.a 或 ${RIME_SHELL_IMPL}。"
    " 產出的 APK 會顯示假候選字。")
  list(APPEND RIME_SOURCES rime_shell_stub.cc)
endif()

add_library(rime_jni SHARED ${RIME_SOURCES})

target_include_directories(rime_jni PRIVATE
  "${RIME_SHELL_INCLUDE}"
  "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(rime_jni PRIVATE RIME_JNI_CLASS="${RIME_JNI_CLASS}")

find_library(log-lib log)
target_link_libraries(rime_jni PRIVATE ${log-lib})

if(RIME_USE_REAL)
  target_compile_definitions(rime_jni PRIVATE RIME_HAVE_LIBRIME=1)
  target_include_directories(rime_jni PRIVATE "${PREBUILT_INC_DIR}")

  # 連結順序優先採用 manifest.json 的 link_order；解析不到就 glob 整個 lib 目錄。
  # 無論哪種都包在 --start-group/--end-group 裡，避免順序寫錯就整包連不起來。
  set(RIME_STATIC_LIBS "")
  if(EXISTS "${PREBUILT_MANIFEST}")
    file(READ "${PREBUILT_MANIFEST}" _rime_manifest)
    string(REGEX MATCH "\"link_order\"[ \t\r\n]*:[ \t\r\n]*\\[[^]]*\\]" _rime_lo "${_rime_manifest}")
    if(_rime_lo)
      string(REGEX MATCHALL "\"[^\"]+\\.a\"" _rime_entries "${_rime_lo}")
      foreach(_e ${_rime_entries})
        string(REPLACE "\"" "" _e "${_e}")
        if(IS_ABSOLUTE "${_e}")
          list(APPEND RIME_STATIC_LIBS "${_e}")
        else()
          get_filename_component(_bare "${_e}" NAME)
          list(APPEND RIME_STATIC_LIBS "${PREBUILT_LIB_DIR}/${_bare}")
        endif()
      endforeach()
      message(STATUS "[rime] manifest.json link_order: ${RIME_STATIC_LIBS}")
    endif()
  endif()

  if(NOT RIME_STATIC_LIBS)
    file(GLOB RIME_STATIC_LIBS "${PREBUILT_LIB_DIR}/*.a")
    message(WARNING "[rime] manifest.json 無可用 link_order，改用 glob: ${RIME_STATIC_LIBS}")
  endif()

  target_link_libraries(rime_jni PRIVATE
    "-Wl,--start-group" ${RIME_STATIC_LIBS} "-Wl,--end-group")

  # manifest.json 的 system_libs（log 已在上面連過）。
  target_link_libraries(rime_jni PRIVATE m)
endif()
