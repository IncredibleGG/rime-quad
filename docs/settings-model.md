# 設定模型（四端共用）

**這份文件存在的理由只有一個:防止四端各長一套設定。**

它**不是**第二份主題規範。主題規範(`docs/theme-format.md`)描述的是
「一份 YAML 檔長什麼樣」,那需要嚴謹到另一個人只讀規範就能寫出一致的解析器。
這一份描述的是「有哪些設定項、叫什麼、預設是什麼、存在哪裡」——
刻意保持輕量,能回答「Windows 端該不該做這一項、做出來要存哪裡」就夠了。

基準是 **Android 端的現況**(它已經是可用的產品),桌面端只做增刪,不重新發明。

| | |
|---|---|
| 維護者 | 誰先實作誰補,**只加不刪別人的段落** |
| 目前實作 | Android(部分)、macOS(全部) |
| 未實作 | Windows、iOS |

---

## 1. 資訊架構:桌面端六頁(+ 一頁待驗證)

Android 端目前是:**鍵盤 / 外觀 / 手感 / 文字 / 連網 / 方案市集 / 進階**。

桌面端的對照:

| 頁 | id | Android | 桌面 | 說明 |
|---|---|---|---|---|
| 輸入方案 | `schemas` | 叫「鍵盤」 | ✅ | 桌面沒有軟鍵盤可選,要選的是**方案** |
| 外觀 | `appearance` | ✅ | ✅ | 桌面只管候選窗 |
| 手感 | `feel` | ✅ | ❌ **整頁拿掉** | 震動、按鍵音、長按延遲都是軟鍵盤專屬 |
| 文字 | `text` | ✅ | ✅ | 簡繁、標點、全半形 |
| 自己加的詞 | `dictionary` | ❌(task #39) | ✅ | 格式已用真的 librime 驗過,見 §5 |
| 方案市集 | `store` | ✅ | ✅ | |
| 連網 | `network` | ✅ | ✅ | 開關 + 連網紀錄 |
| 進階 | `advanced` | ✅ | ✅ | 重新部署、匯入、重設、診斷 |

**做不到的功能不要畫出來。** 桌面端沒有「手感」可調,就不要留一頁空的
或一頁全部是灰的控制項 —— 那比沒有這一頁更難理解。

### 兩條介面紀律(規範性)

1. **每一個設定項都必須有一句白話,說它會改變什麼。**
   使用者是全世界的麻瓜,不是 RIME 老手。原話:「你這個第一屏 99% 的人看不懂」。
2. **不得把 YAML 欄位名搬到畫面上。**
   `schema_list`、`page_size`、`simplification`、`ascii_punct` 一個都不准出現。

macOS 端把這兩條做成**編譯期與 CI 的斷言**(`SettingsCatalog` 的 `blurb`
不是 Optional;`SettingsCatalogTests` 掃描禁用字並確認每個欄位都出現在某一頁)。
其他端**建議**照做 —— 這一類規則靠 code review 記住一定會漏。

⚠ **不要做「編輯 YAML 的文字框」然後叫它視覺化編輯。** 那是把問題丟回給使用者。

---

## 2. 設定存在哪裡:三個層級

這是四端最需要對齊的一件事。同一個設定放錯層級,跨端同步就會互相覆蓋。

| 層級 | 存放 | 誰讀 | 跨端 |
|---|---|---|---|
| **A. RIME 設定** | `<user>/default.custom.yaml` 的 `patch:` | librime 自己 | ✅ 完全通用,換別的 RIME 前端也有效 |
| **B. 應用偏好** | 各端自己(macOS `settings.json`、Android DataStore) | 只有 UI 與渲染層 | ⚠ 鍵名共用,值可同步,但檔案格式各端自訂 |
| **C. session 選項** | 不落地 | 每次建 session 時 `rs_set_option()` | ✅ 由 B 推導出來 |

**判準:librime 自己會讀的東西一律放 A。** 放 B 的話,使用者用別的前端
打開同一個使用者目錄會看到完全不同的行為,而他不知道為什麼。

### A 層目前用到的鍵

| YAML 路徑 | 型別 | 意義 |
|---|---|---|
| `patch` → `schema_list` | list of `{schema: <id>}` | 已啟用的方案,**順序即切換順序,第一個是預設** |
| `patch` → `menu/page_size` | int 3–10 | 一頁幾個候選 |

⚠ **A 層一律用「文字外科手術」寫,不可以「解析再重新輸出」。**
`default.custom.yaml` 常常有使用者自己寫的按鍵綁定與註解,重新輸出會把它們洗掉,
而他做的只是「裝一個方案」。四端都必須:用 YAML 解析器讀,只改要改的那幾行,
**保留行尾註解**。新增項目的縮排統一為**四個空格**(兩端輪流寫時 diff 才不會整片變動)。

⚠ **改 A 層之後必須重新部署才會生效**,而且要先做快照:失敗時整份還原,
不是「套用反向的編輯」——反向編輯的前提是外科手術本身沒有 bug,
那正是出事當下最不該假設的事。

---

## 3. 設定項總表

`平台` 欄:`全` = 四端都該有;`桌面` = macOS/Windows;`行動` = Android/iOS。

### 輸入方案

| id | 型別 | 預設 | 層級 | 平台 |
|---|---|---|---|---|
| `schemas.list` | list\<string\> | 隨附的四個 | A | 全 |
| `schemas.followInputMode` | bool | `true` | B | 桌面 |
| `schemas.pinnedHant` | string? | `null` | B | 桌面 |
| `schemas.pinnedHans` | string? | `null` | B | 桌面 |
| `schemas.pinnedGlobal` | string? | `null` | B | 全 |

`followInputMode` 與兩個 `pinned*` 是桌面專屬,因為只有桌面的作業系統
會替使用者宣告「我現在要打簡體/繁體」(macOS 的輸入模式、Windows 的 langid)。
行動端沒有這個訊號,只有 `pinnedGlobal`。

### 外觀

| id | 型別 | 預設 | 層級 | 平台 |
|---|---|---|---|---|
| `appearance.themeFamily` | string? | `null`(= 預設主題) | B | 全 |
| `appearance.appearance` | enum `followSystem`\|`light`\|`dark` | `followSystem` | B | 全 |
| `appearance.candidateScale` | enum `small`\|`medium`\|`large`\|`extraLarge` | `medium` | B | 全 |
| `appearance.candidateCount` | int 3–10 | 方案的預設 | **A** | 全 |
| `appearance.orientation` | enum `followTheme`\|`horizontal`\|`vertical` | `followTheme` | B | 桌面 |
| `appearance.showLabels` | enum `followTheme`\|`on`\|`off` | `followTheme` | B | 全 |
| `appearance.showStatusBar` | enum `followTheme`\|`on`\|`off` | `followTheme` | B | 桌面 |
| `appearance.keyboardHeight` | — | — | B | **行動** |

⚠ **`themeFamily` 存的是家族 id(不含 `-light`/`-dark` 後綴)**,不是具體那一份。
深淺是**另一個獨立的控制項**;把深淺揉進配色名字裡會讓使用者以為有兩個地方
可以決定同一件事。

⚠ **`candidateCount` 在 A 層,不在 B 層。** 主題規範 §8.6.7.1 明訂主題
**不得**改變一頁有幾個候選(改了會讓序號標籤與使用者按的數字鍵對不上),
所以它只能是 librime 的 `menu/page_size`。

⚠ **`followTheme` 是第三態,不是 `false`。** 只有兩態的話,使用者
按過一次之後就回不到「讓主題決定」了。這一條對每一個 `*Pref` 都成立。

### 文字

| id | 型別 | 預設 | 層級 | 平台 |
|---|---|---|---|---|
| `text.variant` | enum `followInputMode`\|`traditional`\|`simplified` | `followInputMode` | B → C | 全 |
| `text.punctuation` | enum `followSchema`\|`full`\|`half` | `followSchema` | B → C | 全 |
| `text.shape` | enum `followSchema`\|`halfShape`\|`fullShape` | `followSchema` | B → C | 全 |

對應的 session 選項:`simplification` / `ascii_punct` / `full_shape`。

⚠ **`followSchema` = 完全不呼叫 `rs_set_option()`。**
設成 `false` 不是同一件事:很多方案根本沒有那個開關,而有些方案的預設是 true。
無條件設 false 會讓「跟著方案」變成「一律西文標點」,而使用者選的是不干預。

### 我的詞庫

見 §5。

### 連網

| id | 型別 | 預設 | 層級 | 平台 |
|---|---|---|---|---|
| `network.enabled` | bool | **`false`** | B | 全 |
| `store.indexUrl` | string? | `null`(= 內建位址) | B | 全 |

⚠ **預設 `false` 是這個專案的定位,不是一個可有可無的預設值。**
而且連網守門必須 **fail-closed**:讀不到設定、行程正在收尾、政策自己爆掉,
一律視為關閉。詳見 `android/.../net/NetworkGate.kt` 與
`apple/.../LuminaKeyKit/NetworkGate.swift` 的檔頭 —— 兩者是同一份設計。

三條共用紀律:
1. **單一連網出口。** 整個專案只有一個檔案碰得到網路 API,而且驗證方式是一行 grep。
2. **預設拒絕。** 安裝政策那一行被誤刪時,行為是完全離線而不是完全開放。
3. **連網紀錄只記真的發生的連線。** 被開關擋下來的嘗試**不記**——
   「開關從沒開過 → 紀錄是空的」這句話必須成立,那正是使用者驗證我們的方式。
   紀錄只存主機名與用途標籤,**不存路徑、不存查詢字串、不存任何輸入內容**。

### 進階

| id | 型別 | 預設 | 層級 | 平台 |
|---|---|---|---|---|
| `advanced.language` | enum `system`\|`zh-Hant`\|`zh-Hans`\|`en` | `system` | B | 全 |
| `advanced.redeploy` | 動作 | — | — | 全 |
| `advanced.import` | 動作 | — | — | 全 |
| `advanced.reset` | 動作 | — | — | 全 |
| `advanced.diagnostics` | 唯讀文字 | — | — | 全 |

⚠ **「重新部署」必須有進度與結果回饋。** `rs_deploy()` 是非同步的,
直接呼叫會立刻返回,畫面上什麼都不會發生,而背景其實跑了十幾秒
(Android 實測:模擬器 7.2 秒、S24U 首次 12.5 秒)。使用者只能猜它成功了沒 ——
Android 端真機回報過這個 bug,原話是「按了沒反應」。
**librime 不提供百分比**,唯一誠實的進度是**經過的秒數**。

⚠ **成功與失敗走不同的通道。** 成功 → 短暫提示,自己消失;
失敗 → 停在對話框上不自動關(訊息裡有使用者必須採取的行動)。
成功之後彈一個要按「知道了」的對話框,等於在他已經達成目的之後多收一次過路費。

⚠ **「全部回復預設」不歸零「已經發生過的事實」**(看過首次說明、走完引導)。
那些不是設定;清掉的話使用者按一次「設定歸零」就會被丟回引導頁。

⚠ **診斷文字永遠是英文。** 它不是介面文字,是一份要被貼進 issue 的回報載荷。
翻譯它會讓收到的 issue 有三種語言的欄位名,grep 不到也對不起來。
畫面上要先用當地語言說一句「這些數字是給我們看的,你不用懂」。

---

## 4. 輸入模式 ↔ 方案 ↔ 簡繁(規範性)

**這一節是本文件最重要的一節,因為四端已經各錯了一次。**

* macOS 註冊了 `.Hant` / `.Hans` 兩個輸入模式,但兩個都載入繁體方案 ——
  使用者選了「簡體」,打 `hao le` 得到「號」。
* Windows 只註冊了 `0x0404`(zh-Hant-TW)一個 langid,連選都沒得選。

畫面完全正常、自動化全過,錯的只有「打出來是哪一種字」。

### 4.1 三個東西的關係

| | 是什麼 | 誰決定 |
|---|---|---|
| **輸入模式** | 作業系統層級的「我現在要打哪一種中文」 | 使用者在系統設定裡選 |
| **方案** | 怎麼把按鍵變成字(拼音、注音、倉頡…) | 本 app 的設定 |
| **簡繁** | 輸出哪一種字形 | 方案的預設 + `simplification` 開關 |

**輸入模式不等於方案。** 一個方案(例如朙月拼音)可以輸出繁體也可以輸出簡體;
一個字集(簡體)可以由很多方案產生。兩者是**多對多**,所以需要一條明確的規則。

### 4.2 判定輸入模式的字集

各端從系統拿到一個識別字串,對應到 `hant` / `hans` / `unspecified`:

| 平台 | 來源 | 例 |
|---|---|---|
| macOS | `IMKInputController.setValue(_:forTag:client:)` 收到的輸入模式 id | `org.luminakey.inputmethod.LuminaKey.Hans` → `hans` |
| Windows | TSF language profile 的 langid | `0x0804` → `hans`;`0x0404` → `hant` |
| Android / iOS | 沒有這個訊號 | 恆為 `unspecified` |

⚠ **認不出來時回 `unspecified`,不要預設繁體。**
猜錯的代價是使用者打出他不要的字,而且完全不知道為什麼。寧可什麼都不做。

### 4.3 判定方案的字集

兩層,**語言標籤優先**:

1. 方案市集索引/已安裝紀錄裡的 BCP 47 標籤(`zh-Hant` / `zh-Hans` / `zh-TW` / `zh-CN`…)。
   這是作者宣告的,可信。
2. 沒有標籤才看 id 的命名慣例:`*_tw` `*_hk` `*_trad` `bopomofo*` → `hant`;
   `*_cn` `*_sc` `*_simp` → `hans`;其餘 → `unspecified`。

⚠ **不要加「含 pinyin 就是簡體」這種規則。** `luna_pinyin` 的預設輸出是**繁體**。
猜錯比不猜更糟,因為它會讓 `simplification` 被設成相反的值。

⚠ **方案 id 不是全域唯一的。** `double_pinyin` 同時存在於兩個套件,字集相反。
語言標籤要掛在**套件底下的方案參照**上,不是一張全域 id 表。

### 4.4 挑方案(規範性,由高到低)

```
1. 使用者為「這個輸入模式」釘的方案     (pinnedForMode)
2. 使用者釘的單一方案                   (pinnedGlobal)
3. 已啟用清單中第一個字集相符的方案
4. 已啟用清單的第一個
```

* 1 與 2 都是**使用者說的話**,所以即使字集不符也照做 ——
  這時靠 `simplification` 開關把字集補齊,**不要偷偷換掉他選的方案**。
* 釘的方案已經被停用了 → 當作沒釘,往下一條走。不要選一個清單上沒有的東西。
* 清單是空的 → 什麼都不做,並在畫面上說「還沒有任何方案」+ 一顆去市集的按鈕。

### 4.5 設 `simplification`(規範性)

```
text.variant == traditional        → simplification = false
text.variant == simplified         → simplification = true
text.variant == followInputMode:
    followInputMode 開 且模式為 hans → true
    followInputMode 開 且模式為 hant → false
    否則                              → 完全不呼叫 rs_set_option
```

* **「文字」頁的明確選擇優先於輸入模式。** 使用者在 app 裡明講過的話,
  比作業系統替他宣告的更重要。
* **無條件設,不要先判斷方案有沒有這個開關。** 對一個不存在的 switch
  呼叫 `rs_set_option()` 是安全的(librime 只是記下一個沒有人讀的選項),
  而判斷「這個方案有沒有 simplification」要解析第三方方案的 YAML,猜錯的機會更大。
* **關掉 `followInputMode` 時連 `simplification` 都不碰。** 使用者要的是「我自己管」,
  半套(方案不跟、簡繁還是跟)更難理解。

### 4.6 做不到的事(要誠實)

**RIME 的 `simplification` 是單向的:繁 → 簡的 opencc 轉換。**
一個本來就輸出簡體的方案不會因為關掉它而變成繁體。
所以「繁體輸入模式 + 只裝了簡體方案」的組合,字集**仍然是簡體** ——
這時只能靠第 3 條(字集相符優先)先挑對方案,挑不到就是挑不到。
**不要在介面上宣稱做得到**,該說的是「這個方案只有簡體,要繁體請安裝 ○○」。

### 4.7 套用時機

| 時機 | 做什麼 |
|---|---|
| 建立 session 之後 | 挑方案 + 套 session 選項 |
| 輸入模式改變 | 同上 |
| 設定改變 | 套 session 選項(方案只在相關設定變了才重挑) |
| 部署完成 | **舊 session 已失效**,重建之後整套重來 |

---

## 5. 使用者詞庫格式(四端共用)

> ✅ **這一節已經用真的 librime 驗過了(2026-08-09),四端可以照做。**
>
> `apple/scripts/verify_user_dict.sh` 每次 CI 都跑,而且是**硬失敗**。
> 它驗四件事:沒加詞時打不出來、兩種掛載檔各自都生效、
> 以及**空白分隔的編碼必須查不到**(反向測試,證明這一關對編碼格式是敏感的)。
>
> ── 這一節曾經是錯的,錯在哪裡值得留著 ──────────────────────────
>
> 上一版寫的是「編碼欄用空白分隔音節,例如 `ni hao`」,而那**打不出任何字**。
> macOS 端因此把那一頁下架了一輪。當時列出的三個待查方向 ——
> `engine/translators/@next` 沒生效、`stabledb` 要先編譯成二進位、
> `dictionary: ""` 沒有 prism —— **實測後三個全都不成立**。
>
> 真正的原因是空格。librime 那一側:
>
> * `gear/table_translator.cc` 的 `TableTranslator::Query()` 把**使用者按出來的
>   原始字串**(`zhuangzhuangzhuang`)直接交給 `UserDictionary::LookupWords()`;
> * `dict/table_db.cc` 的 `rime_table_entry_parser()` 把 TSV 的一行存成
>   `編碼 + " \t" + 詞`;
> * `dict/user_dictionary.cc` 判斷命中的條件是 `key[len] == ' '` ——
>   **要逐字元相等**。
>
> 編碼欄裡有空格 → 永遠不會命中 → 沒有候選、**也沒有任何錯誤訊息**:
> 檔案讀進去了、詞條也載入了,只是查不到。
>
> **教訓有兩個,兩個都比這個格式本身重要:**
> 1. 第一版把紅的原因歸咎於「`zzq` 不是合法音節」,於是換成
>    `zhuang zhuang zhuang` —— 換完還是紅的,因為診斷是錯的。
>    **改了之後還是紅,代表診斷錯了,不是改得不夠。**
> 2. 那支腳本當時**沒有反向測試**,所以它證明得了「這個寫法不行」,
>    卻證明不了「它對編碼格式是敏感的」。現在補上了。

**目標:使用者在電腦上加的詞,手機上也看得到。**

### 5.1 為什麼不是 librime 的使用者詞典

librime 的 userdb 是 LevelDB,只有 librime 讀得懂,而 `core/include/rime_shell.h`
的 ABI **沒有**匯出它的增刪介面(要加得由協調端動 `core/`,四端一起改)。

改用 RIME 既有的另一條路:`table_translator@custom_phrase` + `db_class: stabledb`,
讀的是使用者目錄裡一份**純文字 TSV**。三個好處:四端都做得到、
使用者看得懂改得動也備份得了、而且它是 RIME 的既有機制 ——
使用者哪天換去鼠鬚管或小狼毫,這份檔案照樣有效。

### 5.2 檔案:`<user>/custom_phrase.txt`

UTF-8,**LF 或 CRLF 都必須讀得懂**(Windows 端匯出的帶 CR,
留著會讓權重欄變成 `1\r` 而解析失敗,錯誤訊息卻看起來像使用者打錯字)。

```
# LuminaKey 使用者詞庫 / user phrases — format 1
# 每行三欄,用 TAB 分隔:  詞<TAB>編碼<TAB>權重
你好	nihao	3
黃小明	huangxiaoming	1
```

| 欄 | 必填 | 說明 |
|---|---|---|
| 1 | ✅ | 打出來的文字 |
| 2 | ✅ | 編碼 = **使用者實際按的那一串鍵,原樣**。一律小寫、**不含任何空白** |
| 3 | | 權重,預設 `1`,越大越前面 |

⚠⚠ **第二欄不是「音節」,是「按鍵」。** `ni hao` 與 `NIHAO` 都查不到,
只有 `nihao` 查得到 —— 因為 librime 拿去比對的是使用者按出來的原始字串,
而且要逐字元相等(理由見本節開頭)。實測對照:

| 檔案裡寫的 | 打 `nihao` |
|---|---|
| `ni hao` | ✗ 查不到 |
| `NIHAO` | ✗ 查不到(librime 只 trim,不轉小寫) |
| `nihao` | ✓ 排在第一個 |

**因此讀取時必須把空白全部拿掉、並轉成小寫**,不是「收成單一空格」。
別的 RIME 前端的碼表、以及我們自己舊版匯出的檔案都可能是空白分隔的,
這條正規化就是接住它們的地方。

編碼**不必**是這個方案的合法音節:`qqq`(拼音的三個聲母)、`su3cl3`(注音)
都實測可用。想從介面上判斷「這個方案打不打得出這個編碼」一定會誤傷,
**不要做**;只擋確定不可能命中的東西(空的、超長的、非鍵盤字元)。

規則(規範性):

* `#` 開頭的行與空行:跳過。
* **欄位分隔一定是 TAB**,不是空格 —— 編碼欄本身含空格。
* 欄位少於 2、第一或第二欄為空:**跳過那一行並記一則問題,不中斷解析**。
  一行寫壞不該讓整本詞庫消失。
* 身分是「詞 + 編碼」。同一個身分出現兩次:後者更新前者的權重,不變成兩筆。
* 上限:50 000 筆、單欄 64 字、檔案 8 MiB。不是為了省空間,
  是為了讓一個壞掉的檔案不會變成一個吃光記憶體的檔案。

**匯入是合併,不是取代。** 使用者按的是「匯入」。衝突時取權重大的那一筆;
權重相同時保留原有的(匯入不該改變既有排序)。

### 5.3 掛載:`<user>/<schema>.custom.yaml`

要對每一個**已啟用的方案**分別掛一次,而且有**兩種形態** ——
差別只在要不要接上翻譯器:

```yaml
# 形態 A:方案自己已經有 table_translator@custom_phrase(內建的都是這種)
# luminakey-managed: custom_phrase v1
patch:
  custom_phrase:
    dictionary: ""
    user_dict: custom_phrase
    db_class: stabledb
    enable_completion: false
    enable_sentence: false
    initial_quality: 99
```

```yaml
# 形態 B:方案沒有,要自己接一個(市集下載的多半是這種)
# luminakey-managed: custom_phrase v1
patch:
  "engine/translators/@next": table_translator@custom_phrase
  custom_phrase:
    dictionary: ""
    # …以下同形態 A
```

⚠⚠ **對已經有的方案再接一次 = 同一個詞在選字窗裡出現兩次。**
RIME 的 patch 只有「接上去」(`@next`),沒有「沒有才接」,所以這一題必須
自己判斷。實測(2026-08-09):對 `luna_pinyin_tw` 用形態 B,編譯出來的方案裡
`table_translator@custom_phrase` 有兩個;內建方案剛好有 `uniquifier`
會把重複的收掉所以看不出來,**把 `uniquifier` 拿掉之後同一個詞立刻變成
第 1、2 名兩筆**。市集裡的方案沒有人保證有 `uniquifier`。

判斷方式:沿著方案的 `__include` / `__patch` 追下去,看有沒有
`table_translator@custom_phrase`。兩條規則:

* **判斷不出來時要用形態 B(照接不誤)。** 兩種錯法代價不對等:
  誤判成「已經有了」→ 使用者加的詞對那個方案完全沒作用,而且沒有任何跡象;
  誤判成「沒有」→ 最壞是看到兩筆一樣的。前者比後者嚴重得多。
* **不可以把自己寫的掛載檔當成證據。** `luna_pinyin_tw.schema.yaml` 的
  `__patch` 就指向 `luna_pinyin_tw.custom.yaml` —— 那正是我們寫的那一份。
  照著追下去的話,第二次掛載會看到第一次留下的 `@next`、判定「方案已經有了」、
  於是把它拿掉,**掛載就這樣自己消失**。帶標記的檔案一律跳過。

⚠ **第一行的標記是「這個檔案可以覆寫」的唯一依據。**
使用者自己寫的 `<schema>.custom.yaml` 裡常有他調了很久的按鍵綁定,
覆蓋是不可逆的損失。沒有標記 → **不要動它**,並在畫面上說明
「這個方案的設定檔是你自己寫的,詞庫沒有掛上去」。

⚠ `user_dict` 的值是 `custom_phrase`(**不含 `.txt`**),必須與檔名對得起來。

⚠ 加詞、刪詞、掛載之後都**必須重新部署**才會生效。介面上要說出來。

### 5.4 驗證

`apple/scripts/verify_user_dict.sh` 用真的 librime 走一遍。
**其他端實作時請照抄這個形狀** —— 四個步驟,少一個這一關就變成一張空頭支票:

| 步驟 | 少了它會怎樣 |
|---|---|
| 1. 沒加詞時**打不出來** | 「那個詞本來就在詞典裡」也會綠,而我們什麼都沒驗到 |
| 2. 形態 A 的掛載檔生效 | 內建方案的那條路沒被驗到 |
| 2b. 編譯後的翻譯器**剛好一個** | 重複掛載會被 `uniquifier` 蓋住,永遠不會被發現 |
| 3. 形態 B 的掛載檔生效 | 市集方案的那條路沒被驗到 |
| 4. **空白分隔的編碼必須查不到** | 這一關證明不了自己對編碼格式敏感 —— 它可能只是在驗「檔案讀得到」 |

第 4 步是**反向測試**,而它就是上一輪失敗的直接教訓:當時那一關紅了,
但因為沒有反向測試,沒有人分得出「格式錯」與「別的東西壞了」,
於是診斷歸咎於一個錯的原因,換了編碼再跑,還是紅的。

⚠ 這一關本身也被變異測試看著(四個植入的違規各自要打紅對應的那一步),
理由見 `docs/coordination.md` §3:**守門自己會失效**。

---

## 6. 跨端同步(尚未實作)

目前四端**沒有**同步機制。這一節只記下已經對齊的前提,免得日後補同步時發現對不上:

* A 層(`default.custom.yaml`、`custom_phrase.txt`、方案檔)是純文字,直接同步即可。
* B 層各端檔案格式不同,但**鍵名共用**(見 §3 的 id 欄),可以逐鍵對應。
* 行動端專屬的檔案(例如 `luminakey-layouts.json` 的自訂鍵位)桌面端
  **必須原樣搬運、不得解析、不得清理** —— 否則同步會把使用者在手機上
  調好的鍵位洗掉。
* 設定檔要帶 `version`,而且**讀到比自己新的版本時只讀不寫**。
  舊版覆寫新版會把另一台機器上調好的東西洗掉,而且沒有任何跡象。
