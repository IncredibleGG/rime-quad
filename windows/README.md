# Windows 端

目前的狀態:**有安裝程式、也有設定介面了。** CI 會真的把安裝程式裝起來、
斷言註冊、打一次字、再解除安裝。使用者已在真 Windows 上裝起來,輸入法出現在語言列上。

這一輪修掉一個使用者實際回報過的缺陷(**選了簡體輸入法卻打出繁體字**),
並補上設定介面與三個入口(語言列按鈕、系統匣、librime 內建的 Ctrl+`)。
**設定介面的每一顆控制項都真的做它宣稱的事** —— 做不到的刻意沒有放上去,
清單見[「刻意沒有做的」](#刻意沒有做的)。

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

### 這兩件事現在接起來了

上一輪這裡寫著「**簡體使用者選了 zh-Hans 那一份,打出來仍然是繁體字**」。
那是使用者實際回報的缺陷,這一輪修掉了。

⚠ **實作照的是 `docs/settings-model.md` §4,不是自己一套。** 那份規範由
macOS 端維護,Windows 端繼承;鍵名、四層優先順序、字集判定全部照它。
規範 §4 開頭記著**四端已經各錯了一次**:macOS 註冊了兩個輸入模式但兩個
都載入繁體方案;Windows 只註冊了一個 langid,連選都沒得選。共同點是
「畫面完全正常、自動化全過,錯的只有打出來是哪一種字」。

DLL 在 `ActivateEx` 時問系統「使用者現在用的是哪一份設定檔」,把 langid
經由 IPC 的 HELLO 帶給服務。服務把它當成規範 §4.2 的**輸入模式**:

| langid | 字集 |
|---|---|
| `0x0404` zh-Hant-TW / `0x0C04` zh-Hant-HK / `0x1404` zh-Hant-MO | 繁 |
| `0x0804` zh-Hans-CN / `0x1004` zh-Hans-SG | 簡 |
| 其他(含 langid = 0,也就是舊版的 DLL) | **不知道** |

⚠ **認不出來時回「不知道」,不要預設繁體**(規範明著寫的)。猜錯的代價
是使用者打出他不要的字,而且完全不知道為什麼。

挑方案是規範 §4.4 的四層,由高到低:

```
1. 使用者為「這個輸入模式」釘的方案   (schemas.pinnedHant / pinnedHans)
2. 使用者釘的單一方案                 (schemas.pinnedGlobal)
3. 已啟用清單中第一個字集相符的方案
4. 已啟用清單的第一個
```

* 1 與 2 都是**使用者說的話**,即使字集不符也照做 —— 這時靠簡繁開關把
  字集補齊,**不要偷偷換掉他選的方案**。
* 釘的方案已經被停用了 → 當作沒釘。選一個清單上沒有的東西,librime 會
  拒絕,而使用者看到的是「輸入法忽然沒有候選」。
* 第 1 層不是只有設定介面寫得到:使用者按 <code>Ctrl+`</code> 換過方案,
  我們從快照的 `schema_id` 變化發現它,並記進對應的桶。少了這一步,
  他換到別的程式就被打回去 —— 那顆鍵在他眼裡等於沒有作用。

⚠ **本專案打包的四個方案裡,沒有一個的 id 是「簡體」的命名慣例**
(`luna_pinyin_tw`、`bopomofo_tw` 是繁;`luna_pinyin`、`t9_pinyin` 不表態)。
所以簡體使用者走的是第 4 層 —— 拿到 `luna_pinyin_tw`,再靠簡繁開關轉成
簡體輸出。規範 §4.6 明著承認這個情形,而且提醒**不要在介面上宣稱做得到
我們做不到的事**:RIME 的簡繁轉換是單向的(繁→簡),一個本來就輸出簡體
的方案不會因為關掉它而變成繁體。

### ⚠ 簡繁不是一個 bool(與規範 §4.5 的一處差異,已回報)

規範 §4.5 說「設 `simplification`」。**光設它在本專案打包的方案上沒有作用。**
luna_pinyin 家族沒有那個開關,它用的是一組互斥的 radio:

```yaml
- options: [ zh_hant, zh_hans, zh_hant_hk, zh_hant_tw ]
  states:  [ 傳統漢字, 简化字,  香港字形,   臺灣字形 ]
```

Android 端在模擬器上實測過:只送 `simplification`,打 `guojia` 出來還是
「國家」。而 `rs_set_option` **不會**替你維持 radio 的互斥 —— 那是 librime
的 switcher 在使用者從選單裡選的時候才做的,同組另外三個要自己設 false,
兩個同時為真的話 t2s 之後會再串一次 t2tw。

所以 Windows 端的**決策**照 §4.5(要不要簡體、還是完全不碰),**套用的機制**
照 Android 驗證過的做法。這不是另一套模型,是同一個模型的正確接法;
§4.5 的措辭要不要補由規範的擁有者決定,已寫進 `docs/coordination.md` §5。

「要繁體時挑哪一種繁體」由 langid 決定(HK 拿香港字形、TW 拿臺灣字形、
不知道就用不轉換的傳統漢字)。**那不是一個設定項** —— 使用者選的是「繁體」。

### 這一格怎麼驗

`common/schema_choice.cc` 是純函式,`run_logic_tests.sh` 有 21 個案例守著。
`verify_installer.sh` §5b 另外對**裝好的**那份 `rime_service.exe` 斷言
(`--print-choice`),而且斷言的是**實際會送給引擎的那一組 option**,
不是一個中間表示 —— 決定簡繁的是那幾個 option。兩條反向測試:
同組字形開關必須正好一個是 true;`0x0409`(en-US)必須**完全不碰**簡繁
(少了它,一個「永遠設成繁體」的實作也會讓繁體那兩條通過)。

⚠ **三份設定檔共用同一個 CLSID。** 所以使用者在繁體與簡體之間切換時,
TSF **不會** Deactivate 再 Activate 這個文字服務,它只發
`ITfInputProcessorProfileActivationSink::OnActivated`。少了那個 sink,
切過去之後打出來仍然是切之前的字形。

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
windows/audit_offline_win.sh                     # 原始碼層面的離線稽核
windows/audit_offline_win.sh --self-check        # 它的反向測試
```

⚠ `syntax_check_mingw.sh` 目前**跳過** `tsf/lang_bar.cc`:mingw-w64 的
`ctfutb.h` 沒有 `ITfLangBarItemButton`(真正的 Windows SDK 有)。
跳過是大聲的,而且**會自己過期** —— 腳本先編一個探針,mingw 哪天補上了
就會失敗並叫人把跳過拿掉。為了讓跳過只有一個檔案,`tsf/lang_bar.h` 對外
只露四個函式與一個不透明型別;型別露在標頭上的話,`text_service.cc`
(全案最大的檔案)也會一起檢查不了。

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

### 「服務就緒」的定義

`rime_service.exe` 印出 `[service] ready`(以及寫 `--ready-file`)的那一刻,
下面三件事**全部**已經成立:

1. 管道實例已經建好,而且 `ConnectNamedPipe` 已經掛在核心裡等 ——
   `PipeServer::Start()` 會等監聽執行緒親自回報才返回。
2. 引擎已經**預熱**過:預設方案的詞典、使用者詞庫、文法都載入完畢
   (`service/main.cc` 的 `WarmUpEngine`)。
3. 因此第一個連上來的宿主不必付那筆冷啟動的錢。

第 2 點看起來像最佳化,其實是正確性:DLL 建 session 的預算只有 300ms
(`EnsureReady()` 跑在**宿主的 UI 執行緒**上,不能久等),而服務端處理
`SESSION_NEW` 時正好會做第一次 `rs_select_schema` —— 那是幾百毫秒到幾秒。
不預熱的話,第一個連上來的宿主幾乎必定逾時,而且每一次逾時的請求仍然會在
引擎執行緒上跑完,重試又排一份新的,佇列可能一直追不上。
使用者看到的是「剛開機 / 剛裝好的時候,第一個程式裡打不出中文」。

### 監聽迴圈死掉時會大聲說出來

管道的監聽迴圈若在沒有人要求停止的情況下結束,`service.log` 裡會出現
`[pipe] **監聽迴圈非預期結束**` 那幾行(帶著 `GetLastError()`),
而且**服務會自行結束**。

自行結束不是放棄,是必要的:沒有管道卻還活著的服務仍然佔著單一實例的
mutex,於是 DLL 想啟動一支新的也啟動不了(新的那支會判定「已經有一支在跑」
然後靜靜地以 0 結束)。那個狀態要重開機才會好。結束掉,DLL 的自動啟動
就能接手。

⚠ 舊版在同樣的情況下 `break` 掉迴圈就算了,**一個字都不印**:服務照樣印
`ready`、照樣寫 ready 檔、照樣活著,只是永遠沒有管道。從外面看,那與
「協議談不攏」長得一模一樣,而兩者該修的地方完全不同。

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

## 設定介面

`%APPDATA%\RimeQuad\rimequad.settings`(純文字,`key = value`,使用者改得動)。

三個入口。**沒有入口的設定介面等於沒做**,所以每一個都要能單獨成立:

| 入口 | 在哪一側 | 按下去做什麼 |
|---|---|---|
| 語言列 / 工作列輸入指示器的「設定」 | **DLL**(`tsf/lang_bar.cc`) | 見下面三條路 |
| 系統匣圖示(左鍵開、右鍵選單) | 服務進程 | 直接開視窗 |
| <code>Ctrl+`</code> / F4 方案切換 | librime 自己 | 我們只負責**發現它換了** |

語言列那顆按鈕按下去有**三條路**,少任何一條都會有一整類情境按了沒反應:

1. 管道已連上且協商到協議 v2 → 送 `Op::kOpenSettings`。
2. 否則 → 開具名事件 `Local\RimeQuadSettings.<SID>` 並 `SetEvent`。
3. 服務沒在跑 → `CreateProcess(rime_service.exe --settings)`。

理由:UWP／市集 App 的宿主跑在 AppContainer 裡,**開不了** `Local\` 底下
別人建立的具名物件 —— 那時只有路 1 走得通;而使用者從「還沒打過任何一個字」
的狀態按這顆按鈕時管道還沒連,那時只有路 2 或 3 走得通。

<code>Ctrl+`</code> 是 librime 內建的,我們沒有攔那顆鍵 —— 引擎自己會處理。
Windows 端要做的只是**發現使用者換了方案**(從快照的 `schema_id` 變化),
把它記進規範 §4.4 第 1 層那兩個桶之一(繁 / 簡)。**全域熱鍵刻意沒有做**:衝突了使用者不會
知道是我們幹的。

### 技術選型:純 Win32 + 通用控制項 v6

完整理由在 `service/settings_window.h` 檔頭。摘要:

| 排除的 | 為什麼 |
|---|---|
| Electron | 打包 Chromium —— 上百 MB,而且是一整個網路堆疊 |
| WebView2 | 一樣是 Chromium,而且**不在我們的控制下**(它自己會更新) |
| WinUI 3 / XAML Islands | 要 Windows App SDK 可轉散發套件,且與貫穿全案的 `/MT` 打架 |
| WPF / WinForms | 要 .NET 執行期,多一個更新來源 |
| 本機 HTTP + 系統瀏覽器 | **會開 socket。** 離線定位下這是最糟的選項 |

硬約束是「不要 Electron、不要打包 Chromium」,而它背後的理由不只是體積:
使用者要能相信「它不連網」,而說服他的方式是 `dumpbin /dependents`
看一眼就知道 —— 塞一個瀏覽器引擎進來,那句話就再也不可能驗證了。

### 為什麼在服務進程裡

設定要做的事幾乎每一件都需要引擎:列方案(`rs_schema_list`)、立刻套用簡繁
(`rs_set_option`)、重新部署並回報進度(`rs_deploy`)。另開一支 exe 的話這些
全部要再走一次 IPC,而「設定改了但引擎沒收到」會變成一整類新的失效。

⚠ **瘦 DLL 那一側只多了 `tsf/lang_bar.cc`。** `ITfLangBarItem` 是 TSF 向
**文字服務**要的介面,語言列的按鈕沒得選只能在 DLL 裡。它不畫東西、
不載字型、不建視窗,按下去只送一則單向訊息。`build.sh logic` 仍然不需要 librime。

### 分頁與每一項真的做了什麼

分頁照規範 §1:**輸入方案 / 外觀 / 文字 / 進階**。「手感」整頁拿掉
(震動、按鍵音、長按延遲都是軟鍵盤專屬);「方案市集」與「連網」這一輪
沒做 —— 空的一頁比沒有那一頁更難理解。

| 分頁 | 項目 | 設定鍵(規範 §3 的 id) | 層級 | 立刻生效? |
|---|---|---|---|---|
| 輸入方案 | 順序(上移/下移/套用) | `schema_list` | **A** | 要重新整理字詞 |
| 輸入方案 | 自動挑(核取方塊) | `schemas.followInputMode` | B | 下一個輸入視窗 |
| 輸入方案 | 一律使用 | `schemas.pinnedGlobal` | B | 立刻 |
| (打字時的方案切換鍵) | 記住換過的 | `schemas.pinnedHant/Hans` | B | — |
| 外觀 | 一次顯示幾個字 | `menu/page_size` | **A** | 要重新整理字詞 |
| 外觀 | 選字視窗的字大小 | `appearance.candidateScale` | B | 立刻 |
| 文字 | 繁體/簡體 | `text.variant` | B → C | 立刻 |
| 文字 | 標點 | `text.punctuation` | B → C | 立刻 |
| 進階 | 重新整理字詞 | — | — | — |
| 進階 | 開啟使用者資料夾 / 設定檔 | — | — | — |

三個層級的判準見規範 §2:**librime 自己會讀的東西一律放 A**
(`<user>/default.custom.yaml` 的 `patch:`)。放 B 的話,使用者用別的 RIME
前端打開同一個使用者目錄會看到完全不同的行為,而他不知道為什麼。

⚠ 上表的鍵名是給開發者看的。規範 §1 第 2 條:**畫面上不得出現 YAML
欄位名** —— 使用者看到的是「一次顯示幾個字」,不是 `menu/page_size`。

⚠ **「一次顯示幾個字」是 A 層,不是「畫幾個」。** 只截掉畫面上的候選是
不行的:數字鍵仍然選得到看不見的那幾個 —— 那是「看得到但摸不到」的
鏡像版本,而且更難查。規範 §3 也明訂主題**不得**改變一頁有幾個候選。
所以它會問使用者要不要現在重新整理字詞。

⚠ **A 層一律用「文字外科手術」寫,不可以「解析再重新輸出」**(規範 §2)。
`default.custom.yaml` 是使用者資料,裡面有我們自己寫的整段說明,也可能
有使用者自己加的按鍵綁定 —— yaml-cpp 會把註解全部吃掉。所以
`common/schema_list_patch.cc` 只換掉要換的那幾行,**保留行尾註解**,
新項目縮排四個空白。認不出檔案結構時**明著失敗**,不猜。

⚠ **改 A 層之後失敗要整份還原**(規範 §2)。還原的是**快照**,
不是「套用反向的編輯」—— 反向編輯的前提是外科手術本身沒有 bug,
而那正是出事當下最不該假設的事。不還原的話使用者會卡在「每次啟動都
部署失敗」,而且沒有自救途徑:設定介面的方案清單也會是空的。

⚠ **設定檔裡「沒有的鍵 = 沒設過 = 跟隨預設」。** 這是 Android 端整套設定的
地基,桌面端照抄:設回預設是**刪掉那個鍵**,不是寫一個哨兵值。
差別在哪天預設值變了 —— 沒表示過意見的人跟著走,明確選過的人不動。
(`schemas.followInputMode` 的預設是 `true`,所以「沒有那個鍵」要讀成
true;寫成「等於 true 才算開」的話,全新安裝的機器上自動挑方案是關的,
而那正是這一輪要修的缺陷本身。)

⚠ **兩條介面紀律**(規範 §1,規範性):每一個設定項都要有一句白話說它
會改變什麼;**不得把 YAML 欄位名搬到畫面上** —— `schema_list`、
`page_size`、`simplification`、`ascii_punct` 一個都不准出現。
上面那張表裡的鍵名是給開發者看的,畫面上寫的是「一次顯示幾個字」。

### 重新部署的進度與結果

`rs_deploy()` 是非同步的,而且 `rs_deploy_callback` **不在**呼叫端的執行緒上、
可能在 `rs_deploy()` 早就返回之後才觸發 —— 所以「呼叫過了所以做完了」是錯的。

更陰險的是**上一輪的結果**:直接讀一個 atomic 狀態的話,剛啟動時那一次
首次部署的 SUCCESS 會被當成這一次的結果,於是使用者按下按鈕的瞬間就看到
「完成」。所以這裡用序號,`PollDeploy` 只認**比按下去那一刻更新**的終局狀態。
(Android 端用 `AtomicBoolean armed` 解同一個問題。)

進度顯示的是**實際耗時**,不畫百分比 —— librime 不給百分比,
畫一條假的進度條比什麼都不畫更糟,它會停在某個數字然後不動。
`rs_deploy()` 拒絕啟動時明著跳訊息:Android 端真機回報過的原話是
「使用者只能猜它成功了沒」。

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
| **「ready 檔存在」等於「現在就連得上」**(反覆重啟 5 次,每次都要求第一下就連上) | `verify_ime.sh`(含反向測試) |
| 連不上時,診斷指得出是**開管道 / 握手 / 建 session** 哪一步 | `verify_ime.sh` 的反向測試 |
| 核心層(librime + 資料)真的打得出字 | `verify_console.sh` |
| 測試框架本身會不會紅 | `rime_tests --self-check`(反向測試) |
| 安裝包少了執行期資料就出不了貨 | `make_installer.sh --self-check`(反向測試) |
| **真的裝一次:登錄檔、TSF 列舉、打字、解除安裝** | `verify_installer.sh`,見下 |
| **langid → 方案/字形**(含 v1↔v2 的線路相容) | `test_schema_choice.cc` / `test_proto_compat.cc` |
| 設定的讀寫:三態、未知鍵保留、下拉索引、值不能偽造一行 | `test_settings.cc` |
| 方案排序寫回 `default.custom.yaml`(含「看不懂就明著失敗」) | `test_schema_list_patch.cc` |
| 離線守門的判斷與紀錄格式(尚未接上,見下) | `test_net_policy.cc` |
| **原始碼裡沒有任何檔案碰網路 API** | `audit_offline_win.sh`(含反向測試) |
| **產物的匯入表裡沒有網路 DLL** | `check_binaries.sh` 的 `NET_DLLS` |
| **裝好的**那份 `rime_service.exe` 真的替簡體使用者選簡體方案 | `verify_installer.sh` §5b(含反向測試) |

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
- `make_installer.sh --lint`:用假的 payload 把 `.iss` 完整編一次(掛在快速 job)

### ⚠ 三件在 CI 上實際踩到、值得記住的事

**1. 「安裝程式以 0 結束」不足以證明安裝做完了。**
`/SUPPRESSMSGBOXES` 之下,`[Code]` 裡的 `RaiseException` **不會**讓 Setup
以非零結束:對話框被自動按掉,例外只留在安裝記錄裡,Setup 照樣回報成功。
實測就是這樣 —— 一路綠燈,而 `CurStepChanged` 其實在中途就炸了。
所以 `verify_installer.sh` 斷言安裝記錄裡**沒有** `raised an exception`,
並斷言記錄裡確實有 `enable-user`。互動安裝時使用者看得到錯誤訊息,
那一條仍然成立;靜默安裝的真正閘門是 CI。

**2. 例外會中止 `CurStepChanged` 剩下的每一步。**
原本 `check` 排在 `enable-user` 之前,`check` 一炸,`enable-user` 一次都沒跑到。
症狀是「全機註冊全綠、使用者清單一片空白」,而錯誤訊息講的是「註冊失敗」——
**一個判斷失誤造成兩個看起來無關的症狀**。順序現在是
`register` → `enable-user` → `check`。

**3. 剛註冊完的當下,CTF 還看不到新的設定檔。**
實測 `register` 回傳成功之後 **0.12 秒**時 `EnumLanguageProfiles` 看不到我們,
**22 秒後**同一支程式跑同一段就三個語言全部看得到。
登錄檔是同步的,CTF 的可見性不是。所以安裝程式用 `check --no-enum`
(只驗登錄檔),而「系統接受了嗎」由 CI 事後不帶 `--no-enum` 再問一次。
`--no-enum` 會**明著印出「跳過」**——安靜地少驗一項與驗過了長得一樣。

另外:解除安裝後 `unins000.exe` 還留著是**正常的**,Inno 沒辦法在執行中刪掉
自己,它排到下次開機。斷言的是「**我們的**檔案都不在了」,不是「目錄全空」。

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

### 這一輪新增、而且**一項都沒有被驗過**的

⚠ 這一整段是這一輪最需要人去按一遍的地方。CI 驗到的是「判斷邏輯對」與
「編得起來」,**沒有一個像素、沒有一次點擊被驗證過**。

11. **`ITfInputProcessorProfileMgr::GetActiveProfile` 在 `ActivateEx` 的當下
    回傳的是不是我們。** 註冊完的 CTF 快取有延遲(實測 0.12 秒看不到、
    22 秒後看得到,見下面「三件實際踩到的事」),`Activate` 的當下會不會
    也有同一類延遲**沒有驗過**。有三層退路(ProfileMgr → GetCurrentLanguage
    → `LOWORD(GetKeyboardLayout(0))`),但**三層都沒有被執行過一次**。
12. **`ITfInputProcessorProfileActivationSink` 到底會不會被呼叫。** 這是
    「使用者從繁體切到簡體」唯一的通知管道 —— 少了它,切換完全沒有效果。
    整條路徑目前是紙上的。
13. **語言列上到底看不看得到那顆「設定」按鈕。** `TF_LBI_STYLE_SHOWNINTRAY`
    是照文件加的;`GetIcon` 回 `E_FAIL` 讓它退回顯示文字,那是慣例,
    **沒有在任何一個宿主上看過**。
14. **按下那顆按鈕之後設定視窗會不會出現。** 三條路(IPC / 具名事件 /
    CreateProcess)一條都沒被走過。UWP 宿主那一條尤其可疑。
15. **系統匣圖示會不會出現、右鍵選單三項會不會動。** 「結束輸入法服務」
    走的是具名事件而不是 TerminateProcess(詞庫是 LevelDB,中途拔掉會壞),
    但那條路沒被跑過。
16. **設定視窗長什麼樣。** 排版是算出來的,不是量出來的 —— 在 125% / 150%
    DPI 下控制項會不會重疊、中文字會不會被截斷,**完全沒有看過**。
    高 DPI 下 `SysTabControl32` 的內容區偏移是我照經驗值寫死的。
17. **每一顆控制項是不是真的做了它宣稱的事。** 這正是這個專案抓過四次的
    那一類問題,而 Windows 端到目前為止**一顆按鈕都沒有被人按過**。
    最可疑的三個:
    - 「套用順序」之後 librime 有沒有真的照新順序;
    - 「字形」改了之後**現有的**輸入視窗會不會立刻變(`SetOptionAll` 只對
      目前活著的 session 生效,而每個宿主進程各有一個 session);
    - 「候選字大小」改了之後候選窗會不會重畫(走的是一則自訂訊息)。
18. **重新部署的進度與結果。** 序號那一段解的是「上一輪的結果被讀成這一輪」,
    而那個 bug 只有在**首次部署剛好還沒結束**時才會出現 —— 那個時間窗
    在 CI 上構造不出來。
19. **`--settings` 在已經有服務在跑時真的把訊息傳過去了。**
20. **explorer 重啟之後系統匣圖示會不會回來**(`TaskbarCreated`)。

### 已知的功能缺口(不是忘了,是本輪範圍外)

- **只有 x64。** arm64 未做(Windows on ARM 的宿主要 arm64 的 DLL);
  32 位元宿主若要支援還需 x86。
- **修飾鍵事件收不到。** TSF 不把純修飾鍵(Shift / Ctrl)交給 key event sink,
  所以 librime 那套「按一下 Shift 切中英」在這條路徑上做不到。
  要補得另外掛低階鍵盤 hook。
- **沒有顯示屬性(組字底線)。** 未實作 `ITfDisplayAttributeProvider`,
  preedit 在部分宿主裡不會有視覺區別。
- ~~**沒有系統匣圖示、沒有設定介面。**~~ 本輪已加,見上面「設定介面」。
- ~~**簡體使用者選 zh-Hans 那一份,打出來仍然是繁體字。**~~ 本輪已解,
  見上面「這兩件事現在接起來了」。⚠ 但**只驗到判斷層**:引擎真的照著切,
  以及使用者真的看到簡體字,仍然要人去按一遍(見上面第 11–13 項)。
- **`enable-user` 只做 `EnableLanguageProfile`。** 它不會替使用者把
  「中文(繁體/簡體)」加進 Windows 的語言清單 —— 那需要 `input.dll` 的
  `InstallLayoutOrTip`,而那不是有文件的 API。所以使用者的語言清單裡
  沒有任何中文時,裝完仍然看不到這個輸入法;安裝完成頁明著寫了怎麼加。
  **不要在文案裡宣稱做得到我們沒做到的事。**
- **沒有編 librime-lua。** 倚賴 `lua_translator` / `lua_filter` 的第三方方案
  在 Windows 上會**部署成功但一個候選都沒有**。補上時
  `patches/librime-lua@sandbox.patch` 必須同時到位 —— `build.sh` 有一道會
  擋下建置的檢查。
- **Windows 端目前完全不連網,而這件事現在驗得到了。**
  `audit_offline_win.sh` 在原始碼層面斷言「`windows/` 底下沒有任何一個檔案
  碰網路 API」(並有反向測試:植入一個真的 `WinHttpOpen` 必須被抓到),
  `check_binaries.sh` 在產物的匯入表層面斷言同一件事。
  `ws2_32.dll` 只放行給服務進程 —— leveldb 與 glog 為了取主機名連結它,
  那是 librime 的相依,不是我們開的連線;**瘦 DLL 一律是零**。

  離線守門的**判斷邏輯與紀錄格式**已經移植好並有測試
  (`common/net_policy.cc`,來自 Android 的 `NetworkGate` / `NetworkLog`),
  但**還沒有任何東西呼叫它** —— 方案市集這一輪沒有做。要接的時候,
  `audit_offline_win.sh` 的檔頭寫了必須先做完哪三件事。
- **`VK_DECIMAL` 一律映成 `KP_Decimal`。** 德文等佈局的數字鍵台小數點其實是逗號,
  X11 有 `KP_Separator` 表示它,但要分辨得回頭問佈局。
- **候選窗沒有多欄/表格排版、沒有狀態列。** 等規範(見上一節)。

---

## 刻意沒有做的

**寧可少一個分頁,不要多一個點了沒反應的按鈕。** 這個專案抓過四個
「畫面完全正常、自動化全過」的鍵,所以下面每一項都是明著決定不做,
不是忘了。

| 沒做 | 為什麼 |
|---|---|
| **方案市集(下載)** | 見下面一整段 |
| **連網分頁** | 沒有東西會用到那個開關。一個什麼都不影響的開關就是一顆死鍵 |
| **候選窗主題 / 外觀分頁** | 規範的六個缺口 macOS 端正在補(`coordination.md` §5)。規範落地前做等於自己發明一套 |
| **詞庫匯出匯入** | `docs/backup-format.md` 還不在 `main` 上(`dict` 支線還沒產出) |
| **全域熱鍵** | 衝突了使用者不會知道是我們幹的 |
| **語言列按鈕的下拉選單** | 一顆按鈕、一件事。選單項目是「看得到但摸不到」最容易長出來的地方;要多做幾件事的入口是系統匣的右鍵選單(在服務進程那一側,改壞了不會把宿主帶走) |
| **自己的系統匣圖示** | 目前用 `IDI_APPLICATION`。要換得加一份 `.rc`,那會把資源編譯器拉進建置 —— 留給有美術資源的時候 |

### 方案市集為什麼沒做

它是這一輪明著砍掉的最大一塊,理由值得寫下來,因為下一輪會再撞到:

**Windows 上沒有現成的 zip 解壓路徑可以用,而且不能用現成的。**

- 我們的相依裡沒有 zlib(librime 的五個相依是 glog / yaml-cpp / leveldb /
  marisa / opencc,一個都不含 DEFLATE)。
- Windows 內建的 Compression API(`cabinet.dll`)只有 XPRESS / MSZIP / LZMS,
  **沒有 raw DEFLATE**,對 zip 沒有用。
- Shell 的 zip folders(`IShellDispatch::CopyHere`)可以解壓,但它會
  **繞過我們自己的 `ArchiveGuard`** —— 而 zip slip 的防護必須是我們的,
  不能是解壓器的。`docs/schema-store.md` §4 把那條列成「缺一不可」。

所以正確的做法是**自己寫一份 inflate**(純邏輯,可以在 Ubuntu 上對
Python 產生的 deflate 串流跑測試,而且每一個 entry 的 CRC32 都驗一次 ——
inflate 有 bug 會被抓到而不是安靜地寫出壞詞庫)。那大約是
「zip 中央目錄解析 + inflate + SHA-256 + WinHTTP + 安裝與回滾 + 市集 UI」
六塊,不是這一輪塞得下的量。**做一半的下載按鈕比沒有更糟。**

已經先落地的地基(有測試,沒接上):

- `common/net_policy.cc` —— 開關(未設 == 關)、fail-closed、
  每一跳的 scheme 檢查、轉址上限、大小上限、以及**連網紀錄只記真的
  發生過的連線**(被開關擋下的嘗試不記 —— 記了的話「開關從沒開過 →
  紀錄是空的」這句話就不成立,而那句話正是使用者稽核我們的方式)。
- `common/schema_list_patch.cc` —— 裝好之後要把方案加進 `schema_list`,
  失敗要回滾。改寫與回滾用的是同一支函式。
- `audit_offline_win.sh` —— 在還沒有連線的今天就先立好那道牆,
  哪天有人加了連線它會紅,而檔頭寫了必須同時做完哪三件事。

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
