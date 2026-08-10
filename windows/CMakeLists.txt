# windows/CMakeLists.txt — Windows 端的所有目標
#
# 這裡刻意分成**兩組**,分界線是「要不要 librime」:
#
#   ── 不要 librime(RIME_PREFIX 可以不設)──
#     rime_common  純邏輯:線路格式、按鍵映射、組字政策、候選窗排版
#     rime_tests   單元測試 + 真實鍵盤佈局測試
#     rime_tsf     TSF 的瘦 DLL —— **它本來就不該碰 librime**
#     rime_probe   走真的具名管道驅動服務的驗證用戶端
#
#   ── 要 librime(必須設 RIME_PREFIX)──
#     rime_service 服務進程:rime_shell + librime + 候選窗
#     rime_console 不經 UI 的核心驗證(第一個里程碑)
#
# 分開的理由是**驗證迴圈**:第一組編一次不到一分鐘,不必等 librime 那十幾分鐘
# 的建置與快取。CI 也照這條線切成兩個 job。而這條線同時就是架構本身 ——
# 瘦 DLL 那一側如果哪天需要 RIME_PREFIX 才編得起來,代表有東西跑錯邊了。
#
# librime 與 5 個靜態相依請先用 windows/build.sh 建到 RIME_PREFIX:
#   windows/build.sh deps
#
cmake_minimum_required(VERSION 3.20)

# CMP0091(用 CMAKE_MSVC_RUNTIME_LIBRARY 而非改 CMAKE_*_FLAGS 選 CRT)必須在
# project() 之前就位,否則下面那行只是個沒人讀的變數。
project(rime_windows LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR
    "這個目標只支援 MSVC。TSF 實務上無法用 mingw 交叉編譯,而讓核心層與最終的\n"
    "  DLL 用不同編譯器,等於把 ABI 問題留到最難查的那一層才爆。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /MT(靜態 CRT)。兩個理由,而第二個是這一輪才變得重要的:
#   1. librime 與 5 個相依全都是 /MT(見 windows/build.sh),混用會是 LNK2038。
#   2. TSF 的 DLL 被載入到**每一個**宿主進程裡。動態 CRT 代表那些進程都得
#      找得到 VCRUNTIME140.dll / MSVCP140.dll —— 少了就是「某些程式裡輸入法
#      整個不存在」,而且錯誤訊息落在宿主那邊,使用者根本看不到。
#      CI 有一道 dumpbin /dependents 在守這件事。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

get_filename_component(RIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# /utf-8 不是可選的。
#
# MSVC 預設用系統 ANSI 代碼頁解讀「沒有 BOM 的原始碼」,也用它當執行字元集。
# 這個專案的原始碼裡到處都是中文字面值(訊息、註解、測試裡的候選字),
# 少了這個旗標,那些字串在編譯階段就已經被解錯 —— 印出來是亂碼,
# 看起來像引擎壞了,實際上引擎好得很。
add_compile_options(/utf-8 /EHsc /W3)

# UNICODE / _UNICODE。程式碼裡一律呼叫明確的 W 版本 API,所以這兩個巨集
# 影響的其實只有**資源巨集**:少了它們,IDC_ARROW 之類的東西會展開成
# MAKEINTRESOURCEA,傳給 LoadCursorW 就是 C2664。
# NOMINMAX:windows.h 的 min/max 巨集會把 std::max(a, b) 弄壞。
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# 產物位置固定,讓腳本與 CI 不必猜。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# ── 第一組:不需要 librime ────────────────────────────────────────────

# 純邏輯層。**這裡的原始碼不可以 include windows.h** —— 那是刻意的約束,
# 為的是讓 windows/run_logic_tests.sh 能在開發用的 Ubuntu 上用 g++ 編起來、
# 跑起來。Windows 端唯一的建置管道是 CI,一輪十幾分鐘;
# 「推上去看會不會過」不是開發方式。
add_library(rime_common STATIC
  common/protocol.cc
  common/keymap.cc
  common/ime_policy.cc
  common/cand_layout.cc
  common/schema_choice.cc
  common/settings.cc
  common/schema_list_patch.cc
  # 「這個宿主可不可以啟動服務」與「這一顆鍵可不可以宣告吃掉」。
  # 兩者都是純邏輯,所以 Ubuntu 上的 run_logic_tests.sh 也跑得到 ——
  # 而它們各自對應一次「畫面完全正常、自動化全過」的實測缺陷。
  common/elevation_policy.cc
  common/key_eat_policy.cc
  # 「清單上要出現在哪一個語言底下、而且只出現一次」的判斷。
  # 純邏輯是刻意的:CI 的 Windows runner 上只有一種語言情境
  #(英文、沒有任何中文輸入語言),簡體/繁體/香港/兩種都有的使用者
  # 只有 run_logic_tests.sh 那一邊走得到。
  common/profile_choice.cc
  # net_policy.cc 是離線守門的**判斷**與紀錄格式(Android NetworkGate 的移植)。
  # ⚠ 它不會開任何連線,目前也還沒有東西呼叫它 —— 方案市集還沒做。
  #   放進來是因為它有測試,而且是下一輪的地基。CI 會斷言**兩個二進位
  #   都沒有任何網路相依**,所以「Windows 端不連網」這句話仍然成立而且驗得到。
  common/net_policy.cc
  # net_gate_core.cc 是連網出口的**流程**(問開關 → 送出 → 跟轉址 →
  # 邊收邊數 → 記一筆)。它同樣不含任何網路 API,所以 Ubuntu 上的
  # run_logic_tests.sh 驗得到「開關關著時傳輸層一次都沒被呼叫」。
  # 真的開 socket 的那一半在 service/net_gate.cc,只連進 rime_service。
  common/net_gate_core.cc
  # net_ui.cc 是「連網」那一頁的**呈現邏輯**:開關現在說哪一句話、
  # 紀錄的四欄長什麼樣、按下「檢查更新」時開關是關的該怎麼辦。
  # ⚠ 它在 common/ 而不是 service/,理由與 ui_layout.cc 一樣:
  #   那三件事寫壞了畫面看起來完全正常,而 service/ 在 Ubuntu 上編不起來。
  common/net_ui.cc
  # ── 方案市集 ────────────────────────────────────────────────
  # 全部是純邏輯(不 include windows.h),所以整條「下載→驗章→
  # 安全檢查→解壓→預檢→改 schema_list→部署→回滾」在 Ubuntu 上
  # 就跑得完:windows/run_logic_tests.sh。真的碰網路與檔案的那一半
  # 在 service/schema_store.cc。
  #
  # ⚠ zip_reader.cc 自己解 DEFLATE。Windows 沒有 zlib,系統只提供
  #   XPRESS/LZMS —— 見那個檔案的檔頭。它是這條線上唯一直接吃
  #   「網路來的位元組」的解析器,所以它有截斷/位元翻轉/亂數的測試,
  #   而且 run_logic_tests.sh --asan 會把同一批再跑一次。
  common/mini_json.cc
  common/sha256.cc
  common/zip_reader.cc
  common/archive_guard.cc
  common/schema_preflight.cc
  common/store_index.cc
  common/store_engine.cc
  common/update_manifest.cc
  common/update_flow.cc
  # ── 這一輪加的:設定介面的純邏輯層 ────────────────────────────
  #
  # ⚠ 全部刻意不 include windows.h —— 版面、色票、狀態列位置還原都是
  #   純函式,所以 windows/run_logic_tests.sh 在 Ubuntu 上驗得到它們
  #   (docs/ui-design.md §12.12 的 W18/W19/W20/W22 靠這條)。
  #   「畫出來好不好看」仍然只有人驗得到,但「算得對不對」不必等 CI。
  common/ui_palette.cc
  common/ui_layout.cc
  common/statusbar_place.cc
  # ⚠ 使用者看得到的每一個字。`windows/` 底下只有這一個檔案可以有
  #   使用者可見字串(W7),而它同時被 DLL、服務、安裝工具連結 ——
  #   語言列的按鈕與提權政策的說明也是使用者讀得到的字。
  common/ui_strings.cc
  # ⚠ 「現在到底怎麼了」的唯一判斷點。三種處境(還在準備 /
  #   準備失敗 / 引擎不在)以前被壓成一個布林,於是畫面上是
  #   同一句紅字「輸入法沒有在跑」——而第一種那句話是假的。
  #   判斷寫成純函式是刻意的:留在繪製碼裡的話,只有真人在
  #   Windows 上才驗得到,也就是實際上沒有人驗。
  common/service_state.cc
  # ⚠ 那一橫每一格畫什麼。以前寫在 service/status_bar.cc 的繪製碼裡,
  #   而那個檔案在 Ubuntu 上編不起來 —— 「第一格畫了兩個字面」這種
  #   只有人看得出來的缺陷,沒有任何自動化碰得到。
  common/status_cells.cc
  # ⚠ Ctrl+空白鍵。判斷只有這一份:瘦 DLL 與服務各要問一次,
  #   兩邊各寫一份就是兩份真相。走 PreserveKey,**不動 key_eat_policy**。
  common/hotkey_policy.cc)
target_include_directories(rime_common PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/common"
  "${RIME_ROOT}/core/include")

# Windows 專用的一小撮工具(UTF 轉換、SID、管道名)。
# 與 common/ 分開放,免得哪天有人在 common/ 裡順手 include windows.h。
add_library(rime_winshared STATIC winshared/winutil.cc)
# ole32:StringFromGUID2(GuidToUtf8)。它已經在瘦 DLL 的允許清單裡,
# 而且每一個載入 COM 的進程本來就有 —— 但 rime_probe 只連 winshared,
# 所以要放在 PUBLIC,不然那一支會是 LNK2019。
# rime_common 是 PUBLIC 的:winutil.h 現在露出 common/elevation_policy.h 的
# 型別(HostElevation / TokenSplit),而 rime_tsf_host 只連 winshared ——
# PRIVATE 的話那一支會是 LNK2019,而且是在 CI 上才發現。
target_link_libraries(rime_winshared PUBLIC advapi32 ole32 rime_common)

add_executable(rime_tests
  tests/test_main.cc
  tests/test_protocol.cc
  tests/test_keymap.cc
  tests/test_policy.cc
  tests/test_layout.cc
  tests/test_link_state.cc
  tests/test_schema_choice.cc
  tests/test_settings.cc
  tests/test_net_policy.cc
  tests/test_net_gate_core.cc
  tests/test_net_ui.cc
  tests/test_proto_compat.cc
  tests/test_schema_list_patch.cc
  tests/test_store_basics.cc
  tests/test_zip_reader.cc
  tests/test_archive_guard.cc
  tests/test_schema_preflight.cc
  tests/test_store_index.cc
  tests/test_store_engine.cc
  tests/test_elevation_policy.cc
  tests/test_key_eat_policy.cc
  tests/test_profile_choice.cc
  tests/test_ui_layout.cc
  tests/test_ui_palette.cc
  tests/test_statusbar_place.cc
  tests/test_ui_strings.cc
  tests/test_service_state.cc
  tests/test_status_cells.cc
  tests/test_hotkey_policy.cc
  # 這一支用**真的**鍵盤佈局(LoadKeyboardLayout + ToUnicodeEx),
  # 是 CI 上唯一驗得到「有沒有真的問佈局」的地方。
  tests/test_win32_layouts.cc
  # 自繪的 ListView **真的畫得出東西嗎**。使用者實機回報「方案清單一列
  # 都沒有」,而 windows/ 底下所有 UI 測試都刻意不 include windows.h ——
  # 也就是「畫得出來嗎」以前在 CI 上完全不存在。這一支開真的控制項、
  # 走真的 comctl32、把畫面渲染進點陣圖數像素,數的是產品碼本身。
  tests/test_win32_listview.cc
  service/ui_listview.cc
  tsf/win32_oracle.cc)
target_link_libraries(rime_tests PRIVATE rime_common user32 gdi32 comctl32)

# ── TSF 瘦 DLL ────────────────────────────────────────────────────────
#
# 相依刻意壓到最小。uuid.lib 是為了 TSF 的介面 GUID(IID_ITfThreadMgr、
# CLSID_TF_CategoryMgr、GUID_TFCAT_* …)—— 它是**靜態**庫,只帶常數進來,
# 不會在匯入表裡多一個 DLL。
#
# ⚠ 不要加 msctf.lib:現代的 Windows SDK **沒有這個檔案**(TSF 完全是 COM,
#   沒有要匯入的函式),連上去只會得到 LNK1181: cannot open input file。
#   實測過(CI run #15)。GUID 全部在 uuid.lib 裡。
#
# 真正不可以出現的是 VCRUNTIME140 / MSVCP140 / ucrtbase 那一組 —— 那代表
# /MT 沒生效,而後果是「在某些程式裡輸入法整個不存在」。
# ⚠ lang_bar.cc 是這一輪唯一加進 DLL 的檔案,而且是不得不加的:
#   ITfLangBarItem 是 TSF 向**文字服務**要的介面,語言列的按鈕只能在這裡。
#   它不畫東西、不載字型、不建視窗,按下去只送一則單向 IPC 訊息。
#   設定介面本體在服務進程那一側(service/settings_window.cc)。
# ⚠ trace.cc 是這一輪加進來的,而且它是**不得不加**的那一種:
#   這支 DLL 住在別人的進程裡,印到 stdout 沒有人看得到,
#   OutputDebugString 需要偵錯器 —— 而使用者手上沒有偵錯器。
#   「DLL 沒被載入」「載入了但 ActivateEx 沒被呼叫」「按鍵映不出 keysym」
#   這三件事在使用者眼裡完全一樣,而它們要修的地方完全不同。
#   它只用 kernel32(見 tsf/trace.h 的規矩 2),不加任何相依。
add_library(rime_tsf SHARED
  tsf/dll.cc
  tsf/guids.cc
  tsf/registration.cc
  tsf/text_service.cc
  tsf/ipc_client.cc
  tsf/lang_bar.cc
  tsf/trace.cc
  tsf/win32_oracle.cc
  tsf/rime_tsf.def)
# oleaut32:SysAllocString(語言列按鈕的 BSTR)。它是 COM 的字串配置器,
# 不是新的能力面 —— 而且**沒有任何網路 DLL**,那是 check_binaries.sh 守的。
target_link_libraries(rime_tsf PRIVATE rime_common rime_winshared
                                        kernel32 user32 advapi32 ole32 oleaut32
                                        uuid)
set_target_properties(rime_tsf PROPERTIES OUTPUT_NAME "rime_tsf")

# ── 安裝程式的手腳 ────────────────────────────────────────────────
#
# 註冊 / 反註冊 / 檢查 / 停服務。安裝程式(Inno Setup)呼叫的就是它,
# 而 CI 的 windows/verify_installer.sh 也靠它的 check 動詞做斷言。
#
# 它與 DLL 共用 tsf/registration.cc —— 那是刻意的:兩份實作會漂移,
# 而漂移的症狀是「用安裝程式裝的能用,自己 regsvr32 的不能用」。
# registration_check.cc 則**只**在這裡,不進 DLL:DLL 住在每一個宿主進程裡,
# 沒有理由為了「檢查」多背一份程式碼(見 windows/check_binaries.sh 的允許清單)。
#
# 它也不需要 librime —— 所以它在 logic 那一組就建得出來,快速 job 就驗得到。
#
# ⚠ 這一輪它多了一個動詞:doctor。那是給**使用者**跑的一頁式自我診斷
#   (見 setup/doctor.h)。為此它必須連進幾個原本只有 DLL 在用的東西:
#     ipc_client + rime_common   → 才問得到「管道連不連得上、卡在哪一步」
#     win32_oracle               → 才問得到「這台機器的鍵盤佈局答不答得出字」
#     trace                      → 才讀得到瘦 DLL 留下的記錄檔
#   這些都不會讓 DLL 變胖:胖的是 rime_ime_setup.exe,而它只在安裝與診斷時跑。
add_executable(rime_ime_setup
  setup/setup_main.cc
  setup/doctor.cc
  tsf/guids.cc
  tsf/ipc_client.cc
  tsf/registration.cc
  tsf/registration_check.cc
  # 「使用者有哪些語言、現在啟用了哪幾份、收斂成一份」。
  # ⚠ **刻意不進 rime_tsf.dll**:那支住在每一個宿主進程裡,它不需要知道
  #   使用者的語言清單長什麼樣。相依上沒有新東西(advapi32 + ole32 兩邊
  #   本來都有),分開純粹是為了「DLL 裡的每一行都說得出為什麼非在那裡不可」。
  tsf/user_langs.cc
  tsf/trace.cc
  tsf/win32_oracle.cc)
target_include_directories(rime_ime_setup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rime_ime_setup PRIVATE rime_common rime_winshared
                                             kernel32 user32 advapi32 ole32 uuid)

add_executable(rime_probe
  tests/probe_main.cc
  tsf/ipc_client.cc
  tsf/trace.cc)
target_link_libraries(rime_probe PRIVATE rime_common rime_winshared)

# ── 真的經過 TSF 的驗證宿主 ──────────────────────────────────────────
#
# rime_tsf_host.exe 是一個**假的文字編輯器**:它建 ITfThreadMgr、給一份
# ITextStoreACP 焦點、啟用我們的語言設定檔、用 ITfKeystrokeMgr 送按鍵,
# 然後看文件裡長出什麼字。也就是說它逼系統走完
#   登錄檔 → CoCreateInstance → 載入 rime_tsf.dll → ActivateEx → 按鍵 → 組字
# 這一整條 —— 而那正是 CI 到目前為止**完全沒有碰過**的一段
# (verify_ime.sh 走的是具名管道,繞過 TSF)。
#
# ⚠ 它**不連結** rime_tsf:那樣就變成直接呼叫我們自己的函式,驗到的是
#   另一件事。它只認 CLSID 與 profile GUID,其餘全部交給系統去解析 ——
#   註冊錯了、路徑錯了、DLL 載不起來,它都會在該紅的地方紅。
add_executable(rime_tsf_host
  tests/tsf_host_main.cc
  tsf/guids.cc)
target_link_libraries(rime_tsf_host PRIVATE rime_winshared
                                            kernel32 user32 ole32 uuid)

# ── 第二組:需要 librime ──────────────────────────────────────────────

if(NOT RIME_PREFIX)
  message(STATUS "沒有 RIME_PREFIX:跳過 rime_service 與 rime_console。")
  message(STATUS "  (這是正常的 —— 瘦 DLL 與純邏輯測試本來就不該需要 librime。)")
  return()
endif()

if(NOT EXISTS "${RIME_PREFIX}/include/rime_api.h")
  message(FATAL_ERROR "${RIME_PREFIX} 裡沒有 rime_api.h;先跑 windows/build.sh deps")
endif()

# 檔名不寫死:glog 與 yaml-cpp 在不同版本/組態下會叫 glog.lib / glogd.lib /
# yaml-cpp.lib / libyaml-cppmt.lib。NAMES 清單抄自 librime 自己的
# cmake/Find*.cmake,免得日後換版本時得回來猜。
# NO_DEFAULT_PATH 是重點:只准用我們自己編的那一份,不准撿到 runner 上別人裝的。
set(CMAKE_FIND_LIBRARY_SUFFIXES .lib)
function(rime_find_lib var)
  find_library(${var}
    NAMES ${ARGN}
    PATHS "${RIME_PREFIX}/lib"
    NO_DEFAULT_PATH
    REQUIRED)
endfunction()

rime_find_lib(RIME_LIB      rime librime)
rime_find_lib(OPENCC_LIB    opencc)
rime_find_lib(MARISA_LIB    marisa libmarisa)
rime_find_lib(LEVELDB_LIB   leveldb libleveldb)
rime_find_lib(YAMLCPP_LIB   yaml-cpp libyaml-cppmt yaml-cppd libyaml-cppmtd)
rime_find_lib(GLOG_LIB      glog glogd libglog)

# 連結順序:被依賴者排在依賴者之後,與 Android 的
# third_party/prebuilt/manifest.json 記載的同一份順序。
#   librime -> opencc, glog, yaml-cpp, leveldb, marisa
#   opencc  -> marisa
# MSVC 的連結器對靜態庫順序比 GNU ld 寬容,但把順序寫對是零成本的,
# 而且讓兩端的「連結順序」是同一件可以互相對照的事。
set(RIME_STATIC_LIBS
  "${RIME_LIB}" "${OPENCC_LIB}" "${MARISA_LIB}"
  "${LEVELDB_LIB}" "${YAMLCPP_LIB}" "${GLOG_LIB}")

# 系統庫:
#   dbghelp — glog 0.7 的 stacktrace 用得到,而 glog 的 CMake 忘了自己帶上
#             (librime 的 src/CMakeLists.txt 為 WIN32 補了同一條)。
#   ws2_32  — leveldb / glog 在 Windows 上的網路與主機名相關呼叫。
#   bcrypt  — boost::uuid 在 MSVC 上取亂數用。Boost 有 auto-link pragma 會自己
#             帶上,這裡明寫一次,免得哪天 BOOST_ALL_NO_LIB 被開起來就斷。
set(RIME_SYSTEM_LIBS dbghelp ws2_32 bcrypt)

add_executable(rime_console
  "${RIME_ROOT}/tools/rime_console.cc"
  "${RIME_ROOT}/core/src/rime_shell.cc")
target_include_directories(rime_console PRIVATE
  "${RIME_ROOT}/core/include" "${RIME_PREFIX}/include")
target_link_libraries(rime_console PRIVATE ${RIME_STATIC_LIBS} ${RIME_SYSTEM_LIBS})

add_executable(rime_service
  service/main.cc
  # ⚠ net_gate.cc 是**整個 windows/ 底下唯一會開 socket 的檔案**,
  #   而且它只進 rime_service.exe。瘦 DLL 那一側永遠是零 ——
  #   rime_tsf.dll 住在每一個接受文字輸入的進程裡(含瀏覽器與提權
  #   進程),它有網路能力這件事光讀原始碼不會有人發現。
  #   兩層守著:windows/audit_offline_win.sh 看原始碼,
  #   windows/check_binaries.sh 看產物的匯入表。
  service/net_gate.cc
  service/engine.cc
  service/pipe_server.cc
  service/cand_window.cc
  service/settings_store.cc
  service/schema_store.cc
  service/settings_window.cc
  # ⚠ 「檢查更新」的接縫。**這個檔案是 win-update 那條線要換掉的那一份**:
  #   本輪只交出介面與呼叫點(背景執行緒、完成訊息、失敗分類),
  #   真的去問版本的那一段還沒有。它不碰網路 API —— 連出去只能走
  #   service/net_gate.cc,那是唯一的出口(audit_offline_win.sh 守著)。
  service/update_check.cc
  service/status_bar.cc
  service/ui_listview.cc
  service/ui_font.cc
  service/ui_theme.cc
  service/ui_confirm.cc
  "${RIME_ROOT}/core/src/rime_shell.cc")
target_include_directories(rime_service PRIVATE "${RIME_PREFIX}/include")
target_link_libraries(rime_service PRIVATE
  rime_common rime_winshared
  ${RIME_STATIC_LIBS} ${RIME_SYSTEM_LIBS}
  # comctl32:設定視窗的分頁控制項。shell32:系統匣圖示與「開啟資料夾」。
  # comctl32:側欄與方案清單的 ListView(分頁控制項已經拿掉)。
  # shell32:系統匣圖示與「開啟資料夾」。
  # ⚠ **沒有** dwmapi:深色標題列那支是動態載入的(見 ui_theme.cc)——
  #   靜態連結會讓這支程式在沒有 dwmapi.dll 的舊系統上根本載不起來,
  #   而症狀是「安裝完什麼都沒發生」。
  user32 gdi32 shell32 comctl32
  # winhttp:service/net_gate.cc 的唯一出口。**只在這一個目標上** ——
  # 加到 rime_tsf 上是這個專案最不該發生的事,見 check_binaries.sh。
  winhttp)
# /MAP:服務進程崩掉時,崩潰過濾器印的是 RVA;要把 RVA 變成函式名就需要這份
# 對照表。CI 會把它一起上傳。這是在沒有偵錯器、沒有符號伺服器的機器上
# 唯一查得動的辦法 —— 而 Windows 端的建置機器正是這種機器。
target_link_options(rime_service PRIVATE /MAP)

# ── 應用程式資訊清單 ────────────────────────────────────────────────────
#
# ⚠ **這是整個 Windows 端外觀的分水嶺,不是可有可無的收尾。**
#
# 在這一段之前,`windows/` 底下 .manifest 0 個、.rc 0 個、
# manifestdependency 0 處 —— 而 MSVC 預設產生的資訊清單**不含**
# `Microsoft.Windows.Common-Controls 6.0.0.0`。於是
# `InitCommonControlsEx` 拿到的是系統目錄裡的 **comctl32 v5**,
# 也就是 Windows 95/2000 那一代的控制項:立體灰按鈕、鑿進去的清單外框、
# 非佈景的核取方塊與捲軸。
#
# 使用者的原話是「感覺就是 win95 的方案」。他看到的是這件事,
# 不是配色沒調好 —— 而且**自繪救不了**,那幾顆是 comctl32/user32 自己畫的。
#
# 掛在有視窗的三支上:
#   · rime_service  設定視窗、懸浮狀態列、候選窗
#   · rime_tsf_host CI 上用真的 TSF 跑的宿主(要與 service 同一組控制項)
#   · rime_tests    test_win32_listview.cc 會開**真的** ListView 量像素 ——
#                   它拿到 v5 而 service 拿到 v6 的話,那支測試量的就不是
#                   使用者看到的東西。這一條比前兩條更容易被忽略。
foreach(_tgt rime_service rime_tsf_host rime_tests)
  if(TARGET ${_tgt})
    target_sources(${_tgt} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")
    # MSVC 會把 .manifest 當成輸入清單併進最終的資訊清單。
    # ⚠ 不可以同時讓 linker 自己產生一份預設的 —— 兩份會衝突,
    #   而症狀是 mt.exe 回 1090650081 然後整個連結失敗。
    set_property(TARGET ${_tgt} APPEND_STRING PROPERTY
                 LINK_FLAGS " /MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")
  endif()
endforeach()

message(STATUS "rime    : ${RIME_LIB}")
message(STATUS "opencc  : ${OPENCC_LIB}")
message(STATUS "marisa  : ${MARISA_LIB}")
message(STATUS "leveldb : ${LEVELDB_LIB}")
message(STATUS "yaml-cpp: ${YAMLCPP_LIB}")
message(STATUS "glog    : ${GLOG_LIB}")
