# Windows 端

目前的狀態:**只有核心層,沒有輸入法。**

這個目錄現在只做一件事 —— 證明「librime + 四端共用的 `rime_shell` 門面 + 方案資料」
在 Windows/MSVC 上真的組得起來、真的打得出字。沒有 TSF、沒有 COM、沒有候選窗。

先做這一層的理由寫在 `docs/handoff-windows.md` §8:核心層沒綠之前做 UI,
出問題分不出是引擎、資料、還是 UI 的錯。這個專案已經吃過這個虧。

---

## 目前有什麼

| 檔案 | 做什麼 |
|---|---|
| `build.sh` | 用 MSVC 建 librime 與 5 個靜態相依,再建 `rime_console.exe` |
| `CMakeLists.txt` | `rime_console` 目標(`core/src/rime_shell.cc` + `tools/rime_console.cc`) |
| `verify_console.sh` | 不經 UI 的核心驗證:斷言 `nihao` → 你好、注音 `su3cl3` → 你好 |

CI:`.github/workflows/windows.yml`(`windows-latest`)。綠燈的意思是上面那兩個
斷言真的過了,不是「編得出來」。

已實測通過的內容(x64,MSVC 14.51 / VS 18,librime 1d0df6e):

```
nihao  → 候選 你好/妳好/逆號/擬好/你 → 選 1 → COMMIT "你好"
su3cl3 → preedit ㄋㄧˇ ㄏㄠˇ → 選 1 → 仍在組字(preedit「你好」)→ 明確 commit → COMMIT "你好"
```

注音那條走的正是 `rime_shell.h` 裡寫的那條政策:選字之後 `is_composing` 仍為
true,要靠 `menu.count == 0` 才能判定「轉換完成待確認」。只測拼音永遠碰不到它。

本機重跑(需要一台 Windows + 含 C++ 工具集的 Visual Studio + Git Bash;
`build.sh` 用 `vswhere` 自己找 VS,不必是特定版本):

```bash
windows/build.sh
scripts/fetch_rime_data.sh && scripts/collect_data.sh
windows/verify_console.sh
```

---

## 幾個不明顯但改了會壞的決定

**腳本是 bash,編譯器是 MSVC。** 寫這一輪的人手上沒有 Windows 機器,唯一的驗證
管道是 CI,一輪十幾分鐘。bash 至少能在別台機器上 `bash -n` 先掃過語法。
產物不受影響 —— 編譯器仍然是 `cl.exe`。

**CMake 釘在 3.31.x。** CMake 4 移除了 `FindBoost`,而 librime 用
`find_package(Boost)` 找 header-only 的 Boost。糟糕的是非 LINUX 分支沒帶
`REQUIRED`,4.x 上**不會報錯**,只會在編譯時噴一整片找不到 boost 標頭。
Android 端釘 cmake 3.22.1 是同一個原因。

**Boost 只解出 header 樹。** librime 不需要任何 Boost 二進位(上游的
`install-boost.bat` 也只做 `b2 headers`)。完整原始碼在 NTFS 上解壓要十幾分鐘。

**`/MT` 必須貫穿全部。** librime、5 個相依、`rime_console` 只要有一個用了 `/MD`
就是 LNK2038,而錯誤訊息指的檔案通常不是真正出錯的那一個。腳本同時傳
`CMAKE_MSVC_RUNTIME_LIBRARY` 與 librime 的 `*_flag_overrides.cmake`:前者在
leveldb(宣告 3.9)、opencc(3.12)這些舊 `cmake_minimum_required` 的專案上
因為 CMP0091=OLD 而無效,得靠後者。

**產生器是 Ninja,不是 Visual Studio 產生器。** VS 產生器的名字裡帶著 VS 的版本號
(`Visual Studio 17 2022`),等於把 **CMake 版本**和 **runner 上的 VS 版本**綁在
一起。第一版就是這樣掛掉的:CMake 釘 3.31,而 `windows-latest` 已經換成
`windows-2025-vs2026` 的映像,3.31 不認得 VS 2026。Ninja 把兩件事解耦 ——
編譯器由 `vcvars64.bat` 設好的環境決定(`build.sh` 用 `vswhere` 自己找,不寫死
VS 路徑),CMake 只負責產生規則。上游 librime 自己的 Windows CI 也是 Ninja + MSVC。

**`/utf-8` 不是可選的。** MSVC 預設用系統 ANSI 代碼頁解讀無 BOM 的原始碼,
也用它當執行字元集。`rime_console.cc` 與 `rime_shell.cc` 都有中文字面值,
少了這個旗標,字串在編譯階段就已經被解錯 —— 印出來是亂碼,看起來像引擎壞了。

**驗證輸出一律導進檔案再比對,而且先 `tr -d '\r'`。** MSVC 的 CRT 在文字模式
下把 `\n` 寫成 `\r\n`;留著 CR 的話整行精確比對會失敗,而訊息看起來會像
「你好 != 你好」。

**opencc 用 `USE_SYSTEM_MARISA=ON`。** 不開的話 opencc 會編自己那份 marisa 並把
`marisa.lib` 裝進同一個 prefix,覆蓋掉 `deps/marisa-trie` 的那一份。兩份剛好都是
0.3.1 所以出不了事,但「誰覆蓋誰」取決於安裝順序 —— 那是會靜默漂移的東西。
代價是上游在這個模式下沒把 include / lib 路徑接到 target 上(它假設 marisa 在
系統目錄),要手動以 `CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES` 與 `/LIBPATH` 注入,所以**倉庫路徑不得含空白**,
`build.sh` 開頭會擋。

**`rime_shell.cc` 重宣告的兩個私有符號真的有被驗到。**
`RimeGetKeycodeByName` / `RimeGetKeyName` 是對 librime 私有標頭
`rime/key_table.h` 的就地重宣告,靠 C++ mangling 對上定義,而 MSVC 的 mangling
與 Clang 不同。對不上會是連結錯誤;但「對上了卻接到別的東西」只有實際查一次表
才看得出來,所以 `verify_console.sh` 明著斷言 `BackSpace → 0x00FF08` 與
未知鍵名 `→ 0`。

---

## 還沒解掉

**只有 x64。** arm64 沒做。Windows on ARM 的宿主要 arm64 的 DLL,32 位元宿主
若要支援還需要 x86。

**沒有編 librime-lua。** 這一輪的範圍是「librime 與 5 個依賴」,外掛不在其中。
後果是實在的:倚賴 `lua_translator` / `lua_filter` 的第三方方案(雾凇拼音、
萬象、openfly…)在 Windows 上會**部署成功但一個候選都沒有** —— 最難察覺的
失敗模式。

補上它的時候,`patches/librime-lua@sandbox.patch` 必須同時到位。沒套沙盒等於
第三方方案能在使用者機器上執行任意程式碼(`os.execute`、`io.popen`、
`package.loadlib`;Android 端已實測確認這些真的會執行)。`build.sh` 裡有一道
會**擋下建置**的檢查:只要 `plugins/lua` 出現而 sandbox patch 沒套用,就停。

**TSF、COM、候選窗、按鍵映射都還沒開始。** 那是下一輪,而且按鍵映射
(`VK_*` + `GetKeyboardState` → X11 keysym,且受使用者實體鍵盤佈局影響)
是四端中最難的一格,見 `docs/handoff-windows.md` §4。

**沒有離線出口的對應物。** Android 端有 `NetworkGate.kt` 單一出口加
`scripts/audit_offline.sh` 守著。Windows 端目前不連網,所以還沒有東西要守;
但一旦加入方案市集或升級檢查,就必須先做出等價的閘門與連網紀錄,
而且**先確認做得到再寫進文案**(`docs/handoff-windows.md` §6)。

**主題規範的候選窗缺口還在。** `docs/theme-format.md` §11 列的多欄/表格排版與
狀態列外觀仍未定義。那份規範由 macOS 端擴充、Windows 端繼承 —— 發現不足要回報,
不要自己加欄位。規範一分岔,「一套配置四端共用」就沒了。
