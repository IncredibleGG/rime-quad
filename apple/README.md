# macOS 端

LuminaKey 的 macOS 輸入法:`IMKServer` + `IMKInputController` + 自繪候選窗,
外加一個**內嵌的視覺化設定介面**。

```
apple/
  LuminaKey/
    Package.swift              LuminaKeyKit(純邏輯層)的 SwiftPM 宣告
    Sources/LuminaKeyKit/      ⭐ 沒有 AppKit、沒有 librime、有單元測試
    Tests/LuminaKeyKitTests/   下限由 run_kit_tests.sh 的 MIN_TESTS 守住
    AppSources/                AppKit + InputMethodKit + librime 綁定
    SettingsSources/           設定視窗(AppKit,沒有自動化)
    Resources/                 Info.plist 樣板、.lproj、圖示產生器
  scripts/
    build_macos.sh             librime + 5 個依賴 + librime-lua + rime_console
    build_app.sh               → apple/build/LuminaKey.app(含設定介面)
    build_pkg.sh               → apple/build/dist/LuminaKey-*.pkg
    verify_names.py            名字一致性(不編譯、不需要 macOS,CI 第一關)
    run_kit_tests.sh           單元測試 + 變異測試
    verify_app_bundle.sh       .app 結構與 IMKit 宣告
    verify_pkg.sh              真的裝一次,並問系統認不認
    verify_single_egress.sh    整個 app 只有一個地方連得上網
    verify_user_dict.sh        使用者詞庫真的會改變輸出
    verify_console.sh          核心層「打得出字」的斷言
    verify_data.sh             執行期資料齊全
    package_core.sh            核心層產物打包
    tis_probe.swift            問 Text Input Services 認不認得這個輸入法
```

---

## 1. 為什麼分成 LuminaKeyKit 與 AppSources / SettingsSources

**因為 CI 驗不了 UI。** `macos-latest` runner 沒有登入的圖形工作階段,
系統不會從 `~/Library/Input Methods` 載入輸入法,NSPanel 與 NSWindow
建得起來也不會出現在畫面上。

所以紀律是:**凡是算得出來的都不要留在 AppKit 那一側。**

| 在 `LuminaKeyKit`(有測試) | 在 AppKit 那一側(沒有自動化) |
|---|---|
| RTS YAML 讀取、主題綁定、繼承合併、診斷 | 主題檔案的實際搜尋路徑 |
| `NSEvent` → keysym 的映射規則、修飾鍵狀態機 | 從 `NSEvent` 取值 |
| 候選窗排版(欄寬、落點、換行、超出處置) | 量字、畫圖、擺窗 |
| 上屏政策(`menu.count` 那三條) | 呼叫 `IMKTextInput` |
| 狀態列每一項顯示成什麼 | 畫狀態列 |
| **輸入模式 → 方案 → 簡繁的判定** | 從 IMKit 拿輸入模式字串 |
| **設定的讀寫、遷移、預設值** | 畫設定視窗 |
| **設定介面的資訊架構與每一句白話** | 把它排成控制項 |
| **市集索引解析、相依展開、壓縮檔守門、方案安裝與回滾** | 檔案選擇器 |
| **連網守門的判斷、連網紀錄的編解碼** | (連 URLSession 那幾行也在 Kit 裡,見 §5) |
| **跨行程訊息的編解碼與逾時狀態機** | 真的發出通知的那兩行 |

AppKit 那一側剩下的東西都短到可以讀完。這不是潔癖,是唯一能在
「編得出來 ≠ 能用」這個前提下還保住品質的分法。

---

## 2. 安裝

**用 `.pkg`。** CI 的 `rime-macos-installer` artifact 裡那一個,雙擊就好。

> ⚠ **不要把 `.app` 拖進 `/Applications`。** 那是任何人拿到一個 `.app` 的
> 正確直覺,而結果是**靜默失敗**:系統會把它當一般 app 登錄、行程也起得來,
> 但它永遠不會出現在「輸入來源」清單裡,沒有任何錯誤訊息。
> 這是真機回報過的事。`.pkg` 存在就是為了讓這件事不可能發生。

裝完之後(這一步只有人做得到):

1. 系統設定 › 鍵盤 › 文字輸入 › 輸入來源 › 編輯…
2. 左下角 **+** → 中文(繁體)或中文(簡體)→ **LuminaKey** → 加入
3. **Control + Space** 切過去

**設定在選單列右上角的輸入法圖示裡**:設定 / 方案切換 / 重新整理字詞 / 關於。

CI 產出的 `.pkg` 仍然是 **CI 產物,不是可散布的版本**:
只有跑 CI 的那一個架構、只有 ad-hoc 簽章(散布需要 Developer ID + 公證)、
librime 的靜態庫沒有指定部署目標,實際最低系統版本可能高於宣告值。

使用者資料在 `~/Library/Application Support/LuminaKey/`。
**刻意不用 `~/Library/Rime`** —— 那是 Squirrel 的目錄,兩個輸入法共用同一份
使用者詞典與 `installation.yaml` 會互相踩,而使用者完全看不出是誰改的。

### 從 RimeQuad 升上來的人會發生什麼

產品改名之前這個目錄叫 `~/Library/Application Support/RimeQuad`。
**bundle id 也一起改了,所以新版不會覆蓋舊版** —— 舊的 `RimeQuad.app` 還留在
`~/Library/Input Methods/`,而且可能還在跑。第一次啟動時
`LuminaKeyKit/LegacyDataMigration.swift` 會做一次性的搬遷:

| 會搬過去 | 不搬,而且原因不只是「懶得搬」 |
|---|---|
| `custom_phrase.txt`(你自己加的詞) | `*.userdb`(librime 學到的字頻)—— 那是 LevelDB,舊版**可能正開著它**,複製一份開著的 LevelDB 會得到一份壞掉但看起來正常的資料庫。而 librime 部署失敗時 `rs_last_error()` 是空字串,你只會看到「部署失敗」四個字 |
| `settings.json` | `installation.yaml`(安裝 id)、`user.yaml` —— 複製過去等於兩份安裝共用同一個 id |
| 裝過的方案與市集的安裝紀錄(檔名一併換成 `luminakey-store.json`) | `build/`(部署快取,下次部署自己重建) |
| 你放進 `themes/` 的自訂主題、`<schema>.custom.yaml` | |

**是複製,不是搬移:舊目錄原封不動留著。** 所以就算這段整個出錯,最壞的結果
是你得到一個乾淨的新目錄,舊版仍然照常運作;真的要救,手動複製也還來得及。
搬遷只在「新目錄還不存在」時做一次,失敗**不會**擋住啟動。

真正會失去的只有「打過的字的頻率」,它本來就會自己重新長回來。
不要的話,把 `~/Library/Application Support/RimeQuad` 刪掉再第一次啟動就好。

---

## 3. ⚠ 沒有被自動化驗到的東西

**這一節是本文件最重要的一節。** CI 全綠**不代表能用**。

### CI 驗得到的

- 200 餘項純邏輯單元測試 + 11 個變異測試(植入違規,斷言**對應的那一組**會紅)
- `rime_console` 不經 UI 直接驅動 librime:`nihao → 你好`、`su3cl3 → 你好`
- 四個方案都部署成功、執行期資料齊全
- `.app` 編得起來、bundle 結構完整、`Info.plist` 的 IMKit 宣告齊全
- **IMKit 找不找得到 controller 類別** —— 由二進位自己照 IMKit 的作法
  `NSClassFromString(Info.plist 裡那個字串)` 查一次(`SelfCheck.swift`)。
  ⚠ 這裡**不是** grep 符號表:實測 `swiftc -O` 單模組編譯把
  `_OBJC_CLASS_$_…` 設成 **local** 符號,`nm -g` 看不到它,而 `.app` 是好的 ——
  也就是那條斷言會**在該綠的時候紅**。腳本裡只留一個不判定成敗的 `nm` 診斷。
- bundle 驗證的 5 個反向變異,每一個都斷言紅的是**它自己那一條**
  (含「多打包了 `core/layouts/`」這條反過來的斷言)
- 二進位真的連上 InputMethodKit,且 librime 是**靜態**連結
- `LuminaKey --self-check`:向真的 librime 問 keysym 表裡每一個名稱
- **`.pkg` 真的裝到 `~/Library/Input Methods`**,圖示解得開,三種語言的
  顯示名都查得到(這兩項對應真機回報的「顯示成 bundle id、圖示是空白方框」)
- **整個 app 只有一個檔案碰得到網路**,且預設拒絕(反向測試證明它會紅)
- ⭐ **系統真的認得這個輸入法。** 裝完之後用 `TISCreateInputSourceList` 問系統,
  斷言三個 id 都在,而且**在地化名稱不等於 id**
  (`FOUND …Hans — LuminaKey (Simplified)`)。這一條直接驗到真機回報的兩個缺陷:
  「輸入法沒出現在清單裡」與「清單顯示的是 bundle id」。
  ⚠ 我們原本以為這在 runner 上驗不了(TIS 應該只掃有登入的圖形工作階段),
  所以第一版把它寫成「不算失敗」的參考資訊。**實測結果相反,查得到。**
  現在它是硬關卡。教訓:**「驗不了」要試過才算數。**

### CI 驗到了、而且**是紅的**(所以那個功能沒有上架)

- **使用者詞庫。** `verify_user_dict.sh` 證明:我們寫出的 `custom_phrase.txt`
  與掛載檔格式看起來對,但加了詞之後 librime 的候選裡**沒有那個詞**。
  整條路徑(解析、合併、掛載、UI、11 項單元測試)都寫好了,
  但「我的詞庫」那一頁**刻意沒有上架** —— 一頁按鈕按得下去、加完詞還看得到,
  而回去打字什麼都不會發生,比沒有這一頁更糟。
  那一關在 CI 上是 `continue-on-error`,每次都跑,**它變綠的那一天就是上架的日子**。

### CI **驗不到**的(只有人在真 Mac 上跑才驗得到)

1. **圖示在真的清單裡長什麼樣。** 系統認得這個輸入法、圖示檔解得開、
   在地化名稱查得到 —— 這三件 CI 都驗了(見上)。但「那顆圖示放大之後
   好不好看、在深色選單列上看不看得清楚」沒有辦法自動判斷。
   → 驗收時請看一眼「輸入來源」的 + 清單與選單列。
2. **設定視窗長什麼樣、每一顆按鈕按下去做了什麼。** 這一輪新增的六頁
   **一頁都沒有被自動化開啟過**。資訊架構、每一項的白話、欄位覆蓋率
   都有單元測試,但「畫出來好不好看、點下去有沒有反應」零驗證。
   → 驗收時請**逐頁逐鍵**按過去。
3. **跨行程的那條線。** 設定介面按「重新整理字詞」是送
   `DistributedNotification` 給輸入法本體,由它部署並回報進度。
   編解碼與逾時狀態機有測試,**真的送出去有沒有到對面完全沒驗過**。
   已知的三種結果都有對應的訊息(成功 / 失敗 / 對方沒回應),
   但沒有人看過它們出現在畫面上。
4. **方案市集的下載與安裝。** 索引解析、相依展開、sha256 比對、壓縮檔守門、
   安裝與回滾全部有測試,但**沒有真的下載過一次**(CI 上不連外網)。
   → 驗收時請至少下載一個方案,並確認離線開關關著時是說明卡而不是紅字。
5. **候選窗有沒有出現、長什麼樣。** 排版數字全部有測試,但顏色、
   多螢幕與 Retina 上的位置一項都沒驗。
6. **在真的 app 裡打不打得出字。** 已知會出事的類型:Electron(Slack / VS Code)、
   Java(JetBrains)、終端機、以及任何自繪文字框的 app。**這幾類必須各試一個。**
7. **`attributes(forCharacterIndex:lineHeightRectangle:)` 回報的插入點。**
   有些宿主回報 `NSRect.zero`,本實作會退回滑鼠位置 —— 那條退路從沒被真的走過。
8. **實體鍵盤佈局。** 映射邏輯有 Dvorak / AZERTY 的單元測試,但那是**餵假資料**。
9. **修飾鍵。** 「輕點 Shift 切中英」靠的是 `flagsChanged` 的 release 事件。
   狀態機有測試,真的按下去會不會動沒驗過。⌘C / ⌘V 有沒有被吃掉也沒驗過。
10. **librime 內建的方案選單(switcher)。** `Control+grave` 與 `F4` 送得進
    librime(它們沒有被 Command 那條規則擋掉,keysym 表也有 F4),
    但**沒有人按過**。這是「看得到但摸不到」最典型的候選。
11. **VoiceOver。** 候選項有 `accessibilityLabel` 也有 `accessibilityPerformPress`,
    但沒有任何自動化。Android 端已經證實注入式的點擊驗不了朗讀器。
12. **首次部署的等待。** Android 實測 7–12 秒。桌面應該更快,但那段時間
    使用者按鍵會發生什麼(現在是被 `hasSession` 擋掉)沒有真的看過。
13. **深淺色跟隨。** 監聽的是 `AppleInterfaceThemeChangedNotification`,
    切換時候選窗與設定視窗會不會即時換沒驗過。

### 驗收時請這樣做

不要只問「打不打得出字」,要問「**每一顆鍵、每一個可點的東西,是不是都真的做了
它宣稱的事**」。這是本專案抓到最多真 bug 的一類 —— 畫面完全正常、自動化全過。

---

## 4. 三個必須一起改的名字

`LuminaKeyInputController` 同時出現在:

1. `AppSources/LuminaKeyInputController.swift` 的 `@objc(LuminaKeyInputController)`
2. `Resources/Info.plist` 的 `InputMethodServerControllerClass`
3. `scripts/verify_app_bundle.sh` 的斷言

少了第 1 個,Swift 會把類別名 mangle 成 `_TtC9LuminaKey24LuminaKeyInputController`,
IMKit 的 `NSClassFromString` 找不到它 —— 症狀是**輸入法裝得起來、選單裡看得到、
但一個字都打不出來,而且完全沒有錯誤訊息**。

**輸入模式 id 同樣是一組必須一致的名字**(這一輪新增):

1. `Resources/Info.plist` 的 `tsInputModeListKey` 的鍵與 `TISInputSourceID`
2. `Resources/<lang>.lproj/InfoPlist.strings` 的鍵
3. `LuminaKeyKit/InputModeBinding.swift` 的 `hantSuffix` / `hansSuffix`

第 2 項少了 → 輸入來源清單顯示的是 `org.luminakey.inputmethod.LuminaKey.Hans`
這串 id(真機回報過);第 3 項對不上 → 選了簡體卻打出繁體字(也回報過)。

### macOS 端的識別碼一覽(規範值,四端一致的部分見 `docs/decisions/product-name.md`)

| 項目 | 值 |
|---|---|
| bundle id | `org.luminakey.inputmethod.LuminaKey` |
| 設定 app 的 bundle id | `org.luminakey.inputmethod.LuminaKey.Settings`(`.Settings` 後綴是 `main.swift` 的分岔條件) |
| `InputMethodConnectionName` | `org.luminakey.inputmethod.LuminaKey_Connection` |
| `TISInputSourceID` | `org.luminakey.inputmethod.LuminaKey.Hant` / `.Hans`(**必須以 bundle id 為前綴**) |
| `.pkg` 識別碼 | `org.luminakey.inputmethod.LuminaKey.pkg` |
| 使用者資料目錄 | `~/Library/Application Support/LuminaKey` |
| 顯示名 | en `LuminaKey` / zh-Hant `LuminaKey 輸入法` / zh-Hans `LuminaKey 输入法` |

### ⚠ 「Rime」不是舊名字,不要順手取代掉

底層引擎就叫 **librime**,講引擎的時候「Rime」是**正確的**:
`RimeEngine.swift`、`rime_shell.h`、`rime_console`、`~/Library/Rime`(Squirrel 的目錄)、
`docs/` 裡講 RIME 方案格式的地方 —— 這些都不該被改成 LuminaKey。
另外兩個也不動:R2 的路徑仍是 `rime/…`(下載頁與應用內升級指著它),
GitHub repo 名仍是 `rime-quad`。

---

## 5. 離線:一行 grep 就該看得完

macOS 的一般 app 不需要宣告網路權限就連得上網,所以我們**沒有辦法**用
權限清單證明什麼。證明方式只剩「你自己查」:

```bash
grep -rnE 'URLSession|NWConnection|CFSocket' apple/LuminaKey --include='*.swift'
```

結果**必須**只出現在 `Sources/LuminaKeyKit/NetworkGate.swift`。
`scripts/verify_single_egress.sh` 把它變成 CI 關卡,而且它自己有反向測試
(植入一行假的 `URLSession`,斷言必須變紅)。

政策**預設拒絕**;連網紀錄只記真的發生的連線,**被開關擋下來的嘗試不記** ——
「開關從沒開過 → 紀錄是空的」這句話必須成立,那正是使用者驗證我們的方式。

---

## 6. 為什麼設定介面是另一個 .app

`LuminaKey.app/Contents/Resources/LuminaKeySettings.app`,**同一份執行檔**,
`main.swift` 依 bundle id 分岔。

理由:輸入法本體的 Info.plist 是 `LSBackgroundOnly`,那種行程照定義
**不能被帶到前景**,設定視窗裡的文字框拿不到鍵盤焦點 ——
而設定裡到處都是文字框(市集的索引位址、詞庫的加詞欄)。
改成 `LSUIElement` 可以解決,但那會動到
一個已經在使用者機器上跑起來的東西,風險不對稱。

分開之後,**只有輸入法本體碰 librime**:兩個行程同時寫同一個使用者目錄
會弄壞詞庫,而且沒有錯誤訊息。設定介面負責改檔案(純檔案操作,沒有併發問題),
改完經 `DistributedNotificationCenter` 請對方部署,對方回報進度與結果。
協定在 `LuminaKeyKit/IPC.swift`,編解碼與逾時狀態機是純邏輯、有測試。

**逾時分成兩段**,因為兩者的意思完全不同,而使用者需要的下一步也不同:
* 4 秒內連第一則回覆都沒有 → 「請先在系統設定把 LuminaKey 選成輸入來源」
* 接上了但進度停住 90 秒 → 「請重新啟動輸入法」

用同一個「逾時」訊息會讓第一種情況的使用者去做完全沒用的事。

---

## 7. 桌面端消費什麼

| `core/` 底下 | 用不用 |
|---|---|
| `core/themes/*.yaml` 的 `candidates` / `preedit` / `status_bar` / `typography` / `palette` / `metrics` / `accessibility` | **要用** |
| `core/themes/*.yaml` 的 `keyboard` / `feedback` / `candidates.bar` | 不用(規範 §1.1 的形態分野) |
| `core/layouts/*.yaml` | **完全不用**,也不進 bundle |
| `core/data/` | **要用**,librime 的執行期資料 |

⚠ **「不用」不等於「可以刪」。** 使用者的自訂佈局是**行動端**資料,
桌面端**必須原樣搬運、不得解析、不得清理**,否則跨裝置同步會把使用者
在手機上調好的鍵位洗掉。⚠ 那個檔案的名字由 Android 端決定(改名前是
`rimequad-layouts.json`),桌面端**不該**把它寫死在任何地方 —— 現在也沒有。

設定模型(有哪些設定項、存在哪裡、輸入模式↔方案↔簡繁的優先順序、
使用者詞庫格式)在 `docs/settings-model.md`,四端共用。
