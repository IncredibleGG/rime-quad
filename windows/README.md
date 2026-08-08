# Windows 端

目前的狀態:**有安裝程式了(`RimeQuad-Setup-x64.exe`),而且 CI 會真的把它裝起來、
斷言註冊、打一次字、再解除安裝。** 使用者已在真 Windows 上裝起來,輸入法出現在語言列上。

還沒有人驗證過的部分見
[「沒有被驗證的部分」](#沒有被驗證的部分) —— 那一節仍然是本文件最重要的一節,
只是這一輪從裡面搬走了好幾項。

---

## 安裝

```
RimeQuad-Setup-x64.exe     下載、雙擊、下一步、裝好。
```

雙擊就會跳 UAC 提權對話框(`PrivilegesRequired=admin` 寫在安裝程式自己的
manifest 裡),不需要右鍵「以系統管理員身分執行」。裝完之後按
`Win + 空白鍵` 切換輸入法。

| 東西 | 位置 |
|---|---|
| 程式與唯讀資料 | `C:\Program Files\RimeQuad`(位置固定,安裝時不讓選) |
| **使用者詞典、設定** | **`%APPDATA%\RimeQuad`** |

解除安裝走「新增或移除程式」,它會停掉服務、反註冊、刪掉程式,
**但刻意不刪 `%APPDATA%\RimeQuad`** —— 那是使用者的資料。

### 為什麼使用者資料一定要在 `%APPDATA%`

裝在 `C:\Program Files` 之下的話,一般權限的進程對那棵樹只有讀取權。
而 **librime 寫不進使用者目錄時不會停下來**:它照常給候選、照常上屏,
只是一個學過的詞都留不住,而且完全沒有錯誤訊息。等使用者發現
「它從來沒學會我的詞」已經是好幾天以後,那時沒有任何線索指向權限。

所以:

- `service/main.cc` 有一道檢查,使用者目錄若落在安裝目錄底下就**大聲停下來**。
- CI 在跑完一輪之後比對安裝目錄的檔案清單與時間戳,**一個位元都不准變**。
  (runner 上我們是系統管理員,權限本身擋不出這個 bug,只有那道比對擋得住。)
- 首次執行時 `<安裝目錄>\data\user` 的範本會被**只補不覆蓋**地複製過去。
  少了裡面的 `default.custom.yaml`,librime 會照上游 `default.yaml` 去部署
  cangjie5 / quick5 等我們沒有詞庫的方案 —— 部署噴錯,而使用者看到的是
  「有些方案切過去一個候選都沒有」。

### 打包工具:Inno Setup(不是 WiX)

完整理由在 `windows/installer/rimequad.iss` 檔頭。摘要:需求是**一個 .exe**,
而 WiX 的原生產物是 .msi,要變成單一 .exe 得再套一層 Burn bootstrapper;
解除安裝要跑真的邏輯(停服務、反註冊、保留詞典),Inno 的 Pascal script
直接做得到,MSI 要另外編一個 custom action DLL;而且 runner 內建 Inno。
MSI 唯一贏的是網域 GPO 派送,那不是這一輪的目標。

### 註冊在哪些語言底下

| langid | | 描述字串 |
|---|---|---|
| `0x0404` | zh-Hant-TW | RIME 四端輸入法 |
| `0x0804` | zh-Hans-CN | RIME 四端输入法 |
| `0x0C04` | zh-Hant-HK | RIME 四端輸入法 |

第一版只註冊了 `0x0404`,結果**系統語言是簡體中文的使用者在自己的語言底下
找不到這個輸入法** —— 它掛在「繁体中文(中国台湾)」那一欄。使用者實際回報過。

⚠ **語言標籤與實際打出簡繁是兩件事。** 清單上顯示成哪一種中文由這裡的 langid
決定;實際上屏的是簡體還是繁體字由 RIME 方案(`luna_pinyin` vs
`luna_pinyin_tw`)與簡繁開關決定,與 langid 無關。

⚠ **目前這兩件事還沒有接起來。** 服務進程不知道使用者是從哪一個 langid 的
profile 進來的,而預設方案是 `default.custom.yaml` 的 `schema_list` 第一項
(`luna_pinyin_tw`,繁體)。也就是說**簡體使用者選了 zh-Hans 那一份,
打出來仍然是繁體字。** 這是已知的、還沒解的問題,不是這一輪的範圍;
要解得讓 DLL 把 profile 的 langid 帶進 IPC 交給服務去挑方案。

zh-SG(`0x1004`)與 zh-MO(`0x1404`)刻意沒註冊:每多一份,有該語言的
使用者清單上就多一項。要加的成本只有一個 GUID 加一列。

---

## 架構:瘦 DLL + 獨立服務進程

> TSF 的 DLL 會被載入到**每一個**接受文字輸入的進程裡 —— 瀏覽器、Office、
> 以及提權的系統進程。**你的 DLL 崩潰,宿主跟著崩潰。**

```
每個宿主進程                              單一服務進程
┌────────────────────────┐              ┌──────────────────────────┐
│ rime_tsf.dll(瘦)      │              │ rime_service.exe          │
│  · TSF 協議             │              │  · rime_shell + librime   │
│  · VK_* → X11 keysym    │◄── 具名管道 ─►│  · 單一引擎執行緒          │
│  · 線路格式編解碼        │              │  · 候選窗(獨立 top-level)│
│  · fail-open 狀態機     │              │                          │
└────────────────────────┘              └──────────────────────────┘
   沒有 librime、沒有 YAML                    可以慢、可以崩
   沒有字型、沒有視窗                          崩了不會帶走使用者的文件
```

這條線在建置系統裡也是實的:`windows/build.sh logic` 建瘦 DLL 與測試,
**完全不需要 librime**。哪天它需要了,就代表 librime 跑進了每一個宿主進程。
`build.sh` 裡有一道檢查會擋下這件事。

| 目錄 | 內容 | 能不能在 Linux 上編 |
|---|---|---|
| `common/` | 線路格式、按鍵映射、組字政策、候選窗排版、連線狀態機 | **可以**(刻意不 include windows.h) |
| `winshared/` | UTF 轉換、使用者 SID、管道名 | 否 |
| `tsf/` | COM 外殼、文字服務、IPC 用戶端、ToUnicodeEx | 否(但可語法檢查) |
| `service/` | 引擎、管道伺服器、候選窗 | 否(但可語法檢查) |
| `setup/` | `rime_ime_setup.exe`:註冊/反註冊/檢查/停服務 | 否(但可語法檢查) |
| `installer/` | Inno Setup 腳本 | —(不編譯) |
| `tests/` | 單元測試、真實佈局測試、probe | 部分 |

註冊的實作在 `tsf/registration.cc`,**`rime_tsf.dll` 與 `rime_ime_setup.exe`
共用同一份**。兩邊各寫一份會漂移,而漂移的症狀是「用安裝程式裝的能用、
自己 `regsvr32` 的不能用」—— 兩種註冊狀態長得幾乎一樣,只差一個子鍵。
檢查與列舉那一份(`tsf/registration_check.cc`)**只**連進 setup,不進 DLL:
DLL 住在每一個宿主進程裡,它的相依有一份很短的允許清單。

---

## 開發迴圈(重要:不要用「推上去看會不會過」)

CI 一輪要幾分鐘到十幾分鐘。這兩支在開發機(Ubuntu)上就跑得動:

```bash
windows/run_logic_tests.sh          # g++ 編 common/ + tests/,跑 54 個案例
windows/run_logic_tests.sh --asan   # 加 ASan/UBSan(解碼器的模糊邊界)
MINGW=<mingw g++> windows/syntax_check_mingw.sh   # tsf/ 與 service/ 的語法檢查
```

`syntax_check_mingw.sh` **不是建置**,是 `-fsyntax-only`。它綠了不代表 MSVC 會綠;
但它紅了 MSVC 幾乎一定也紅,而它十秒就給答案。取得 mingw 的方法(不需要 root)
寫在那支腳本的檔頭。

---

## 按鍵映射:四端最難的一格

librime 吃 X11 keysym,TSF 給的是 `VK_*` + `GetKeyboardState`。
難點不是 keysym 表,是**同一顆實體按鍵在不同佈局下產生不同的字元**。

`common/keymap.cc` 把映射切成兩半:

1. **與佈局無關的鍵**(方向鍵、F1–F24、Backspace、數字鍵台、修飾鍵)用固定表。
   這些鍵在任何佈局下都是同一件事,寫死是正確的。
2. **會產生字元的鍵**(字母、數字、OEM 標點)**一律問佈局** ——
   `ToUnicodeEx(vk, scan, state, ..., hkl)`。問法抽象成 `KeyboardOracle`,
   所以測試可以餵假佈局,而且完全不必碰 Windows API。

### 為什麼寫死一張表在美式鍵盤上看起來完全正常

Windows 的佈局檔含 scancode→VK 對照,所以**字母鍵的 VK 會跟著字元走**:
德文 QWERTZ 上產生 `z` 的鍵回報 `VK_Z`,Dvorak 上產生 `o` 的鍵回報 `VK_O`。
對純字母而言,一張寫死的表**碰巧**是對的。真正會壞的是這三類:

| 類別 | 例子 | 後果 |
|---|---|---|
| OEM 標點鍵 | `VK_OEM_1`:美式 `;`、德文 `ü`、法文 `$` | Rime 的 punctuator 全對錯 |
| 數字列 | 法文 AZERTY 不按 Shift 是 `é`,按了才是 `2` | 法國使用者打不出數字 |
| AltGr 層 | 德文 `AltGr+Q` = `@`,而 Windows 回報成 Ctrl+Alt | 那顆鍵永遠打不出字 |

三類都有測試守著,其中 `test_win32_layouts.cc` 用的是
**真的**佈局(`LoadKeyboardLayout` 載入 `00000409` / `00000407` / `0000040C`)。
把 `MapKey` 改回常數表的話,在 GitHub 的美式 runner 上就會紅。

### 幾個容易錯的細節

- **AltGr**:成立時要**帶著它去問佈局**,但回報給引擎的 modifier
  **不可以**帶 Control/Alt。少做前者拿不到第三層的字元;少做後者,
  librime 會把 `AltGr+Q` 當成 `Ctrl+Alt+@` 這種快捷鍵。
- **Ctrl**:問佈局時要把 Ctrl 拿掉。`Ctrl+A` 在 Windows 上產生的是控制碼 `0x01`。
  引擎要的是 keysym `a` 加上 Control 修飾鍵(ibus / fcitx 的慣例)。
- **`ToUnicodeEx` 會消費核心裡的死鍵狀態。** 必須帶 `wFlags` 的第 2 位元
  (值 `4`,「不要改動鍵盤狀態」)。少了它,使用者按了法文的 `^` 之後,
  我們為了查表呼叫一次就把它吃掉了 —— 症狀是「裝了這個輸入法之後別的語言的
  重音字打不出來」,而且與輸入法的功能毫無關聯。這個旗標需要 Windows 10 1607+。
- **`kSuperMask` 是 `1 << 26` 不是 `1 << 6`** —— 但那是 librime 的遮罩,
  由 `core/src/rime_shell.cc` 負責轉換。Windows 端只用 `rs_modifier`,
  而 `test_keymap.cc` 有一條斷言把兩份定義釘在一起。

---

## IPC

具名管道 `\\.\pipe\rime-quad.<使用者 SID>.v1`,自帶長度分幀,
payload 是明確序列化的訊息(不是 memcpy 結構體 —— DLL 與服務可能來自不同建置)。

**管道上流的是使用者的每一次按鍵**,所以:

- DACL 明確只授權目前使用者的 SID(`D:P(A;;GA;;;<sid>)`)。
  預設的具名管道 DACL 允許同一台機器上的其他使用者連進來。
- `PIPE_REJECT_REMOTE_CLIENTS`。
- 握手時比對協議版本與 `rs_abi_version()`,不符就整條連線放棄。

### fail-open 是這一層的第一原則

`common/link_state.h` 只保護一條不變式:

> **只有在確定拿得到服務進程的回覆時,才可以吃掉按鍵。**

沒連上、握手版本不合、逾時、對面崩了、解碼失敗、序號對不上 ——
每一種都讓 `MayEatKey()` 變成 false,那顆鍵原樣交回宿主。

理由是這個專案吃過的虧:**編譯成功、單元測試全過、發布關卡全綠,
而使用者一裝就按鍵永久變灰。** 在 TSF 上,「變灰」的具體長相就是
DLL 回報 `pfEaten = TRUE` 卻拿不到結果 —— 那顆鍵既沒進文件、也沒變成候選,
它消失了,而且在**每一個程式裡**都消失。

輸入法沒作用,使用者會抱怨;輸入法吃掉按鍵,使用者的電腦不能打字。
這兩者不是同一個等級的失敗。

按鍵往返的逾時是 50ms。效能紅線是一到兩幀(16–33ms),50ms 不是目標值,
是**放棄的門檻**。

---

## 候選窗

服務進程開的獨立 top-level window(`WS_EX_NOACTIVATE`,絕不搶焦點)。
排版計算在 `common/cand_layout.cc`,是純函式,文字量測由外面注入 ——
所以「窗有多寬」「翻不翻面」「截斷在第幾個」在 Linux 上就測得到。

**視覺刻意做到最小。** 只用 `docs/theme-format.md` §8.6.1–8.6.5 與 §8.6.7
**已經寫下來**的欄位,取規範預設值,**不讀主題檔、不加欄位**。

理由:候選窗的規範由 macOS 端(第一個桌面端)擴充、Windows 端繼承
(`docs/coordination.md` §2)。在那些落地之前把視覺做滿,等於自己發明一套,
「一套配置四端共用」這個主張就沒了 —— 而且會安靜地分岔,等到第三端才發現。
發現的缺口已寫進 `docs/coordination.md` §5。

---

## 沒有被驗證的部分

**這一節是本文件最重要的一節。** CI 綠了**不代表**這個輸入法能用。

### CI 驗得到的

| 項目 | 由誰驗 |
|---|---|
| 瘦 DLL、服務進程、測試都編得起來 | `build.sh logic` / `build.sh ime` |
| 匯出的四個 COM 進入點齊全 | `check_binaries.sh`(`dumpbin /exports`) |
| DLL 沒有意外的相依(尤其不相依動態 CRT) | `check_binaries.sh`(`dumpbin /dependents`) |
| 按鍵映射,含**真實**的德文/法文佈局與 AltGr | `test_win32_layouts.cc` |
| 線路格式:往返、每一個截斷點、亂長度、分幀 | `test_protocol.cc` |
| 組字政策(`menu.count` 那條)、標籤格式 | `test_policy.cc` |
| 候選窗排版與定位(含翻面、夾回螢幕內) | `test_layout.cc` |
| fail-open:每一種失敗都不吃按鍵 | `test_link_state.cc` |
| **經由真的具名管道**打出「你好」(luna_pinyin_tw) | `verify_ime.sh` + `rime_probe.exe` |
| 核心層(librime + 資料)真的打得出字 | `verify_console.sh` |
| 測試框架本身會不會紅 | `rime_tests --self-check`(反向測試) |
| 安裝包少了執行期資料就出不了貨 | `make_installer.sh --self-check`(反向測試) |
| **真的裝一次:登錄檔、TSF 列舉、打字、解除安裝** | `verify_installer.sh`,見下 |

### 這一輪從「驗不了」搬到「驗得了」的

上一輪把「regsvr32 是否真的註冊成功、輸入法是否出現在系統的清單上」列在
只有人做得到那一欄。**那個判斷有一半是錯的** —— `windows-latest` 的 runner 上
我們是系統管理員,所以 `windows/verify_installer.sh` 這一整條跑得動:

| 斷言 | 具體內容 |
|---|---|
| 靜默安裝 | `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`,結束碼必須 0 |
| 檔案 | 三個二進位 + `data\shared` 的四個 schema、三本詞庫、`essay.txt`、`.ocd2`、`data\user\default.custom.yaml` |
| COM | `HKLM\SOFTWARE\Classes\CLSID\{E94B9FC2-…}` 存在 |
| COM | `…\InprocServer32` 預設值 **等於**`C:\Program Files\RimeQuad\rime_tsf.dll`(精確比對,抓「登錄檔指著建置樹裡那份」) |
| COM | `…\InprocServer32` 的 `ThreadingModel` = `Apartment` |
| TSF | `HKLM\SOFTWARE\Microsoft\CTF\TIP\{CLSID}` 存在 |
| TSF | `…\LanguageProfile\0x00000404\{07FB3057-…}`、`\0x00000804\{57BE9E4D-…}`、`\0x00000C04\{23BBABB2-…}` **三個都在**(不是「至少一個」) |
| TSF | `…\Category\Category` 底下正好 6 個類別,底下正好 1 + 5×3 筆 |
| **系統接受了嗎** | `ITfInputProcessorProfiles::EnumLanguageProfiles(langid)` 對**每一個** langid 都列舉得到我們;`ITfInputProcessorProfileMgr::EnumProfiles` 也列得到 |
| 使用者側 | `HKCU\…\CTF\TIP\{CLSID}\LanguageProfile\…` 有東西(`enable-user` 真的生效) |
| 新增或移除程式 | `…\Uninstall\{7A033CF7-…}_is1` 有 `DisplayName` / `DisplayVersion` / `UninstallString` |
| 資料目錄 | `rime_service.exe --print-dirs`:shared 在 Program Files 底下,user **不在** |
| **真的打得出字** | 用**安裝好的**服務與**安裝好的**詞庫(不給 `--shared` / `--user`),經由真的具名管道 `nihao → 你好` |
| **沒有寫進 Program Files** | 跑完之後安裝目錄的檔案清單與時間戳與跑之前逐字元相同 |
| 解除安裝 | 走登錄檔裡那一筆 `UninstallString`(使用者按下去會跑的同一支) |
| 解除安裝後 | CLSID、CTF\TIP、三份語言設定檔、ARP 那一筆**全部消失**,安裝目錄清空 |
| **解除安裝後** | **`%APPDATA%\RimeQuad` 還在而且非空** —— 唯一一項重裝也補救不回來的失敗 |

反向測試(證明上面那些不是恆真):

- 安裝**之前** `rime_ime_setup.exe check` 必須以非零結束
- 裝好之後**故意刪掉** `InprocServer32`,`check` 必須紅;重新註冊後必須又綠
- 解除安裝**之後** `check` 必須再度紅
- `make_installer.sh --self-check`:空的 payload 紅、只少 `default.custom.yaml` 紅、補齊後綠

### 只有人在真 Windows 上跑才驗得到的

**以下每一項目前都是「寫出來了,沒被驗過」。**

1. **切到這個輸入法之後 `ActivateEx` 有沒有被呼叫**、sink 有沒有掛上。
2. **在記事本裡打不打得出字。** 組字視窗(TSF composition)會不會出現、
   preedit 更新對不對、commit 進不進得去文件。
   ⚠ CI 驗到的是「服務端打得出字」,不是「TSF 這一層打得出字」。
3. **使用者的語言列上到底看不看得到它。** CI 斷言得到「系統接受了這個輸入法」,
   但看不看得到還取決於使用者的語言清單裡有沒有那幾種中文,
   而 runner 上沒有辦法製造那個情境。
4. **候選窗會不會出現**、位置對不對、在高 DPI 與多螢幕下對不對。
   `GetTextExt` 在很多宿主上會失敗或給空矩形(已有退回宿主視窗左上角的路徑,
   但那條路徑也沒被驗過)。
5. **在瀏覽器、Office、UWP／市集 App 裡能不能用。** 那需要
   `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` 等能力類別註冊正確 —— 已經寫了,沒驗過。
6. **提權的視窗上能不能打字**(`GUID_TFCAT_TIPCAP_SECUREMODE`)。
   相關的:DLL 在提權宿主裡**不會**自動啟動服務(那會產生一支提權的服務,
   把使用者的詞庫檔案擁有者換掉)。所以提權視窗裡預期是「沒有輸入法」,
   而不是崩潰 —— **這個預期也沒被驗過。**
7. **服務自動啟動**:DLL 找不到管道時 `CreateProcess` 起 `rime_service.exe`。
8. **首次部署那幾分鐘的行為。** 服務在部署完成前對每一顆按鍵立刻回「沒處理」,
   使用者應該看到的是「輸入法還沒好,打出來是英文」而不是卡住。
9. **「每一顆鍵是不是都真的做了它宣稱的事」。** 這個專案已經抓到四個
   「畫面完全正常、自動化全過」的功能:重輸鍵呼叫的是結束組字而不是清空、
   中英鍵切了模式卻不換佈局、按下後顏色回不來、工具列的 emoji 鍵什麼都不做。
   Windows 端目前**一顆鍵都沒有被人按過**。
10. **`ToUnicodeEx` 的死鍵狀態沒有被我們吃掉。** 程式碼帶了「不改動鍵盤狀態」
    的旗標,但那件事只有真的按一次法文的 `^` 再按 `e` 才驗得到。

### 已知的功能缺口(不是忘了,是本輪範圍外)

- **只有 x64。** arm64 未做(Windows on ARM 的宿主要 arm64 的 DLL);
  32 位元宿主若要支援還需 x86。
- **修飾鍵事件收不到。** TSF 不把純修飾鍵(Shift / Ctrl)交給 key event sink,
  所以 librime 那套「按一下 Shift 切中英」在這條路徑上做不到。
  要補得另外掛低階鍵盤 hook。
- **沒有顯示屬性(組字底線)。** 未實作 `ITfDisplayAttributeProvider`,
  preedit 在部分宿主裡不會有視覺區別。
- **沒有系統匣圖示、沒有設定介面。** 服務靠 DLL 自動啟動;結束的方式是
  `rime_ime_setup.exe stop-service`(送具名事件,安裝程式與解除安裝程式用它),
  但使用者手上沒有按鈕。
- **簡體使用者選 zh-Hans 那一份,打出來仍然是繁體字。** 語言設定檔已經按
  langid 分開註冊了(見上面「註冊在哪些語言底下」),但服務進程不知道
  使用者是從哪一份進來的,預設方案仍是 `schema_list` 的第一項
  `luna_pinyin_tw`。要解得讓 DLL 把 profile 的 langid 帶進 IPC。
- **`enable-user` 只做 `EnableLanguageProfile`。** 它不會替使用者把
  「中文(繁體/簡體)」加進 Windows 的語言清單 —— 那需要 `input.dll` 的
  `InstallLayoutOrTip`,而那不是有文件的 API。所以使用者的語言清單裡
  沒有任何中文時,裝完仍然看不到這個輸入法;安裝完成頁明著寫了怎麼加。
  **不要在文案裡宣稱做得到我們沒做到的事。**
- **沒有編 librime-lua。** 倚賴 `lua_translator` / `lua_filter` 的第三方方案
  在 Windows 上會**部署成功但一個候選都沒有**。補上時
  `patches/librime-lua@sandbox.patch` 必須同時到位 —— `build.sh` 有一道會
  擋下建置的檢查。
- **沒有離線出口的對應物。** Windows 端目前不連網,所以還沒有東西要守;
  一旦加入方案市集或升級檢查,必須先做出等價的閘門與連網紀錄,
  而且**先確認做得到再寫進文案**。
- **`VK_DECIMAL` 一律映成 `KP_Decimal`。** 德文等佈局的數字鍵台小數點其實是逗號,
  X11 有 `KP_Separator` 表示它,但要分辨得回頭問佈局。
- **候選窗沒有多欄/表格排版、沒有狀態列。** 等規範(見上一節)。

---

## 本機重跑(需要一台 Windows)

```bash
windows/build.sh                                  # deps + console + ime
scripts/fetch_rime_data.sh && scripts/collect_data.sh
windows/verify_console.sh                         # 核心層
windows/verify_ime.sh                             # 經由具名管道的端到端
windows/check_binaries.sh third_party/build/windows-x64/ime/bin
third_party/build/windows-x64/ime/bin/rime_tests.exe
windows/make_installer.sh                         # → installer/RimeQuad-Setup-x64.exe
```

安裝與驗證(需要系統管理員權限):

```bash
windows/verify_installer.sh \
  --setup third_party/build/windows-x64/installer/RimeQuad-Setup-x64.exe \
  --probe third_party/build/windows-x64/ime/bin/rime_probe.exe \
  --tool  third_party/build/windows-x64/ime/bin/rime_ime_setup.exe
```

⚠ 它會**真的**裝到 `C:\Program Files\RimeQuad`、註冊、然後解除安裝。
不要在自己日常用的機器上跑。

手動操作(開發用;一般使用者走安裝程式,不碰這些):

```
rime_ime_setup.exe register        全機註冊(需提權)
rime_ime_setup.exe unregister
rime_ime_setup.exe enable-user     目前使用者
rime_ime_setup.exe check [--user]  斷言註冊狀態,不通過就非零結束
rime_ime_setup.exe paths           印出所有會被寫到的登錄檔路徑與 GUID
rime_ime_setup.exe dump            印出登錄檔實況
rime_ime_setup.exe stop-service
```

`regsvr32 rime_tsf.dll` 仍然有效(那兩個匯出是 COM in-proc server 的既定介面,
而且與 `rime_ime_setup.exe register` **共用同一份實作** —— 見 `tsf/registration.cc`),
但它只會告訴你一個 HRESULT。開發時用 `rime_ime_setup.exe check`。

`rime_service.exe` 找資料的順序:

```
rime_service.exe --print-dirs      # 印出解析結果就結束,不啟動引擎
```

| | 預設 | 覆寫 |
|---|---|---|
| shared | `<執行檔目錄>\data\shared` | `--shared` / `RIME_SHARED_DATA_DIR` |
| user | `%APPDATA%\RimeQuad` | `--user` / `RIME_USER_DATA_DIR` |
| 範本 | `<執行檔目錄>\data\user` | `--seed` |

---

## 幾個不明顯但改了會壞的決定

**腳本是 bash,編譯器是 MSVC。** 寫這一輪的人手上沒有 Windows 機器,唯一的驗證
管道是 CI,一輪十幾分鐘。bash 至少能在別台機器上 `bash -n` 先掃過語法。
產物不受影響 —— 編譯器仍然是 `cl.exe`。

**`common/` 底下不可以 include windows.h。** 那不是潔癖,是那條驗證管道本身:
`run_logic_tests.sh` 靠它在 Ubuntu 上跑 54 個案例、786 個斷言(MSVC 上再加
4 個真實鍵盤佈局的案例,共 58 個、815 個斷言)。
Windows 專屬的小工具放在 `winshared/`。

**CMake 只在建 librime 相依時釘 3.x。** CMake 4 移除了 `FindBoost`,而 librime 用
`find_package(Boost)` 找 header-only 的 Boost,4.x 上**不會報錯**,只會在編譯時
噴一整片找不到 boost 標頭。但那是 librime 的限制,與瘦 DLL 無關 ——
所以快速 job 不必為此多下載一份 CMake。

**Boost 只解出 header 樹。** librime 不需要任何 Boost 二進位(上游的
`install-boost.bat` 也只做 `b2 headers`)。完整原始碼在 NTFS 上解壓要十幾分鐘。

**`/MT` 必須貫穿全部。** 除了 LNK2038 之外,這一輪多了一個更重要的理由:
TSF 的 DLL 被載入到**每一個**宿主進程裡,相依到 `VCRUNTIME140.dll` 的話,
在沒裝 VC++ 執行檔套件的機器上它會在載入階段失敗 ——
而症狀是「輸入法在某些程式裡整個不存在」,錯誤落在宿主那邊,使用者看不到。
`check_binaries.sh` 就是守這件事的。

**產生器是 Ninja,不是 Visual Studio 產生器。** VS 產生器的名字裡帶著 VS 的版本號
(`Visual Studio 17 2022`),等於把 **CMake 版本**和 **runner 上的 VS 版本**綁在
一起。第一版就是這樣掛掉的。Ninja 把兩件事解耦。

**`/utf-8` 不是可選的。** MSVC 預設用系統 ANSI 代碼頁解讀無 BOM 的原始碼,
也用它當執行字元集。少了這個旗標,中文字面值在編譯階段就已經被解錯 ——
印出來是亂碼,看起來像引擎壞了。

**驗證輸出一律導進檔案再比對,而且先 `tr -d '\r'`。** MSVC 的 CRT 在文字模式
下把 `\n` 寫成 `\r\n`;留著 CR 的話整行精確比對會失敗,而訊息看起來會像
「你好 != 你好」。

**opencc 用 `USE_SYSTEM_MARISA=ON`。** 不開的話 opencc 會編自己那份 marisa 並把
`marisa.lib` 裝進同一個 prefix,覆蓋掉 `deps/marisa-trie` 的那一份。兩份剛好都是
0.3.1 所以出不了事,但「誰覆蓋誰」取決於安裝順序 —— 那是會靜默漂移的東西。
代價是要手動注入 include / lib 路徑,所以**倉庫路徑不得含空白**,`build.sh` 開頭會擋。

**`rime_shell.cc` 重宣告的兩個私有符號真的有被驗到。**
`RimeGetKeycodeByName` / `RimeGetKeyName` 是對 librime 私有標頭
`rime/key_table.h` 的就地重宣告,靠 C++ mangling 對上定義,而 MSVC 的 mangling
與 Clang 不同。對不上會是連結錯誤;但「對上了卻接到別的東西」只有實際查一次表
才看得出來,所以 `verify_console.sh` 明著斷言 `BackSpace → 0x00FF08` 與
未知鍵名 `→ 0`。

**⚠ 服務進程的進入點必須是 `main`,不可以是 `wmain`。**

這一條花了五輪 CI 才查到,而它與輸入法一點關係都沒有。glog 的
`ProgramInvocationShortName()` 在 Windows/MSVC 上走這一條
(`deps/glog/src/utilities.cc` 的 `HAVE___ARGV` 分支):

```cpp
return const_basename(__argv[0]);
```

而 `__argv` **只有在 CRT 以窄字元進入點啟動時才會被填**。用 `wmain` 的話
CRT 只填 `__wargv`,`__argv` 是 `NULL` —— 於是 librime 的 `SetupLogging`
一呼叫 `google::InitGoogleLogging` 就是空指標解參考,服務進程一啟動就死。

症狀完全不指向進入點:`0xC0000005`,堆疊在 glog 深處,而同一個 job 裡的
`rime_console`(同一份 `rime_shell.cc`、同一批靜態庫、同一份資料)完全正常 ——
差別只在**它用的是 `main`**。`service/main.cc` 裡有一道會講人話的檢查守著這件事。

參數則仍然用 `CommandLineToArgvW` 取寬字元版本:窄字元 `argv` 走系統 ANSI
代碼頁,使用者目錄或安裝路徑裡有中文時會被換成 `?`。`rime_probe.exe` 也踩過
同一個坑 —— `--expect 你好` 變成 `--expect ??`,斷言永遠不會通過。

**驗證用的使用者目錄要明確指定方案。** librime 把「上次選的方案」記在
`<user>/user.yaml` 裡。`verify_ime.sh` 沿用 `verify_console.sh` 編好的使用者目錄
(省掉好幾分鐘的詞庫編譯),而那支腳本最後一個案例是注音 —— 於是 `nihao`
被當成注音打出了「所噢草莓」。現在 probe 明著送 `select_schema` 並斷言
引擎真的切過去了。**「不指定」不是中性的,它等於「取決於上一個人做了什麼」。**

**單元測試框架沒有 SKIP。** 要嘛跑、要嘛不存在。而且案例跑完一個斷言都沒有
就算失敗 —— 「測試函式存在但裡面什麼都沒斷言」在報表上與通過長得一樣。
另有 `--self-check` 反向測試,CI 要求它非零結束。
理由:這個專案有過「測試是綠的,因為它沒在測」。
