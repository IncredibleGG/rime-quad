# Windows 端

目前的狀態:**CI 現在會真的經過 TSF 打一次字。** 這一輪之前,
「切到這個輸入法之後系統有沒有把 DLL 載進來、ActivateEx 有沒有被呼叫、
按鍵經 TSF 進來之後打不打得出字」整條是**紙上的** —— 而使用者回報的
「裝好了,三個語言都打不出中文,也沒有任何 UI」撞的正是那一段。

這一輪做了四件事:

1. **找到並修掉一個會讓「打不出字」與「沒有 UI」同時發生的缺陷。**
   見下面[「一個根因,兩個症狀」](#一個根因兩個症狀)。
2. **瘦 DLL 現在會留下落地的除錯記錄。** 它住在別人的進程裡,
   印到 stdout 沒有人看得到,而使用者手上沒有偵錯器。
3. **`rime_ime_setup.exe doctor`** —— 使用者跑一次、把輸出貼過來就夠了。
   見[「使用者說『不能用』的時候」](#使用者說不能用的時候)。
4. **CI 往「真的經過 TSF」推了一大步。** `windows/verify_tsf.sh` 用一個
   假的文字編輯器逼系統走完
   `登錄檔 → CoCreateInstance → 載入 DLL → ActivateEx → 送按鍵 → 組字 → 上屏`。

仍然沒有人驗證過的部分見
[「沒有被驗證的部分」](#沒有被驗證的部分) —— 那一節仍然是本文件最重要的一節,
只是這一輪又從裡面搬走了好幾項。

---

## 一個根因,兩個症狀

使用者回報的是兩句話:「無論什麼語言都不能打中文」「也沒有任何 UI 界面」。
看起來像兩個問題。**它們是同一個。**

服務進程(`rime_service.exe`)持有引擎、候選窗、系統匣圖示與設定視窗。
在這一輪之前,它唯一的啟動時機是「第一顆按鍵走到 `EnsureReady()`」。
而按鍵要走到那裡,得先讓 `MapKey` 映出非零的 keysym —— 那一步是問
`ToUnicodeEx`「這顆鍵在你的鍵盤佈局上是什麼字」。

**問題是:我們的文字服務被啟用時,`GetKeyboardLayout(0)` 拿到的不保證是
一份真的鍵盤佈局。** TSF 的文字服務在系統裡也有自己的 HKL,形狀是
`0xFxxx<langid>`;IMM32 那一代的 IME 是 `0xExxx<langid>`。兩者都不是鍵盤
佈局的控制代碼,`ToUnicodeEx` 對它們**一個字都不會給**。於是:

```
ToUnicodeEx 回 0
  → MapKey 回 keysym == 0
    → OnTestKeyDown 直接放行,不吃這顆鍵
      → OnKeyDown 根本不會被呼叫
        → 引擎一顆按鍵都收不到,連線永遠不會建立
          → **服務進程永遠不會被啟動**
            → 沒有系統匣圖示、沒有設定視窗、沒有候選窗
```

一個判斷失誤,兩個看起來無關的症狀,而且**每一層都「正常地」回報成功**。
這正是本專案反覆抓到的那一類問題的又一個實例。

⚠ 為什麼既有的測試抓不到:`tests/test_win32_layouts.cc` 拿
`LoadKeyboardLayout` 載入的**真佈局**(美式 / 德文 / 法文)去測 ——
而那正好是唯一不會出事的情形。它測的是「有沒有真的問佈局」,
不是「問到的那一份答不答得出來」。

### 修法

`Win32KeyboardOracle` 建構時實地問幾顆一定有字的鍵(A / S / K / 1 / 5)。
問不出來就換一份**真的**佈局來問:先找語言相同的,再找任何一份答得出來的,
最後明著 `LoadKeyboardLayoutW(L"00000409")` 載入美式。

⚠ 換的仍然是「問一份真的佈局」,**不是**退回「`VK_A` 就當 `'a'`」。
後者在 QWERTY 上看起來完全正確,而 Dvorak 使用者打出來的每一個字都是錯的 ——
那正是 `common/keymap.cc` 整個檔案要避免的東西。走到「載入美式」那一步的
Dvorak 使用者會拿到不合他鍵帽的映射,但那是「輸入法完全不能用」與
「按鍵位置不對」之間的選擇,而後者他看得出來、也修得掉(切一次佈局)。

反向與正向都有測試(`test_win32_layouts.cc` 的
`win32_blind_hkl_falls_back_to_a_real_layout` 與
`win32_good_hkl_does_not_fall_back`)—— 佈局本來就好好的時候**不可以**被換掉。

### 並且把兩個症狀解耦

服務改成在 `ActivateEx` 的**背景執行緒**上啟動(不是在宿主的 UI 執行緒上 ——
那會讓切換輸入法卡住)。所以:

* 使用者切到本輸入法幾秒內就會看到系統匣圖示與設定視窗,不必先打字;
* 首次部署(要編譯詞庫,一到數分鐘)提早開始,而不是等到他第一次按鍵;
* 而「打不出字」如果還在,**就只剩按鍵那條路可以查了**。

把兩個症狀解耦,比同時修兩件事重要。

⚠ 提權的宿主仍然刻意不啟動服務(那會產生一支提權的服務,把使用者詞庫
檔案的擁有者換掉)。所以提權視窗裡預期仍然是「沒有輸入法」。

---

## 使用者說「不能用」的時候

**請他執行「開始」功能表裡的「診斷:輸入法為什麼不能用」,把記事本裡跳出來
的那份報告整份貼過來。** 就這樣,不需要再問任何問題。

命令列的等價寫法:

```
"C:\Program Files\RimeQuad\rime_ime_setup.exe" doctor
```

⚠ **不要提權**跑它。提權時看到的 `HKCU` 與具名管道都是**另一個帳號的**,
報告會說「使用者沒有被啟用」「連不上服務」,而那兩句都是假的。
報告開頭會明著寫出它自己是不是提權的。

### 給還在用舊版(沒有 `doctor`)的使用者

`doctor` 是這一輪才有的。手上是更舊的版本時,資訊量最大的單一指令是
**把服務放到前景跑一次**,看它自己說什麼:

```
:: 開「命令提示字元」,不要用系統管理員身分,整行貼上:
"C:\Program Files\RimeQuad\rime_service.exe" > "%USERPROFILE%\rime-service.txt" 2>&1
:: 畫面會停住不動(那是正常的,它在跑)。等 5 分鐘之後按 Ctrl+C,
:: 把 C:\Users\<你的名字>\rime-service.txt 傳回來。
```

那一份會回答:`rs_init` 過不過、資料目錄解析到哪裡、首次部署成不成功、
預熱花了多久、具名管道開起來沒有、候選窗與設定視窗建不建得起來。
也就是「服務這一半」的全部。

⚠ 不要用 `%USERPROFILE%\Desktop`:桌面可能被 OneDrive 重新導向,
那時檔案不會出現在他看得到的地方,而他會以為指令沒有作用。

### 報告裡的九格

| 格 | 它在回答什麼 |
|---|---|
| 1 檔案 | 裝齊了嗎(尤其 `data\shared` —— 少了它每一步都成功而一個候選都沒有) |
| 2 註冊 | HKLM 的 COM 與 TSF 鍵、三份語言設定檔、**TSF 自己列舉得到我們嗎**、HKCU 啟用了嗎 |
| 3 語言設定檔與佈局 | 目前啟用的是不是我們;**這台機器上每一份佈局問不問得出字**(見上面的根因) |
| 4 服務進程 | `rime_service.exe` 在不在、pid、執行檔位置 |
| 5 具名管道 | 連得上嗎;連不上的話卡在**開管道 / 握手 / 建 session** 哪一步、`os_error` 是多少 |
| 6 誰載入了 DLL | 掃描所有看得到的進程,列出載入了 `rime_tsf.dll` 的那些 —— **不必再教使用者裝 Process Explorer** |
| 7 引擎層 | 呼叫 `rime_console.exe` 直接問 librime(**完全不經 TSF、不經管道**)。它打得出「你好」就代表引擎、詞庫、方案都好,問題必定在 TSF 或 IPC 那一側;反過來也一樣 |
| 8 除錯記錄 | 瘦 DLL 在宿主進程裡留下的最後 40 行(含 `ActivateEx 完成:key sink=…` 那一行 —— 它一句話說完這個宿主裡能不能用) |
| 結論 | 有幾格失敗;每個 `[FAIL]` 後面的 `→` 就是接下來要做的事 |

⚠ **量法很重要,報告裡也寫了**:第 6 格與第 8 格要有意義,得先開一個記事本、
`Win + 空白鍵`切到本輸入法、按幾個鍵、**不要關掉記事本**,再跑診斷。
不然「沒有任何進程載入 DLL」是理所當然的 —— 沒有程式正在用它。

### ⚠ 「沒有系統匣圖示」有一半可能不是我們的錯

Windows 11 **預設把新出現的系統匣圖示收進溢位區**(工作列上那個 `^`)。
所以「沒有任何 UI」這句回報裡,系統匣那一半有可能只是被收起來了。

判斷方法:`doctor` 的第 4 格說服務在跑的話,圖示就**一定**被加過
(它在 `WM_CREATE` 裡加,見 `service/settings_window.cc`)—— 那就去點開 `^`,
或到「設定 → 個人化 → 工作列 → 其他系統匣圖示」把它打開。

⚠ 這件事不會被「修掉」:那是 Windows 的預設值,應用程式不該去改它。
能做的是**在使用者問之前就先講**,而那正是 `doctor` 第 4 格那三行註記的用途。

### 手動的分層檢查

要更快分層的話,這三行由粗到細:

```
:: 1. 引擎層(不經 TSF、不經管道)。印得出「你好」就代表引擎與資料都好
"C:\Program Files\RimeQuad\rime_console.exe" "C:\Program Files\RimeQuad\data\shared" "%APPDATA%\RimeQuad" nihao 1 luna_pinyin_tw

:: 2. 註冊層。TSF 列舉得到我們嗎
"C:\Program Files\RimeQuad\rime_ime_setup.exe" check

:: 3. 瘦 DLL 在宿主進程裡發生了什麼
notepad "%LOCALAPPDATA%\RimeQuad\diagnostics\tsf.log"
```

### 除錯記錄

| | |
|---|---|
| 位置 | `%LOCALAPPDATA%\RimeQuad\diagnostics\tsf.log` |
| 關掉 | 環境變數 `RIME_TSF_TRACE=0` |
| 換位置 | `RIME_TSF_TRACE=<完整路徑>`(CI 用這條) |

為什麼是檔案而不是 `OutputDebugString`:這支 DLL 住在**別人的進程**裡
(記事本、瀏覽器、Office),它印到 stdout 沒有人看得到,而
`OutputDebugString` 需要一個偵錯器接著 —— **使用者手上沒有偵錯器**。

記錄的內容:DLL 載入、`DllGetClassObject`、`ActivateEx`(含 clientid 與
**當下的 HKL**)、語言設定檔走了三層退路的哪一層、profile sink 有沒有被呼叫、
語言列按鈕加不加得上、**前五顆按鍵的 vk / scan / keysym / 吃不吃**、
連線失敗的階段與 `os_error`。

紀律(見 `tsf/trace.h` 檔頭):只用 kernel32(相依白名單一個都不加)、
不在按鍵路徑上無上限地做 I/O(每個進程 400 行、按鍵只記前 5 顆)、
寫不進去一律安靜(診斷壞掉不該讓輸入法跟著壞掉)。

隱私:記錄裡有宿主程式的**檔名**(`notepad.exe`)——「在 Edge 裡不行、
在記事本裡可以」是這一類問題裡最有價值的一句話。但**沒有**完整路徑、
沒有視窗標題、**絕對沒有按過的鍵或候選字**;檔案只在本機,
`audit_offline_win.sh` 在原始碼層面守著「沒有任何檔案碰網路 API」。

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
**預設不刪 `%APPDATA%\RimeQuad`** —— 那是使用者的資料。

### 「連我的資料一起刪」

真的不用了的人可以一次清乾淨,不必自己去翻 AppData:解除安裝時會問一次

> 要順便刪除您的詞典與設定嗎?…⚠ 刪除之後**無法復原** —— 重新安裝也救不回來。

⚠ **每一條路徑的預設都是「不刪」**,而且是刻意設計成這樣的:

| 情境 | 行為 |
|---|---|
| 互動解除安裝 | 問一次,**預設按鈕是「否」**(`MB_DEFBUTTON2`) |
| `/SUPPRESSMSGBOXES` | 對話框被壓掉時採用的答案是 `IDNO` —— 不刪 |
| `/VERYSILENT`(靜默) | **不問也不刪** |
| `/VERYSILENT /PURGEUSERDATA` | 刪 —— 唯一會刪的靜默路徑,要明著傳旗標 |

第三列不是潔癖:`windows/verify_installer.sh` 第 9 節斷言「解除安裝後
使用者的詞典還在」,那道關卡守的是**「移除輸入法不會順手毀掉使用者的東西」**。
靜默模式若預設會刪,那道斷言就會開始紅 —— 而最糟的情況是有人為了讓它變綠
去把斷言改掉。

**CI 兩條路都驗**(只驗一條等於沒驗到這個功能):

| 斷言 | 在哪 |
|---|---|
| 不帶旗標的靜默解除安裝 → 使用者的詞典還在而且非空 | §9 |
| `rime_ime_setup.exe purge-user-data` **不帶確認參數**時什麼都不做 | §10a |
| `user-data-path` 說的路徑與實際的使用者目錄一致 | §10b |
| 重裝之後詞典還在 | §10c |
| 帶 `/PURGEUSERDATA` 的靜默解除安裝 → 使用者目錄**整個消失** | §10c |

#### 路徑只有一份

刪除這種不可回復的動作,最怕的不是「刪錯地方」,是**「刪了一個不存在的
地方然後回報成功」**。所以:

* 路徑的唯一決定處是 `winshared/winutil.cc` 的 `RimeUserDataDir()`;
* `rime_service.exe`、`rime_ime_setup.exe`(doctor 與刪除)全部走它;
* **安裝程式也不自己拼** —— 它跑 `rime_ime_setup.exe user-data-path`
  把路徑問出來(見 `.iss` 的 `QueryUserDataDir`),而且是在檔案被刪掉
  **之前**問,因為 `usPostUninstall` 的時候那支工具已經不在了。

產品改名時只要改 `winutil.cc` 裡那一個字串常數。

#### 刪之前的五道檢查

`rime_ime_setup.exe purge-user-data` 在動手之前逐條檢查,任何一條不過就拒絕
並說出是哪一條(`setup/setup_main.cc` 的 `SafeToDelete`):

1. 路徑非空、而且長度合理(`< 12` 字元的東西一定不是它)
2. 路徑裡沒有 `..`
3. 路徑**真的**在 `%APPDATA%` 底下,而且不是 `%APPDATA%` 自己
4. 最後一段正好是我們的資料夾名 —— 擋的是「少接了一段,結果指到
   `%APPDATA%\Microsoft`」這種算錯
5. 遞迴刪除時**不跟著 reparse point(junction / symlink)走**,只刪連結本身 ——
   一個指向 `C:\` 的 junction 會把整台機器帶走

⚠ 而且參數名字刻意又長又白話:`--yes-delete-my-dictionary`。
`--force` / `-y` 那種東西會被人習慣性地帶上,而這是整個產品裡唯一一個
帶錯就救不回來的動作。

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
| `tests/` | 單元測試、真實佈局測試、probe、**TSF 驗證宿主** | 部分 |

服務進程是**什麼時候被啟動的**,這一輪改了,而且那是一個影響很大的改動:

| | 之前 | 現在 |
|---|---|---|
| 啟動時機 | 第一顆按鍵走到 `EnsureReady()` | `ActivateEx`(使用者切到本輸入法的當下),在**背景執行緒**上 |
| 後果 | 按鍵那條路一斷,服務就永遠不會起來 —— 於是同時失去輸入**與全部 UI** | 兩件事解耦:UI 幾秒內出現,首次部署提早開始 |

⚠ 一定要在背景執行緒上做。`ActivateEx` 跑在宿主的 UI 執行緒上,
在那裡 `CreateProcess`(甚至只是開一次登錄檔)都會讓切換輸入法卡一下,
而那是使用者每天做很多次的動作。執行緒活著期間會抓住 DLL 的參考計數,
否則宿主可能在 `DllCanUnloadNow` 回 `S_OK` 之後把 DLL 卸載掉,
而執行緒還在那段已經不存在的程式碼裡。

「服務在不在」的判斷用的是**單一實例的互斥鎖**,不是管道:
服務啟動之後要先跑完 `rs_init` 才開管道,而首次部署那段時間是好幾分鐘。
拿管道當依據會在那幾分鐘裡一直說「不在」,於是每次都再啟動一支 ——
新的那支被互斥鎖擋掉、安靜地以 0 結束,誰都不會報錯。
名字的唯一定義處是 `winshared/winutil.cc` 的 `RimeServiceMutexName()`
(三個呼叫者:服務自己、瘦 DLL、`doctor`)。

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

⚠ `syntax_check_mingw.sh` 現在也涵蓋 `tests/tsf_host_main.cc`。
mingw 的 `msctf.h` 缺 `TF_IPPMF_*` 那組旗標,值補在
`tests/mingw_syntax_shim.h`(照 Windows SDK 抄)。

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

### ⚠ 但「問佈局」本身有一個更前面的前提:那份 HKL 得答得出來

**這一輪的根因就在這裡**(完整說明見開頭的
[「一個根因,兩個症狀」](#一個根因兩個症狀))。

`Oracle()` 拿的是 `GetKeyboardLayout(0)`,而我們的文字服務啟用時,
那個值不保證是一份真的鍵盤佈局:TSF 的文字服務有自己的 HKL(`0xFxxx<langid>`),
IMM32 的 IME 是 `0xExxx<langid>`,兩者對 `ToUnicodeEx` 都是「什麼都不給」。

上面那張表列的三類是「問對了佈局但答案錯」;這一類是「**根本沒有答案**」,
而它的後果更嚴重也更難看出來 —— 不是標點打錯,是**一顆鍵都進不了引擎**,
連帶服務不會被啟動、全部 UI 不會出現。

`Win32KeyboardOracle` 的建構子因此多了一段:實地問幾顆一定有字的鍵,
問不出來就換一份真的佈局(`used_fallback()` / `blind()` 會說出發生了什麼,
而 `ActivateEx` 會把它寫進除錯記錄)。正反兩面的測試在
`test_win32_layouts.cc` 的最後三個 `TEST`。

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
- **`scan_code` 為 0 時要自己從 VK 推**,而且要用**實際在查的那一份佈局**
  (`MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl_)`)。宿主送來的 lParam 通常帶著
  實體掃描碼(與佈局無關,原樣可用),但不是每一個宿主都帶。
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
| **系統真的把 `rime_tsf.dll` 載入宿主進程** | `verify_tsf.sh` + `rime_tsf_host.exe`(含正反兩面) |
| **`ActivateEx` 真的被呼叫** | 同上 |
| **按鍵經 TSF 進來之後映得出非零 keysym** | 同上(這一格就是本輪根因所在) |
| **經由真的 TSF** 打出「你好」(裝好的 DLL + 裝好的服務) | `verify_installer.sh` §6c |
| 佈局問不出字時會換一份真的來問,而佈局好好的時候不會被換掉 | `test_win32_layouts.cc`(正反兩面) |
| **`doctor` 這支診斷工具本身會不會紅** | `verify_installer.sh`(裝之前紅、裝好綠、停掉服務再紅、解除安裝後紅) |
| `doctor` 的引擎層那一格真的跑了 `rime_console` | `verify_installer.sh` §6b(而且斷言它不是安靜地跳過) |
| 瘦 DLL 的落地除錯記錄真的有寫出東西 | `verify_tsf.sh` / `verify_installer.sh` §6c |

### 這一輪從「驗不了」搬到「驗得了」的:**TSF 那一整條**

上一輪這裡寫著「TSF 的 Activate / 組字 / edit session 只有人做得到」。
**那個判斷是錯的。**

`windows/tests/tsf_host_main.cc` 建出來的 `rime_tsf_host.exe` 是一個
**假的文字編輯器**:它建 `ITfThreadMgr`、`Activate`、建一份最小可用的
`ITextStoreACP`、`Push` 進 `ITfDocumentMgr`、`SetFocus`、用
`ITfInputProcessorProfileMgr::ActivateProfile` 啟用我們的語言設定檔,
再用 `ITfKeystrokeMgr::TestKeyDown / KeyDown` 送按鍵,最後看文件裡長出什麼字。

也就是說**它逼系統走完真正的那一條**:

```
登錄檔 → CoCreateInstance → 把 rime_tsf.dll 載入這個進程 → ActivateEx
       → AdviseKeyEventSink → OnTestKeyDown → OnKeyDown → RequestEditSession
       → StartComposition → SetText → EndComposition
```

⚠ 它**不連結** `rime_tsf`。連結了就變成直接呼叫我們自己的函式,
驗到的是另一件事。它只認 CLSID 與 profile GUID,其餘全部交給系統去解析 ——
註冊錯了、`InprocServer32` 指錯了、DLL 載不起來,它都會在該紅的地方紅。

兩種模式:

| 在哪 | 要求 |
|---|---|
| `logic-x64`(四分鐘,不需要 librime) | DLL 被載入、`ActivateEx` 被呼叫、按鍵映得出非零 keysym |
| `install-x64` §6c(用**安裝好**的那一份) | 再加上:按鍵被吃掉、文件裡真的是「你好」 |

反向測試(證明它不是恆真的):

- **註冊之前**跑一次,必須非零結束
- **反註冊之後**再跑一次,必須再度非零結束
- 腳本用 `trap` 保證登錄檔一定被清乾淨 —— 沒有的話,同一台機器上的下一個
  job 會在 `verify_installer.sh` 的第一條斷言(「安裝之前 check 必須紅」)
  失敗,而那個失敗看起來與這裡完全無關

⚠ `ActivateProfile` 的旗標組合是**逐一試過去並把每一種的 HRESULT 都印出來**
的,不是猜一種然後宣布結論。runner 是非互動的工作階段,走不通的時候要看得出
卡在哪 —— 而 `rime_tsf_host` 連不出 `ITfThreadMgr` 時會以結束碼 3 明說
「TSF 在這個工作階段裡不可用」,`verify_tsf.sh` 對那個碼**明著拒絕略過**。

#### ⚠ 做這件事的過程中學到的兩件事(值得記住)

**1. 宿主視窗沒有前景的話,TSF 一顆按鍵都不會交給文字服務 —— 而且不報錯。**

第一版的 `rime_tsf_host` 建了視窗但沒有 `ShowWindow`。結果:
`ActivateEx` 被呼叫、`key sink` 掛上了、語言列按鈕也加上了,
而 `ITfKeystrokeMgr::TestKeyDown` / `KeyDown` **六顆按鍵一顆都沒有到達
`OnTestKeyDown`** —— 兩者都回 `S_OK`、`pfEaten` 都是 `FALSE`。
從呼叫端看,那與「輸入法決定不吃這顆鍵」**完全無法分辨**。

補上 `ShowWindow` + `SetForegroundWindow` 之後,同一份程式碼立刻收到按鍵
(`keysym=0x6E` / `0x69`)。所以那支驗證宿主現在會把
`GetForegroundWindow()`、`ITfThreadMgr::IsThreadFocus()` 與
`ITfThreadMgr::GetFocus()` 全部印出來 —— 少了那幾行,下一次撞到同一件事
還是會去查佈局或連線,而那兩段都是好的。

⚠ 這一課對產品本身也成立:「按鍵沒有到達 `OnTestKeyDown`」與
「到達了但 keysym 是 0」是**兩個完全不同的故障**,要查的地方不同。
除錯記錄與驗證腳本現在分開講這兩件事。

**2. `AdviseKeyEventSink` 的回傳值以前完全沒有人看。**

它失敗的話,`OnTestKeyDown` / `OnKeyDown` 從此不會被呼叫 —— 引擎收不到按鍵、
連線不建立、服務不啟動、全部 UI 不出現,而 `ActivateEx` 照樣回 `S_OK`。
與「佈局問不出字」一模一樣的症狀組合。現在會檢查、會記錄,
而且前景版失敗時會退成非前景版(`fForeground = FALSE`)——
少的是「別的 TIP 也在時的優先權」,遠好過一顆按鍵都收不到。

### 這一輪從「驗不了」搬到「驗得了」的:安裝

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
| **解除安裝後** | **使用者的詞典還在而且非空** —— 唯一一項重裝也補救不回來的失敗 |
| **帶 `/PURGEUSERDATA` 的解除安裝** | 使用者目錄**整個消失**(§10c) |
| `purge-user-data` 不帶確認參數時什麼都不做 | §10a(擋「參數被忽略」) |
| `user-data-path` 說的路徑與實際的使用者目錄一致 | §10b(擋「刪了一個不存在的地方然後回報成功」) |

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

~~1. 切到這個輸入法之後 `ActivateEx` 有沒有被呼叫、sink 有沒有掛上。~~
~~2. 在記事本裡打不打得出字(TSF composition、preedit、commit)。~~
→ 這兩項**這一輪搬走了**,由 `verify_tsf.sh` 與 `verify_installer.sh` §6c 驗。
⚠ 但要誠實:驗到的宿主是我們自己寫的 `rime_tsf_host.exe`,不是記事本。
真實宿主有自己的版面、DPI、UI 執行緒模型,而 `GetTextExt` 在很多宿主上
會失敗 —— 我們的假宿主一定成功。所以「在**真的**記事本裡打得出字」
仍然沒有被驗過,只是它前面那幾格不再是紙上的。

1. **使用者的語言列上到底看不看得到它。** CI 斷言得到「系統接受了這個輸入法」,
   但看不看得到還取決於使用者的語言清單裡有沒有那幾種中文,
   而 runner 上沒有辦法製造那個情境。
2. **候選窗會不會出現**、位置對不對、在高 DPI 與多螢幕下對不對。
   `GetTextExt` 在很多宿主上會失敗或給空矩形(已有退回宿主視窗左上角的路徑,
   但那條路徑也沒被驗過)。
3. **在瀏覽器、Office、UWP／市集 App 裡能不能用。** 那需要
   `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` 等能力類別註冊正確 —— 已經寫了,沒驗過。
   ⚠ 市集 App 跑在 AppContainer 裡,而具名管道與 `Local\` 具名物件的 DACL
   只授權目前使用者 —— **那一格很可能是壞的,而且沒有人驗過。**
4. **提權的視窗上能不能打字**(`GUID_TFCAT_TIPCAP_SECUREMODE`)。
   相關的:DLL 在提權宿主裡**不會**自動啟動服務(那會產生一支提權的服務,
   把使用者的詞庫檔案擁有者換掉)。所以提權視窗裡預期是「沒有輸入法」,
   而不是崩潰 —— **這個預期也沒被驗過。**
5. **首次部署那幾分鐘的行為。** 服務在部署完成前對每一顆按鍵立刻回「沒處理」,
   使用者應該看到的是「輸入法還沒好,打出來是英文」而不是卡住。
6. **「每一顆鍵是不是都真的做了它宣稱的事」。** 這個專案已經抓到四個
   「畫面完全正常、自動化全過」的功能:重輸鍵呼叫的是結束組字而不是清空、
   中英鍵切了模式卻不換佈局、按下後顏色回不來、工具列的 emoji 鍵什麼都不做。
   Windows 端目前**一顆鍵都沒有被人按過**(`rime_tsf_host` 按的是 `nihao1`,
   那只證明基本的組字與選字,不是每一顆功能鍵)。
7. **`ToUnicodeEx` 的死鍵狀態沒有被我們吃掉。** 程式碼帶了「不改動鍵盤狀態」
   的旗標,但那件事只有真的按一次法文的 `^` 再按 `e` 才驗得到。
8. **`ActivateEx` 背景啟動服務那條路,在真的宿主裡的時序。** CI 上驗得到
   它不會崩、不會卡住 `ActivateEx`;但「使用者切過去之後幾秒內看到系統匣圖示」
   這件事本身沒有人看過。
9. **使用者機器上 `GetKeyboardLayout(0)` 到底是什麼。** 本輪的佈局退路是
   對著「它可能不是真的佈局」寫的,而且正反兩面都有測試 ——
   但**使用者那台機器上實際的值**要等他跑一次 `doctor`(報告第 3 節與
   第 8 節的除錯記錄)才知道。這是本輪最想拿到的一個數字。

### 上一輪新增、這一輪部分搬走的

**11 與 12 這一輪不再是紙上的**,但方式與原本想的不一樣:它們不是被
「驗成綠的」,而是**被記下來了** —— 瘦 DLL 現在會把
「語言設定檔:第 N 層 …」與「profile sink:啟用 langid=…」寫進除錯記錄,
`verify_tsf.sh` 會把整份記錄印出來,`doctor` 的第 8 節也會。
所以下一次使用者回報時,我們看得到走的是哪一層 ——
而在這之前那是一個沒有辦法問出來的問題。

⚠ 差別要講清楚:**「有記錄」不等於「驗過了」。** 目前 CI 只斷言記錄裡
有那幾行,沒有斷言它們的值是對的(runner 上的語言環境與使用者不同,
斷言一個值等於把 runner 的組態當成規格)。

11. `ITfInputProcessorProfileMgr::GetActiveProfile` 在 `ActivateEx` 當下回傳的
    是不是我們 —— **三層退路走了哪一層現在記錄得到**,但哪一層才是對的
    仍然要看使用者的機器。
12. `ITfInputProcessorProfileActivationSink` 會不會被呼叫 ——
    **被呼叫時會留下一行**,但「使用者從繁體切到簡體之後打出來真的變了」
    仍然只有人驗得到。

### 這一輪新增、而且**一項都沒有被驗過**的

⚠ 這一整段是這一輪最需要人去按一遍的地方。CI 驗到的是「判斷邏輯對」與
「編得起來」,**沒有一個像素、沒有一次點擊被驗證過**。

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
21. **解除安裝那個「要不要順便刪資料」對話框長什麼樣。** CI 走的是靜默路徑
    (旗標),所以文案、預設按鈕、以及使用者會不會看漏 ——
    **一次都沒有被人看過**。這一項比看起來重要:那是整個產品裡唯一一個
    不可回復的動作,而它的安全性有一半靠「預設按鈕是否」這件視覺事實。
22. **`doctor` 報告在使用者眼裡讀不讀得懂。** 每一格的 `[FAIL]` 後面都接了
    「接下來做什麼」,但那些句子沒有給任何一個真的使用者看過。
    這一項比看起來重要:一份看不懂的診斷報告等於沒有診斷。
23. **開始功能表那兩個捷徑按下去會怎樣。** 「診斷」那一個會開一個主控台視窗、
    跑一到三分鐘(引擎層那一格要等 librime)、然後跳出記事本。
    **那段等待中間沒有任何進度提示**,而使用者很可能以為它當掉了。

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
windows/verify_ime.sh                             # 經由具名管道的端到端(**繞過 TSF**)
windows/verify_tsf.sh --bin third_party/build/windows-x64/ime/bin
                                                  # 真的經過 TSF(需提權;會動登錄檔)
windows/check_binaries.sh third_party/build/windows-x64/ime/bin
third_party/build/windows-x64/ime/bin/rime_tests.exe
windows/make_installer.sh                         # → installer/RimeQuad-Setup-x64.exe
```

安裝與驗證(需要系統管理員權限):

```bash
windows/verify_installer.sh \
  --setup third_party/build/windows-x64/installer/RimeQuad-Setup-x64.exe \
  --probe third_party/build/windows-x64/ime/bin/rime_probe.exe \
  --tool  third_party/build/windows-x64/ime/bin/rime_ime_setup.exe \
  --host  third_party/build/windows-x64/ime/bin/rime_tsf_host.exe
```

`--host` 是可選的;給了才會跑 §6c(用**裝好的**那份 DLL 經由真的 TSF 打字)。

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
rime_ime_setup.exe doctor          一頁式自我診斷(這一支是給**使用者**的)
rime_ime_setup.exe doctor --report 另存一份並用記事本打開
rime_ime_setup.exe user-data-path  印出使用者資料目錄(安裝程式問它,不自己拼)
rime_ime_setup.exe purge-user-data --yes-delete-my-dictionary
                                   刪掉使用者的詞典與設定。**無法復原**;
                                   沒有帶那個參數就什麼都不做
```

⚠ `doctor` 的結束碼是「失敗的格數是不是 0」,所以它可以被斷言 ——
`verify_installer.sh` 對它有四條正反斷言(裝之前紅、裝好綠、
停掉服務再紅、解除安裝後紅)。一支只會印綠字的診斷工具比沒有更糟。

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
