# 主題與鍵盤佈局格式規範 v1

> 本文件是**規範性（normative）**文件。四端（Android / iOS / macOS / Windows）各自
> 實作解析器，唯一的一致性來源就是本文件。凡本文件未明確規定者，即為規範缺陷，
> 請提 issue 而非各自發明。
>
> 對應檔案：`core/themes/*.yaml`、`core/layouts/*.yaml`
> 參考實作：`android/app/src/main/java/org/luminakey/ime/theme/`

---

## 0. 用語

| 詞 | 意義 |
|---|---|
| **MUST / 必須** | 不遵守即為不合規實作 |
| **MUST NOT / 不得** | 同上 |
| **SHOULD / 應** | 有正當理由可偏離，但須在實作中留註記 |
| **MAY / 可** | 純選用 |
| **診斷（diagnostic）** | 解析過程產生的一則 `{severity, code, args, path, line}` 記錄，見 §6.5 |
| **致命錯誤（fatal）** | 文件被整份拒絕，不產生任何結果物件 |
| **可回復錯誤（recoverable）** | 該欄位改用預設值，解析繼續，並產生一則 WARNING 診斷 |

---

## 1. 範圍與設計原則

### 1.1 兩種形態，一套配置

| | 行動端（Android / iOS） | 桌面端（macOS / Windows） |
|---|---|---|
| UI 型態 | 整塊自繪軟鍵盤 + 上方候選列 | 懸浮候選窗，無軟鍵盤 |
| 消費 `core/themes/` | 全部 | 除 `keyboard`、`feedback`、`candidates.bar` 之外全部**解析**；`candidates.syllables` 解析但**不渲染**（§8.6.6.3.5） |
| 消費 `core/layouts/` | 全部 | **完全不消費** |

主題檔案是四端共用的；佈局檔案只有行動端消費。這個分野在格式裡是**結構性**的，
不是靠註解約定：軟鍵盤相關的一切集中在 `theme.keyboard` 與獨立的 layout 檔中，
桌面端實作者可以整段跳過而不會漏掉任何東西。

### 1.2 設計原則

1. **顯式優於隱式。** 四個獨立解析器之下，任何「聰明的推導」都會變成四種不同的推導。
   例如大寫層是一個完整寫出的 layer，而不是「對小寫層自動 uppercase」。
2. **失敗要能看見畫面。** 輸入法畫不出來比畫錯嚴重得多。除少數結構性缺陷外，
   一律採「以預設值替代 + 產生診斷」（§6）。
3. **鍵面文字與送出內容是兩件事。** librime 吃的是 X11 keysym，不是字元（§9.4）。
   這一條決定了注音、倉頡等方案能不能被描述。
4. **能被 diff 與繼承。** 使用者改一個顏色不該複製 400 行（§7）。
5. **不做序列的逐元素合併。** 序列一律整體取代。理由見 §7.2。

---

## 2. 檔案、識別與搜尋路徑

### 2.1 檔案

* 編碼 **必須** 為 UTF-8，**不得** 有 BOM。解析器遇到 BOM **必須** 略過它而非報錯。
* 換行 `\n` 或 `\r\n` 皆可，解析器 **必須** 正規化為 `\n`。
* 副檔名 `.yaml`（不接受 `.yml`，避免同 id 兩檔）。
* 檔案 basename **必須** 等於文件內的 `id`。不相等時 → **致命錯誤**。

### 2.2 識別

`id` 的字元集為 `[a-z0-9][a-z0-9._-]*`，長度 1–64。大小寫敏感但 **必須** 全小寫。
不合規 → **致命錯誤**。

### 2.3 搜尋路徑

解析器依序在下列位置尋找 `<id>.yaml`，**先找到者勝出**：

1. 使用者目錄 `<user_data_dir>/themes/`、`<user_data_dir>/layouts/`
2. 隨附目錄 `<shared_data_dir>/themes/`、`<shared_data_dir>/layouts/`
3. 內建資源（App 包內）

> iOS：這兩個目錄 **必須** 位於 App Group 容器內，否則主 App 寫入的主題鍵盤擴展看不到。
> 見 `core/include/rime_shell.h` 的 `rs_setup.user_data_dir` 註記。

### 2.4 主題套件（theme package）

一份主題若要自帶字體或圖片，**可** 以目錄形式散佈：

```
sakura-dark/
  sakura-dark.yaml      ← 必須與目錄同名
  fonts/Iansui.ttf
```

`typography.assets[].file` 是相對於該目錄根的路徑。**不得** 出現 `..` 或絕對路徑；
違反者該筆 asset 被丟棄並產生 WARNING。單檔散佈（`sakura-dark.yaml` 直接放在
`themes/` 下）也合法，此時 `assets` 必須為空，否則產生 WARNING 並忽略。

---

## 3. YAML 子集（RTS — Rime Theme Subset）

四端會用到四種不同的 YAML 實作（Kotlin 自製 / Swift Yams / C++ yaml-cpp / C# YamlDotNet）。
完整 YAML 的邊角（anchor、alias、tag、多文件、複雜 key、YAML 1.1 vs 1.2 的型別解析差異）
是行為分歧的溫床，因此本格式只使用下列子集。**寫入者必須遵守；讀取者只需支援本子集。**

### 3.1 允許

* 單一文件（無 `---` / `...` 分隔符，或至多一組包住整份內容）
* 區塊映射 `key: value`、區塊序列 `- item`
* 流式序列 `[a, b]`、流式映射 `{a: b}`
* 純量：無引號、單引號 `'...'`（`''` 表示一個單引號）、雙引號 `"..."`
  （支援 `\\ \" \/ \n \r \t \0 \uXXXX` 逸出）
* 註解：行首 `#`，或前面至少一個空白的 `#`
* 空值：`null`、`~`、或 `key:` 後接空

### 3.2 禁止

錨點 `&` / 別名 `*` / 合併鍵 `<<`、標籤 `!`、區塊純量 `|` `>`、
多文件、複雜 key `? `、以 Tab 縮排、同一映射中重複的 key。

讀取者遇到禁止構造時 **應** 產生 WARNING 並盡力忽略；**可** 視為致命錯誤。
重複 key 是唯一例外：**必須** 產生 WARNING 並採用**最後一次**出現的值。

### 3.3 型別解析（關鍵）

**本格式不依賴 YAML 的隱式型別解析。**
所有純量在讀取層一律視為**字串**，由本規範定義的欄位型別在綁定時自行轉換（§4）。

這條規則消滅了 YAML 1.1 與 1.2 之間最惡名昭彰的分歧：`y` / `n` / `on` / `off` /
`yes` / `no` 在 YAML 1.1（yaml-cpp、Yams 預設）是布林，在 YAML 1.2 core schema
（多數新實作）是字串。若讀取層已把 `keysym: y` 解析成布林 `true`，任何補救都太遲。

因此：

* 寫入者 **必須** 把 `keysym`、`id`、`label`、`hint`、`text` 這五個欄位的值加引號。
* 讀取者若使用會做隱式型別解析的第三方 YAML 函式庫，**必須** 在綁定前把布林與數值
  節點還原為其原始字面文字（Yams / yaml-cpp / YamlDotNet 皆可取得原始 scalar 字串）。
* 本 repo 的 `core/` 檔案由 CI 檢查上述引號規則。

---

## 4. 型別系統

### 4.1 `bool`

接受（大小寫不敏感）：`true` / `false` / `yes` / `no` / `on` / `off` / `1` / `0`。
其餘 → 可回復錯誤。

### 4.2 `int`、`number`

十進位。`number` 接受小數與科學記號。前後空白容許。其餘 → 可回復錯誤。
超出欄位表所列範圍者 → **夾到範圍內** + WARNING（不是錯誤）。

### 4.3 `length`（單位：`dp`）

**所有長度都是無單位的數字，單位固定為 `dp`。**
不接受 `"16dp"` 這種帶單位字串 —— 四個解析器不需要一個單位詞法分析器。

`dp` 定義為**邏輯單位**，各端 1:1 對應到自己的邏輯單位：

| 平台 | 對應 | 換算到裝置像素 |
|---|---|---|
| Android | dp | `px = dp * (densityDpi / 160)` |
| iOS / macOS | pt | `px = dp * UIScreen.scale`（`pt = dp`） |
| Windows | DIP（1/96 in） | `px = round(dp * GetDpiForWindow(hwnd) / 96)` |

> **刻意的不對稱：** Android 的 dp 實體上是 1/160 英吋，Windows 的 DIP 是 1/96 英吋。
> 也就是說同一個 `16` 在 Windows 上實體較大。這是**故意**的：手機握持距離約 30 cm，
> 桌面螢幕約 60 cm，桌面 UI 本來就該大一些。若改採實體等長映射，
> 桌面候選窗的字會小到不可讀。
> 代價是：`metrics` 區塊的數值在行動端與桌面端的觀感不完全一致，
> 需要調整時請用 `platform_overrides`（§7.4）。

### 4.4 `size`（單位：`sp`，僅用於文字）

文字尺寸同樣是無單位數字，但套用**有效字體縮放**：

```
effective_scale = typography.respect_system_font_scale
                  ? clamp(system_font_scale, font_scale_min, font_scale_max)
                  : 1.0
rendered_px = size * effective_scale * <該平台 dp→px 係數>
```

`system_font_scale` 取得方式（規範性）：

| 平台 | 來源 |
|---|---|
| Android | `Configuration.fontScale` |
| iOS | `UIContentSizeCategory` 依下表映射 |
| macOS | 固定 `1.0`（系統無等價設定） |
| Windows | `UISettings.TextScaleFactor` |

iOS `UIContentSizeCategory` → 標量（規範性，四端不得自行調整）：

| category | 值 |
|---|---|
| ExtraSmall | 0.82 |
| Small | 0.88 |
| Medium | 0.94 |
| Large（預設） | 1.00 |
| ExtraLarge | 1.12 |
| ExtraExtraLarge | 1.24 |
| ExtraExtraExtraLarge | 1.35 |
| Accessibility*（全部） | 1.60 |

夾制 `font_scale_max` 存在的理由很實際：使用者把系統字體開到最大時，
無上限的縮放會讓鍵盤按鍵文字互相重疊，或讓候選列高度吃掉半個螢幕。

#### 4.4.1 `rendered_px` 是最終值，不得重複套用（實作陷阱）

上式的 `rendered_px` 是**送進點陣化階段的最終像素值**。
各端的 UI 框架多半已經自行套用過一次系統字體縮放，實作 **必須** 先把框架
已套用的部分除掉，否則縮放會被平方，同一份主題在四端會有四種字級。

| 平台 | 框架是否已套用 | 實作必須怎麼做 |
|---|---|---|
| Android / Compose | **是**，`.sp` 內含 `Configuration.fontScale` | 傳入 `.sp` 的值 **必須** 為 `size * effective_scale / LocalDensity.current.fontScale` |
| Android / View | 用 `TypedValue.COMPLEX_UNIT_SP` 時**是**；`COMPLEX_UNIT_DIP` 時**否** | 選 `DIP` 並自行乘 `effective_scale`，或用 `SP` 並除掉 `fontScale` |
| iOS | `preferredFont(forTextStyle:)` **是**；`systemFont(ofSize:)` **否** | **應** 使用 `systemFont(ofSize:)` 並自行乘 `effective_scale` |
| macOS | 否（系統無等價設定，`effective_scale` 恆為 1.0） | 直接使用 |
| Windows | 否，`TextScaleFactor` 不會自動套用到 DIP | 直接乘 `effective_scale` |

判定方式很簡單：把系統字體大小調到最大，`font_scale_max` 若是 `1.30`，
文字**必須**恰好放大 30%。若放大了約 69%（1.30 × 1.30），就是重複套用了。

> Android 參考實作把這個除法收在一個 `Scaler` 類別裡，四端 **應** 比照辦理：
> 讓「除掉框架縮放」只出現在一處，而不是散落在每個 `fontSize =` 呼叫點。

### 4.5 `ratio`

`0.0`–`1.0` 的 `number`。超出 → 夾制 + WARNING。

### 4.6 `duration`

毫秒整數，`0`–`5000`。

### 4.7 `color`

```ebnf
color        = hex-color | palette-ref | "transparent" ;
hex-color    = "#" ( 3*HEXDIG | 4*HEXDIG | 6*HEXDIG | 8*HEXDIG ) ;
palette-ref  = "$" name [ "@" alpha ] ;
name         = ALPHA *( ALPHA | DIGIT | "_" ) ;
alpha        = number [ "%" ] ;
```

* 十六進位大小寫不敏感。3/4 位形式以「每位重複一次」展開：`#f0a` → `#ff00aa`。
  6 位形式的 alpha 補 `ff`。順序固定為 **RGB / RGBA**（不是 ARGB）。
* `transparent` 等同 `#00000000`。
* `palette-ref` 查 `palette` 區塊；`@alpha` 對查得的顏色做透明度調變：
  * 帶 `%`：`0`–`100`，`$accent@30%` = 該色的 alpha 乘以 0.30。
  * 不帶 `%`：`0.0`–`1.0`，`$accent@0.3` 同上。
  * 調變是**相乘**而非覆寫：若 `$accent` 本身 alpha 為 `0x80`，`@50%` 後為 `0x40`。
* `palette` 的值本身 **可** 是 `palette-ref`，遞迴解析，深度上限 8。
  超過深度或成環 → 該 palette 條目視為**不存在**（§6）。

### 4.8 `enum`

見各欄位表。**未知的列舉值 → 可回復錯誤**（採預設值 + WARNING）。
新增列舉值不構成破壞性變更（§5），因此舊版客戶端遇到新值必須能優雅退化。

### 4.9 `localized-string`

兩種寫法皆合法：

```yaml
name: "Default Dark"                    # 等價於 { und: "Default Dark" }
name: { en: "Default Dark", zh-Hant: "預設深色" }
```

Key 為 BCP-47 標籤。查詢演算法（規範性）：

1. 與使用者當前 locale 完整比對（大小寫不敏感）
2. 比對主語言子標籤（`zh-Hant-TW` → 先試 `zh-Hant`，再試 `zh`）
3. `en`
4. `und`
5. 文件順序中的第一筆

找不到任何一筆 → 空字串。

### 4.10 `string-list`

`["a", "b"]` 或區塊序列。單一字串 `"a"` **必須** 被接受，等價於 `["a"]`。

---

## 5. 版本化

### 5.1 `format` 欄位

```yaml
format: rime-theme/1        # 或 rime-layout/1
```

* 語法：`<kind>/<major>`，`major` 為正整數。
* 解析器 **必須** 拒絕 `kind` 不符者（把 layout 當 theme 讀是使用者錯誤，靜默忽略更糟）。
* 解析器 **必須** 拒絕 `major` 大於自己所知者，並產生一則對使用者可讀的錯誤：
  「此主題需要較新版本的客戶端」。
* 解析器 **必須** 接受 `major` 等於自己所知者。
  舊 major **可** 支援，本規範 v1 尚無舊 major。

### 5.2 什麼算破壞性變更（需遞增 major）

* 刪除欄位、更名欄位
* 改變既有欄位的型別、單位、預設值或語義
* 改變合併規則、顏色語法、單位定義

### 5.3 什麼不算（不遞增 major）

* 新增選用欄位
* 新增列舉值
* 新增 `key_styles` / `fonts` / `palette` 的具名條目

因為 §6 規定未知欄位與未知列舉值一律優雅退化，這類變更對舊客戶端是安全的。

### 5.4 `min_client`（選用）

```yaml
min_client: "0.4.0"
```

若客戶端版本低於此值 → **致命錯誤**，訊息需指出所需版本。
本欄位用於「格式沒變，但某欄位的**渲染**需要新版才正確」的情況。

### 5.5 `revision`（選用）

正整數，作者每次發佈遞增。解析器不使用它，但 M6 配置同步用它做衝突解決。

### 5.6 棄用政策

被標記為 deprecated 的欄位 **必須** 在下一個 major 之前持續被支援，
且解析器 **應** 對其產生 INFO 級診斷。

### 5.7 v1 期間的例外（鍵盤高度模型，兩次）

§8.8 的高度模型在 v1 期間改寫過**兩次**，都沒有遞增 major。逐次記錄：

**第一次**（初稿 → v2）：`keyboard.height.*` 被 `key_aspect` / `key_height` /
`max_screen_ratio` 取代。理由：

1. 初稿模型是**缺陷**而非設計選擇 —— 它讓鍵高綁螢幕高、鍵寬綁螢幕寬，
   長寬比必然失控。保留它沒有任何價值。
2. 此時尚無任何第三方主題（客戶端版本 0.1.x），影響範圍為零。
3. 遞增 major 會讓所有 v1 文件被拒絕載入，代價遠大於收益。

**第二次**（v2 → v3，本次）：改為「固定高度預算 ÷ Σweight」，
`key_aspect` 與 `key_height` 的語義收窄為「參考格上那顆鍵」，
新增 `reference_grid` 與 `row_height`。理由：

1. 同樣是**缺陷**：v2 讓鍵盤總高隨列數線性成長，使用者一打開數字列鍵盤就
   長高 20%，而三星實機是四列與五列**總高只差 1%**（§8.8.0）。
   照 v2 出貨等於明知故犯。
2. **欄位沒有被移除，只是語義收窄**，且收窄後既有主題的算出值變化很小
   （本 repo 六份主題全部落在 ±8% 內，`cn-compact-*` 為 0%）。
   新欄位皆有預設值，不寫也完全合法。
3. 影響範圍仍然為零（客戶端未發 1.0，無第三方主題）。

補償措施：舊 `height:` 區塊被忽略時 **必須** 產生 INFO 診斷（§8.8.0.2）；
`row_height` 護欄生效（總高偏離預算）時 **應** 產生 INFO 診斷（§8.8.0）。

**不會再有第三次。** 客戶端進入 1.0 之後，任何語義變更一律遞增 major。
高度模型現在有實機量測背書（三星 + Gboard 兩家、兩種螢幕），
而前兩版都是先寫規範再看實機——這才是要記取的教訓。

---

## 6. 錯誤處理約定

這一節是本規範的核心。四端行為不一致的絕大多數來源不是「正常路徑」，而是壞檔案。

### 6.1 兩級模型

| 級別 | 行為 |
|---|---|
| **fatal** | 整份文件被拒絕，`load()` 回傳失敗與診斷清單。呼叫端 **必須** 退回上一個成功載入的主題；若無，退回內建 `default-light` / `default-dark`。**不得** 顯示空白鍵盤。 |
| **recoverable** | 該欄位採用本規範所列預設值，解析繼續，累積一則 WARNING 診斷。 |

解析器 **必須** 回傳完整診斷清單（不是只回第一筆），且 **必須** 在
「主題編輯器」情境下可取得（M2 之後會有 UI）。
每則診斷 **必須** 含：嚴重度、**穩定的診斷碼**、該碼的位置參數、
YAML 路徑（如 `keyboard.key_styles.default.background`）、以及**行號**
（若讀取層能提供）。**訊息文字不是規範的一部分** —— 理由與完整碼表見 §6.5。

### 6.2 致命錯誤的完整清單

超出此清單者一律為可回復錯誤。

| # | 情況 |
|---|---|
| F1 | 不是合法 YAML（語法錯誤） |
| F2 | 根節點不是映射 |
| F3 | 缺少 `format`，或 `format` 語法不合、kind 不符、major 未知 |
| F4 | 缺少 `id`，或 `id` 字元集不合，或 `id` ≠ 檔名 |
| F5 | `inherits` 指向的 id 找不到 |
| F6 | `inherits` 成環，或繼承鏈深度 > 8 |
| F7 | `min_client` 高於當前客戶端版本 |
| F8 | 佈局：`layers` 缺失、非序列、或為空 |
| F9 | 佈局：`default_layer` 指向不存在的 layer id |
| F10 | 佈局：某 layer 沒有任何 `rows`，或某 row 沒有任何 `keys` |

> **F5 為什麼是致命的？** 若靜默忽略繼承，使用者會拿到一份「只有主色、其餘全是預設值」
> 的主題 —— 看起來像壞掉的主題而不像壞掉的設定，除錯成本極高。明確失敗並退回上一版更好。

### 6.3 可回復錯誤：逐類規則

| 情況 | 行為 |
|---|---|
| 欄位缺失 | 採欄位表的預設值。**不產生診斷**（缺失是正常的，這是稀疏格式的重點） |
| 欄位為顯式 `null` | 同上，且在繼承情境中代表「刪除父值」（§7.2）。不產生診斷 |
| **未知欄位** | **忽略 + WARNING**（訊息應含最接近的已知欄位名以利拼字錯誤除錯）。永不致命 |
| 顏色字串格式錯誤 | 採該欄位預設值 + WARNING。**不得** 採用洋紅等「醒目除錯色」——使用者拿到的是一個能用但顏色不對的鍵盤，不是一個被塗花的鍵盤 |
| `$name` 指向不存在的 palette 條目 | 同上（採欄位預設值 + WARNING） |
| 型別不符（期待數字得到字串） | 先嘗試字面轉換（§4.1/4.2）；轉不出來則採預設值 + WARNING |
| 數值超出範圍 | 夾制到範圍內 + WARNING |
| 未知列舉值 | 採預設值 + WARNING |
| 期待映射卻得到序列（或反之） | 採預設值（整個子樹）+ WARNING |
| 佈局：未知的 `action` 字串 | 該鍵變成 `noop` + WARNING（仍會被畫出來，不會留空洞） |
| 佈局：未知的 `icon` 名 | 改用 `label`；`label` 也沒有則畫空白 + WARNING |
| 佈局：`style` 指向主題沒有的 key style | 改用 `default` style + WARNING |
| 佈局：無法解析的 `keysym` 名 | 該鍵變成 `noop` + WARNING |
| 佈局：`send` 與 `tap` 同時存在 | `tap` 勝出，`send` 被忽略 + WARNING |
| 佈局：`repeat: true` 與 `long_press` 同時存在 | `repeat` 勝出，`long_press` 被忽略 + WARNING |
| 佈局：某 row 的 `width` 總和 ≠ `units` | 依 §9.3 佈版，不產生錯誤；差距 > 0.01 時產生 WARNING |
| `assets` 路徑越界或平台不支援自帶字體 | 丟棄該筆 + WARNING |

### 6.4 診斷嚴重度

* `ERROR` — 僅用於致命錯誤。
* `WARNING` — 上表全部。
* `INFO` — 棄用欄位、被忽略的平台專屬欄位。

### 6.5 診斷的身分：`code` + `args`（v1 期間的模型變更）

**決定：一則診斷的身分是 `(severity, code, path)`。`message` 不再是規範的一部分。**

初稿把診斷定義成 `{severity, path, message, line}`，其中 `message` 是自由文字。
這個定義在**要把診斷拿給使用者看**的那一刻自我矛盾：

* §10 檢核第 9 條要求「同一份壞檔案，四端報一樣多則、內容一致」。
* 但訊息一旦在地化（Android 端已經在做介面在地化），四端就沒有任何
  可以互相比對的東西了 —— 中文的「不是合法的顏色」與英文的
  `is not a valid color` 是同一則診斷，字串比對卻永遠不相等。
* 而不在地化，就等於規劃中的主題編輯器只能對非英語使用者顯示英文。

因此：

| 欄位 | 型別 | 說明 |
|---|---|---|
| `severity` | enum | **由 `code` 決定**，不得由產生點自行選擇（見下） |
| `code` | string | 下表的穩定識別碼。**這是規範的一部分**，不得為了好看而更名 |
| `args` | list<string> | 填進在地化樣板的參數，順序由下表逐碼固定 |
| `path` | string | YAML 路徑，如 `candidates.window.background`；根層級為空字串 |
| `line` | int \| null | 來源行號，讀取層取不到時為 null |

`message` **可** 繼續存在，但它是**開發者用的英文回退**：不上使用者畫面、
不參與四端比對、不受本規範約束。UI 端 **必須** 以 `code` 查自己的在地化樣板，
並以 `args` 依序填入。

**`severity` 由 `code` 決定，不由產生點決定。** 這一條看似瑣碎，卻是
第 9 條檢核能不能成立的關鍵：只要有一端把同一件事記成 INFO 而別端記成 WARNING，
「四端報一樣多則 WARNING」就失守了，而且失守得無聲無息。實作 **應** 讓
`severity` 是 `code` 上的一個函式，而不是每個 `diag.warn(...)` 呼叫點的參數。

**未知的 `code`：** 讀到自己不認得的 code 的 UI（例如舊版客戶端讀新版產生的
診斷清單）**必須** 退化為顯示 `code` 字面值與 `args`，**不得** 丟棄該則診斷。

#### 6.5.1 碼表（規範性）

`args` 欄列出的是**位置參數**，順序固定。

**致命（ERROR）** —— 與 §6.2 的 F 編號對應：

| code | F | args |
|---|---|---|
| `fatal.yaml_syntax` | F1 | `[detail]` |
| `fatal.root_not_mapping` | F2 | `[]` |
| `fatal.format_missing` | F3 | `[document-id]` |
| `fatal.format_malformed` | F3 | `[format-tag, document-id]` |
| `fatal.format_kind_mismatch` | F3 | `[actual-kind, expected-kind, document-id]` |
| `fatal.format_major_unsupported` | F3 | `[document-id, kind, major, supported-major]` |
| `fatal.id_missing` | F4 | `[document-name]` |
| `fatal.id_invalid` | F4 | `[id]` |
| `fatal.id_mismatch` | F4 | `[id, document-name]` |
| `fatal.document_not_found` | — | `[id]`（最初要求的那一份就找不到，不是父代） |
| `fatal.parent_not_found` | F5 | `[parent-id]` |
| `fatal.inherits_cycle` | F6 | `[chain]`（`a -> b -> a`） |
| `fatal.inherits_too_deep` | F6 | `[max-depth]` |
| `fatal.min_client` | F7 | `[required-version, running-version]` |
| `fatal.layers_missing` | F8 | `[]` |
| `fatal.default_layer_unknown` | F9 | `[layer-id]` |
| `fatal.alpha_layer_unknown` | F9 | `[layer-id]` |
| `fatal.layer_empty` | F10 | `[layer-id]` |

**可回復（WARNING）**：

| code | 何時 | args |
|---|---|---|
| `unknown_field` | §6.3 未知欄位 | `[field]` 或 `[field, suggestion]` |
| `duplicate_key` | §3.2 同一映射中重複的 key | `[key]` |
| `type_mismatch` | 期待映射／序列／純量卻得到別的 | `[expected, found]` |
| `bad_bool` | §4.1 轉不出布林 | `[value]` |
| `bad_number` | §4.2 轉不出數字 | `[value]` |
| `out_of_range` | §4.2 夾制 | `[value, min, max, clamped]` |
| `bad_enum` | §4.8 未知列舉值 | `[value, allowed, default]` |
| `bad_color` | §4.7 顏色字面值不合法，或 `$ref` 指向不存在的 palette 條目 | `[value]` |
| `palette_not_scalar` | palette 條目不是純量 | `[name]` |
| `palette_bad_color` | palette 條目的值不是合法顏色 | `[name, value]` |
| `palette_unresolved_ref` | palette 條目指向解不出來的條目 | `[name, target]` |
| `palette_self_reference` | palette 條目指向自己 | `[name]` |
| `palette_cycle_or_too_deep` | 成環或深度 > 8 | `[name]` |
| `entry_dropped` | 序列／映射中某一筆型別不合而被丟棄 | `[]` |
| `asset_incomplete` | font-asset 缺 `family` 或 `file` | `[]` |
| `asset_path_escape` | §2.4 asset 路徑越界 | `[file]` |
| `unknown_script_tag` | §8.4.2 未知 ISO 15924 標籤 | `[tag]` |
| `unknown_icon` | §9.6 未知圖示名 | `[icon]` |
| `unknown_action` | §9.5 未知 verb | `[raw]` |
| `bad_action_argument` | §9.5 已知 verb 但參數缺失或不合法 | `[raw]` |
| `toolbar_item_no_tap` | §8.6.6.1 工具列項目缺 `tap` | `[]` |
| `status_item_no_source` | §8.12 狀態列項目缺 `source` 或 source 未知 | `[]` |
| `syllables_no_slots` | §8.6.6.3.3 D1：`keyboard_slot` 但當前 layer 的可用格位 < 2。⚠ **尚未實作**（§8.6.6.3.4） | `[layout-id, layer-id, count]` |
| `syllables_slot_unknown` | §9.3.1：`syllable_slots` 的某個 id 在該 layer 找不到對應的鍵 | `[layer-id, key-id]` |
| `syllables_toggle_missing` | §8.6.6.3.3 D3：`on_demand` 但佈局沒有 `syllables:toggle` 鍵。⚠ **尚未實作**（§8.6.6.3.4） | `[layout-id]` |
| `nested_platform_overrides` | §7.4 巢狀的 `platform_overrides` | `[]` |

**INFO**：

| code | 何時 | args |
|---|---|---|
| `required_item_restored` | §8.6.6.1 / §8.12 的必備項被補回 | `[action]` |
| `deprecated_field` | §5.6 棄用欄位 | `[field]` |
| `feature_unsupported` | 該平台不支援某個值（如 `backdrop: vibrancy`），靜默退化 | `[field, value]` |
| `legacy_block_ignored` | §8.8.0.2 舊的 `keyboard.height` 區塊被忽略 | `[field]` |

> **新增 code 不算破壞性變更**（§5.3）：舊 UI 讀到未知 code 會退化為顯示字面值。
> **更名或移除 code 算**，因為在地化樣板是照 code 查的。

---

---

## 7. 繼承與合併

### 7.1 `inherits`

```yaml
inherits: default-dark
```

* **單一父代**。不支援多重繼承 —— 多重繼承需要定義線性化順序，
  四個解析器實作出四種順序的風險遠大於它的價值。
* 父代以 §2.3 的搜尋路徑解析。
* 鏈長上限 8（含自身）。成環或超長 → 致命錯誤 F6。
* 主題只能繼承主題，佈局只能繼承佈局。

### 7.2 合併規則

自根祖先向下逐份合併，**後者勝出**：

| 子文件的節點 | 結果 |
|---|---|
| 映射，且父為映射 | **遞迴合併**（逐 key） |
| 映射，但父為純量或序列 | 整體取代 |
| 序列 | **整體取代**，永不逐元素合併 |
| 純量 | 取代 |
| 顯式 `null` | **刪除**該 key（該欄位回到規範預設值，而非父值） |

**為什麼序列不合併：** 序列在本格式中承載三種東西 —— 字體 fallback 順序、
鍵盤 rows、popup keys。這三者的語義都是「有序且完整的一份清單」。
逐元素合併對它們全部無意義（合併第 3 個 key 是什麼意思？），
而按索引覆寫則會在父代新增一個鍵時整份錯位。整體取代是唯一不會靜默出錯的規則。

**代價（誠實說明）：** 這使得「只想改佈局裡的一個鍵」必須整份重寫 `layers`。
§9.7 的 `key_patches` 就是為此而設的逃生口。

### 7.3 不參與繼承的欄位

下列欄位**永遠取自最終文件本身**，不從父代繼承：

`format`、`id`、`revision`、`inherits`、`min_client`

`name` / `description` / `author` / `license` / `appearance` / `counterpart`
**會**繼承（子文件通常會覆寫 `name` 與 `appearance`）。

### 7.4 完整解析管線（規範性順序）

```
1. 讀取並解析 YAML → 節點樹                        （F1、F2）
2. 檢查 format / id / min_client                    （F3、F4、F7）
3. 遞迴解析 inherits，取得祖先鏈                     （F5、F6）
4. 自最遠祖先向下 deep-merge（§7.2）
5. 套用 platform_overrides.<當前平台>（同樣用 §7.2 的規則 deep-merge）
6. 綁定欄位、填入預設值、型別轉換、範圍夾制          （§6.3）
7. 解析 palette（含遞迴 $ref）
8. 解析所有 color 欄位中的 $ref
9. 結構性驗證（佈局的 F8–F10、layer 參照完整性）
10. 回傳結果物件 + 診斷清單
```

第 5 步的平台鍵：`android`、`ios`、`macos`、`windows`。
**未知平台鍵必須被忽略且不產生 WARNING** —— 這是新增平台時的前向相容機制。

`platform_overrides` 本身 **不得** 出現在 `platform_overrides` 內（巢狀 → 忽略 + WARNING）。

---

## 8. 主題格式（`rime-theme/1`）

### 8.1 文件層級欄位

| 欄位 | 型別 | 預設 | 平台 | 說明 |
|---|---|---|---|---|
| `format` | string | — **必填** | 全 | `rime-theme/1` |
| `id` | string | — **必填** | 全 | 見 §2.2 |
| `revision` | int | `1` | 全 | 見 §5.5 |
| `name` | localized-string | `""` | 全 | 顯示名稱 |
| `description` | localized-string | `""` | 全 | |
| `author` | string | `""` | 全 | |
| `license` | string | `""` | 全 | SPDX 識別碼為佳 |
| `appearance` | enum `light` \| `dark` | `light` | 全 | 本檔描述哪一種外觀 |
| `counterpart` | string \| null | `null` | 全 | 另一外觀的主題 id，見 §8.2 |
| `inherits` | string \| null | `null` | 全 | §7.1 |
| `min_client` | string \| null | `null` | 全 | §5.4 |

#### 8.2 深色／淺色：為什麼是兩個檔案

**決定：一份檔案描述且僅描述一種外觀（`appearance`），深淺以 `counterpart` 成對。**
被否決的替代方案是單檔內分 `light:` / `dark:` 兩個區塊。

理由：

1. **深淺不是只有顏色不同。** `default-light` 的 `elevation` 是 3、`bar.border_top_width`
   是 1；`default-dark` 分別是 2 與 0 —— 深色介面靠明度分層，淺色介面靠陰影與描邊分層。
   單檔雙區塊的寫法會誘導作者以為「只要換顏色」，然後產出在另一個外觀下發悶的主題。
2. **繼承規則不必翻倍。** 若單檔內有兩個平行子樹，§7.2 的合併必須定義
   「父的 light 與子的 dark 如何互動」，而任何答案都不直觀。
3. **載入成本。** iOS 鍵盤擴展的記憶體額度只有數十 MB，且外觀在載入時就已知。
   只讀一份文件、只建一組物件，比讀兩份丟一份好。
4. **可分享性。** 使用者要分享「我的深色主題」時給的是一個檔案，而不是一份包含
   他沒調過的淺色設定的檔案。

代價是變體主題要寫兩份檔案。`core/themes/sakura-light.yaml` 與 `sakura-dark.yaml`
就是用來證明這個代價很小：靠 `inherits`，兩份加起來不到 90 行。

執行期的外觀選擇（規範性）：

1. 使用者若明確指定了某個主題 id → 直接用它，**不做** 深淺切換。
2. 使用者若選擇「跟隨系統」→ 取當前主題的 `appearance`；若與系統外觀不符且
   `counterpart` 非空且可載入 → 改用 `counterpart`。
3. `counterpart` 載入失敗 → 沿用當前主題 + WARNING（**不是**致命錯誤）。

### 8.3 `palette`

型別：`map<string, color>`，預設 `{}`。平台：全。

Key 為 §4.7 的 `name`。值可為 hex 或另一個 `$ref`（深度上限 8）。
本區塊在繼承中**逐 key 合併**（它是映射）。

### 8.4 `typography`

| 欄位 | 型別 | 預設 | 平台 |
|---|---|---|---|
| `respect_system_font_scale` | bool | `true` | 全 |
| `font_scale_min` | number 0.5–1.0 | `0.85` | 全 |
| `font_scale_max` | number 1.0–2.0 | `1.30` | 全 |
| `fonts` | map<string, font-stack> | 見下 | 全 |
| `assets` | list<font-asset> | `[]` | 全（**可**忽略） |

**具名字體堆疊（font-stack）。** `fonts` 是開放映射，但下列名稱有規範語義，
且解析器 **必須** 在缺失時以內建預設補齊：

| 名稱 | 用於 |
|---|---|
| `ui` | 一般 UI 文字（工具列、設定） |
| `candidate` | 候選文字 |
| `label` | 候選序號標籤 |
| `comment` | 候選註解 |
| `key` | 鍵面文字 |
| `preedit` | 組字串 |

font-stack 欄位：

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `family` | string-list | `["$system"]` | 有序 fallback |
| `weight` | int 100–900 | `400` | 四捨五入到最近的百位 |
| `italic` | bool | `false` | |
| `script_fallback` | map<script-tag, string-list> | `{}` | 見 §8.4.2 |

font-asset 欄位：`family`（string，必填）、`file`（string，必填，相對路徑）、
`weight`（int，預設 400）、`italic`（bool，預設 false）。

#### 8.4.1 通用字體代號

`family` 清單中以 `$` 開頭者為代號，由各端映射到自己的系統字體。
**未知代號必須被當作一般字體名處理**（而非報錯），以利未來新增。

| 代號 | Android | iOS / macOS | Windows |
|---|---|---|---|
| `$system` | `sans-serif`（Roboto） | `.AppleSystemUIFont` | `Segoe UI Variable` → `Segoe UI` |
| `$serif` | `serif`（Noto Serif） | `New York` | `Georgia` |
| `$mono` | `monospace` | `SF Mono` → `Menlo` | `Cascadia Mono` → `Consolas` |
| `$rounded` | `sans-serif`（無圓體則退回） | `SF Pro Rounded` | `Segoe UI Variable`（無則退回） |
| `$emoji` | `Noto Color Emoji` | `Apple Color Emoji` | `Segoe UI Emoji` |
| `$system-hant` | `Noto Sans CJK TC` | `PingFang TC` | `Microsoft JhengHei UI` |
| `$system-hans` | `Noto Sans CJK SC` | `PingFang SC` | `Microsoft YaHei UI` |
| `$system-jpan` | `Noto Sans CJK JP` | `Hiragino Sans` | `Yu Gothic UI` |
| `$system-kore` | `Noto Sans CJK KR` | `Apple SD Gothic Neo` | `Malgun Gothic` |

#### 8.4.2 CJK fallback（本規範最容易做錯的一段）

問題：`家` 這個字在 `Noto Sans CJK TC`、`SC`、`JP`、`KR` 四個字體中的字形都不同，
但 Unicode 碼位完全相同。單一 `family` 清單無法表達「同一個碼位，依語境選不同字體」。

解法：`script_fallback` 依 ISO 15924 小寫 script tag 提供分支清單。

**最終字體解析順序（規範性）：**

```
resolved(run) = script_fallback[script_of(run)]   (若存在)
              ++ family
              ++ <平台預設字體>
```

`++` 為串接，重複項保留首次出現。逐字形（per-glyph）在此清單中找第一個
含有該字形的字體。無法做逐字形 fallback 的渲染管線 **應** 退化為
「用清單第一個成功載入的字體渲染整段」。

**`script_of(run)` 的判定（規範性）：**

1. run 中含平假名或片假名 → `jpan`
2. run 中含諺文 → `kore`
3. run 中含注音符號（U+3100–U+312F、U+31A0–U+31BF）→ `bopo`
4. run 全為拉丁／數字／ASCII 標點 → `latn`
5. run 含中日韓統一表意文字 → **依當前狀態決定**：
   `rs_status.is_simplified == true` → `hans`，否則 → `hant`
6. 其他 → `zyyy`

第 5 條是關鍵妥協：漢字本身不帶語言資訊，唯一可靠的訊號是使用者當前的方案／簡繁開關。
四端 **必須** 都用這條規則，否則同一份主題在 macOS 上顯示日文字形、
在 Android 上顯示繁體字形，而使用者無從得知為什麼。

`script_fallback` 中出現未知 script tag → 忽略該筆 + WARNING。

### 8.5 `metrics`

| 欄位 | 型別 | 預設 | 平台 |
|---|---|---|---|
| `corner_radius` | length 0–64 | `8` | 全 |
| `border_width` | length 0–8 | `0` | 全 |
| `padding` | length 0–64 | `6` | 全 |
| `spacing` | length 0–64 | `4` | 全 |
| `elevation` | length 0–32 | `2` | 全 |

本區塊是**建議值來源**，不直接被畫出來。具體欄位（如 `candidates.item.corner_radius`）
若未指定，其預設值取自本區塊的對應項；本區塊也未指定時才用表列的字面預設。

### 8.6 `candidates`

共用區塊，行動端候選列與桌面端候選窗**都**消費。

| 欄位 | 型別 | 預設 | 平台 |
|---|---|---|---|
| `orientation` | enum `horizontal` \| `vertical` | `horizontal` | 全 |

#### 8.6.0 文字區塊的字體綁定（規範性）

§8.4 定義了具名字體堆疊，但**沒有說哪一塊文字用哪一個堆疊** ——
Windows 端因此整個候選窗退回系統 UI 字型，只套規範裡有的 `size`。
結果是「主題指定了候選字體」這件事在桌面端完全沒有效果。這一節補上綁定。

**預設綁定（規範性）：**

| 區塊 | 預設字體堆疊 |
|---|---|
| `candidates.label` | `typography.fonts.label` |
| `candidates.text` | `typography.fonts.candidate` |
| `candidates.comment` | `typography.fonts.comment` |
| `preedit` | `typography.fonts.preedit` |
| `status_bar`（§8.12） | `typography.fonts.ui` |
| `candidates.bar.toolbar`（§8.6.6.1） | `typography.fonts.ui` |
| `keyboard.key_styles.*`（§8.8.1） | `typography.fonts.key` |

上表的六個堆疊名稱在 §8.4 已經是規範性的，且解析器**必須**在缺失時以內建預設補齊，
所以綁定永遠解得出東西，不需要「找不到就用系統字型」這條退路。

**桌面端不得改用系統 UI 字型當預設。** `$system` 代號（§8.4.1）已經是那個意思，
而走代號才能讓「主題指定了字體」與「主題沒指定，用系統字體」是**同一條程式路徑**。
兩條路徑的實作必然分岔：Windows 目前就是「有指定也不看」。

**每個文字區塊新增一個 `font` 欄位（選用）：**

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `font` | string | 見上表 | `typography.fonts` 的**鍵名**，不是字體家族名 |

寫的是鍵名而不是家族名，是為了讓一份主題只在 `typography.fonts` 一個地方描述字體。
候選窗與鍵盤各寫一份 `family: [...]` 的話，換字體要改兩處，而漏改的那一處
不會有任何診斷。

解析規則（規範性）：

1. `font` 缺席 → 用上表的預設鍵名。
2. `font` 的值出現在**合併後**的 `typography.fonts` 裡 → 用它。
   （合併後：`inherits` 帶進來的鍵也算數，見 §7.2 映射逐鍵合併。）
3. 否則 → 退回上表的預設鍵名，並發一則 WARNING `bad_enum`，
   `args = [寫下的值, 合併後 fonts 的所有鍵名以 `,` 串接（依 §3.3 的出現順序）, 預設鍵名]`。

`font` **只**決定 `family` / `weight` / `italic` / `script_fallback`。
**字級一律取該區塊自己的 `size`**，不從堆疊來 —— `fonts` 沒有字級欄位，
而 `candidates.label.size` 與 `candidates.text.size` 本來就必須能分開設。


#### 8.6.1 `candidates.label`（序號標籤）

| 欄位 | 型別 | 預設 | 平台 |
|---|---|---|---|
| `show` | bool | `true` | 全 |
| `format` | string | `"{label}"` | 全 |
| `size` | size | `12` | 全 |
| `color` | color | `#808080` | 全 |
| `highlight_color` | color | = `color` | 全 |

`format` 支援的佔位符：`{label}`（librime 給的標籤，見 `rs_candidate.label`）、
`{index}`（本頁 1 起算）、`{index0}`（本頁 0 起算）。
未知佔位符 **必須** 原樣保留（不得丟棄，也不得報錯）。

> 桌面端的慣例是顯示 `1. 候選`，行動端候選列通常關掉標籤。
> 因此 `bar` 與 `window` 都能各自覆寫 `label.show`（§8.6.6/8.6.7）。

##### 8.6.1.1 序號「按下去真的選得到」才畫（規範性，僅行動端 `candidates.bar`）

序號 `1 2 3` 只有一個用途：讓使用者**按數字鍵**選第 N 個。按不到的序號是
畫面上一段每格都要付寬度的裝飾（實測 9.90 dp/格 = 一格的 9.1%），
而且它承諾了一件做不到的事。

因此行動端候選列 **必須** 在下面兩個條件**同時**成立時才畫序號；
任何一個問不出來就**不畫**（fail-closed）：

1. **當前層直接按得到整排 `1`–`9`。** 判準是九顆鍵各自有 `send.keysym`
   落在 `1`–`9` 且不帶 modifier。
   `hint` **不算**（鍵面角落的小字不送出任何東西）；
   `swipe` **不算**（§9.6 明訂 swipe 為 OPTIONAL，行動端可以不實作）；
   `popup` / `long_press` **不算** —— 序號 `3` 承諾的是「按 3」，
   不是「長按某顆鍵再從盤裡挑 3」。
2. **`(佈局, 方案)` 這一格在真機上量過而且按得到。**

> **為什麼第 1 條要「九顆全在」而不是「有任何一顆」。** 實測（emulator-5558，
> 2026-08-13）四份會亮的佈局裡**三份**在「有任何一顆」之下是錯的：
> `bopomofo-dachen/bopomofo` 的 ㄅ=1、ㄉ=2、ˇ=3 是**大千鍵位**真的送那些
> keysym（那一條擋不住它，擋它的是第 2 條）；`t9-pinyin/t9` 只有 `k1`
> 送得出 `1`，其餘八顆什麼都不送（這一條當場擋下）。

⛔ **第 2 條不得由靜態判準取代。** 「鍵送得出 `3`」與「按 `3` 會選第 3 個」
之間隔著 librime 的兩層攔截，兩層都**不在佈局檔裡**：

| 攔截 | 它做什麼 | 實例 |
|---|---|---|
| `speller/alphabet` | 數字是**字母**時 speller 先收走 | `bopomofo` 的 alphabet 是 `'1qaz2wsx…6347'`；`3` 在「聲調還沒打」時是合法的下一個字母 |
| `recognizer/patterns` | 整串輸入落在某個 pattern 裡時，後續字元被 recognizer 收走 | `default.yaml` 的 `uppercase: "[A-Z][-_+.'0-9A-Za-z]*$"` 字元集**含 `0-9`**，而九宮格刻意送大寫 `A/D/G/J/M/P/T/W` |

而且第 2 條的答案**會隨渲染端怎麼送鍵而翻面**。所以本規範要的是**保守的
常數答案**：只要那個 `(佈局, 方案)` 在那一端存在任何一個「按下去不選字」的
常見狀態，整格就是**不畫**。

> ⚠ **本節上一版拿 `cn-t9-pinyin-numrow` × `t9_pinyin` 當「會翻面」的例子
> （`MGGAM` 不行、消歧成 `niGAM` 之後可以）—— 那一句已經過期。**
> 2026-08-14 於 `lumina_test2` 重量，那一格是 `yes`（`MGGAM` 按 `3` 上屏
> 第 3 個候選）；`core/selection-digit.tsv` 是唯一的真相，本文件只是抄錄。
> 它翻面是因為 Android 端在**門面層**攔下了專用數字列的 `1`–`9`
> （工單 #99），而**那套機制目前不在本規範裡** —— 見 `docs/coordination.md`。
> 這一段留著是因為它說明的紀律沒有變：答案是**那一端量出來的**，
> 而且必須是常數，不是隨組字狀態閃爍的。

⚠ **視覺上不畫序號與朗讀時不念序號是兩件事。** 無障礙朗讀 **必須** 永遠
帶序號（「第三個，你好」）—— 使用者說「我要第三個」靠的是後者。

##### 8.6.1.1.1 `core/selection-digit.tsv`（規範性，四端共用資料檔）

實測表 **必須** 是量出來的、不是手寫的（手寫的表從寫下的那一刻就開始腐爛）。
它放在 `core/`，四端共用；本 repo 由 `scripts/verify_selection_digit.sh`
（同時是產生器 `--bless` 與斷言者）維護。

⚠ **上一版把「格式由渲染端決定」寫進規範，而這個檔案在 `core/` 且被打進
Android 的 APK assets —— 一個四端共用的資料檔沒有格式規範，就是四端各解各的。**
所以格式在這裡定死：

**六欄，TAB 分隔；`#` 開頭與空行是註解。**

| # | 欄 | 內容 |
|---|---|---|
| 1 | `layout` | 佈局 id（`core/layouts/<id>.yaml` 的 `id`） |
| 2 | `schema` | 方案 id |
| 3 | `verdict` | `yes` = 按得到；**其餘任何值**（`no` / `unknown` / 空）一律當成按不到 |
| 4 | `compose` | 量測時打的那一串按鍵，讓別人重現得了 |
| 5 | `measured_on` | 量測環境（機型／螢幕／日期） |
| 6 | `note` | 為什麼 —— 尤其 `no` 的那幾列 |

* 讀取端 **必須** 只信第 3 欄，而且 **必須** fail-closed：檔案讀不到、
  解析不了、查無此列 —— 一律**不畫**。
* 讀取端 **可以** 只讀前三欄（第 4–6 欄是給人看的、給重現用的），但
  **不得** 因為某一列少了後三欄就把整份表判成無效。

⛔ **一端量出來的列，另一端 MUST NOT 直接沿用。** 第 1、2 欄是四端共用的
id，但第 3 欄的答案取決於**那一端自己**怎麼把按鍵送進 librime
（Android 的九宮格送大寫 `A/D/G/J/M/P/T/W` 才踩到 `recognizer`；
另一端若送小寫就是另一個答案）。所以：

* 表 **必須** 能同時容納多端的量測結果。本 repo 目前只有 Android 量過，
  第 5 欄因此都是 Android 的模擬器 —— **那不代表 iOS 可以照抄**。
* iOS **必須** 自己量過才畫。照字面 fail-closed 的結果是「iOS 在量出來
  之前一格序號都不畫」—— **那是正確的**，不是缺陷：少一段版面提示，
  換到的是不會交付一次「看得到、按不到」。
* 量測環境不同而結論不同時，**不得**互相覆蓋。要嘛第 5 欄記清楚是哪一端
  量的並各佔一列，要嘛取交集（`yes` ∧ `yes`）。本 repo 現行是前者。

#### 8.6.2 `candidates.text`

| 欄位 | 型別 | 預設 |
|---|---|---|
| `size` | size | `20` |
| `color` | color | `#000000` |
| `highlight_color` | color | = `color` |

#### 8.6.3 `candidates.comment`（註解，如拼音提示）

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `show` | bool | `true` | |
| `position` | enum `after` \| `below` \| `hidden` | `after` | `after` = 同一行接在候選文字之後；`below` = 置於候選文字下方（垂直候選窗常用） |
| `size` | size | `12` | |
| `color` | color | `#808080` | |
| `highlight_color` | color | = `color` | |

`position: below` 在 `orientation: horizontal` 下 **必須** 被支援
（候選項變成兩行高），這是桌面端很常見的排法。

##### 8.6.3.1 註解與消歧欄互斥（規範性，僅行動端 `candidates.bar`）

`candidates.syllables`（§8.6.6.3）畫的內容，是候選的 `comment` 逐則套
`reading_of()` 之後去重的結果。註解畫的就是 `comment` 原文。也就是說
**同一份讀音被畫了兩次，而只有註解要付寬度** —— 實測 38.48 dp/格
（emulator-5558，411.43 dp，九宮格）＝ 一格的 **35.3%**，
關掉它把「一列排得下幾個」從 3 變成 4 完整 ＋ 第 5 個露一半。

規則：

* 若消歧欄那一側**畫得出來**，而且本輪的 `reading_of()` 結果**非空**
  → 行動端候選列 **不得** 畫註解。
* 否則 **必須** 照畫。

`reading_of(comment)`（規範性）：取 `comment` 以空白切開後的**第一段**；
該段**全部**落在 `a`–`z` 才算一個讀音，否則回空。
因此 `terra_pinyin` 的 `nǐ`（帶聲調）與 `simplifier` 的 `〔简〕`
解析不出讀音，**照畫** —— 那些本來就不是讀音。

⛔ **「消歧欄畫得出來」是前提，不得省略。** 消歧欄那一側根本不會出現的時候
壓掉註解，是**把讀音憑空刪掉**：使用者哪裡都看不到它，而畫面完全正常。
兩種必須照畫的狀態：

| 狀態 | 為什麼消歧欄不出現 |
|---|---|
| `candidates.syllables.placement: none` | 主題自己關掉了 |
| 引擎改寫不了輸入串（§8.6.6.3 的退化規則三） | 前端沒有東西可以接 |

⚠ **兩邊的門檻刻意不同：註解是「≥ 1 個讀音就關」，消歧欄是「≥ 2 個才開」。**
相同的話，讀音收斂到只剩一個的那一瞬間消歧欄收起來、註解同時跑出來，
每一格寬度一起改變 —— 使用者正在挑字，而整列在他眼前重排。
所以「只有一個讀音」時兩邊都不畫，這是**刻意的取捨**，不是缺陷。

⚠ 桌面端的 `candidates.window` **不套用本條**：桌面端不畫消歧欄，
關掉註解等於憑空少一份資訊、沒有任何東西補上。

#### 8.6.4 `candidates.item`

| 欄位 | 型別 | 預設 |
|---|---|---|
| `padding_h` | length | `metrics.padding` |
| `padding_v` | length | `metrics.padding` |
| `spacing` | length | `metrics.spacing` |
| `corner_radius` | length | `metrics.corner_radius` |
| `min_width` | length | `0` |
| `background` | color | `transparent` |
| `highlight_background` | color | `#3060C0` |
| `border_width` | length | `0` |
| `border_color` | color | `transparent` |
| `highlight_border_width` | length | `0` |
| `highlight_border_color` | color | `transparent` |

⚠ **還有一個 `item` 欄位不在這張表上：`highlight_style`。** 它的作用域是
**`candidates.bar.item` 專屬**（欄位表見 §8.6.6，語義見 §8.6.4.3）——
寫在這裡的共用 `candidates.item` 或桌面端的 `candidates.window.item`
底下，四端**一律**產生 `unknown_field` 且不生效（§10 第 44 條）。
放在共用表裡會讓實作者以為它到處都合法，而那正是 §10 第 9 條
「四端診斷序列必須相同」最難查的一種紅。

#### 8.6.4.1 item 內部的間距與量測（規範性）

§11 原本列著「候選窗的量測與排版是分開的兩件事，而格式只規範了後者」——
§8.6.7.1 的輸入是「每一項的寬高」，但那個寬高**怎麼量**沒有規範。
Windows 端因此暫時拿 item **之間**的 `metrics.spacing` 來當 item **內部**的間距。
這一節補上它。**新增欄位全部有預設值，預設等同既有行為，不遞增 major（§5.3）。**

一個候選項由三段組成：**標籤**（§8.6.1）、**候選文字**（§8.6.2）、**註解**（§8.6.3）。
`candidates.item` 新增三個欄位描述它們之間的距離：

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `label_gap` | length 0–64 | `metrics.spacing` | 標籤與候選文字之間 |
| `comment_gap` | length 0–64 | `metrics.spacing` | `comment.position: after` 時，候選文字與註解之間 |
| `comment_gap_v` | length 0–64 | `2` | `comment.position: below` 時，兩行之間的垂直距離 |

前兩者的預設值刻意取 `metrics.spacing`（與 `item.spacing` 同源），
這正是 Windows 端目前的行為 —— **既有實作不必改就已經合規**，
而想把「項內」與「項間」分開的主題現在有辦法說出口。

##### 量測演算法（規範性）

輸入：三段各自的量測寬高 `lw/lh`（標籤）、`tw/th`（候選文字）、`cw/ch`（註解）。
各段以 §8.6.0 綁定的字體堆疊與自己的 `size` 量測。

```
0. 空段一律 w = 0、h = 0，且**不參與**任何間距：
     label.show == false                      → lw = lh = 0
     格式化後的標籤為空字串                    → lw = lh = 0
     comment.show == false 或 position: hidden → cw = ch = 0
     候選的 comment 為空字串                   → cw = ch = 0

1. position 為 after（或註解為空）：
     inner_w = lw
             + (lw > 0 && tw > 0 ? label_gap   : 0) + tw
             + (tw > 0 && cw > 0 ? comment_gap : 0) + cw
     inner_h = max(lh, th, ch)

2. position 為 below：
     top_w   = lw + (lw > 0 && tw > 0 ? label_gap : 0) + tw
     inner_w = max(top_w, cw)
     inner_h = max(lh, th) + (ch > 0 ? comment_gap_v + ch : 0)

3. 項的量測尺寸 —— 這就是 §8.6.7.1 的輸入 w[i] / h[i]：
     w[i] = max(item.min_width, inner_w + 2 * item.padding_h)
     h[i] =                     inner_h + 2 * item.padding_v
```

三條容易做錯的地方：

* **`min_width` 夾在加上 padding 之後**，不是之前。§8.6.7.1 第 8 步的
  `max(item.min_width, ...)` 夾的是欄寬，兩處必須是同一個座標系，否則
  `min_width` 在單行與多行排版下的意思會不一樣。
* **空段不留空隙。** 標籤關掉時 `inner_w` 少的是 `lw + label_gap`，
  不是只少 `lw`。留下來的那一格空白看起來像對齊錯誤，而且沒有任何線索。
  這與 §8.12「空狀態必須整項略過」是同一條規則。
* **`h[i]` 是逐項的**，§8.6.7.1 第 1 步再取全頁最大值當格高。
  `position: below` 時只有帶註解的那幾項是兩行高，取最大值才不會互相蓋住
  （§10 第 22 條）。

##### 這一節不規範什麼

**字形本身的量測。** `lw` / `tw` / `cw` 由平台的排版引擎決定（advance width、
kerning、行高），四端不可能逐 px 相同，規範它只會產生沒有人做得到的要求。
所以檢核（§10 第 26–27 條）驗的是「**給定** lw/tw/cw，算出的 w/h」，
不是「同一個字串在兩端寬度相同」。


#### 8.6.4.2 候選列的可用寬度與密度下界（規範性，僅行動端 `candidates.bar`）

§8.6.4.1 算的是**一格**多寬。這一節算的是**一列排得下幾格**。

##### 可用寬度（規範性）

```
usable = screen_width
       − 2 × bar.padding_h        ← 候選列自己的左右內距
       − reserved                 ← 右端**真的會畫出來**的那幾顆（§8.6.6.4）
       − leading                  ← 候選之前那一段（見下）

# 一列**完整**畫得出幾個(w[i] 由 §8.6.4.1 給出)
n = 最大的 k，使得  Σ(i<k) w[i] + item.spacing × (k − 1) ≤ usable
```

三件容易做錯的事：

* **`leading` 不得漏扣。** `bar.show_preedit_inline` 的行內組字串與右端保留區
  一樣是真的擠掉候選的。實測（411.43 dp、`luna_pinyin_tw` 打 `ni`）那一塊
  **33.7 dp**；不扣它時模型說排得下 7 個而畫面只畫得出 6 個。
* **`reserved` 是「畫出來的那幾顆」，不是「可能會畫的那幾顆」**（§8.6.6.4）。
* ⛔ **只數完整畫得出來的。** 畫一半的那一格對使用者而言是「右邊還有東西」
  的提示，不是一個可以放心點的候選。這個數字是 §8.6.6.4 判斷「本頁看完了沒」
  的依據 —— **多算一個就等於給出一顆讓使用者跳過他沒看見的候選的翻頁鍵。**

⚠ **量測寬度只准估寬，不准估窄。** §8.6.4.1 自承不規範字形量測，所以 `w[i]`
是估計值。方向必須固定成**估寬**（本 repo 取拉丁 0.55 em、CJK 1 em）：
估寬的代價是少排一個候選；估窄的代價是把一個畫不出來的候選當成畫得出來，
而那會退化成上一段那顆不誠實的翻頁鍵。
因此 `n` 是**下界** —— 畫面上**可以**多出一個被裁掉一半的格子
（實測 `font_scale: 1.30` 就會），那不違規：它可以被捲到、可以被點，
而且此時 `n < 本頁候選數`，§8.6.6.4 保證右端一定有出口。
⛔ 反過來則違規：`n` **不得**大於畫面上完整畫出來的格數。

##### 空白必須由 `padding_h` 提供，不得由 `spacing` 提供

兩者畫出來一模一樣，但 **`padding` 算在觸控目標裡、`spacing` 不算**。
照著別家的截圖抄間距，會抄出一排看起來很舒服、卻低於 §3.6 觸控下界的候選。
`item.min_width` 是同一件事的下界保險。

##### 密度下界的基準情境（規範性）

⛔ **基準情境 MUST 固定 `reserved` 與 `leading`，否則兩端會得出相反的判決。**

```
reserved = bar.reserved_end     ← **一顆**控制鍵（不是兩顆、也不是 0）
leading  = 0                    ← 基準情境不含行內組字串
```

> 上一版只固定了 `text.size: 20`、兩字 CJK、無序號無註解，**沒有說 `reserved`
> 與 `leading` 取什麼**。Android 取「一顆 ＋ 0」；取「兩顆」的實作在 411 dp 上
> 必然判紅（80 dp 對 40 dp 在 411.43 dp 上**恰好差一個候選**，5 vs 6，
> 見 §8.6.6.4 第四段）—— **同一份主題,兩端一綠一紅**,而兩邊都能指著規範說
> 自己照做了。取「一顆」的理由：那是「還有更多」時右端最常見的形狀，
> 而密度下界要量的是主題的固定開銷，不是最壞頁況。

##### 密度下界（規範性）

基準情境固定 **`text.size: 20`、兩字 CJK、無序號無註解** —— 量的是主題往
一格裡加的**固定開銷**（內距、間距、最小寬、右端保留區），
**不是主題自己的字級**（不然「字調大一點」就自動合格了）。

| 螢幕寬 | 一列至少 |
|---|---|
| 360 dp | **5** 個 |
| 411 dp | **6** 個 |
| 456 dp | **6** 個 |

`core/themes/` 底下**每一份**主題都 **必須** 通過（掃目錄，不是白名單）。

> 這是下界式檢核，天生比等式鬆 —— 它擋得住「某一份主題悄悄多加 10 dp 內距」，
> 擋不住「某端只差 1 dp 就少一個」。已知限制，不是疏忽。

#### 8.6.4.3 `item.highlight_style`：高亮的畫法（規範性，僅 `candidates.bar.item`）

| 值 | 畫法 |
|---|---|
| `fill`（**預設**） | 整格鋪滿 `item.highlight_background` 的實心塊 |
| `underline` | 候選文字底下一條 2 dp、顏色為 `item.highlight_background` |
| `outline` | 描邊，寬度 `item.highlight_border_width`、顏色 `item.highlight_border_color` |

⛔ **三種畫法的量測寬度必須完全相同。** 不然使用者每移動一次選字，
整列就在他眼前重排一次。實測把 `highlight_background` 換成 `transparent`
之後，每一段墨跡座標與原版**逐 px 相同** —— 也就是說那個大色塊的寬度成本
是 **0 dp**。它不是密度的成因；它是讓「每格 10 dp 內距」看起來合理的那個
**理由**。

⚠ 非 `fill` 的兩種 **不得** 使用 `text.highlight_color`：那個顏色是設計來畫在
重點色實心塊上的（隨附主題是 `$on_accent` ＝ 白），底色換回 surface 之後
白字畫在白底上就是**看不見** —— 高亮的那個候選會整個消失，而畫面看起來
一切正常。這兩種畫法 **應** 改用 `item.highlight_background` 當前景色
（它與 surface 的對比是主題自己保證過的）。

⚠ `outline` 在 `highlight_border_width: 0` 的主題上 **必須** 退回一條看得見的
邊（本 repo 取 1 dp），**不得**靜靜地變成「沒有高亮」。

⚠ 作用域見 §8.6.4 表格底下那一段與 §10 第 44 條。

#### 8.6.5 `candidates.separator` / `candidates.page_indicator`

`separator`：`show`（bool，`false`）、`color`（color，`#808080`）、`width`（length，`1`）。

`page_indicator`：`show`（bool，`true`）、
`style`（enum `arrows` \| `dots` \| `text` \| `none`，預設 `arrows`）、
`color`（color，`#808080`）、`disabled_color`（color，= `color`）、`size`（size，`14`）。

`style: text` 表示以 `n/m` 形式顯示頁碼。

#### 8.6.6 `candidates.bar` — 行動端候選列（僅 Android / iOS）

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `height` | length 24–96 | `44` | 候選列高度，**加在**鍵盤高度之上 |
| `padding_h` | length 0–48 | `4` | 候選列自己的左右內距，見 §8.6.4.2 的 `usable` |
| `reserved_end` | length 0–96 | `40` | 右端**第一顆**控制鍵的寬度預算，見 §8.6.6.4 |
| `background` | color | `#FFFFFF` | |
| `orientation` | enum | 繼承 `candidates.orientation` | |
| `border_top_width` | length | `0` | |
| `border_top_color` | color | `transparent` | |
| `max_visible` | int ≥ 0 | `0` | `0` = 有多少畫多少 |
| `scroll` | enum `none` \| `horizontal` \| `expandable` | `expandable` | `expandable` = 可下拉展開成多列面板 |
| `expand_button.show` | bool | `true` | |
| `expand_button.color` | color | = `label.color` | |
| `expand_button.size` | size | `18` | |
| `show_preedit_inline` | bool | `true` | 組字串顯示在候選列左端而非獨立區塊 |
| `empty_shows_toolbar` | bool | `true` | 無候選時候選列改顯示工具列，見 §8.6.6.1 |
| `toolbar` | toolbar | 見 §8.6.6.1 | 工具列內容 |

`bar` **可** 覆寫 §8.6.1–8.6.5 的任一子區塊（同名子區塊寫在 `bar` 之下即可）。

`bar.item` 除了 §8.6.4 的欄位之外，還多一個**只在這裡合法**的欄位：

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `highlight_style` | enum `fill` \| `underline` \| `outline` | `fill` | 高亮的畫法，語義見 §8.6.4.3 |

⛔ **`highlight_style` 的作用域就是 `candidates.bar.item`，不多也不少。**
寫在共用的 `candidates.item` 或桌面端的 `candidates.window.item` 底下，
四端**一律**產生 `unknown_field` 且不生效（§10 第 44 條）。桌面端的候選窗
沒有「六個並排時大色塊會蓋掉其餘五個」這個問題，所以那裡刻意不開這個欄位。

⚠ **`padding_h` / `reserved_end` / `bar.item.highlight_style` 三個欄位依 §10
第 9 條的作用域表屬於 `candidates.bar`，也就是 Android **與 iOS** 都必須
認得。** 桌面兩端不 descend 進 `candidates.bar`，對它們而言這三個欄位不存在
—— 那是正確的，**不要**把它們補進桌面端的 key set。

##### 8.6.6.1 `candidates.bar.toolbar`

行動端沒有選單列，所以「無候選時的候選列」實際上承擔了**主要導覽**功能 ——
切換方案的唯一入口就在這裡。因此它必須被規範，不能留給各端自由發揮。

| 欄位 | 型別 | 預設 |
|---|---|---|
| `show` | bool | `true` |
| `items` | list<toolbar-item> | 見下方預設清單 |

**toolbar-item** 欄位：

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `icon` | string \| null | `null` | §9.6 的語義圖示名 |
| `label` | string | `""` | 無圖示時的文字 |
| `label_from` | enum | `none` | 同 §9.6，用執行期狀態當文字 |
| `tap` | action | — **必填** | §9.5 的 action 字串 |

鍵面文字的解析、`icon` 的退化、`active` 的觸發條件，
與按鍵完全相同（§9.6、§8.8.1）—— 工具列項目就是「沒有 `send` 的鍵」。

**預設清單（規範性，`items` 缺席時必須產生這一份）：**

```yaml
items:
  - { icon: "globe",         tap: "schema:picker" }
  - { label_from: input_mode, tap: "toggle:ascii_mode" }
  - { label_from: variant,    tap: "toggle:simplification" }
  - { icon: "emoji",          tap: "emoji" }
  - { icon: "settings",       tap: "settings" }
  - { icon: "keyboard_hide",  tap: "hide_keyboard" }
```

**必備項（規範性）：** 不論主題怎麼寫，實作 **必須** 保證使用者能觸達
`schema:picker` 與 `settings`。若解析後的 `items` 不含這兩者，
實作 **必須** 自行補上，並產生 INFO 級診斷。

> 理由很直接：`schema:picker` 是方案切換的唯一入口，`settings` 是修好一切
> 其他問題的入口。允許主題把它們刪掉，等於允許主題把使用者鎖死在
> 一個他無法離開的方案裡。這不是美學自由該涵蓋的範圍。

`tap` 缺席或無法解析的項目 → 丟棄該項 + WARNING（不影響其他項）。

##### 8.6.6.2 工具列的外觀（僅行動端）

§8.6.6.1 規定了工具列的**內容**與必備項，但排列方式、間距、能不能捲動
都還沒定義（§11 列著）。補上：

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `arrangement` | enum `leading` \| `center` \| `space_between` \| `space_evenly` | `leading` | |
| `spacing` | length 0–64 | `2` | 項與項之間 |
| `padding_h` | length 0–64 | `4` | |
| `padding_v` | length 0–64 | `3` | |
| `item_size` | size | `18` | 圖示與文字的字級 |
| `item_min_width` | length 0–128 | `36` | 觸控目標的下界 |
| `scroll` | bool | `true` | 放不下時可橫向捲動 |

**`scroll: true` 是預設，而且它不只是體驗問題。** 工具列的必備項（`schema:picker`、
`settings`）在窄螢幕上可能被擠出可視範圍；不能捲動的話，使用者就真的**觸達不到**
那兩個入口，而 §8.6.6.1 的必備項規定也就跟著失效。實作若不支援捲動，
**必須** 改為讓項目縮小或換行，**不得** 直接裁掉。

`space_between` / `space_evenly` 與 `scroll: true` 同時成立時，
項目總寬小於可用寬度才套用排列，否則以捲動為準（排列失去意義）。

##### 8.6.6.3 `candidates.syllables` — 逐音節消歧欄

九宮格（T9）上一串按鍵對應好幾個音節：`MG` 可能是 `mi` / `ni` / `m` / `n` / `o`。
消歧欄讓使用者一個音節一個音節收斂，而不是把候選整片藏起來。

**這一節只規範它畫在哪、佔多高、什麼時候出現。** 內容從哪來見 §8.6.6.3.6，
那一半今天不在本格式的管轄範圍。

**為什麼位置是「風格」。** 使用者給的截圖裡，iOS 的消歧是**候選列上方一橫排**
（外加底列一顆「选拼音」鍵），三星與語燕是**左側直欄**。同一個功能、同一個方案、
同一份詞庫，位置卻不同 —— 那就是風格的定義（`docs/decisions/style-schema-dictionary.md` §5.1）。
它一度寫死在 Android 的 `keyboard/T9Syllables.kt` 裡，那個檔案自己寫著「本該住在資料檔」。

| 欄位 | 型別 | 預設 | 誰消費 | 說明 |
|---|---|---|---|---|
| `placement` | enum `none` \| `above_candidates` \| `keyboard_slot` | `keyboard_slot` | 行動端渲染；桌面端**解析但不渲染**（§8.6.6.3.5） | `above_candidates` = iOS 慣例，不需要佈局宣告任何東西；`keyboard_slot` = 三星／語燕慣例，借用佈局宣告的格位（§9.3.1） |
| `trigger` | enum `while_composing` \| `on_demand` | `while_composing` | 同上 | ⚠ **`on_demand` 尚未有任何一端實作**，見 §8.6.6.3.4 |
| `max_items` | int 0–32 | `0` | 僅行動端 | 一次最多列幾個讀音；`0` = 有多少畫多少。`keyboard_slot` 底下真正的上界是格位數，本欄再夾一次 |
| `height` | length 24–96 | `40` | 僅行動端 | **僅 `above_candidates`**。`keyboard_slot` 不佔額外高度，見 §8.6.6.3.1 |

**預設值是 `keyboard_slot`，所以沒有宣告 `syllables:` 的主題行為不變。** 這是刻意的：
預設值配上 D1 退化（§8.6.6.3.3），既有的左欄式佈局照舊，沒有左欄的佈局退化成上方橫排，
兩邊都不會落到「什麼都不畫」。

**`orientation` 是推導的，不是欄位。** `above_candidates` 恆為 `horizontal`；
`keyboard_slot` 的方向由格位自己在鍵盤上的位置決定（宣告在左側直欄就是直的），
本格式不另外描述。不開這個欄位，是因為兩種 placement 各自只有一種說得通的方向，
開了就等於開一個可以寫成矛盾值的欄位。真的需要覆寫時再開（§11）。

**外觀沿用 §8.6.1–8.6.5**（`text` / `item` / `separator`）。v1 **沒有**專屬的外觀子區塊，
也**沒有** `selected.*`：「已確定的音節與待選的音節長得不一樣」需要「哪些音節已確定」
這個執行期狀態，而它今天還不是主題看得見的東西（§11）。

###### 8.6.6.3.1 高度與 §8.8.0 預算的關係（規範性）

`above_candidates` 那一排**加在**鍵盤高度與候選列高度之上。§8.8.0 末尾的總高因此是：

```
total = h + candidates.bar.height
          + （消歧欄正在顯示 ? candidates.syllables.height : 0）
          + （honor_bottom_inset ? 系統底部 inset : 0）
```

* ⛔ **不得從 `candidates.bar.height` 裡挖。** 候選列會在組字途中忽然變矮、組完又變回來，
  候選字跟著上下跳 —— 而使用者正在看著它們挑字。
* ⛔ **不得吃掉鍵盤預算。** §8.8.0 第 3 步的 `budget` 與消歧欄無關，鍵高**不得**因為
  多了一排而改變。動到它，§10 第 16 條（同一份主題下任兩份佈局總高相同）會在
  「正在組字」這個狀態下悄悄失守，而那正是最難重現的一種。
* `keyboard_slot` 對高度的貢獻恆為 **0**：它借用的是既有的鍵，不新增任何一列。
* ⚠ **代價要說在明處：** 消歧欄出現與消失時，輸入法視窗的總高會變。上面兩條的代價更大，
  所以選了這一邊。實作 **必須** 讓這個高度變化走與候選列自身出現／消失**同一條**路徑，
  **不得** 為它另外加動畫或延遲 —— 兩條路徑會在同一幀裡打架。

###### 8.6.6.3.2 解析與生效是兩個階段（規範性）

這是本節最容易做錯的一段，而且它決定了 §10 第 9 條在這個區塊上成不成立。

| 階段 | 輸入 | 誰做 | 產生什麼診斷 |
|---|---|---|---|
| **解析** | 只有主題文件 | **四端全部** | `unknown_field`、`bad_enum`、`out_of_range` 等一般欄位診斷 |
| **生效** | （主題，當前佈局，當前方案） | 僅行動端 | §8.6.6.3.3 的 `syllables_*` 退化診斷 |

* 解析階段屬於 §10 第 9 條的**共用作用域**：`candidates.syllables.placemnt: above` 這種
  拼字錯誤，四端都**必須**恰好報一則 `unknown_field`。桌面端 **不得** 因為自己不畫這一欄
  就整個區塊跳過 —— 那會讓同一份壞主題在手機上一則、在電腦上零則，第 9 條當場失守。
* 生效階段的診斷**不參與**第 9 條的比對，理由與 §8.6.7.4 第 4 條相同：它們相依於主題
  以外的東西（佈局與當前方案），而桌面端連佈局都不消費，不可能算得出來。
* **生效階段的診斷，每一個（主題，佈局，方案）組合最多一則，不得每畫一次一則。**
  消歧欄在組字期間每按一個鍵都會重算，發在重算路徑上等於每敲一個字母刷一則。
  這與 §8.6.7.4 第 2 條是同一條紀律。

###### 8.6.6.3.3 退化規則（規範性）

**求值順序是規範的一部分，不是實作細節。** 順序寫反的話，每一份 QWERTY 佈局都會
刷一則 D1 的 WARNING —— QWERTY 佈局當然沒有 `syllable_slots`，但它配的方案本來就
給不出讀音，D2 早該把整條關掉了。

```
0. placement == none                    → 不顯示。結束。不產生診斷
1. 當前方案給不出讀音                    → D2：不顯示，並隱藏觸發鍵。結束
2. 當前這一個音節的讀音數 < 2            → 不顯示（沒有東西要消歧）。結束。
                                          這不是退化，不產生診斷
3. trigger == on_demand，且佈局沒有
   任何 syllables:toggle 鍵              → D3：視為 while_composing
4. placement == keyboard_slot，且當前
   layer 的可用格位 < 2                  → D1：視為 above_candidates
5. 顯示
```

| # | 情況 | **必須**怎麼做 | 診斷 |
|---|---|---|---|
| **D1** | `placement: keyboard_slot`，但當前 layer 的**可用格位少於 2** | **視為 `above_candidates`**。⛔ **不得什麼都不畫** | WARNING `syllables_no_slots`，args `[layout-id, layer-id, count]` |
| **D2** | 當前方案給不出讀音（方案沒有 `spelling_hints`） | **整條不出現**，且 `syllables:toggle` 那顆鍵**必須隱藏** —— 不是變灰、不是按了沒反應。做法見下方 ⚠ | 無。方案沒有 `spelling_hints` 是它的正常狀態，不是缺陷 |
| **D3** | `trigger: on_demand`，但當前佈局沒有任何 `syllables:toggle` 鍵 | **視為 `while_composing`**（否則使用者永遠看不到它） | WARNING `syllables_toggle_missing`，args `[layout-id]` |
| **D4** | `syllable_slots` 裡的某個 id 在該 layer 找不到對應的 `key.id` | **丟棄該筆**，再依 D1 重新判定可用格位數。⛔ **不得默默退回** | WARNING `syllables_slot_unknown`，args `[layer-id, key-id]`（佈局解析時發，見 §9.3.1） |

**為什麼 D1 的門檻是 2 而不是 1。** 讀音可能比格位多，多出來的要翻頁，而翻頁鍵自己
要佔一格。只剩一格時：拿去當翻頁鍵就沒有讀音可點，拿去放讀音就有讀音**看得到卻翻不到**。
一格是描述不出來的狀態，所以下界是 2。

**為什麼 D1 不能「什麼都不畫」。** 沒有宣告格位時，畫面會照常顯示那三顆標點鍵 ——
一個完全正常的九宮格，只是消歧功能整個不存在，而且沒有任何東西會叫。
這正是本專案抓過七次的那一類缺陷。退化成上方橫排是為了讓它至少**看得見**。

⚠ **D2 的「必須隱藏」與 §9.5.1 的「不得在執行期移除按鍵」會撞。** 兩條都對，
撞的是同一顆鍵：§9.5.1 說佈局按鍵不得在執行期移除，因為鍵有寬度，少一顆整列會重排，
使用者會看到一個位置飄移的鍵盤；D2 說觸發鍵必須隱藏，因為一顆按下去什麼都不會發生的鍵
是這個專案已經抓過六次的形狀。裁決（規範性）：

1. **工具列項目／狀態列項目**形態的 `syllables:toggle` → 照 §9.5.1，**不渲染**。
   項目沒有固定寬度，少一項不影響其他項的幾何。
2. **佈局按鍵**形態的 `syllables:toggle` → 該鍵的 `id` **必須**同時列在該 layer 的
   `syllable_slots` 裡。這樣「隱藏」的意思就變成「那一格顯示它原本宣告的鍵面」——
   鍵還在、寬度不變、按下去做的是它原本那顆鍵該做的事，§9.5.1 與 D2 同時成立。
3. **`syllables:toggle` 按鍵不在 `syllable_slots` 裡，是佈局的錯誤**，不是渲染端的自由。
   渲染端 **必須** 由建置期測試擋下（§9.5.1 對佈局按鍵的同一條處置），
   **不得** 在執行期把它移除，也 **不得** 讓它留在鍵盤上按了沒反應。
4. ⚠ **本條從未被執行過** —— 隨附的佈局沒有任何一份含 `syllables:toggle`（§8.6.6.3.4）。
   寫在這裡是因為第一個做 `on_demand` 的人會立刻撞到，而三種想得到的解法裡有兩種是壞的。

###### 8.6.6.3.4 已定義、但尚未有任何一端實作（規範性地誠實）

下列東西**規範定義了，四端都還沒有做**。不要照著寫主題或佈局 —— 寫了不會生效，
而且不會有任何訊息告訴你：

| 東西 | 狀態 | 後果 |
|---|---|---|
| `trigger: on_demand` | **零端實作。** Android 解析得出這個值，渲染端只走 `while_composing` | 主題寫 `on_demand` 會得到 `while_composing` 的行為，**而且拿不到 D3 的 WARNING**（下一列） |
| `syllables:toggle`（§9.5） | **零端實作。** 隨附的 12 份佈局沒有一份用它 | 它只在 `on_demand` 底下有意義，所以與上一列一起卡住 |
| D1 / D3 的 WARNING | **Android 做了退化本身，沒有發診斷**（`keyboard/KeyboardView.kt` 的 `effectivePlacement`） | 主題作者宣告 `keyboard_slot` 卻拿到 `above_candidates` 時，今天沒有任何訊息 |
| ~~D4 的 WARNING~~ | **Android 已實作**（`theme/LayoutParser.kt`，佈局解析時發，並把那一筆丟掉） | 已解。第三方佈局把 `syllable_slots` 指到不存在的鍵時會被指名 |
| 桌面端的消歧欄 | **零端實作，而且 v1 刻意不做**（§8.6.6.3.5） | 桌面使用者今天沒有逐音節消歧 |

**碼表上的 ⚠ 尚未實作 是規範性記號，不是註解。** §6.5.1 裡帶這個記號的 code
**不得**出現在任何一端的實作裡 —— 規範可以走在實作前面，但兩者的差距必須
**寫在碼表上讀得出來**。行動端的 `DiagnosticCodeSpecTest` 兩個方向都驗：
沒有記號的 code 必須實作，有記號的 code 必須**還沒**實作。所以做完之後
「把記號拿掉」不是禮貌，是讓測試轉綠的必要步驟。

**這張表是規範的一部分。** 理由與 §9.5.1 一樣：「還沒做」與「這個形態上不存在」是
兩件事，分不出來的人會刪掉不該刪的東西。表上的每一項做完之後 **應** 從這張表移走，
**不是** 從規範移走。

###### 8.6.6.3.5 桌面端的預期行為（規範性）

桌面端（macOS / Windows）**必須**：

1. **完整解析 `candidates.syllables` 的每一個欄位**，套用型別、列舉與範圍檢查，
   並產生與行動端逐則相同的解析階段診斷（§8.6.6.3.2）。
   ⛔ **不得**把整個區塊當成「已知但不進入」—— 那正是 §10 第 9 條在這裡會破的方式。
2. **不渲染任何消歧欄。** v1 的桌面候選窗沒有這一列。
3. **不產生任何 `feature_unsupported` 診斷。** `placement` 的預設值是 `keyboard_slot`，
   所以**每一份主題**都會命中，每次載入刷一則 INFO，而主題作者沒有做錯任何事。
   這是 §9.5.1 第二條紀律（「不得產生診斷」）的同一個理由。
4. **不執行 §8.6.6.3.3 的任何一條退化規則。** D1、D3、D4 相依於佈局，
   而桌面端完全不消費 `core/layouts/`（§1.1）—— 這幾條在它身上沒有輸入。

**`keyboard_slot` 在桌面端是「形態上不存在」，不是「進度落後」。** 沒有軟鍵盤就沒有
格位，這與 §9.5.1 裡 macOS 的 `hide_keyboard` 同一類。`none` 與 `above_candidates`
則**不是** —— 桌面候選窗完全有地方畫一橫排讀音，只是 v1 還沒做。

**日後桌面端要做的時候，必須是這個形狀。** 寫死在這裡是為了讓兩個桌面端不會各自發明
（§8.12 的懸浮狀態列已經示範過一次同一個問題的兩半長成兩個東西）：

* 消歧列畫在**候選窗內部**、候選項之上，與 §8.12 的 `status_bar` 在同一個窗裡。
  **不是**螢幕上的另一條帶子 —— 理由與 §8.12 末段完全相同。
* `placement` 的桌面語義：`none` → 不畫；`above_candidates` → 畫；
  `keyboard_slot` → **視為 `above_candidates`**（沒有格位可借）。
  這個視為**不產生診斷**，理由同上第 3 點。
* `height` 在桌面端是**內容高度的下界**，不是固定高度。候選窗的高度由 §8.6.7.1
  第 9 步算出來，多一列就多那一列的高；行動端那條「加在鍵盤之上」的公式在這裡
  沒有意義（桌面端沒有鍵盤）。
* 消歧列**必須**計入 §8.6.7.1 的 `window_h`，且 §8.6.7.4 第 1 條
  （退化不得改變排版）在它身上一樣成立。
* ⚠ **擋在前面的不是 UI，是 ABI。** 見 §8.6.6.3.6：`rs_snapshot` 的 `menu` 只有
  **當前那一頁**，一頁之外的讀音看不到。在 `core/` 補上「不動頁碼走完整份候選」
  的 API 之前，桌面端做出來的消歧列會與行動端一樣不完整。桌面端的「展開候選網格」
  需要的是同一件事，兩者應該一起做。

###### 8.6.6.3.6 內容從哪來（資訊性，不是規範）

本節規範的是**位置與外觀**，不是內容。目前的內容來源是候選的 `comment`
（方案 `spelling_hints` 給的原始拼寫），而它有兩個已知缺口，都在 `core/` 那一側：

* **分頁。** `rs_snapshot` 的 `menu` 就是一頁（本專案 `menu/page_size: 9`），
  所以「這串按鍵可能是哪些音節」只看得到當前頁的那幾個。
* **選了讀音之後怎麼把高亮移過去。** 沒有 `rs_highlight_candidate` 的話，
  篩選會讓使用者按空白鍵拿到一個他沒看過的字。

列在這裡是為了讓讀規範的人知道**為什麼上面那張欄位表這麼小** —— 不是漏寫，
是內容那一半還不歸這份文件管。細節見 `docs/coordination.md` §5 的 androidkbd 條目。

###### 這一節的可驗證檢核項

§10 第 34–39 條。

##### 8.6.6.4 候選列右端的控制鍵（規範性，僅行動端 `candidates.bar`）

候選列右端那一格 **最多畫一顆**，三選一：

| 值 | 畫什麼 | 什麼時候 |
|---|---|---|
| 無 | 什麼都不畫 | 本頁沒有候選（候選列現在畫的是工具列）；或本頁全部看得完而且後面也沒有頁 |
| 展開 | `∨` / `∧`，翻頁移進展開面板內部 | 見下面的判準 |
| 翻頁 | `‹` `›`（各自按不動時該顆不畫） | 見下面的判準 |

##### 一、判準（規範性）

```
morePages   = 不是最後一頁
hidden      = 本頁畫得出來的 < 本頁候選總數           （§8.6.4.2 的 n）
pagerDrawn  = page_indicator 這一組**真的會畫出至少一顆**
              （show 且 style != none 且 prev/next 至少一顆是活的）
expandDrawn = scroll == expandable 且 expand_button.show

本頁候選數 == 0                → 無
面板開著 且 expandDrawn        → 展開（收合鍵）
hidden                         → expandDrawn ? 展開 : (pagerDrawn ? 翻頁 : 無)
pagerDrawn                     → 翻頁
morePages 且 expandDrawn       → 展開
其餘                           → 無
```

⛔ **本頁還有畫不出來的候選時，不得提供「下一頁」——除非那是唯一的出口。**
翻頁鍵的語義是「本頁我看完了」；本頁沒看完就給翻頁，等於讓使用者
**跳過他從未看見的候選**，而畫面完全正常。這是本規範反覆點名的那一類失敗。

> ⚠ **這一段從前寫成無條件的禁令，而正上方的虛擬碼（規範性）寫的是
> `hidden → expandDrawn ? 展開 : (pagerDrawn ? 翻頁 : 無)`——
> 同一節同時規定了兩件相反的事。** 照虛擬碼實作（Android）與只讀這一段
> 實作（另一端）會做出不同的東西，而兩邊都能指著規範說自己對。
> **以虛擬碼為準**，理由是：`hidden` 而 `expandDrawn == false` 這一格，
> 兩個選項是「跳過幾個沒看見的候選」與「**把使用者鎖死在第 1 頁**」，
> 後者更糟。這一格本身是主題的設計錯誤（展開面板被關掉了），
> 由 §10 第 41 條在建置期擋下來 —— 執行期沒有正確答案可選，只有比較不糟的。
> 隨附主題不會走到這一格。

⛔ **判準是「這一顆真的會被畫出來」，不是「照理說應該是這一顆」。**
`page_indicator.show: false`（或 `style: none`）時翻頁那一組一顆都不畫；
一個「回翻頁、而翻頁畫不出來」的實作，結果是**右端一片空白、使用者鎖死在
第 1 頁**，而畫面完全正常、沒有任何東西會叫。隨附主題不會踩到；
第三方主題踩到就是死路。

⚠ **面板開著時那一顆必須是收合鍵。** 展開面板自己沒有關閉鍵；翻到一頁候選
比較少的頁面之後「本頁看得完」成立，若因此換成翻頁鍵，就等於把一片蓋住
鍵盤的浮層留在畫面上而**沒有出口**。

⛔ **展開面板裡的翻頁列，不受 `page_indicator` 控制（規範性）。**
`page_indicator` 管的是**候選列右端**那一組 `‹ ›`；展開面板底部那一列翻頁是
**面板自己的唯一導覽**，實作 **不得** 讓 `page_indicator.show: false` 或
`style: none` 把它一起關掉，`style` 為 `dots` / `text` 時面板 **必須** 退回箭頭。

> 兩者綁在一起時的實測結局（emulator-5558，2026-08-13）：主題寫
> `page_indicator.show: false` → 上面的判準把右端推成「展開」→ 使用者按 `∨`
> 打開面板 → **面板底部一顆翻頁鍵都沒有** → 第 2 頁永遠進不去。
> 而「右端有東西」這件事讓 §10 第 41 條的死路偵測**永遠不會紅** ——
> 把 `rightEnd` 推進展開那一支的**唯一原因**（翻頁畫不出來），
> 同時讓那條路的終點也畫不出來。這是同一條死路第三次以不同形狀出現。
>
> 主題仍然管得到面板翻頁列**長什麼樣**（顏色、大小走同一份 `page_indicator`
> 樣式欄位），管不到它**在不在**。

##### 二、⛔ 出口必須存在（規範性）

**只要還有使用者沒看到的候選（`hidden` 或 `morePages`），右端就必須至少
畫得出一顆，而且那一顆按下去 MUST 真的到得了他沒看到的東西。**
兩條出口同時關不掉的情況**不是**一種要在執行期優雅處理的頁況，
它是**主題的設計錯誤**，而實作 **必須** 在建置期或裝置端把它擋下來
（本 repo：`CandidateDensity.deadEnd()` ＋ `ThemeDensityTest` 逐份主題、
逐種頁況掃過一遍）。

⛔ **判準是「這條路到得了」，不是「右端有東西」（規範性）。** 逐格：

| 右端 | 使用者要怎麼看到下一個候選 | 何時是死路 |
|---|---|---|
| 無 | 沒有東西可按 | **一律是** |
| 翻頁 | 按 `›` | 從不是（但可能跳過本頁 `hidden` 那幾個，見上一段） |
| 展開 | 按 `∨` 開面板 → 本頁全部在面板裡；**下一頁靠面板自己的翻頁列** | 面板的翻頁列畫不出來而 `morePages` 時**是** |

> 上一版的判準是「右端 == 無」，於是死路只要**搬進面板**就躲得過去 ——
> 而那正是這一輪實測到的形狀。

> 候選列本身可以橫向捲動，但**捲動不得是唯一路徑**：沒有捲軸、沒有提示，
> 使用者不會知道右邊還有東西。44 dp 高的一條帶子上，看不見的東西等於不存在。

##### 三、⛔ 量測扣掉的寬度就是畫出來的那幾顆（規範性）

```
reserved = 0                                        （一顆都不畫）
         | bar.reserved_end                         （一顆）
         | bar.reserved_end + control_w × (n − 1)   （n 顆）
```

**`control_w`（規範性定義）** = 候選列右端一顆控制鍵的寬度。
本規範**不指定它的數值**（§8.6.4.1 自承不規範字形與觸控幾何），但要求：

1. 它 **必須** 是實作內部的**單一常數**：量測扣掉的與畫出來的用同一個值。
2. 它 **必須** 滿足 `ui-design` §3.6 的觸控目標下界。
3. 每一端 **必須** 把它的值寫在自己的原始碼裡並註明位置，讓另一端對得起來。

> 上一版這裡寫的是「按鍵寬」——**規範全篇沒有這個量**，讀者無從得知它是
> `item.min_width`、是鍵盤的鍵寬、還是別的東西。實際上 Android 用的是
> `KeyboardView.kt` 的 `internal const val CANDIDATE_BAR_BUTTON_DP = 40`
> （實作內部常數，不是主題欄位）。iOS 端請自己定一個並寫在這裡。

這條等式**會漂掉**：把它拆成「量測時扣多少」與「實際畫幾顆」兩份實作之後，
11 種頁況裡有 **5 種**對不上（第 1 頁又是最後一頁：扣 40 畫 0；第 2 頁而
右端是展開鍵：扣 80 畫 40 …），而**沒有任何東西會叫**。所以檢核必須用
**同一組參數**逐格斷言「右端畫什麼」與「右端佔多寬」兩件事（§10 第 45 條）。

##### 四、實測參考（emulator-5558，1080×2400 @420dpi = 411.43 dp，`default-light`）

`reserved_end: 40`、按鍵 40 dp、基準情境一格 56 dp、節距 60 dp：

| 頁況 | 右端 | 佔多寬 |
|---|---|---|
| 第 1 頁又是最後一頁、看得完 | 無 | 0 dp |
| 第 1 頁、本頁 9 個看不完 | 展開 | 40 dp |
| 第 2 頁、本頁 9 個看不完 | 展開 | 40 dp |
| 第 2 頁又是最後一頁、本頁 3 個看得完 | 翻頁（只有「上一頁」） | 40 dp |
| 第 2 頁、本頁 3 個看得完 | 翻頁（兩顆） | 80 dp |
| 面板開著（任何頁況） | 展開（收合鍵） | 40 dp |
| `page_indicator.show: false`、本頁看得完、還有下一頁 | 展開 | 40 dp |

兩顆一起畫吃掉候選列 **19.4%** 的寬度 —— 411.43 dp 上 80 dp 對 40 dp
**恰好差一個候選**（5 vs 6）。翻頁鍵與展開鍵解決的是同一個問題
（「還有更多」），兩顆一起出現是同一份資訊的第二份。

#### 8.6.7 `candidates.window` — 桌面端候選窗（僅 macOS / Windows）

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `background` | color | `#FFFFFF` | |
| `orientation` | enum | 繼承 `candidates.orientation` | |
| `corner_radius` | length | `metrics.corner_radius` | |
| `padding` | length | `metrics.padding` | |
| `border_width` | length | `metrics.border_width` | |
| `border_color` | color | `transparent` | |
| `min_width` | length | `0` | |
| `max_width` | length | `640` | **不是硬上界**，超出時的處置見 §8.6.7.1 的 `overflow` 與 §8.6.7.2。不得因為放不下而丟棄候選 |
| `placement` | enum `below` \| `above` \| `auto` | `auto` | `auto` = 空間不足時翻面 |
| `offset_x` / `offset_y` | length −64–64 | `0` / `6` | 相對插入點的位移 |
| `follow_caret` | bool | `true` | `false` = 固定在螢幕角落，哪一個角由 `anchor` 決定 |
| `anchor` | enum `top_leading` \| `top_trailing` \| `bottom_leading` \| `bottom_trailing` | `bottom_trailing` | 僅 `follow_caret: false` 時有效，見 §8.6.7.3 |
| `backdrop` | enum `none` \| `blur` \| `vibrancy` | `none` | 不支援的平台 **必須** 退化，見 §8.6.7.4 |
| `opacity` | ratio 0.05–1.0 | `1.0` | 下界不是 0，見 §8.6.7.4 |
| `shadow.show` | bool | `true` | 不支援的平台 **必須** 退化，見 §8.6.7.4 |
| `shadow.radius` | length 0–64 | `18` | |
| `shadow.offset_x` / `offset_y` | length | `0` / `4` | |
| `shadow.color` | color | `#00000040` | |

`window` **可** 覆寫 §8.6.1–8.6.5 的任一子區塊。

#### 8.6.7.1 多行與表格排版（僅桌面端）

§11 原本列著「候選窗的多列／表格排版沒有定義，只有 `orientation` 與 `max_width`，
不足以描述」。這一節補上它。**這是 v1 的新增欄位，全部有預設值，
不寫等同於 v1 既有行為**（單行），所以不遞增 major（§5.3）。

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `lines` | int 0–16 | `1` | 次要軸上的行數。`1` = v1 的單行；`0` = 自動（見下） |
| `equal_columns` | bool | `true` | `true` = 所有欄同寬（取全頁最寬項）；`false` = 逐欄取該欄最寬項 |
| `column_gap` | length 0–128 | = `candidates.item.spacing` | 欄與欄之間 |
| `row_gap` | length 0–128 | = `candidates.item.spacing` | 列與列之間 |
| `max_height` | length 0–4096 | `0` | `0` = 不限 |
| `item_align` | enum `leading` \| `center` \| `trailing` | `leading` | 項在格內的水平對齊（欄比項寬時才看得出來） |
| `overflow` | enum `shrink` \| `clip` | `shrink` | 排完仍超出 `max_width` 時怎麼辦 |

**`orientation` 決定主要軸，`lines` 決定次要軸上有幾行。** 兩者正交：

| `orientation` | `lines` | 結果 |
|---|---|---|
| `horizontal` | `1` | 一列，候選由左至右（v1 行為） |
| `horizontal` | `n` | n 列的表格，**逐列填滿**（row-major） |
| `vertical` | `1` | 一欄，候選由上至下 |
| `vertical` | `n` | n 欄的清單，**逐欄填滿**（column-major）—— 中文輸入法的兩欄候選 |

##### 排版演算法（規範性）

輸入：本頁 n 個候選的量測尺寸 `w[i]`、`h[i]`（**已含 `item.padding_h/​padding_v`**）。

```
1. 列高一律相同：item_h = max(h[0..n-1])
   （`comment.position: below` 會讓部分項高一倍；用最大值才不會互相蓋住。）

2. 可用內容寬 avail_w = max_width > 0 ? max_width - 2*padding : ∞
   可用內容高 avail_h = max_height > 0 ? max_height - 2*padding : ∞

3. 決定 L（行數）：
     lines >= 1 → L = min(lines, n)
     lines == 0 → 自動。從 L = 1 起遞增，取**第一個**讓內容塞得下的 L：
                    horizontal 比的是內容寬 <= avail_w
                    vertical   比的是內容高 <= avail_h
                  都塞不下 → L = n。相關的上限是 ∞ 時 → L = 1。

   ⚠ 自動的收斂方向由 orientation 決定，這不是任意的：
     horizontal 增加**列**數會減少欄數，所以往「不要太寬」收斂；
     vertical 增加**欄**數會減少列數，所以往「不要太長」收斂。
     反過來做的話 `lines: 0` 對其中一種 orientation 完全沒有作用。

4. K = ceil(n / L)          # 主要軸上每行放幾個
   實際用到的行數 L' = ceil(n / K)   # L 可能用不完：n=5、L=4 → K=2 → 只需 3 行
     horizontal → rows = L',  columns = K
     vertical   → rows = K,   columns = L'

5. 第 i 個候選（0 起算）落在：
     horizontal → row = floor(i / K),  column = i mod K
     vertical   → row = i mod K,       column = floor(i / K)

6. 欄寬：
     equal_columns == true  → 每一欄都是 max(w[0..n-1])
     equal_columns == false → 第 c 欄是落在該欄的項的 max(w)，該欄無項則為 0

7. 內容尺寸：
     content_w = Σ colw[c] + (columns - 1) * column_gap
     content_h = rows * item_h + (rows - 1) * row_gap

8. 超出 avail_w 時（`overflow`）：
     shrink → room = max(0, avail_w - (columns-1)*column_gap)
              scale = room / Σ colw
              colw[c] := max(item.min_width, colw[c] * scale)
              夾到 `item.min_width` 之後仍然超出 → **接受超出**，
              並由第 9 步的 9a 讓窗跟著變寬(§8.6.7.2)。
              **不得** 把任何一欄縮成 0：那會產生一個看得見卻讀不到的候選。
     clip   → 欄寬不動，窗寬由第 9 步夾住，超出的部分被裁掉。

   兩種情況下，**量測寬度大於它拿到的格寬**的項 **必須** 被標記為需要截斷，
   渲染端 **必須** 在那些項的尾端加上 `…`（U+2026）。

9. 窗的外框（`effective_max` 的兩條例外見 §8.6.7.2 第二節）：
     window_w = clamp(content_w + 2*padding, min_width, effective_max)
     window_h = content_h + 2*padding
                （+ preedit 區塊的高度，若 `preedit.show`）
                （+ 狀態列的高度，若 `status_bar.show`，見 §8.12）
     max_height > 0 時 window_h 再夾到 max_height。
```

**`lines` 與 `max_visible` 無關。** 一頁有幾個候選是 librime 的 `page_size` 決定的，
本格式**不得**改變它 —— 改了會讓候選的序號標籤與使用者按的數字鍵對不上。
`lines` 只描述已經拿到的這一頁怎麼排。

**檢核（見 §10 第 19–21 條）** 提供了可逐項驗算的具體數字。

#### 8.6.7.2 `max_width` 溢出：候選完整性與窗寬的例外（規範性）

§8.6.7 的表格原本在 `max_width` 那一列寫「超出則換行／截斷，由實作決定」。
那正好是最需要一致的一格：Windows 端據此做成「橫排時**丟掉**放不下的候選、
直排時讓窗變寬」，而 macOS 端做的是別的 —— 同一份主題在兩台電腦上
看到的候選**數量**不一樣。這一節取消那句「由實作決定」。

##### 一、不得丟棄候選（規範性）

**候選窗不得少畫本頁的任何一個候選。**

一頁有幾個候選由方案的 `page_size` 決定（§8.6.7.1 已寫明本格式不得改變它），
而序號標籤與使用者按下的數字鍵是一一對應的。丟掉第 5 個候選之後，
使用者按 `5` **仍然會選到那個看不見的字** —— 畫面與行為分岔，
而且沒有任何提示。這是「看得到但摸不到」的鏡像，更難查。

因此 §8.6.7.1 第 8 步的兩種處置都保留全部 n 個候選：

* `shrink` —— 縮欄寬，放不下的項被標記為需要截斷，尾端加 `…`（U+2026）。
* `clip` —— 欄寬不動，超出窗的部分被裁掉。**被裁掉的是像素，不是候選。**
  裁切**必須**只發生在窗的內容區邊界上，**不得**改變第 5 步算出的任何落點；
  被裁到一半的項**不**加 `…`（它沒有被截斷，只是被窗蓋住了）。

##### 二、`max_width` 不是硬上界（規範性）

把 §8.6.7.1 第 9 步的第一行改寫成下列形式。其餘各步不變。

```
9. effective_max = (max_width > 0) ? max_width : ∞

   9a. overflow == shrink 且第 8 步夾到 item.min_width 之後 content_w 仍 > avail_w
         → effective_max := content_w + 2 * padding
       （shrink 的承諾是「縮，不裁」。縮到底還是放不下時，
         守住承諾的唯一辦法是讓窗變寬。）

   9b. 否則若 colw[0] + 2 * padding > effective_max
         → effective_max := colw[0] + 2 * padding
       （colw[0] 是第 5 步落在第一格的那一欄，經第 8 步處理後的寬度。
         **第一個候選一定要看得見** —— 一個空的候選窗比一個太寬的候選窗
         更難理解，而 max_width 寫得太小是主題的筆誤，不是使用者的錯。）

   window_w = clamp(content_w + 2 * padding, min_width, effective_max)
```

`min_width` 仍然是下界，且**優先於** `effective_max`：兩者衝突時（`min_width`
大於 `effective_max`）以 `min_width` 為準 —— `clamp` 的既有語義。

**9a / 9b 不產生任何診斷。** 這是執行期計算的夾制，不是欄位綁定時的夾制，
依 §10 第 4b 條的但書不發診斷。（發了的話同一份主題會因為使用者這一次
剛好打了一個長詞而多一則診斷，§10 第 9 條的序列比對立刻失守。）

##### 三、直排與橫排走同一套（規範性）

`orientation` 只決定主要軸（§8.6.7.1），**不**決定溢出處置。
直排也適用 `overflow`、也適用第一項例外。
「橫排截斷、直排變寬」這種依 orientation 分岔的行為**不合規** ——
主題作者換一個 `orientation` 不該連帶換掉溢出語義。


#### 8.6.7.3 `follow_caret: false` 落在哪一個角（規範性）

原本只寫「固定在螢幕角落」，沒說哪一個角。Windows 端暫取右下。
新增一個欄位把它說清楚，**預設值就是 Windows 目前的行為**，既有實作不必改。

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `anchor` | enum `top_leading` \| `top_trailing` \| `bottom_leading` \| `bottom_trailing` | `bottom_trailing` | 僅 `follow_caret: false` 時有效 |

**語義（規範性）：**

* `top` / `bottom` 指**螢幕可用區**的上／下緣。可用區 = 扣掉系統永久佔用的
  區域之後剩下的矩形（macOS：選單列與 Dock；Windows：工作列）。
  用整個螢幕矩形會讓窗被工作列蓋住，那是實測會發生的事，不是理論問題。
* `leading` / `trailing` 指**書寫方向**的起點／末端：LTR 下 `leading` = 左；
  RTL 下 `leading` = 右。不寫「左／右」是為了讓 §11 的 RTL 缺口日後補起來時
  不必改這一格。
* 螢幕的選定：**含有目前插入點的那一個**。取不到插入點（宿主回報空矩形，
  見 §8.6.7 的 `follow_caret`）時取**目前作用中視窗**所在的螢幕；
  再取不到才取主螢幕。三段回落是規範性的 —— 直接用主螢幕的話，
  雙螢幕使用者的候選窗會固定出現在另一台螢幕上。
* `offset_x` / `offset_y` 在此模式下是**向內**的邊距：正值把窗推離它靠著的那兩條邊。
  `bottom_trailing` + `offset_y: 6` = 窗的下緣距可用區下緣 6。
  （`follow_caret: true` 時 `offset_x`/`offset_y` 仍是相對插入點的位移，語義不同，
  這是刻意的：兩種模式下「位移」本來就是相對不同的東西。）
* 窗**必須**完整落在可用區內。第 9 步算出的窗比可用區還大時，先夾窗的尺寸，
  再擺位置 —— 位置的計算不得產生負的邊距。

#### 8.6.7.4 平台做不到時怎麼退化（規範性）

`backdrop` / `opacity` / `shadow.*` 需要平台級的視窗合成能力。
原本只有 `backdrop` 那一列寫了「必須靜默退化為 `none`」，其餘兩項沒規定，
於是 Windows 端（要用分層視窗才做得到）乾脆三項都不實作，而規範不知道。

| 欄位 | 平台做不到時 **必須** 的行為 | 診斷 |
|---|---|---|
| `backdrop` | 視為 `none`，並改以 `background` 的**不透明**色繪製底 | INFO `feature_unsupported`，args `["candidates.window.backdrop", <值>]` |
| `opacity` < 1.0 | 視為 `1.0`（完全不透明） | INFO `feature_unsupported`，args `["candidates.window.opacity", <值>]` |
| `shadow.show: true` | 視為 `false`；`shadow.radius` / `offset_*` / `color` 一律無效 | INFO `feature_unsupported`，args `["candidates.window.shadow", "true"]` |

四條共同規則：

1. **退化不得改變排版。** 背板、透明度、陰影都**不佔內容空間**：
   §8.6.7.1 算出的 `window_w`、`window_h` 與每一項的落點，
   在支援與不支援的平台上**必須完全相同**。§10 第 19–22 條那幾組數字
   就是靠這條才能在四端一起驗算。陰影尤其容易做錯 —— 把 `shadow.radius`
   加進窗的大小裡，整組落點就全部偏掉了。
2. **每個欄位每次載入最多一則診斷**，不得每畫一次一則。診斷是主題的性質，
   不是每一幀的性質。
3. **是 INFO，不是 WARNING。** 主題作者沒有做錯任何事，是平台做不到。
4. **這三則診斷不參與 §10 第 9 條的比對。** 它們是平台能力相依的：
   同一份主題在做得到的平台上零則、做不到的平台上三則。
   §10 第 9 條比的是解析結果，不是渲染能力。

`opacity` 的下界改為 **0.05**（原為 `ratio` 的 0.0）。理由：`opacity: 0` 的候選窗
完全看不見，等於這個輸入法壞了，而使用者不會想到去看主題檔的一個小數。
超出 → 依 §10 第 4b 條夾制 + 一則 WARNING `out_of_range`。


### 8.7 `preedit`

| 欄位 | 型別 | 預設 |
|---|---|---|
| `show` | bool | `true` |
| `size` | size | `16` |
| `color` | color | `#404040` |
| `background` | color | `transparent` |
| `padding_h` / `padding_v` | length | `metrics.padding` |
| `corner_radius` | length | `0` |
| `caret.show` | bool | `true` |
| `caret.color` | color | `#3060C0` |
| `caret.width` | length 0–4 | `1.5` |
| `caret.blink` | bool | `true` |
| `selection.color` | color | `#3060C040` |
| `selection.text_color` | color | = `preedit.color` |

游標位置與選取範圍取自 `rs_composition`（單位為 **UTF-8 位元組**，
渲染前 **必須** 轉成該平台的字串索引 —— 這是四端最常見的越界崩潰來源）。

**桌面端:組字串畫在哪(規範性)。** §11 原本列著「有兩個地方可以畫，
規範沒說該畫在哪，而兩邊同時畫會出現兩份組字串」。裁決如下:

1. 桌面端 **必須** 把組字串交給宿主(macOS 的 marked text／Windows TSF 的
   composition string)。這不是樣式選擇 —— 插入點的位置、宿主自己的重繪、
   以及無障礙工具讀得到組字中的內容，都只有這條路徑做得到。
2. 因此 `preedit.show` 在桌面端的語義是「**候選窗裡是否再畫一份**」，
   而不是「要不要顯示組字串」。**桌面端的預設值是 `false`**
   (行動端仍為 `true`:軟鍵盤沒有宿主 marked text 可用)。
3. 主題把它設成 `true` 時，兩份組字串同時存在是**刻意的**——
   有些使用者要在候選窗裡看到帶選取標記的完整組字。實作 **不得**
   因此改成不送 marked text。
4. `preedit.show: true` 時，候選窗的高度依 §8.6.7.1 第 9 步加上這一塊;
   `false` 時 **不得** 為它保留任何空間。

### 8.8 `keyboard`（僅行動端；桌面端必須整段忽略）

| 欄位 | 型別 | 預設 |
|---|---|---|
| `background` | color | `#D0D0D0` |
| `key_aspect` | number 0.6–2.5 | `1.28` |
| `key_height.min` | length 20–200 | `40` |
| `key_height.max` | length 20–200 | `56` |
| `reference_grid.units` | number 4–20 | `10` |
| `reference_grid.rows` | number 1–8 | `4` |
| `row_height.min` | length 16–200 | `32` |
| `row_height.max` | length 16–200 | `96` |
| `max_screen_ratio.portrait` | ratio 0.2–0.8 | `0.45` |
| `max_screen_ratio.landscape` | ratio 0.2–0.9 | `0.62` |
| `padding.left` / `.right` / `.top` / `.bottom` | length | `5` / `5` / `4` / `4` |
| `row_spacing` | length 0–32 | `12` |
| `key_spacing` | length 0–32 | `6` |
| `honor_bottom_inset` | bool | `true` |

#### 8.8.0 高度模型：固定高度預算，由列數分掉

**本節是高度模型的第三版，前兩版都被實機量測否決。** 先講結論，再講為什麼：

```
# 1. 參考鍵寬：只看裝置寬度與參考格的欄數。與當前佈局、與螢幕高度都無關。
ref_key_w = (kb_width − padding.left − padding.right
             − key_spacing × (reference_grid.units − 1)) / reference_grid.units

# 2. 參考鍵高：key_aspect 仍是主控參數，再夾制到絕對上下界。
ref_key_h = clamp(ref_key_w × key_aspect, key_height.min, key_height.max)

# 3. 高度預算：參考格排滿的高度。**這就是鍵盤總高**，
#    與當前 layer 的欄數、列數都無關。height_scale 來自佈局（§9.2）。
budget = (ref_key_h × reference_grid.rows
          + row_spacing × (reference_grid.rows − 1)
          + padding.top + padding.bottom) × height_scale

# 4. 安全網：鍵盤永遠不得超過螢幕的這個比例（摺疊機、平板橫放、超小螢幕）。
budget = min(budget, avail × max_screen_ratio.<當前方向>)

# 5. 當前 layer 把預算分掉。**除以 Σ weight，不是列數。**
usable_h = budget − padding.top − padding.bottom − row_spacing × (rows.count − 1)
row_h    = clamp(usable_h / Σ weight, row_height.min, row_height.max)

# 6. 實際高度。第 5 步的夾制沒有作用時，h 恰好等於 budget。
h = row_h × Σ weight + row_spacing × (rows.count − 1) + padding.top + padding.bottom

# 7. 安全網是最外層的保證，沒有例外。第 5 步的下界把 h 推過上限時，下界讓位。
if h > avail × max_screen_ratio.<當前方向>:
    row_h = (avail × max_screen_ratio − padding.top − padding.bottom
             − row_spacing × (rows.count − 1)) / Σ weight
    h     = avail × max_screen_ratio

total = h + candidates.bar.height
          + (消歧欄正在顯示且 placement 生效為 above_candidates
             ? candidates.syllables.height : 0)          # §8.6.6.3.1
          + (honor_bottom_inset ? 系統底部 inset : 0)
```

`avail` = 當前方向下宿主視窗的可用高度（dp）。
`rows` 與 `weight` 來自**當前 layer**（§9.3）；`units` **完全不參與高度計算**。

第 1、3 步的 `key_spacing` / `row_spacing` **必須**取自**主題**，即使佈局用
§9.2 覆寫過它們。第 5、6 步（以及 §9.3 的列內佈版）才用覆寫後的值。
理由：預算若吃得到佈局的覆寫，佈局只要調一下鍵距、鍵盤總高就跟著變 ——
「同一份主題下任兩份佈局總高相同」這條保證會在最不起眼的地方破功。
（實測：`bopomofo-dachen` 的 `key_spacing: 4` 會讓它比其他佈局高 3%。）
`height_scale` 是唯一被允許改變預算的佈局欄位，因為那是作者的明示意圖。

**`padding` 算在 `h` 之內**（見第 6 步），`candidates.bar.height` 則是**外加**在
`h` 之上。兩者語義不同，最容易搞混。
`candidates.syllables.height`（§8.6.6.3.1）與 `bar.height` 同一類：**外加**，
而且**不得**從 `bar.height` 或 `budget` 裡挖 —— 挖了會讓候選字在組字途中上下跳，
或讓「同一份主題下任兩份佈局總高相同」只在沒有組字時成立。

##### 為什麼是這個模型（兩版錯誤的病歷）

* **v1 初稿**：`鍵盤高 = clamp(螢幕高 × ratio, min, max)`，鍵高再平分而來。
  致命缺陷是**鍵高綁螢幕高、鍵寬綁螢幕寬**，兩者各自獨立變動，長寬比失控。
  20:9 的長螢幕上實測 S24U 鍵高 68 dp、鍵寬 35 dp，長寬比 1:1.94，
  使用者的原話是「感覺被拉伸了,然後沒有自適應」。
* **v2**：`鍵高 = 鍵寬 × key_aspect`，`鍵盤高 = 鍵高 × 列數`。長寬比穩了，
  但**鍵盤總高隨列數線性成長**：使用者一打開數字列，鍵盤長高 20%，
  聊天視窗被擠掉一整段。v2 自己把這寫成「刻意的」，那句話是錯的 ——

  > 三星 Galaxy S24U 實機量測（截圖 1182×2560 為 1440×3120 等比縮圖，
  > 1 px = 0.3860 dp）：
  >
  > | 佈局 | 列數 | 鍵盤區總高 | 單鍵高 |
  > |---|---|---|---|
  > | 九宮格 | 4 | 510 px | 54.4 dp |
  > | 全鍵盤＋數字列 | 5 | 505 px | 43.2 dp |
  >
  > **總高只差 1%，鍵高差 26%。** 三星是把總高固定住、再除以列數。
  > Gboard 同樣是「固定鍵高」那一系（411 dp 與 456 dp 兩種寬度的機器上
  > 字母鍵都是 47 dp）。兩家主流實作都不讓鍵盤隨列數長高。

##### 取捨：總高固定與「長寬比隨欄數自適應」不可兼得

這一點必須寫在規範裡，否則下一個人會再走一次 v2 的路。

固定總高 ⇒ 列高固定 ⇒ 欄數一變（鍵寬變），長寬比就跟著變。反過來，
要長寬比恆定就得讓鍵高跟著鍵寬走，總高必然隨欄數與列數浮動。
兩者在數學上互斥，沒有第三條路。實測把選擇定死了：

* 從九宮格（5 欄）切到全鍵盤（10 欄），舊模型下鍵寬變一半、鍵盤高度變一半 ——
  這比「鍵稍胖稍瘦」明顯一個數量級。
* 使用者感知的是**鍵盤佔掉多少螢幕**，不是單顆鍵的胖瘦比值。

所以本模型選擇**總高固定**，欄數變化由**寬度方向**吸收（欄多則鍵窄），
這與所有出貨鍵盤一致。v2 用來反對固定鍵高的兩個理由，在預算模型下都不成立：

1. v2 說「固定鍵高無法處理欄數變化，11 欄的注音會比 10 欄的 QWERTY 瘦長」。
   **在預算模型下不會發生**：注音大千比 QWERTY 多一列，列高同時變矮。
   S24U ＋ `default-*` 實測 w/h：QWERTY 39.2/50.0 = 0.78，
   大千 35.1/38.5 = 0.91 —— 注音反而比 QWERTY **更胖**，不是更瘦長。
   欄數與列數在真實佈局裡是正相關的（欄多的佈局通常也列多），
   兩者對長寬比的影響方向相反，大致互相抵消。
2. v2 說「固定鍵高在平板上會產生極寬極扁的鍵」。**預算不是固定 dp 值**，
   它由 `ref_key_w × key_aspect` 推導，所以會隨螢幕變寬而變高；
   `key_height.max` 才是最終的守門員。平板上的行為與 v2 相同。

##### 兩組夾制，各守各的（不要混用）

| 欄位 | 夾制對象 | 什麼時候該動它 |
|---|---|---|
| `key_height.min` / `.max` | **參考鍵高**（第 2 步）＝預算的基準 | 要把鍵盤總高對齊某個參考鍵盤時 |
| `row_height.min` / `.max` | **分完之後的列高**（第 5 步） | 幾乎不動；它是可用性護欄 |

`row_height` 存在的理由：預算是固定的，列數卻不是。六列的佈局在小螢幕上
會把列高分到 20 dp 以下 —— 那種鍵按不到。護欄一旦生效，`h ≠ budget`，
鍵盤會長高（或變矮）。**這是刻意的**：與其守住總高卻給出按不到的鍵，
不如讓鍵盤高一點。實作 **應** 在護欄生效時產生 INFO 診斷。

三者的優先序是固定的，**不得**調換：
`max_screen_ratio`（第 7 步，硬上限）＞ `row_height`（第 5 步，可用性）
＞ `budget`（總高固定）。極矮的視窗上鍵按不到，好過鍵盤蓋掉整個畫面 ——
前者使用者按不準，後者他連自己在打字給誰看都看不到。

反過來，**不得**把 `key_height.min/max` 拿去夾制第 5 步的列高。那會讓
「五列的佈局」在下界處被撐開，總高固定的性質當場失效 —— 這是最容易做錯的一格。

#### 8.8.0.1 `key_aspect` 該設多少：實測基準

Gboard（Android 參考對象）在兩種螢幕上的實測值：

| 螢幕 | 鍵寬 | 鍵高 | 實際 aspect |
|---|---|---|---|
| 1080×2400 @420dpi（411.4 dp 寬） | 35.4 dp | 47.0 dp | 1.33 |
| 1440×3120 @505dpi（456.2 dp 寬） | 39.6 dp | 47.2 dp | 1.19 |

Gboard 的鍵高在兩種螢幕上都是 47 dp（差 0.2 dp），鍵寬卻從 35.4 變成 39.6。
在**預算模型**下這組數字的意義變得直接了當：`key_aspect` 描述的是
**參考格上那顆鍵**的胖瘦，而參考格是 10 欄 4 列，正好就是 Gboard 的字母鍵。
所以這張表可以逐字照抄成參數：預設 `key_aspect: 1.28` 搭配
`key_height: {min: 40, max: 56}`，在上表兩種螢幕上分別得到 44.4 dp 與 50.3 dp，
夾住 Gboard 的 47 dp。

**在需要精確對齊某個參考鍵盤時，正確做法是收緊 `key_height` 的上下界，
而不是去動 `key_aspect`。** 例：`cn-compact-*` 用 `{min: 54, max: 56}`
把預算對齊三星的 260 dp（實機九宮格列高 54.4 dp）。

> 預算模型讓這件事比 v2 簡單得多。v2 下同一組 `key_height` 夾制必須同時
> 命中 10 欄的全鍵盤與 7 欄的符號面板兩種鍵高，那是碰運氣；
> 現在只需要對齊**一個**數字——參考鍵高——其餘佈局自動跟上。

`reference_grid` 幾乎不需要改。它的用途是「這個主題心目中的標準鍵盤長什麼樣」，
10 欄 4 列涵蓋 Gboard / iOS / 三星。只有在做**刻意更矮或更高**的主題
（例如平板上的分離鍵盤）時才有理由動它。

#### 8.8.0.2 相容性與版本

本節在 v1 期間**第二次**改變既有欄位的語義，依 §5.2 本應遞增 major。
**本次仍不遞增**，理由與記錄見 §5.7。變更摘要：

| 變更 | 說明 |
|---|---|
| 移除 `height.portrait.*` / `height.landscape.*`（v1 初稿） | 已由 `max_screen_ratio` + `key_height` 取代 |
| `key_aspect` 語義收窄 | 從「每一顆鍵的長寬比」變成「**參考格上那顆鍵**的長寬比」 |
| `key_height.min/max` 語義收窄 | 只夾制參考鍵高，不再夾制實際列高 |
| 新增 `reference_grid.units` / `.rows` | 預算的分母，預設 10 / 4 |
| 新增 `row_height.min` / `.max` | 分完之後列高的可用性護欄 |

解析器遇到舊的 `height:` 區塊 **必須** 忽略它並產生一則 INFO 診斷。
**不得** 因此拒絕載入。既有主題不寫 `reference_grid` / `row_height` 也完全合法
——預設值就是本節推薦值，行為即為本節所述。

#### 8.8.1 `keyboard.key_styles`

開放映射。下列名稱有規範語義，解析器 **必須** 在缺失時以內建預設補齊：
`default`、`modifier`、`action`、`space`、`accent`。
自訂名稱合法；佈局引用不存在的名稱 → 退回 `default` + WARNING。

每個 key-style 的欄位：

| 欄位 | 型別 | 預設 |
|---|---|---|
| `background` | color | `#FFFFFF` |
| `pressed_background` | color | = `background` |
| `foreground` | color | `#000000` |
| `pressed_foreground` | color | = `foreground` |
| `active_background` | color | = `pressed_background` |
| `active_foreground` | color | = `pressed_foreground` |
| `corner_radius` | length | `metrics.corner_radius` |
| `border_width` | length | `metrics.border_width` |
| `border_color` | color | `transparent` |
| `elevation` | length | `metrics.elevation` |
| `font` | string | `"key"` | `typography.fonts` 中的名稱 |
| `label_size` | size | `22` |
| `hint_size` | size | `10` |
| `hint_color` | color | = `foreground`（alpha × 0.6） |
| `hint_position` | enum `top_right` \| `top_center` \| `top_left` \| `bottom_right` \| `none` | `top_right` |
| `icon_size` | size | `22` |

**「active」的觸發條件（規範性）。** 一個鍵在下列**任一**條件成立時
以 `active_background` / `active_foreground` 繪製：

1. 佈局把該鍵標了 `active: true`（無條件，見 §9.6）。
2. 該鍵的 `tap` 是 `layer:` / `layer_lock:`，且目標層就是**當前顯示中**的層。
3. 該鍵的 `tap` 是 `switch_layout:`，且目標佈局就是當前佈局。
4. 該鍵的 `label_from` 對應到一個 librime 布林開關，且**該開關為 ON**：

   | `label_from` | 對應開關 | active 條件 |
   |---|---|---|
   | `input_mode` | `ascii_mode` | 處於英文模式時 active |
   | `shape` | `full_shape` | 全形時 active |
   | `variant` | `simplification` | 簡體時 active |
   | `schema_name` / `schema_id` / `none` | — | 永不因狀態而 active |

5. shift 類鍵處於 `layer_lock` 鎖定狀態（`layer_once` 的一次性狀態
   **不算** active，實作 **應** 另以圖示區分，見 §9.6 的 `shift_lock`）。

第 4 條的原則是「**偏離預設狀態才 active**」：中文輸入法的預設是中文、半形、
當前字集，所以是 `ascii_mode` / `full_shape` / `simplification` 為 ON 時才高亮。
這條寫死在規範裡，否則四端會對「哪一邊算 active」得出相反結論。

#### 8.8.2 `keyboard.popup` / `keyboard.press_preview`

`popup`（長按彈出盤）：
`show`（bool，`true`）、`background`（color，`#FFFFFF`）、`foreground`（color，`#000000`）、
`highlight_background`（color，`#3060C0`）、`highlight_foreground`（color，`#FFFFFF`）、
`corner_radius`（length，`metrics.corner_radius`）、`item_size`（size，`22`）、
`item_padding`（length，`10`）、`elevation`（length，`6`）、`max_columns`（int 1–12，`6`）。

`press_preview`（按下時鍵上方的放大氣泡）：
`show`（bool，`true`）、`background`（color，= `popup.background`）、
`foreground`（color，= `popup.foreground`）、`size`（size，`28`）、
`corner_radius`（length，= `popup.corner_radius`）、`elevation`（length，`6`）。

平板 **應** 預設關閉 `press_preview`（實作層決定，不寫進格式）。

### 8.9 `motion`

| 欄位 | 型別 | 預設 | 平台 |
|---|---|---|---|
| `enabled` | bool | `true` | 全 |
| `respect_reduce_motion` | bool | `true` | 全 |
| `curve` | enum `standard` \| `linear` \| `decelerate` \| `accelerate` | `standard` | 全 |
| `key_press_ms` | duration | `40` | 行動 |
| `key_release_ms` | duration | `90` | 行動 |
| `candidate_change_ms` | duration | `120` | 全 |
| `popup_ms` | duration | `90` | 行動 |
| `window_show_ms` | duration | `110` | 桌面 |

`respect_reduce_motion: true` 且系統開啟「減少動態效果」時，
所有 duration **必須** 視為 `0`。

`curve` 的貝茲控制點（規範性，避免四端手感不同）：
`standard` = `cubic-bezier(0.2, 0.0, 0.0, 1.0)`、
`decelerate` = `cubic-bezier(0.0, 0.0, 0.2, 1.0)`、
`accelerate` = `cubic-bezier(0.4, 0.0, 1.0, 1.0)`、
`linear` = 線性。

> **候選更新動畫的紅線：** README 的效能預算是「按鍵到候選上屏一到兩幀」。
> `candidate_change_ms` **不得** 延後候選文字本身的可讀時機，
> 它只能作用於位移／淡入等裝飾層。實作若做不到，**必須** 直接忽略此欄位。

### 8.10 `feedback`（僅行動端）

| 欄位 | 型別 | 預設 |
|---|---|---|
| `haptic` | bool | `true` |
| `haptic_strength` | enum `none` \| `light` \| `medium` \| `heavy` | `medium` |
| `sound` | bool | `false` |
| `sound_volume` | ratio | `0.3` |

> **已知的分類瑕疵：** 觸覺回饋在概念上是「行為」不是「外觀」，放在主題檔裡並不乾淨。
> 放這裡的唯一理由是它隨主題散佈時使用者預期一起生效（擬物主題配重觸感、
> 極簡主題配關閉）。若日後拆出 `preferences`，這個區塊會被標記 deprecated 而非直接移除。

### 8.11 `platform_overrides`

`map<enum{android, ios, macos, windows}, <主題文件的任意子集>>`，預設 `{}`。
套用時機與規則見 §7.4 第 5 步。

### 8.12 `status_bar`（僅桌面端）

§11 原本列著「狀態列／工具列的外觀完全未規範」。這一節補上桌面端那一半，
行動端那一半在 §8.6.6.2。

**桌面端沒有工具列。** 行動端的工具列（§8.6.6.1）承擔的是主要導覽，因為
軟鍵盤沒有選單列可用；桌面端的等價物是 IMKit / TSF 提供的**輸入法選單**
（macOS 是選單列上那顆圖示，Windows 是語言列），方案切換與設定都在那裡，
而且是系統畫的、輸入法不能也不該重畫。

所以桌面端這一塊要解決的是另一個問題：**使用者看不見自己在什麼狀態**。
中／英、繁／简、全／半這幾個開關會被鍵盤上的操作改變，而候選窗一收起來
就沒有任何地方看得到它們。`status_bar` 是候選窗裡的一條狀態帶，
**預設關閉**（`show: false`）—— 大多數時候候選窗應該愈小愈好。

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `show` | bool | `false` | |
| `position` | enum `top` \| `bottom` | `bottom` | 在候選格陣的上方或下方 |
| `background` | color | `transparent` | |
| `height` | length 0–64 | `0` | `0` = 依內容（字高 + 2×`padding_v`） |
| `padding_h` | length 0–64 | `metrics.padding` | |
| `padding_v` | length 0–64 | `2` | |
| `spacing` | length 0–64 | `metrics.spacing` | 項與項之間 |
| `arrangement` | enum `leading` \| `center` \| `trailing` \| `space_between` | `leading` | |
| `size` | size | `11` | |
| `color` | color | `#808080` | |
| `active_color` | color | = `color` | 「當前這一態」那一段的顏色 |
| `separator.show` | bool | `false` | 與候選區之間的分隔線 |
| `separator.color` | color | `#808080` | |
| `separator.width` | length 0–8 | `1` | |
| `items` | list<status-item> | 見下方預設清單 | |

**status-item** 欄位：

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `source` | enum | — **必填** | 見下表 |
| `text` | string | `""` | 僅 `source: text` 使用 |
| `tap` | action \| null | `null` | §9.5 的 action 字串；點下去做什麼 |

`source` 的取值與顯示（規範性，四端字面一致）：

| `source` | 顯示 |
|---|---|
| `schema_name` | `rs_status.schema_name` |
| `schema_id` | `rs_status.schema_id` |
| `input_mode` | `is_ascii_mode ? "En" : "中"` |
| `input_mode_pair` | `中/En` 兩態同時顯示，見 §9.6 的 `label_from: input_mode_pair` |
| `shape` | `is_full_shape ? "全" : "半"` |
| `variant` | `is_simplified ? "简" : "繁"` |
| `page` | 頁碼，見下 |
| `text` | 該項的 `text` 欄位 |

**空狀態必須整項略過（規範性）。** 解析出來是空字串時（schema 尚未載入完成、
`source: text` 但 `text` 為空、只有一頁時的 `page`），該項 **必須** 完全不佔位置，
**不得** 畫成一塊看不出用途的空白。這與 §9.6 第 1 步「狀態值非空」是同一條規則。

**`page` 的顯示規則（規範性）：**

```
page_no == 0 且 is_last_page  → 空（不顯示）
否則                          → "<page_no + 1>" ，非最後一頁時後綴 "+"
```

只有一頁時不顯示的理由：候選只有一頁是最常見的情形，每次組字都掛一個「1」
是純粹的噪音。後綴用 `+` 而不是 `n/m`，是因為 librime 不提供總頁數
（`rs_menu` 只有 `is_last_page`），寫成 `1/3` 就得靠猜。

**預設清單（規範性，`items` 缺席時必須產生這一份）：**

```yaml
items:
  - { source: schema_name,     tap: "schema:picker" }
  - { source: input_mode_pair, tap: "input_mode:toggle" }
  - { source: variant,         tap: "toggle:simplification" }
  - { source: page }
```

`source` 缺席或未知 → 丟棄該項 + WARNING（`status_item_no_source`），不影響其他項。
`tap` 無法解析 → 該項仍然顯示但不可點 + WARNING（§9.5 的 `unknown_action` /
`bad_action_argument`）。**刻意與工具列不同**：工具列項目沒有 `tap` 就完全沒有用途，
而狀態列項目本來就以「顯示狀態」為主，可點只是加分。

**無必備項。** 與 §8.6.6.1 不同，這裡**不**規定「必須能觸達 `schema:picker`」——
桌面端的方案切換入口是系統畫的輸入法選單，永遠在那裡，主題刪不掉。
規定一個主題無法威脅到的必備項只會產生噪音。


##### 這一節與 §11 的關係（給第三個桌面端）

Windows 端回報「中／英、簡／繁的狀態指示完全沒有畫，因為沒有規範可依」。
**那個缺口在這一節關掉了**，不需要再等：`source: input_mode_pair` 與
`source: variant` 就是那兩個指示，字面（`中`/`En`、`简`/`繁`）是規範性的、
四端一致，預設清單裡也已經有它們。`status_bar.show` 預設 `false` 是刻意的，
但主題可以打開，而**實作必須支援** —— 「預設關閉」不是「可以不做」。

同理，「候選窗的多欄／表格排版沒有定義」在 §8.6.7.1 關掉了。
桌面端目前只剩下一個與狀態列有關的缺口仍在 §11：**`source` 全部是文字，
沒有圖示**。在補上之前，任何「一顆代表當前方案的小圖示」都是各端自己發明的，
不要做。

`status_bar` 的字體綁定見 §8.6.0（預設 `typography.fonts.ui`，可用 `font` 欄位改）。
`follow_caret: false` 時狀態列仍然在候選窗內部，不是螢幕上的另一條帶子。

### 8.13 `accessibility`

| 欄位 | 型別 | 預設 | 平台 |
|---|---|---|---|
| `announce_candidates` | enum `full` \| `text_only` \| `none` | `full` | 全 |
| `candidate_announcement` | string | `"{label} {text} {comment}"` | 全 |
| `announce_input_mode` | bool | `true` | 全 |
| `announce_page` | bool | `true` | 全 |

`candidate_announcement` 的佔位符與 §8.6.1 同源：`{label}`、`{text}`、`{comment}`。
**未知佔位符必須原樣保留。**

展開後 **必須** 把連續空白收成一個、並去掉前後空白 ——
空的 `{comment}` 會在朗讀器裡變成一個沒有理由的停頓。

`announce_candidates: text_only` 時只念候選文字本身（給覺得序號與註解太吵的使用者）；
`none` 時候選項不對輔助技術曝光（此時 **必須** 仍讓候選窗本身有一個 role，
否則朗讀器會報告一個沒有內容的視窗）。

> **這一節只規範「念什麼」，不規範「怎麼被摸到」。** 候選項在四端都是自繪的矩形，
> 要讓輔助技術摸得到得各自建語意節點，那是實作的事。但有一條跨端的教訓
> 必須寫在這裡：**朗讀名補上之後，動作也要補。**
> Android 端實測發現，TalkBack 的「輕點兩下」送的是無障礙的 `ACTION_CLICK`，
> **不會**變成觸控事件；macOS 的 VoiceOver 同理送的是 `accessibilityPerformPress`，
> 不會變成 `mouseDown`。只補名字的結果是一顆
> **念得出名字、聚焦得到、按下去什麼都不會發生**的候選 ——
> 而這種缺陷只有用朗讀器的人碰得到，所以更不會有人回報。

---

## 9. 鍵盤佈局格式（`rime-layout/1`）

**僅行動端消費。** macOS / Windows 實作 **必須** 完全不讀 `core/layouts/`。

### 9.1 文件層級欄位

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `format` | string | — **必填** | `rime-layout/1` |
| `id` | string | — **必填** | |
| `revision` | int | `1` | |
| `name` / `description` | localized-string | `""` | |
| `author` / `license` | string | `""` | |
| `inherits` | string \| null | `null` | §7.1 |
| `kind` | enum `alphabetic` \| `numeric` \| `symbol` \| `phonetic` \| `grid` \| `other` | `other` | 純資訊性，供 UI 分類 |
| `targets` | string-list | `["android","ios"]` | 資訊性 |
| `for_schema` | string-list | `["*"]` | **資格**：哪些方案可以用這份佈局。見 §9.1.1 |
| `auto_for_schema` | string-list | = `for_schema` 去掉 `"*"` | **自動命中**：切到哪些方案時自動換上它。見 §9.1.1 |
| `direction` | enum `ltr` \| `rtl` | `ltr` | `rtl` 時列內順序鏡射 |
| `default_layer` | string | 第一個 layer 的 id | 不存在 → F9 |
| `alpha_layer` | string \| null | `null` | 本佈局的拉丁字母層。只有 `input_mode:toggle` 讀它，見 §9.1.2。指向不存在的層 → F9 |
| `primary` | bool | `false` | 見 §9.5 的 `@primary` |
| `metrics` | 見 §9.2 | `{}` | |
| `layers` | list<layer> | — **必填** | 空 → F8 |
| `key_patches` | map<key-id, partial-key> | `{}` | §9.7 |

#### 9.1.1 `for_schema` 與 `auto_for_schema`

**這是兩個問題，所以是兩個欄位。**

| 欄位 | 回答的問題 | 誰在讀 |
|---|---|---|
| `for_schema` | 「這份佈局**可以**給哪些方案用？」 | 鍵盤類型選單（列出使用者選得到什麼） |
| `auto_for_schema` | 「切到某方案時**預設**該挑哪一份？」 | 下面的切換演算法第 1 步 |

`for_schema` 的 `"*"` 表示全部方案。
`auto_for_schema` **不接受** `"*"`：自動命中必須是明確點名的，
否則第一份寫 `"*"` 的佈局會把所有方案都吃掉。寫了 `"*"` → 忽略該筆 + WARNING。

`auto_for_schema` 未宣告時，預設為 `for_schema` 去掉 `"*"`。
這個預設讓最常見的情形（「只給某方案用、也是它的預設佈局」）一行都不用寫，
同時保證泛用佈局（`for_schema: ["*"]`）**永遠不會**搶到自動命中。

**切換演算法（規範性）。** 使用者切換 schema（`rs_select_schema`）後：

```
1. 找出所有 auto_for_schema 含 <新 schema id> 的佈局 → 自動命中。
   有命中 → 切到其中在搜尋路徑裡最先找到的那一份。結束。
2. 無命中時，檢查**當前佈局**是否仍適用於新 schema
   （matches := for_schema 含 "*" 或含 <新 schema id>）。
   仍適用 → 什麼都不做。結束。
3. 否則 → 切回 primary 佈局（`primary: true` 者）。
```

**第 2、3 步是 v1 初稿漏掉的，不補會讓使用者卡死。** 具體場景：
使用者從注音切回拼音，`bopomofo-dachen` 不適用於 `luna_pinyin`，
所以第 1 步無命中；若照初稿「無命中則沿用當前佈局」，使用者就會
**停在注音鍵盤上打拼音** —— 鍵面全是ㄅㄆㄇ，送出的卻是 ASCII，
畫面與行為完全對不上，而且他找不到任何一顆鍵可以離開。

> 使用者若曾為當前 schema **明確指定**過佈局，實作 **應** 記住該選擇並跳過
> 第 1 步。自動切換是便利機制，不該覆蓋使用者的明示意圖。

##### 為什麼非拆不可（合成一個欄位時發生了什麼）

v1 初稿只有 `for_schema`，它同時回答上表兩個問題。後果是：
**想給同一個方案提供第二份佈局的作者，被迫把 `for_schema` 寫成 `"*"`**
——因為那是唯一能避開第 1 步自動命中的寫法。本 repo 的
`cn-t9-pinyin-numrow.yaml` 曾經白紙黑字這麼註解：

> 不繼承 cn-t9-pinyin 的 ["t9_pinyin"]：避免兩份佈局搶同一個方案的自動命中。

**而那句 `"*"` 是個謊。** 照字面它宣告「九宮格佈局可以給 `luna_pinyin` 用」，
但九宮格送的是 `A/D/G/J/M/P/T/W` 八個代表鍵，那套字母表是 `t9_pinyin`
方案的 speller 契約（`speller/alphabet: 'ADGJMPTW'`）。配上 `luna_pinyin`
按「abc」送出的只是字母 a —— **鍵盤渲染正常，一個中文字也打不出來。**

Android 參考實作為了不讓這個謊傷到使用者，長出了兩層變通：

1. `"*"` 的佈局若 `inherits` 自一份有點名方案的佈局，就沿用父代的宣告
   （靠繼承關係反推作者真意）；
2. 泛用佈局只收 `kind: alphabetic`，把打不出字的 `"*"` 擋在選單外。

兩層都是在猜。欄位拆開之後**兩層一起刪掉**：作者老實寫
`for_schema: ["t9_pinyin"]` + `auto_for_schema: []`，資格與命中各自表述，
選單直接讀 `for_schema` 就對了，不必再從 `kind` 或 `inherits` 推論任何事。

```yaml
# cn-t9-pinyin：t9_pinyin 的預設九宮格
for_schema: ["t9_pinyin"]
auto_for_schema: ["t9_pinyin"]    # 可省略，這正是預設值

# cn-t9-pinyin-numrow：同一個方案的第二份佈局
for_schema: ["t9_pinyin"]         # 選單裡有
auto_for_schema: []               # 但不搶自動命中

# qwerty：泛用拉丁字母佈局
for_schema: ["*"]                 # 誰都能用
                                  # auto_for_schema 預設為 []，不會搶任何方案
```

這是「一套配置四端共用」的實際兌現點：使用者裝了注音方案，
四端裡的兩個行動端會自動換上 `bopomofo-dachen`，不需要各自再設定一次。

#### 9.1.2 `alpha_layer`：中英切換的另一半

`alpha_layer` 宣告本佈局的**拉丁字母層**。只有 `input_mode:toggle`（§9.5.2）讀它。

它存在的理由是一個真的壞掉過的東西：舊的 `toggle:ascii_mode` 只切引擎開關、
不動佈局。那在 QWERTY 上剛好沒問題（本來就是 26 鍵），在**九宮格上等於按了沒用** ——
使用者進了英文模式，眼前仍然是 `abc` / `def` / `ghi` 八顆鍵，26 個字母一個都打不出來。

規則：

* `alpha_layer` **必須** 是本佈局宣告過的層 id。指向不存在的層 → **致命錯誤 F9**
  （與 `default_layer` 同級：這不是外觀瑕疵，是那顆鍵會壞掉）。
* **不得** 跨佈局。跨佈局跳轉是 `switch_layout` 的事，而那條路徑會走進
  「進得去出不來」—— 從九宮格 `switch_layout:@primary` 跳到 `qwerty` 之後，
  `qwerty` 上沒有任何一顆鍵知道要回哪裡。
* 回程是**結構性**存在的：字母層是本佈局的一層，切回中文永遠落在 `default_layer` 上，
  **不需要**那一層自己記得放一顆回程鍵。
* 沒宣告 `alpha_layer` 的佈局（`qwerty`、`intl-*`）按下 `input_mode:toggle` 時
  **只切模式，佈局原地不動**。這是正確行為，不是退化。

> **非字母鍵盤應該宣告 `alpha_layer`。** 九宮格、筆畫、注音大千的主層都打不出
> 26 個字母，沒有字母層的話那顆中英鍵就是一顆「按了沒用」的鍵 ——
> 而「按了沒用」是本規範反覆點名要防的失敗類型。這是 **SHOULD** 而非 MUST，
> 因為判定「這份佈局是不是字母鍵盤」需要語義知識，格式無法檢查；
> 渲染端 **應** 在自己的建置期測試裡守住它（Android 端已經這麼做）。

#### 9.1.2.1 `alpha_layer` 的形狀必須等於 `default_layer`（規範性）

`alpha_layer`（以及它的 shift 層，若有）的**列數**與 **Σ`row.weight`**
**必須**等於 `default_layer`。不等 → 那一份佈局不合規。

── 為什麼 ──────────────────────────────────────────────────────────

§8.8.0 把**鍵盤總高**固定住了：總高不隨列數改變，所以列數一少，
每一列就變高。使用者按一下中英切換鍵，佈局換到 `alpha_layer`，
如果那一層少一列，他看到的是**整個鍵盤在原地被拉伸**。

實測（1080×2400 @420dpi，`default-light`，`cn-t9-pinyin-numrow`）：

| | 列數 | Σweight | 列高 |
|---|---|---|---|
| `t9`（中文） | 5 | 4.83 | 97 px |
| `alpha`（英文，改動前） | 4 | 4.00 | **125 px（+28.9%）** |
| `alpha`（改動後，補上數字列） | 5 | 4.83 | 97 px（差 **0%**） |

⚠ **對齊的方式是「兩層都矮」，不是「兩層都高」。** 補完之後英文鍵比
改動前矮 22.4%（125 → 97 px）。兩層都高＝鍵盤長高，而那正是 §8.8.0 的
v2 模型被實機量測否決的那條路。

⚠ **不是所有換層都適用。** 這一條只約束 `alpha_layer` —— 它是同一顆鍵
（`input_mode:toggle`）來回切的**同一個鍵盤的兩面**。經由 `layer:` 抵達的
符號頁（`num` / `punct` / `sym1`）有同型現象但不在本條範圍內：
那是使用者主動去到的另一個面，而且他知道自己換了頁。
（本 repo 已知三處：`cn-t9-pinyin-numrow` 的 `num`、`bopomofo-dachen` 的
`punct`、`intl-samsung` 的 `sym1`/`sym2`。列在這裡，免得下一個人再數一次。）

── 怎麼檢核 ────────────────────────────────────────────────────────

**本規範不指定它是致命錯誤還是建置期測試。** 新增一個只有行動端會發的
診斷碼，會動到 §10 第 9 條那張「四端報一樣多則」的比對表；在碼表補上
`layer_geometry_mismatch` 之前，行動端 **應** 用建置期測試守住隨附佈局
（本 repo：`keyboard/LayerGeometry.kt` ＋ `LayerGeometryTest`，掃
`core/layouts/` 每一份）。⚠ 在那之前，**第三方佈局違反這一條時不會有診斷**。

> 桌面兩端不消費 `core/layouts/`（§1.1），整條不適用。

### 9.2 `metrics`（佈局層級，覆寫主題）

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `row_spacing` | length \| null | `null` | `null` = 用主題的值 |
| `key_spacing` | length \| null | `null` | 同上 |
| `height_scale` | number 0.5–2.0 | `1.0` | 乘在**高度預算**上（§8.8.0 第 3 步），鍵盤總高隨之等比改變 |

注音佈局比 QWERTY 多一列，`key_spacing` 調小是常見需求，
所以這個覆寫點必須存在於佈局側而非主題側。

> **`height_scale` 不再是「多一列就要補償一次」的工具。** v2 的模型下，
> 每一份加了列的佈局都得自己算一個係數把鍵盤壓回去
> （本 repo 的 `cn-qwerty-numrow` 與 `cn-t9-pinyin-numrow` 都寫過
> `height_scale: 0.80`），而係數與主題的 `key_height` 夾制耦合，換個主題就得重算。
> §8.8.0 的預算模型已經把總高固定住，那類補償**一律應刪除**。
> `height_scale` 現在只該用在真正的意圖上：「這份佈局就是要比別的佈局高／矮」。
>
> 想讓某一列比其他列矮，用 §9.3 的 `row.weight`（例如數字列 `0.83`），
> 不要用 `height_scale` —— 後者動的是整個鍵盤。

### 9.3 `layer` 與 `row`

layer：

| 欄位 | 型別 | 預設 |
|---|---|---|
| `id` | string | — **必填**，layer 內唯一 |
| `label` | localized-string | `""` | 供 UI 顯示（如層切換選單） |
| `units` | number > 0 | 各 row `width` 總和的最大值 | 見下 |
| `rows` | list<row> | — **必填**，空 → F10 |
| `syllable_slots` | string-list | `[]` | 組字中讓給消歧欄的 `key.id`，見 §9.3.1 |

row：

| 欄位 | 型別 | 預設 |
|---|---|---|
| `weight` | number 0.1–4.0 | `1.0` | 相對列高 |
| `keys` | list<key> | — **必填**，空 → F10 |

**佈版演算法（規範性）：**

```
# 列高（keyboard_height 由 §8.8.0 第 6 步給出）
usable_h = keyboard_height − padding.top − padding.bottom
           − row_spacing * (rows.count − 1)
row_h[i] = usable_h * weight[i] / Σ weight

# 列內
items    = 該列實際排出的元素數
           = keys.count + (Σ width < units ? 1 : 0)   ← 尾端補白也是一個元素
usable_w = keyboard_width − padding.left − padding.right
           − key_spacing * (items − 1)
unit_w   = usable_w / units
key_w[j] = unit_w * width[j]
```

各鍵自左（`direction: rtl` 時自右）依序排列。
若 `Σ width < units`，剩餘空間留在該列末端；
若 `Σ width > units`，該列會溢出，實作 **必須** 等比壓縮該列
（`unit_w' = usable_w / Σ width`）並產生 WARNING。

> **`items` 而不是 `keys.count`。** 間距是排在**元素之間**的，`n` 個元素有
> `n − 1` 個間距；`Σ width < units` 時尾端那塊補白本身就是一個元素，
> 因此多帶一個間距。初稿寫 `keys.count − 1`，在有補白的列上會少算一個
> `key_spacing`，整列因此比實際寬 —— 差值不大（每顆鍵約 `key_spacing / units`），
> 但足以讓「照規範算座標」的自動化測試戳不中鍵。
>
> **兩條公式對 spacing 的算法必須一致。** v2 的 §8.8.0 用
> `key_spacing × (units − 1)` 推鍵高，本節用元素數推鍵寬，兩者對不上：
> `units` 一大（例如 `units: 23`），每單位寬就縮小，鍵高崩到下界
> （實測 `units: 23` 時鍵高 17.8 dp），而列內寬度卻完全正常。
> §8.8.0 改成預算模型之後，**高度計算完全不看當前 layer 的 `units`**，
> 這個分歧從根上消失了：本節是 `key_spacing` 唯一參與寬度計算的地方。

> **列高用 `Σ weight`，不是列數。** 一份佈局若把數字列寫成 `weight: 0.83`，
> 分母必須是 `4 + 0.83 = 4.83` 而不是 5，否則那一列變矮省下來的高度會憑空
> 消失（或被別的列吃掉），總高就不再等於 §8.8.0 的預算。
> v1 初稿在這裡寫的是列數，是錯的。

> 需要置中的短列（如 QWERTY 的 `asdfghjkl`）**應** 使用顯式的
> `{ spacer: true, width: 0.5 }` 佔位鍵，而不是仰賴任何自動置中規則。
> 自動置中是隱式行為，四端會做出四種結果。

#### 9.3.1 `syllable_slots`：組字中讓出去的格位（規範性）

```yaml
layers:
  - id: t9
    syllable_slots: ["pu_comma", "pu_period", "pu_question"]
```

宣告這一層有哪幾顆鍵在**組字中**讓給逐音節消歧欄（主題的
`candidates.syllables.placement: keyboard_slot`，§8.6.6.3）。值是**本層既有的 `key.id`**。
缺席或空清單 = 這一層沒有消歧欄，照常畫它自己的鍵。

**規範性條文：**

1. 每一筆 id **必須**在同一個 layer 內找得到對應的 `key.id`。找不到 →
   **丟棄該筆 + WARNING `syllables_slot_unknown`**（args `[layer-id, key-id]`），
   ⛔ **不得**默默略過。⚠ 這是「消歧欄整欄靜靜消失、畫面只是照常顯示標點」的
   唯一防線 —— 有人改一個 key id 就會發生，而且沒有任何東西會叫。
2. 可用格位**少於 2** 時，該 layer 視為沒有宣告，依 §8.6.6.3.3 的 D1 退化。
   下界為什麼是 2（翻頁鍵自己要佔一格）見該節。
3. 格位 **不得**落在**底列**。底列是導覽列；在組字中把一顆導覽鍵換掉，
   等於在 §9.5 的「進得去出不來」死路檢查上開一個測不到的洞 ——
   那個檢查走的是佈局檔的**靜態**內容，看不見執行期的替換。
4. 格位 **應**彼此落在**不同列**。同一列的多個格位不算錯，但那排出來的東西
   會與 `above_candidates` 的橫排長得一樣卻在別的位置，使用者分不出是哪一種。
5. 讀音**少於格位數**時，多出來的格位**必須顯示空格**。
   ⛔ **不得**顯示它原本的鍵面：讀音只有 `ni` / `mi` 時第三格還印著「？」，
   整欄變成「ni ／ mi ／ ？」，使用者沒有理由知道第三個不是第三個讀音 ——
   一欄只能有一種意思。也 **不得**畫一顆沒有字的鍵，那是一顆按得到、
   無障礙工具念得出「按鈕」、按下去沒反應的鍵。
6. 第 1、3、4 條 **必須**由渲染端的**建置期測試**逐條守著。理由同 §9.5.1 對佈局按鍵的
   處置：等到執行期才發現，使用者已經看著一個功能整個不存在的鍵盤了。

⚠ **這份宣告曾經寫死在 Android 的 `keyboard/T9Syllables.kt`**（白名單兩個佈局 id
＋寫死的 layer id ＋寫死的三個 key id）。搬進佈局檔是為了讓「新增一份九宮格佈局」
不必改任何一端的程式碼 —— 漏改的樣子正是第 1 條講的那種靜靜消失。

**桌面端整節不適用**：它不消費 `core/layouts/`（§1.1）。

### 9.4 `key`：送出什麼

**這是整份規範最要緊的一段。**

librime 的輸入貨幣是 **X11 keysym**（見 `third_party/librime/src/rime/key_table.h`），
不是字元、不是平台鍵碼。桌面端必須自行維護「原生鍵碼 → keysym」的映射表，
且會被使用者的實體鍵盤佈局（Dvorak、各國佈局）影響。

**行動端因為鍵盤是自繪的，可以完全繞開這個問題：佈局檔直接指定 keysym。**
這是本格式存在的最大實質價值之一，也是先做 Android 的理由之一。

`send` 有且僅有兩種形態，**互斥**：

```yaml
send: { keysym: "1", modifiers: [Shift] }   # 形態 A：走引擎
send: { text: "、" }                        # 形態 B：直接上屏
```

#### 形態 A：`keysym`（預設選擇）

```yaml
send:
  keysym: "<名稱 | 整數 | U+XXXX>"
  modifiers: [Shift, Control, Alt, Super]     # 選用，預設 []
```

行為：呼叫 `rs_process_key(session, keysym, modifiers)`。
回傳 `false`（librime 未消費）時，客戶端 **必須** 退回宿主的預設處理
（例如把該字元直接送進輸入框、或執行 BackSpace）。

`keysym` 的解析順序（規範性）：

1. 若匹配 `^U\+[0-9A-Fa-f]{4,6}$` → Unicode keysym：
   碼位 ≤ 0xFF 時 keysym = 碼位；否則 keysym = `0x01000000 | 碼位`。
2. 若匹配 `^0[xX][0-9A-Fa-f]+$` → 直接當數值。
   **十進位不被接受**：`"1"` 是 X11 的**名稱**（其 keysym 為 `0x31`），不是數值 `1`。
   這個歧義曾經是設計上的陷阱，因此規範只承認 `0x` 形式的數值。
3. 否則視為 X11 keysym 名稱，交由門面層的 **`rs_keysym_by_name()`** 解析
   （`core/include/rime_shell.h`；它包裝 librime 的 `RimeGetKeycodeByName()`）。
   客戶端 **可** 內建常用名稱的靜態表作為快路徑，但 **必須** 與門面層的結果一致；
   靜態表查不到時 **必須** 回落到 `rs_keysym_by_name()`，**不得** 直接把該鍵當成 noop。
4. `rs_keysym_by_name()` 查不到時回傳 **`0`** → 該鍵變成 `noop` + WARNING。

> **⚠ 兩個「查不到」的哨兵值,必須在邊界正規化。**
> librime 的 `RimeGetKeycodeByName()` 查不到時回傳 `XK_VoidSymbol`（`0xFFFFFF`），
> 那是個**看起來很像有效 keysym** 的值；門面層刻意把它正規化成 `0`。
> 於是解析器內部的靜態表（可能用 `0xFFFFFF` 當哨兵）與門面層（用 `0`）
> 各有一套約定。實作 **必須** 在呼叫門面層的那一行就把兩者統一，
> 並且 **不得** 把 `0` 或 `0xFFFFFF` 當成有效 keysym 送進 `rs_process_key()`。
> 送進去不會崩，只會安靜地什麼都不發生 —— 這種 bug 極難從症狀回推原因。

> **v1 初稿的缺陷：** 初稿直接要求回落到 librime 的 `RimeGetKeycodeByName()`，
> 但門面層當時沒有暴露等價函式，所以這條**在四端都無法實作** ——
> Android 參考實作只能讓查不到的 keysym 變成 noop，等於靜態表就是全部。
> `rs_keysym_by_name()` / `rs_keysym_name()` 補進 ABI 後本條才成立。
>
> 這也決定了解析器的一個設計約束：**綁定階段必須保留 keysym 的原始名稱字串**，
> 不能只存解析出的整數。名稱是回落所需的唯一輸入，丟掉就回不去了。
> （Android 參考實作的 `SendSpec.Keysym` 同時保留 `name` 與 `code`，
> `code == VOID_SYMBOL` 即代表「這顆要問門面層」。）

常用名稱（皆為 X11 標準名，非本規範發明）：
`space` `exclam` `quotedbl` `numbersign` `dollar` `percent` `ampersand` `apostrophe`
`parenleft` `parenright` `asterisk` `plus` `comma` `minus` `period` `slash`
`0`–`9` `colon` `semicolon` `less` `equal` `greater` `question` `at`
`A`–`Z` `bracketleft` `backslash` `bracketright` `asciicircum` `underscore` `grave`
`a`–`z` `braceleft` `bar` `braceright` `asciitilde`
`BackSpace` `Tab` `Return` `Escape` `Delete` `Home` `End` `Left` `Up` `Right` `Down`
`Page_Up` `Page_Down` `F1`–`F12` `Shift_L` `Control_L` `Alt_L` `Super_L` `Caps_Lock`

> ASCII 可見字元的 keysym 值等於其 ASCII 碼，所以 `"1"` 的 keysym 是 `0x31`。
> 但 **不得** 因此就把 `label` 當 keysym 用 —— 注音佈局的 `label` 是 `ㄅ`（U+3105），
> 而它必須送出的 keysym 是 `0x31`。

`modifiers` 的名稱與位元（對應 `rime_shell.h` 的 `rs_modifier`）：

| 名稱 | `rs_modifier` |
|---|---|
| `Shift` | `RS_MOD_SHIFT` (1<<0) |
| `Control` | `RS_MOD_CONTROL` (1<<1) |
| `Alt` | `RS_MOD_ALT` (1<<2) |
| `Super` | `RS_MOD_SUPER` (1<<3) |
| `Caps` | `RS_MOD_CAPS` (1<<4) |

未知的 modifier 名 → 忽略該筆 + WARNING。
`Release` **不得** 出現在佈局中（軟鍵盤不模擬 key-up）。

> `rs_modifier` 的位元值與 librime 內部的 `RimeModifier` **不同**
> （librime 的 Control 是 1<<2）。轉換是 `rime_shell` 那一層的責任，
> 佈局檔只使用上表的名稱。

#### 形態 B：`text`

```yaml
send: { text: "、" }
```

行為：**繞過 librime**，直接呼叫宿主的 commit 介面上屏該字串。

只有在 librime 無法表達該字元時才用它。實務上就兩類：

* 非 ASCII 且無對應 keysym 名的符號：`€` `£` `¥` `•` `§` `°` `…`
* 全形標點層裡不希望被 `punctuator` 二次轉換的字元

**誤用的後果很具體：** 把主佈局底列的 `，` 寫成 `send: { text: "，" }`
看起來能動，但使用者切到英文模式時它仍然吐出全形逗號，因為它從沒經過
librime 的 `punctuator`。正確寫法是 `send: { keysym: "comma" }`，
讓 librime 依 `ascii_punct` 開關自己決定要吐 `,` 還是 `，`。

##### 這條規則的適用範圍：只限**主佈局的標點鍵**

`kind: symbol` / `kind: numeric` 的**符號面板**是例外，**應**使用 `text`。

理由是面板的語義不同。主佈局的逗號鍵意思是「這裡要一個逗號，是哪一種請依
我現在的模式決定」——那正是 `punctuator` 該管的事。符號面板的鍵意思是
「我現在就要**這一個**字元」，是使用者剛剛翻頁挑出來的明示意圖，
不該再被模式開關二次解釋。

照 keysym 寫的後果很具體：中文標點頁的 `，` 與英文標點頁的 `,` 送出的是
**同一個 keysym `comma`**，實際吐什麼完全由 `ascii_punct` 決定。於是兩頁在
任一模式下輸出完全相同、**互為冗餘**，使用者翻到中文頁按下 `，` 卻拿到 `,`。

規範性：

* 主佈局（`kind` 非 `symbol` / `numeric`）的標點鍵 **應** 使用 `keysym`。
* 符號／數字面板的字元鍵 **應** 使用 `text`。
* 面板裡的 ⌫ / ␣ / ⏎ 仍 **必須** 走 `keysym` —— 那三顆得讓 librime
  有機會消費（退格要能刪 preedit，見 §9.5）。
* 兩種情形下，`text` 撞上組字中的處理一律依 §9.4.1。

`text` 為空字串 → 該鍵變成 `noop` + WARNING。

#### 9.4.1 `send.text` 撞上「正在組字」時的行為（規範性）

形態 B 繞過 librime，但**宿主的組字區（composing region）是 librime 開的**。
組字中直接呼叫宿主的 commit，新文字會**取代**整個組字區：使用者的 preedit
無聲消失，而 librime 那邊仍以為自己在組字，下一次按鍵就會把已經消失的
preedit 重新畫回來。這是 UI 與引擎狀態脫節，症狀難以歸因。

因此，處理 `send: { text: T }` **必須** 依下列順序：

```
1. 取快照。status.is_composing == false
   → 直接 commit(T)。結束。

2. 正在組字，先把組字結果沖出去：
   a. 呼叫 rs_commit_composition()
   b. 回傳 true 時，取下一份快照並把其中的 commit_text 送給宿主
   c. 結束宿主的組字區（Android: finishComposingText()）
3. 然後才 commit(T)。
```

規範性約束：

* **不得** 用 `rs_clear_composition()` 代替第 2a 步 —— 那會**丟掉**使用者已經
  輸入的內容。使用者按下一個標點，預期是「把剛打的字上屏，然後加標點」，
  不是「把剛打的字丟掉」。
* 兩次 commit **必須** 依「組字結果先、`T` 後」的順序送達；宿主 API 允許時
  **應** 合併為一次編輯，以免中間狀態被輸入框的監聽器看見。
* 第 2 步同樣適用於 `text` 之外的**任何**繞過引擎的上屏路徑
  （未來若新增其他形態，這條先於它們成立）。

> 注意這裡**刻意不**依 §`rs_commit_composition` 註解裡的 `menu.count` 政策分支。
> 那條政策回答的是「使用者按了確認鍵，現在該不該上屏」；
> 而這裡使用者按的是一顆完全無關的標點鍵，語義是「我這段打完了」，
> 所以無論 `menu.count` 為何都應該沖出去。兩者是不同的問題，不要混用。

### 9.5 `action`：非按鍵的功能鍵

action 是字串，語法 `<verb>` 或 `<verb>:<arg>[:<arg>]`。

| action | 語義 |
|---|---|
| `noop` | 什麼都不做（用於佔位） |
| `layer:<layer-id>` | 切到同檔的某層並停留 |
| `layer_once:<layer-id>` | 切到某層，**送出一個鍵之後**自動回到切換前的層 |
| `layer_lock:<layer-id>` | 切到某層並鎖定（渲染為 active） |
| `switch_layout:<layout-id>` | 切到另一份佈局檔 |
| `switch_layout:@primary` | 切回使用者的主要英數佈局（`primary: true` 者） |
| `switch_layout:@previous` | 切回上一次使用的佈局 |
| `toggle:<option>` | 切換 librime 開關，呼叫 `rs_set_option(!rs_get_option(o))` |
| `input_mode:toggle` | 「切中英」的**完整**語義：切 `ascii_mode`，**並且**切到本佈局的 `alpha_layer`。見 §9.5.2 |
| `set:<option>:<on\|off>` | 設定 librime 開關 |
| `schema:next` / `schema:prev` | 循環切換已啟用的 schema |
| `schema:picker` | 開啟 schema 選單 UI |
| `schema:<schema-id>` | 直接切到指定 schema |
| `candidate:select:<n>` | 選取本頁第 n 個候選（0 起算），`rs_select_candidate` |
| `candidate:delete:<n>` | 刪除本頁第 n 個使用者詞，`rs_delete_candidate` |
| `candidate:next_page` / `prev_page` | `rs_change_page` |
| `candidate:next` / `prev` | 高亮移動一格，見下方 |
| `cursor:left` / `right` / `home` / `end` | 移動輸入框游標（非組字游標） |
| `clear` | `rs_clear_composition` |
| `hide_keyboard` | 收起鍵盤 |
| `settings` | 開啟本 App 設定 |
| `emoji` | 開啟表情面板 |
| `syllables:toggle` | 開關逐音節消歧欄。**只在 `trigger: on_demand` 底下有意義**（§8.6.6.3）。⚠ **零端實作**，見 §8.6.6.3.4；這顆鍵的擺放另有硬性條件，見 §8.6.6.3.3 的 ⚠ |

常見的 `<option>`（librime 具名開關）：`ascii_mode`、`full_shape`、
`simplification`、`ascii_punct`、`extended_charset`。
本規範**不限定**可用的 option 名稱 —— 它們由 schema 定義，客戶端只是轉發。

**`candidate:next` / `candidate:prev` 的實作（規範性）。**
librime 沒有「把高亮移動一格」的原生概念，它只有「選定第 n 個」。
因此這兩個 action **必須** 這樣實作：

```
snap = rs_snapshot_acquire(s)
i    = snap.menu.highlighted            # 無候選時為 -1
if i < 0: 什麼都不做
next: i + 1 >= snap.menu.count → 若非最後一頁則 rs_change_page(前進) 並高亮該頁第 0 個
                                  否則不動（不環繞）
prev: i - 1 < 0                → 若非第一頁則 rs_change_page(後退) 並高亮該頁最後一個
                                  否則不動（不環繞）
其餘情況 → rs_highlight_candidate(s, i ± 1)
```

**不得環繞（wrap-around）。** 在最後一個候選再按 `next` 應該不動，
而不是跳回第一個 —— 連按時的環繞會讓使用者永遠選不到想要的字。

> **v1 初稿的缺陷：** 初稿只寫「高亮移動一格」，而門面層當時只有
> `rs_select_candidate`（選定並上屏）與 `rs_change_page`，
> 沒有「只移動高亮、不選定」的函式。Android 參考實作因此只能記一筆
> log 然後忽略。`rs_highlight_candidate(session, index_on_page)`
> 補進 ABI 後本條才成立。
>
> 注意 `rs_highlight_candidate` 與 `rs_select_candidate` 語義不同：
> 前者只移動高亮，後者會**選定**該候選（依方案可能直接上屏）。
> 把 `candidate:next` 實作成 `rs_select_candidate(i+1)` 是錯的。

未知 verb → 該鍵變成 `noop` + WARNING。
已知 verb 但參數缺失或不合法（如 `layer:` 指向不存在的層）→ `noop` + WARNING。

> **BackSpace / Return / space 不是 action。** 它們 **必須** 用 `send.keysym`，
> 因為 librime 在組字中會消費它們（退格刪除輸入碼、空白選字、Return 上屏原文）。
> 寫成 action 會讓組字狀態下的行為錯掉。

#### 9.5.1 渲染端的動詞支援宣告（規範性）

動詞表是四端共用的，**某一端還沒跟上是常態**。危險的不是沒跟上，
是沒跟上的**表現方式**：

```
ActionVerb.EMOJI -> log("表情面板尚未實作")
```

這一行在畫面上完全看不出來。鍵在、圖示在、按下去有按壓色也有震動，
只是什麼都沒發生 —— 而自動化測試會全綠，因為畫面確實正確。
本專案已經抓到四顆這種鍵（重輸、中英、按壓色、工具列表情），共同點都是這個。
最糟的一次是 `intl-gboard` 逗號的 `long_press: emoji`：§9.6 規定 long_press 勝過
popup，所以那個什麼都不做的 emoji 一直把逗號自己的標點盤整個遮住，
**四份佈局裡那盤符號從上線起沒有人叫得出來。**

因此，每個渲染端 **必須** 維護一份「本端未實作的 verb」清單，
並讓三個消費端各自照它做該做的事：

| 消費端 | 規則 |
|---|---|
| **工具列 / 狀態列項目** | **必須** 不渲染。項目沒有固定寬度，少一項不影響其他項的幾何，可以在執行期做 |
| **佈局按鍵** | **不得** 在執行期移除。鍵有寬度，少一顆整列會重排，使用者會看到一個位置飄移的鍵盤 —— 比一顆沒反應的鍵更糟。**必須** 改由渲染端的建置期測試擋下，讓佈局作者決定那個位置該放什麼 |
| **動作分派** | **必須** 在進分派表之前就據此早退，使分派表裡不會留下「安靜的 noop 分支」 |

三條共同的紀律：

* **解析層不受影響。** 解析出來的結果物件是文件的忠實表示，四端要拿它互相對照
  （§10 第 9 條）；「本端這一版剛好還沒做」是渲染端的事實，不該混進去。
* **不得產生診斷。** 這不是文件的缺陷，是實作的進度。發診斷會讓每一份主題
  都刷出一堆與作者無關的噪音。
* **不得從規範或 `core/` 的 YAML 裡刪掉那個動詞。** 別端做出來時，
  一份 YAML 都不必改就會自己回來。

**清單裡的每一項都必須寫明理由。** 沒有理由的項目，下一個讀到的人不知道
它是「還沒做」還是「這個形態上不存在」，也就不知道能不能刪。

已知的兩端宣告（資訊性，非規範）：

| 端 | 未實作 | 理由 |
|---|---|---|
| Android | `emoji` | 表情面板尚未實作 |
| Android | `candidate:next` / `candidate:prev` | 曾是 ABI 缺口，`rs_highlight_candidate` 補上後可移除 |
| macOS | `emoji` | 同上 |
| macOS | `hide_keyboard` | 桌面沒有軟鍵盤，**形態上不存在**，不是進度問題 |
| macOS | `cursor:*` | IMKit 沒有讓輸入法移動宿主 app 游標的 API |
| macOS | `layer` / `layer_once` / `layer_lock` / `switch_layout` | 桌面端不消費 `core/layouts/`，沒有「層」這個東西 |
| Android | `syllables:toggle` | `trigger: on_demand` 尚未實作，這顆鍵只在它底下有意義 |
| macOS | `syllables:toggle` | 同上；而且桌面端不渲染消歧欄（§8.6.6.3.5），在狀態列上留一顆按了沒反應的項目正是這一節要擋的事 |

> ⚠ `input_mode:toggle` **不在**桌面端的清單裡。它的語義是「切模式，並且切到
> 本佈局的 `alpha_layer`」，而規範明文規定沒宣告字母層的佈局**只切模式** ——
> 桌面端連佈局都沒有，所以它退化成純粹的模式切換，是做得到的。
> 把它列為不支援會讓狀態列的「中/En」整項消失，那才是真的壞掉。
#### 9.5.2 `input_mode:toggle`：切中英是一件事，不是兩件

```
1. ascii := !rs_get_option("ascii_mode")
2. rs_set_option("ascii_mode", ascii)
3. 本佈局有宣告 alpha_layer 時：
     ascii == true  → 切到 alpha_layer
     ascii == false → 切回 default_layer，並清掉 layer_once 的回程記錄
   沒宣告時 → 佈局不動
```

**為什麼要一個新動詞而不是讓 `toggle:ascii_mode` 也換層。** 因為
`toggle:<option>` 的語義是「轉發一個 librime 開關」，它對 `ascii_mode` 以外的
開關（`full_shape`、`simplification`…）都不該有佈局副作用。給其中一個開關
偷偷加上特例，下一個讀到的人不會知道，而且 §9.5 的表也寫不下這件事。

**桌面端**（不消費 `core/layouts/`）沒有「層」這個東西，所以第 3 步整個不成立，
本動詞退化成純粹的模式切換。這是**合規的**，不必宣告不支援（見 §9.5.1）。

### 9.6 `key` 的完整欄位

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `id` | string \| null | `null` | layer 內建議唯一；`key_patches` 靠它定位 |
| `label` | string | `""` | 鍵面主文字 |
| `hint` | string | `""` | 鍵面角落小字 |
| `icon` | string \| null | `null` | 語義圖示名，見下 |
| `label_from` | enum | `none` | 見下 |
| `width` | number 0.1–12.0 | `1.0` | 寬度權重（單位為 layer 的 `units`） |
| `style` | string | `"default"` | 主題 `key_styles` 中的名稱 |
| `spacer` | bool | `false` | `true` = 不繪製、不可點的佔位 |
| `active` | bool | `false` | 以 active 配色繪製（如當前層對應的層切換鍵） |
| `repeat` | bool | `false` | 按住時自動重複送出 `send` |
| `send` | send-spec \| null | `null` | §9.4 |
| `tap` | action \| null | `null` | §9.5 |
| `double_tap` | action \| null | `null` | |
| `long_press` | action \| null | `null` | |
| `popup` | popup-spec \| null | `null` | 見下 |
| `swipe` | map<enum{up,down,left,right}, sub-key> | `{}` | 見下 |
| `a11y_label` | string | `""` | 朗讀器要念的名字。留空時由渲染端依 §9.6.1 推導 |

`icon` 的規範名稱（渲染器自繪向量，格式不承載圖形資料）：
`backspace` `enter` `shift` `shift_lock` `space` `globe` `keyboard_hide`
`settings` `emoji` `search` `go` `done` `next` `clipboard` `undo` `mic`
`arrow_left` `arrow_right` `arrow_up` `arrow_down`。
`shift` 圖示在鎖定狀態下 **應** 由渲染器自動改繪為 `shift_lock` 樣式
（`layer_once` 的一次性狀態 **應** 與鎖定狀態在視覺上可區分）。

**圖示的退化路徑（規範性）。** 初稿要求「渲染器自繪向量」卻沒給退化路徑，
於是尚未備妥向量資源的實作只能各自挑字元，四端會挑出四種樣子。規則：

1. 有向量資源 → 畫向量。這是 **應**（SHOULD）而非必須。
2. 沒有 → 用下表的**規範性替代字形**。四端 **必須** 用同一份，不得自行挑選。
3. 該字形在當前字體中缺字 → 退回該鍵的 `label`。
4. `label` 也是空的 → 畫空白（**不得** 畫成 tofu 或問號）。

| icon | 替代字形 | icon | 替代字形 |
|---|---|---|---|
| `backspace` | `⌫` | `settings` | `⚙` |
| `enter` | `↵` | `emoji` | `☺` |
| `shift` | `⇧` | `search` | `⌕` |
| `shift_lock` | `⇪` | `clipboard` | `❐` |
| `space` | **空白**（見下） | `undo` | `↶` |
| `globe` | `🌐` | `mic` | `🎤` |
| `keyboard_hide` | `⌄` | `arrow_left` / `right` / `up` / `down` | `←` `→` `↑` `↓` |
| `go` / `done` / `next` | `↵` | | |

**`space` 刻意退化為空白。** 空白鍵靠尺寸與位置就足以辨識，
硬塞一個 `␣` 之類的字元在多數字體裡會變成方框，比空白更糟。
需要在空白鍵上顯示東西時，正確做法是用 `label` 或 `label_from`
（本 repo 的兩份佈局都用 `label_from: schema_name` 顯示當前方案名）。

**鍵面文字的解析順序（規範性）：**

```
1. label_from != none 且該狀態值非空  → 用狀態值
2. icon != null                       → 用圖示（依上面的退化路徑）
3. 否則                                → 用 label
```

**`label_from` 勝過 `icon`。** 初稿沒規定兩者同時出現時誰勝出，而空白鍵正是
這種鍵（同時有 `icon: space` 與 `label_from: schema_name`）。讓狀態勝出的理由：
`label_from` 承載的是**使用者當下必須看到的執行期資訊**（現在是中文還是英文、
用的是哪個方案），而 `icon` 是靜態裝飾。反過來的話，中／英切換鍵會變成一個
永遠不變的圖示，使用者無從得知自己在哪個模式。

第 1 步的「狀態值非空」是必要的保險：schema 尚未載入完成時 `schema_name`
是空字串，此時 **必須** 往下退到 `icon` / `label`，不得畫出一顆空白鍵。

**鍵面文字放不下時（SHOULD）。** 解析出的文字可能遠長於鍵寬
（`schema_name` 可以是「注音·臺灣正體」）。渲染器 **應** 等比縮小字級以求完整顯示，
下限為 `label_size × 0.5`；到下限仍放不下才截斷。
**不應** 直接截斷或省略號化 —— 方案名被截成「注音·臺」對使用者沒有意義。

`label_from` 讓鍵面顯示執行期狀態（取自 `rs_status`）：

| 值 | 顯示 |
|---|---|
| `none`（預設） | 用 `label` |
| `input_mode` | `is_ascii_mode ? "英" : "中"` |
| `input_mode_pair` | `中/En` —— 兩態同時顯示，當前那一態強調。見下 |
| `shape` | `is_full_shape ? "全" : "半"` |
| `variant` | `is_simplified ? "简" : "繁"` |
| `schema_name` | `rs_status.schema_name` |
| `schema_id` | `rs_status.schema_id` |

`label_from != none` 時 `label` 作為狀態不可用時的後備。
上表的中文字面是規範性的（四端一致），需要本地化時由客戶端資源覆蓋。

**`input_mode_pair` 為什麼不是外觀偏好。** 它與 `input_mode` 的差別是**歧義**，
不是美感：只寫一個「中」的切換鍵有兩種讀法 ——「現在是中文」與「按了會變中文」——
而它們指向相反的操作。真機使用者回報過。同時畫出兩態、強調當前那一態，
這個歧義**在結構上就不存在**，不必靠使用者猜。三星實機是同一個作法。

渲染（規範性）：

```
三段，順序固定：  "中"  "/"  "En"
is_ascii_mode == false → 第一段強調
is_ascii_mode == true  → 第三段強調
分隔線那一段永遠不強調
```

「強調」的具體表現由渲染端決定（前景色 + 粗體 vs. 未強調的 hint 色是建議作法），
但**兩態必須都看得見**，不得只畫當前那一態。

拉丁那一段用 `En` 而不是 `英`：一邊漢字一邊拉丁，兩態一眼就分得開；
兩邊都是漢字時，「中／英」在小字級下要辨認得花時間。

⚠ **`label_from: input_mode_pair` 的鍵不得再套用 §8.8.1 的 active 配色。**
那顆鍵已經在鍵面上同時畫出兩態並強調了當前那一態，再把整顆鍵染成 accent 色
只是重複同一個訊息，而且會讓「英文模式」看起來像「這顆鍵被鎖住了」。
（`label_from: input_mode` 則相反，它**應該**在 ascii 模式下畫成 active ——
它只有一個字，需要另一個訊號。）


#### 9.6.1 朗讀名（`a11y_label`）

自繪的鍵盤與候選窗在輔助技術眼中是一塊念不出來的方格。渲染端**必須**替
每一個可互動的元素提供朗讀名；`a11y_label` 讓佈局作者在推導不出合理讀法時
直接把答案寫下來。

**推導順序（規範性，`a11y_label` 為空時）。** 刻意與鍵面文字的解析順序**不同**：

```
1. a11y_label 非空                → 用它
2. label_from != none             → 念這顆鍵**切換什麼**（「中英切換」），
                                     現在停在哪一態走平台的「狀態」通道，不混進名字
3. icon != null                   → 念它**做什麼**：地球鍵是「選擇鍵盤」，不是「地球」
4. tap 是層／佈局切換             → 念**目標層自己宣告的名字**：
                                     「?123」念出來是「切換到 符號」，不是「問號一二三」
5. 否則                            → 念鍵面文字（字母、標點、漢字直接念就對了）
```

第 2–4 步的字面由**客戶端資源**提供（要在地化），格式只規定推導**規則**。

**`a11y_label` 存在的理由是第 5 步推不出來的鍵。** CJK 佈局上「々」「〆」
這類鍵直接念字元對朗讀器沒有意義，而它們既沒有 `icon` 也沒有 `label_from`，
規則怎麼寫都推不出合理讀法。這一格只有佈局作者知道答案。

**這是 OPTIONAL 能力。** 不實作朗讀名的渲染端仍然合規：它 **必須** 能解析
`a11y_label` 而不報錯，**不得** 因為忽略而產生 WARNING。但一旦實作了，
就 **必須** 連同**動作**一起實作 —— 見 §8.13 結尾那條跨端教訓：
只補名字的結果是一顆「念得出名字、聚焦得到、按下去什麼都不會發生」的鍵。

**popup-spec：**

| 欄位 | 型別 | 預設 |
|---|---|---|
| `layout` | enum `row` \| `grid` | `row` |
| `columns` | int 1–12 | `4`（僅 `grid` 有效） |
| `keys` | list<sub-key> | `[]`（空 → popup 視為不存在） |

**sub-key** 是 key 的子集，只認得 `label`、`hint`、`send`、`tap`、`style`。
其餘欄位出現 → 忽略 + WARNING。sub-key **不得** 再有 `popup` 或 `swipe`（巢狀）。

**swipe：** 值同樣是 sub-key。滑動距離門檻由實作決定（**應** 為 24 dp）。

**`swipe` 是 OPTIONAL 能力（規範性）。** 一個不實作 swipe 的渲染器仍然合規：
它 **必須** 能解析 `swipe` 而不報錯，**可** 完全忽略其行為，
且 **不得** 因為忽略而產生 WARNING（那會讓每一份佈局都刷出一堆噪音）。

理由是實作成本與收益不成比例：軟鍵盤的點擊、長按、自動重複、彈出盤選取
已經需要一個自寫的統一手勢辨識器，再加上四向拖曳偵測會與既有的
tap / long-press 偵測互搶事件。這是純粹的體驗加分項，不該擋住其他三端上線。

**因此，佈局作者 MUST NOT 把任何功能設計成只有 swipe 能觸達。**
每一個 swipe 都必須是某個「另有他途」的捷徑。`core/layouts` 的 12 份佈局裡
有 11 份用到 `swipe`，共 119 條，全部落在下表四類之一：

| swipe | 等效路徑 |
|---|---|
| 字母鍵上滑出數字 | 長按彈出盤、或 `numeric-symbol` 佈局 |
| 空白鍵左右滑動移動游標 | 直接點輸入框（宿主提供） |
| 退格鍵左滑清除 | 按住退格自動重複，直到清空 |
| 注音空白鍵上下滑翻頁 | 候選列的翻頁指示器 |

⚠ **現況（2026-08-13）：四端沒有一端實作 swipe 分派，所以上表左欄那 119 條
現在使用者一條都觸發不到。** Android 端的 `KeyboardView` 只有
`detectTapGestures`；桌面兩端根本沒有觸控手勢。這不是違規（`swipe` 是
OPTIONAL，而右欄那條路都還在），但它很容易被讀成「做好了」——
例如空白鍵左右滑的 `cursor:left/right`，程式碼裡有完整的分派、
單元測試也綠，而使用者按不到。所以：

* 每一份含 `swipe` 的佈局檔頭都標著「⚠ swipe 現在按不到」；
* Android 端由 `LayoutSwipeReachabilityTest` 守著三件事 ——
  沒有人分派 swipe（前提還成立）、每一條 swipe 都落在上表四類之一、
  每一份佈局檔頭都標了。哪天有人把滑動分派接上去，那一支會紅，
  提醒把這一段連同檔頭註解一起更新。

實作階段建議：**M1–M5 皆可不做，M6 之後再補**。

**觸發解析（規範性，避免四端手感不同）：**

| 手勢 | 解析順序 |
|---|---|
| 點擊 | `tap` → `send` → `noop` |
| 雙擊 | `double_tap` → 視為兩次點擊 |
| 長按 | `repeat`（勝出，見 §6.3）→ `long_press` → `popup` → 視為點擊 |
| 滑動 | `swipe[dir]` → 視為點擊 |

`spacer: true` 時所有互動欄位 **必須** 被忽略（不產生 WARNING，因為
`{spacer: true, width: 0.5}` 是慣用寫法）。

### 9.7 `key_patches`（佈局繼承的逃生口）

因為 §7.2 規定序列整體取代，繼承一份佈局只為了改一個鍵是不可行的。
`key_patches` 補上這個缺口：

```yaml
inherits: qwerty
key_patches:
  comma:
    label: "。"
    send: { keysym: "period" }
  space:
    long_press: "settings"
```

套用時機：§7.4 第 9 步之前，在 deep-merge **之後**。
對**所有 layer 的所有 row 中**，`id` 等於該 patch key 的每一個 key，
以 §7.2 的合併規則套用該 partial-key（因此 patch 內的 `null` 同樣是刪除）。

* 一個 id 在多層出現（如 `qwerty` 的 `shift` 同時在 `lower` 與 `upper`）→ **全部**套用。
  需要只改一層時，請在該 layer 內給該鍵不同的 `id`。
* patch 的 id 在佈局中找不到 → **忽略 + WARNING**（不是致命錯誤）。
* patch **不得** 新增或刪除鍵。需要增刪就整份重寫 `layers`。

> 這是一個刻意受限的機制。全功能的 JSON-Patch 式定址（`layers[0].rows[2].keys[5]`）
> 會在父佈局插入一個鍵時整份錯位，比不能 patch 更糟。

---

## 10. 一致性檢核清單

新平台的解析器上線前 **必須** 逐項通過：

1. `core/themes/` 與 `core/layouts/` 底下**每一份**檔案都解析成功，零 ERROR 診斷。
   （檢核清單不寫死份數 —— 寫死的那一刻，新加的檔案就自動免檢。）
2. `sakura-dark` 解析後：
   `palette.bg == #151016`（自身），`palette.fg == #E6E9EF`（繼承自 `default-dark`），
   `candidates.item.corner_radius == 14`（自身），
   `candidates.item.padding_h == 10`（繼承），
   `typography.fonts.candidate.family == ["Iansui", "$system"]`（整體取代，長度為 2），
   `typography.fonts.candidate.script_fallback` 仍含 `hans`/`jpan`/`kore`（映射逐鍵合併），
   `ancestry == ["default-dark", "sakura-dark"]`。
   `sakura-light` 解析後：`keyboard.key_styles.accent.border_width == 0` 且
   `border_color == transparent` —— 父主題 `default-light` 明明是 `1` 與 `$accent@35%`，
   被子主題的顯式 `null` 刪除後**回到規範預設值而非父值**。這是 §7.2 最容易做錯的一格。
3. 空文件 `{}` → 致命錯誤 F3，且客戶端仍能顯示上一個主題。
4. `background: "#ZZZ"` → 恰好一則 WARNING，該欄位為預設值，其餘欄位正常。
4b. `keyboard.height.portrait.ratio: 4.0` → 值被夾成 `0.6`，**且恰好產生一則 WARNING**。
   靜默夾制是不合規的：使用者寫了 `4.0` 卻拿到 `0.6`，不告知說不過去，
   而且會讓下面第 9 條（四端診斷數一致）直接失守。
   注意這條只約束**欄位綁定**時的夾制；執行期計算的夾制
   （如 §8.8 的 `HeightSpec` 高度解析、§4.4 的字體縮放）不產生診斷。
5. 未知欄位 `keyboard.blahblah: 1` → 恰好一則 WARNING，主題可用。
6. `bopomofo-dachen` 的 `b_` 鍵：`label == "ㄅ"`、`hint == "1"`、
   `send.keysym` 解析為 `0x31`。
7. `bopomofo` 層每一列的 `Σ width` 皆為 `11.0`。
8. `inherits` 指向自己 → 致命錯誤 F6，不得無限遞迴或堆疊溢位。
9. **診斷比對（作用域限定）。** 同一份壞檔案，四端報出的
   `(severity, code, path)` **序列**必須相同 —— 比對的是這個三元組，
   不是訊息文字（§6.5），也不含 `args` 與行號。

   ⚠ **但只在共用作用域內成立。** 本規範有形態專屬的區塊，桌面端整段不讀
   （§1.1），因此不可能為它們產生診斷。作用域規則：

   | 區塊 | 誰必須參與比對 |
   |---|---|
   | 除下列以外的一切 | **四端全部** |
   | `keyboard`、`feedback`、`candidates.bar`、以及所有佈局文件 | 僅 Android / iOS |
   | `candidates.window`、`status_bar` | 僅 macOS / Windows |
   | `candidates.syllables` 的**欄位**診斷 | **四端全部** —— 桌面端不渲染它，但**必須解析**（§8.6.6.3.5 第 1 點） |
   | §8.6.6.3.3 的**退化**診斷（`syllables_*`） | 僅 Android / iOS —— 它們相依於佈局與當前方案，不是主題單獨算得出來的（§8.6.6.3.2） |

   **所有致命錯誤（§6.2）一律屬於共用作用域**，無論它出現在哪個區塊：
   四端必須拒絕同一批文件，否則「這份主題在我的手機上壞掉，在電腦上正常」
   會變成一種正常現象。

   > 初稿沒有這條作用域限定，而它其實一開始就不成立：桌面端不讀 `keyboard`，
   > `keyboard.blahblah: 1` 在 Android 上是一則 WARNING、在 macOS 上是零則。
   > 要嘛要求桌面端把用不到的區塊也完整解析一遍（純粹為了產生診斷），
   > 要嘛承認比對有作用域。選後者。
10. `candidates.bar.toolbar.items` 缺席時，解析結果為 §8.6.6.1 的六項預設清單；
    主題若把 `items` 覆寫成不含 `schema:picker` 的清單，實作補回該項並發 INFO。
11. 空白鍵（同時有 `icon: space` 與 `label_from: schema_name`）在 schema 已載入時
    顯示方案名；schema_name 為空字串時退回圖示而非畫成空白鍵。
12. 系統字體縮放調到最大時，文字放大倍率恰為 `font_scale_max`（不是它的平方）。
13. 組字中按下 `send: {text: ...}` 的鍵：組字內容先上屏，標點接在其後，
    兩者皆不遺失（§9.4.1）。
14. 把 `keyboard.padding.top` 從 4 改成 20：鍵盤總高度**增加 16dp**，
    列高**減少**對應的量（padding 在預算之內，見 §8.8.0 第 3、5 步）。
15. 同一份主題在 411 dp 寬與 456 dp 寬的螢幕上，**參考鍵**的長寬比一致
    （皆為 `key_aspect`，除非撞到 `key_height` 夾制）。
16. **同一份主題下，任兩份佈局的鍵盤總高相同**（誤差 < 0.5 dp），
    無論列數是 4（`qwerty`）、5（`bopomofo-dachen`）還是含 `weight: 0.83`
    數字列的 5 列（`cn-qwerty-numrow`）。這是 v2 模型做不到的一條，
    也是本規範最容易被下一版改壞的一條。
16b. 同一份佈局開關數字列（`cn-t9-pinyin` ⇄ `cn-t9-pinyin-numrow`）：
    總高不變，列高由 54.0 變成 42.2（S24U ＋ `cn-compact-*`）。
16c. `units: 23` 的 layer 與 `units: 10` 的 layer 列高**相同** ——
    `units` 不參與高度計算。v2 下前者會崩到 17.8 dp。
17. 舊主題（含 `keyboard.height:` 區塊、無 `key_aspect`）仍能載入，
    產生恰好一則 INFO 診斷，且渲染採用預設的預算模型。
18. 佈局寫 `auto_for_schema: ["*"]` → 該筆被忽略 + 恰好一則 WARNING；
    `for_schema: ["t9_pinyin"]` + `auto_for_schema: []` 的佈局
    **出現在選單裡**，但切換方案時**不會**被自動選中。

19. **候選窗單行**（`lines: 1`、`orientation: horizontal`）：5 個各寬 40、高 20 的候選，
    `padding: 6`、`column_gap: 4` → 內容 216×20，窗 228×32，
    第 i 項的 x 為 `i × 44`。這是 v1 行為，任何改動都不得動到這一組數字。
20. **表格排版**：同樣 5 個候選、`lines: 2`：
    `horizontal` → 2 列 3 欄，落點依序 (0,0)(0,1)(0,2)(1,0)(1,1)；
    `vertical` → 3 列 2 欄，落點依序 (0,0)(1,0)(2,0)(0,1)(1,1)。
    **兩者的落點不同，這正是 row-major 與 column-major 的差別。**
21. **`overflow: shrink`**：3 個各寬 200 的候選、`max_width: 300`、`padding: 6`、
    `column_gap: 4` → 欄寬各縮成 93⅓，內容寬恰為 288，且**三項都被標記為需要截斷**。
    改成 `overflow: clip` 時欄寬維持 200、內容寬 608，窗寬被夾成 300。
22. **列高取該頁最高的項**：一項高 20、一項高 36（`comment.position: below`）→
    兩項的格高都是 36，內容高 36。取最小值或取第一項都會讓其中一項被切掉。
23. **狀態列的空狀態**：`schema_name` 為空字串時該項**完全不佔位置**；
    只有一頁時 `source: page` 同樣不顯示。
24. **`input_mode_pair`**：`is_ascii_mode == false` 時三段為
    `中`(強調) `/` `En`；`true` 時強調落在第三段。純文字形態恆為 `中/En`。
25. **未實作的動詞**：把 `emoji` 加進渲染端的未實作清單後，
    §8.6.6.1 的預設工具列少一項且**不產生任何診斷**；
    佈局裡指向 `emoji` 的鍵**不會**在執行期消失，而是讓建置期測試變紅。

26. **item 內部量測**（§8.6.4.1）。標籤寬 12、候選文字寬 40、註解寬 24，三段行高皆 20；
    `label_gap: 4`、`comment_gap: 4`、`comment_gap_v: 2`、`padding_h: 6`、`padding_v: 6`、
    `item.min_width: 0`：
    * `comment.position: after` → `inner_w = 12+4+40+4+24 = 84`，`w = 96`，`h = 32`。
    * `comment.position: below` → `top_w = 56`，`inner_w = max(56,24) = 56`，`w = 68`；
      `inner_h = 20 + 2 + 20 = 42`，`h = 54`。
27. **空段不留空隙**（§8.6.4.1 第 0 步）。承上，`label.show: false` →
    `after` 的 `inner_w = 40+4+24 = 68`，`w = 80`。
    **不是 `84 − 12 = 72`** —— 那是把 `label_gap` 留下來的錯法，畫面上會看到一格
    對不齊的空白。註解為空字串時同理：`inner_w = 12+4+40 = 56`，`w = 68`。
28. **`anchor`**（§8.6.7.3）。`follow_caret: false`、可用區 1440×860、窗 228×32、
    `offset_x: 0`、`offset_y: 6`：
    * `anchor: bottom_trailing` → 窗的下緣距可用區下緣 **6**，trailing 緣距可用區
      trailing 緣 **0**。
    * `anchor: top_leading` → 窗的上緣距可用區上緣 **6**，leading 緣距 leading 緣 **0**。
    位移恆為**向內**的邊距，不隨 `top`/`bottom` 改變正負號。
29. **`max_width` 不是硬上界**（§8.6.7.2 第二節）。單一量測寬 400 的候選、
    `max_width: 300`、`padding: 6`、`min_width: 0`、`item.min_width: 0`：
    * `overflow: shrink` → 欄寬縮成 288，窗寬 **300**，該項**被標記為需要截斷**。
    * `overflow: clip` → 欄寬維持 400，窗寬 **412**（9b 抬高了上界），
      該項**不**被標記為需要截斷。
    再把 `item.min_width` 設成 150、候選改成 3 個各寬 200、`column_gap: 4`：
    `shrink` 縮到 93⅓ 後被 `min_width` 夾回 150，`content_w = 458` 仍超出 →
    9a 把窗寬抬成 **470**，三項全部標記為需要截斷。
    （對照第 21 條：`item.min_width` 為 0 時同樣的輸入是窗寬 300。）
30. **不得丟棄候選**（§8.6.7.2 第一節）。3 個各寬 200 的候選、`max_width: 300`、
    `overflow: clip` → 排版結果**必須含有三項**，落點依序 x = 0 / 204 / 408，
    窗寬 300；需要截斷的項數為 **0**（它們是被窗蓋住，不是被截斷）。
    只回傳兩項、或把第三項的落點改掉，都是不合規。
31. **退化不改排版**（§8.6.7.4 第 1 條）。在第 19 條那組輸入上加開
    `backdrop: blur`、`opacity: 0.8`、`shadow.show: true` → 窗仍是 **228×32**、
    第 i 項的 x 仍是 `i × 44`；在完全不支援這三者的平台上也是同一組數字，
    只是多三則 INFO `feature_unsupported`（且這三則不參與第 9 條的比對）。
32. **`opacity: 0`** → 夾成 **0.05**，且恰好一則 WARNING `out_of_range`，
    path 為 `candidates.window.opacity`（§8.6.7.4 末段，規則同第 4b 條）。
33. **字體綁定**（§8.6.0）。主題只寫
    `typography.fonts.candidate.family: ["Iansui", "$system"]`，未寫任何 `font` 欄位 →
    候選文字用該堆疊、序號標籤用 `fonts.label`（未定義時為內建預設），
    兩者**不得**都退回系統 UI 字型。
    `candidates.label.font: "candidate"` → 標籤改用 candidate 堆疊，
    但字級仍是 `candidates.label.size`（**不是** `text.size`）。
    `candidates.label.font: "nope"` → 恰好一則 WARNING `bad_enum`，退回 `fonts.label`。
34. **消歧欄的欄位診斷四端一致**（§8.6.6.3.2）。`candidates.syllables.placemnt: above` →
    **四端各恰好一則** WARNING `unknown_field`，path 為 `candidates.syllables.placemnt`；
    `placement: sideways` → 四端各恰好一則 `bad_enum`，值退回 `keyboard_slot`。
    桌面端報零則就是不合規 —— 那表示它把整個區塊跳過了。
35. **預設值不改變既有主題的樣子**（§8.6.6.3）。完全沒有 `candidates.syllables:` 的主題
    解析後 `placement == keyboard_slot`、`trigger == while_composing`、`max_items == 0`、
    `height == 40`，且**不產生任何診斷**（附錄 A 那份最小主題必須維持零診斷）。
36. **D1 的門檻是 2**（§8.6.6.3.3、§9.3.1）。`placement: keyboard_slot` ＋ 該層
    `syllable_slots: ["pu_comma"]`（一格）→ 效力等同 `above_candidates`，
    **而且畫得出東西**；宣告兩格 → 維持 `keyboard_slot`。
    宣告 `["pu_comma", "nope"]` → 恰好一則 `syllables_slot_unknown`，可用格位剩 1，
    再依 D1 退化成 `above_candidates`（**一則診斷，不是兩則**）。
37. **求值順序**（§8.6.6.3.3）。當前方案沒有 `spelling_hints` ＋ `placement: keyboard_slot`
    ＋ 佈局沒有 `syllable_slots`（例：`luna_pinyin` 配 `cn-qwerty`）→ **零則診斷**。
    D2 必須排在 D1 之前；順序寫反的話，**每一份 QWERTY 佈局**都會刷一則 D1。
38. **高度**（§8.6.6.3.1）。`above_candidates`、`height: 40`、`bar.height: 44`：
    消歧欄顯示時的 `total` 比不顯示時**多 40**，而鍵盤自身的 `h` 與每一列的列高
    **兩者完全相同**。`placement: keyboard_slot` 生效時 `total` 與不顯示時**完全相同**。
39. **桌面端不畫、也不抱怨**（§8.6.6.3.5）。`intl-ios-light`
    （含 `candidates.syllables.placement: above_candidates`）在 macOS / Windows 鍵下解析：
    **零則 WARNING、零則 INFO**，且解析結果的 `placement` 就是 `above_candidates`
    —— 不得被改寫成 `none`，也不得產生 `feature_unsupported`。
    候選窗的排版數字與第 19 條**完全相同**（多一個區塊不得動到任何一個座標）。
40. **候選列的密度下界**（§8.6.4.2，**僅行動端 `candidates.bar`**）。基準情境固定
    `text.size: 20`、兩字 CJK、無序號無註解 —— 量的是主題往一格裡加的**固定開銷**
    （內距、間距、最小寬、右端保留區），不是主題自己的字級。`core/themes/` 的
    **每一份**主題在 360 dp 上 ≥ 5 個、411 dp 上 ≥ 6 個、456 dp 上 ≥ 6 個。
    ⚠ 這是下界式檢核，天生比等式鬆（§8.6.4.1 自承不規範字形量測）——
    它擋得住「某一份主題悄悄多加 10 dp 內距」，擋不住「某端只差 1 dp 就少一個」。
41. **每一份隨附主題都排得下**（§8.6.4.2）。第 40 條是公式，這一條是**逐份**
    套用它：`core/themes/` 底下**每一份** `.yaml`（掃目錄，不是手寫的白名單 ——
    這個 repo 已經因為清單寫死漏檢過兩次），在上面三個寬度上都不得低於下界。
    ⚠ 同一條也要驗**出口**（§8.6.6.4 第二段）。上一版只掃
    `scroll: expandable` ＋ `expand_button.show: true` 兩個欄位，
    **沒有看 `page_indicator`** —— 而右端那一顆是三選一：翻頁畫不出來時
    展開鍵是唯一的出口，展開鍵關掉時翻頁是唯一的出口，**兩個都關掉就是
    右端一片空白、使用者鎖死在第 1 頁**，而畫面完全正常。
    （關掉展開鍵那一半退回翻頁 —— 那是第 45 條禁止的；`page_indicator`
    也關掉的那一半連退路都沒有，上一版一句話都沒說。）
    所以這一條不是「檢查兩個欄位」，而是**逐份主題、逐種頁況**跑一遍
    §8.6.6.4 的判準，問「還有候選沒看到而右端一顆都畫不出來嗎」。
    最少要涵蓋 `(第 1/2 頁) × (是/不是最後一頁) × (本頁看得完/看不完)`
    八格，並附一條反向測試（兩條出口同時關掉 → 這一關必須紅）。
42. **註解與消歧欄互斥**（§8.6.3.1，**僅行動端 `candidates.bar`**）。候選的
    `comment` 逐則套 `reading_of()` 之後去重，結果**非空**就不畫註解 ——
    同一份讀音由消歧欄畫過了，而只有註解要付寬度。門檻刻意是 **1**，
    而消歧欄的門檻是 **2**：相同的話，讀音收斂到只剩一個時消歧欄收起來、
    註解同時跑出來，每一格寬度一起改變，使用者正在挑字而整列在他眼前重排。
    帶聲調的拼寫（`terra_pinyin` 的 `nǐ`）與 `simplifier` 的「〔简〕」解析不出讀音，
    **照畫**。桌面端的 `candidates.window` **不套用本條**（桌面端不畫消歧欄，
    關掉註解等於憑空少一份資訊）。
43. ⛔ **序號「按下去真的選得到」才畫**（§8.6.1.1，**僅行動端 `candidates.bar`**）。
    序號 `1 2 3` 只有一個用途：讓使用者按數字鍵選第 N 個。判準有**兩個**條件，
    任何一個問不出來就是**不畫**（fail-closed）：

    1. 當前層**直接按得到整排 `1`–`9`**（九顆 `send.keysym`，無 modifier）。
       `hint` 不算（角落小字不送出任何東西）；`swipe` 不算（**§9.6** 明列
       swipe 為 OPTIONAL，行動端可以不實作 —— 上一版這裡寫的是「§9.5」，
       而那一句在 §9.6，同文件 §8.6.1.1 寫的也是 §9.6）；`popup` / `long_press`
       也不算 —— 序號 `3` 承諾的是「按 3」，不是「長按某顆鍵再從盤裡挑 3」。
    2. **(佈局, 方案) 這一格在真機上量過而且按得到。**

    ⚠ **第 2 條不得由靜態判準取代，而且四端都一樣。** 「鍵送得出 3」與
    「按 3 會選第 3 個」之間隔著 librime 的兩層攔截，兩層都不在佈局檔裡：
    `speller/alphabet`（`bopomofo` 的 alphabet 含 `0-9`，數字是**字母**）與
    `recognizer/patterns`（`default.yaml` 的 `uppercase` 樣式字元集含 `0-9`，
    而九宮格刻意送大寫 `A/D/G/J/M/P/T/W`，於是整串組字落在那個樣式裡、
    數字被 recognizer 收走）。而且答案**會隨使用者打到哪裡而翻面**
    （`MGGAM` 不行、消歧成 `niGAM` 之後可以）—— 一個「每一刻都正確」的序號
    就是一個會在使用者眼前出現與消失、每次都讓整列重排的序號，那與第 42 條
    守的是同一條紀律。所以本規範要的是**保守的常數答案**：只要那個
    (佈局, 方案) 存在任何一個「按下去不選字」的常見狀態，整格就是不畫。

    **檢核方式：斷言綁在「按下去真的選中」，不綁在佈局名字上。**
    手搓一個「每顆鍵的 `send.keysym` 都是數字」的層再問「這層有沒有數字鍵」
    是同義反覆，永遠不會紅。要驗的是三件事，缺一不可：
    (a) 打字之後點**高亮那一格**記下上屏的詞 `T1`；
    (b) 重打之後按送得出 `1` 的那顆鍵，上屏的詞**必須逐字等於 `T1`**；
    (c) **畫面上畫了序號 ⟺ (b) 成立**（實心塊高亮的寬度差得出來：
    隨附主題兩字候選 56 dp vs 65.6 dp）。
    本 repo 的實作：`scripts/verify_selection_digit.sh`（`core/selection-digit.tsv`
    的產生器兼斷言者）與 `SelectionDigitTableTest`（CI 上沒有裝置的那一半）。
    2026-08-14 於 `lumina_test2` 重量的四格（唯一的真相是
    `core/selection-digit.tsv`，這裡只是抄錄；兩邊不一致時以那個檔案為準）：
    `cn-qwerty-numrow` × `luna_pinyin_tw` / `luna_pinyin` = **畫**；
    `cn-t9-pinyin-numrow` × `t9_pinyin` = **畫**（`MGGAM` 按 `3` 上屏第 3 個
    候選 `你敢`）；`bopomofo-dachen` × `bopomofo_tw` = **不畫**
    （那幾顆是注音字母鍵，按 1 得到 ㄅ、按 3 得到 ˇ）。
    ⚠ 第三格 2026-08-13 量到的是**不畫**（按 3 之後輸入框變成 `3⋯`，
    使用者已經打好的組字被毀掉）。它翻面是因為 Android 端在門面層攔下了
    專用數字列的 `1`–`9`（工單 #99）—— **那套機制目前不在本規範裡**，
    見 `docs/coordination.md`。這正是第 2 條「必須真機量過」的理由：
    同一個 `(佈局, 方案)` 的答案取決於**那一端自己**怎麼把按鍵送進 librime。
44. **高亮不得改變該格的量測寬度**（§8.6.4.3）。`item.highlight_style` 的
    `fill`（**預設**）、`underline`、`outline` 三種畫法，同一格的量測寬度
    **完全相同** —— 不然使用者每移動一次選字，整列就在他眼前重排一次。
    ⚠ 非 `fill` 的兩種**不准用 `text.highlight_color`**：那個顏色是設計來畫在
    重點色實心塊上的（隨附主題是 `$on_accent`＝白），底色換回 surface 之後
    白字畫在白底上就是看不見。`outline` 在 `highlight_border_width: 0` 的主題上
    必須退回一條看得見的邊，不得靜靜地變成「沒有高亮」。
    ⚠ `item.highlight_style` 的作用域是 **`candidates.bar.item`**（依第 9 條，
    僅 Android/iOS 需要認得）。寫在共用的 `candidates.item` 或桌面端的
    `candidates.window.item` 底下，四端**一律**產生 `unknown_field` 且**不生效**。
45. ⛔ **右端最多一「種」控制，而且量測扣掉的寬度就是畫出來的那幾顆**（§8.6.6.4）。
    ⚠ 標題說的是**種類**不是顆數：翻頁與展開不得同時出現（兩者解決的是同一個
    問題「還有更多」，一起出現是同一份資訊的第二份），但翻頁那一「種」在
    第 2 頁而且不是最後一頁時本來就是 `‹` `›` **兩顆**。
    上一版的標題寫「最多一顆」而內文自己列著一格 80 dp 的兩顆 ——
    只讀標題的另一端會做錯。
    而且：**本頁還有畫不出來的候選時，不得提供「下一頁」**—— 翻頁鍵的語義是
    「本頁我看完了」，本頁沒看完就給翻頁，等於讓使用者跳過他從未看見的候選，
    而畫面完全正常。
    ⛔ **判準是「這一顆真的會被畫出來」**：`page_indicator.show: false` 時
    翻頁那一組一顆都不畫，此時「回翻頁」＝右端空白＝鎖死在第 1 頁。
    出口必須存在，兩條都畫不出來是設計錯誤（見第 41 條）。
    ⚠ 「量測扣掉的 == 畫出來的」不是廢話，它是一條**會漂掉**的等式：把它拆成
    「量測時扣多少」與「實際畫幾顆」兩份實作之後，11 種頁況裡有 5 種對不上
    （第 1 頁又是最後一頁：扣 40 畫 0;第 2 頁起而右端是展開鍵:扣 80 畫 40 …），
    而**沒有任何東西會叫**。檢核用同一組參數逐格斷言「右端畫什麼」與
    「右端佔多寬」兩件事：`reserved_end: 40`、按鍵 40 dp、一格 56 dp、
    第 1 頁又是最後一頁 → **什麼都不畫** ＋ **0 dp**；第 2 頁且本頁看不完 →
    `EXPAND` ＋ 40 dp；第 2 頁又是最後一頁且本頁看得完 → `PAGER` ＋ 40 dp
    （只有「上一頁」一顆）；第 2 頁且本頁看得完 → `PAGER` ＋ 80 dp；
    面板開著（任何頁況）→ `EXPAND` ＋ 40 dp；
    `page_indicator.show: false` ＋ 本頁看得完 ＋ 還有下一頁 → `EXPAND` ＋ 40 dp。
    ⚠ 一併要扣的還有**候選之前**那一段：`bar.show_preedit_inline` 的行內組字串
    與右端保留區一樣是真的擠掉候選的（411 dp 的機器上打 `ni` 實測 33.7 dp，
    沒扣它時模型說 7 個而畫面只畫得出 6 個）。
    ⚠ 「一列排得下幾個」是**下界**（§8.6.4.2）：畫面上可以多出一個被裁掉
    一半的格子，那不違規（`font_scale: 1.30` 實測會）；反過來
    ——模型說的數目大於畫面上完整畫出來的格數——才違規。

---

## 11. 尚未規範、已知的缺口

誠實列出，避免各端各自發明：

* **主題預覽圖。** 主題商店需要縮圖，格式尚未定義 `preview` 欄位。
* **深色主題的自動生成。** 目前必須手寫 counterpart，沒有「由淺色推導深色」的機制。
* **`text` 與 `keysym` 的混合送出**（例如一鍵送出多個 keysym 序列）。
  倉頡的簡碼、日文的濁音變換可能會需要。
* **RTL 的完整語義。** `direction: rtl` 目前只鏡射列內順序，
  沒有處理 popup 展開方向與 hint 位置。
* **`schema:picker` 選單本身的外觀。** 它現在是方案切換的唯一入口，
  但格式只規定「有這個 action」，沒規定選單長什麼樣、用哪些顏色欄位。
  這是 §8.6.6.1 補完之後浮出來的下一個同類缺口。
* **候選列與鍵盤之間的過場。** `motion.candidate_change_ms` 只描述候選項自身，
  沒描述「工具列 ⇄ 候選列」的切換，而那是使用者每次組字都會看到的動畫。
* **`@previous` 只記得一份佈局。** 規範說它是「上一次使用的佈局」，實作只能是
  一格堆疊。兩份 `kind: symbol` 的佈局互相指著對方時就會來回彈，使用者回不到
  真正的起點（本 repo 的 `cn-symbols` ⇄ `numeric-symbol` 就撞過，已改成同檔
  換層繞開）。要根治得規範一個「佈局返回堆疊」的深度與清空時機。
* **成對符號與游標。** `“”`、`（）` 這類鍵，實機是插入一對並把游標移到中間；
  本格式一顆鍵只能有一個 action，`send.text` 之後接不了 `cursor:left`。
* **一個層沒有「我就是當前頁」的天然指示。** §8.8.1 規則 2 只在鍵指向**當前層**
  時才 active，而分頁鍵（△▽）依定義指向別層，所以翻頁面板無法標出當前頁。
  `cn-symbols` 的 `num` 層是靠「指向自己」繞過的，那是巧合不是機制。
* **佈局與主題沒有繫結。** 九宮格的鍵長寬比同時取決於欄數（佈局）與高度預算
  （主題）：`cn-t9-pinyin` 配 `cn-compact-*` 是 1.53（＝三星），配 `intl-gboard-*`
  會是 1.80。作者沒有辦法說「這份佈局是為那份主題設計的」。

* **`status_bar` 沒有圖示。** §8.12 的 `source` 全部是文字。桌面輸入法常見的
  「一顆代表當前方案的小圖示」描述不出來，而 §9.6 的語義圖示表是為鍵盤設計的。
* **候選窗的鍵盤操作沒有規範。** 桌面使用者會期待方向鍵移動高亮、
  `-`/`=` 翻頁，但那些是 librime 的 keybinding（方案層），不是本格式的。
  結果是「候選窗長什麼樣」由主題決定、「怎麼操作它」由方案決定，
  兩者無法互相對齊 —— 主題作者畫了上下排列的候選，方案的 keybinding 卻是左右翻頁。
* **消歧欄的方向不能覆寫。** §8.6.6.3 由 `placement` 推導方向，主題作者沒有辦法說
  「上方那一排改成直的」或「格位那一欄改成橫的」。今天兩種 placement 各自只有一種
  說得通的方向，所以先不開欄位；第一個真的需要的主題出現時再開。
* **消歧欄沒有專屬的外觀欄位。** 它沿用 `candidates` 的 §8.6.1–8.6.5，所以
  「已確定的音節與待選的音節長得不一樣」（語燕會變色）描述不出來。缺的是 `selected.*`，
  而它需要「哪些音節已確定」這個執行期狀態先變成主題看得見的東西。
* **消歧欄的內容不歸本格式管。** 見 §8.6.6.3.6：讀音從候選的 `comment` 反推，
  而 `rs_snapshot` 只給一頁。這是 `core/` 的 ABI 缺口，四端都卡在同一個地方
  （桌面端的「展開候選網格」要的是同一個 API）。

### 本節之外：v1 實作回饋已修補的項目

下列缺陷是 Android 端把 `bopomofo-dachen.yaml` 真正渲染出來時撞到的，
已在對應章節補齊,列此備查:§4.4.1（縮放重複套用）、§8.6.6.1（工具列）、
§8.8（padding 算在高度之內）、§8.8.1（active 的觸發條件）、
§9.1.1（schema 切換的退回規則）、§9.4（keysym 回落所需的 ABI）、
§9.4.1（`send.text` 撞上組字）、§9.5（`candidate:next/prev` 所需的 ABI）、
§9.6（鍵面解析順序、圖示退化表、swipe 為 OPTIONAL）。

第三輪（macOS 端做出 IMKit 與候選窗時的回饋）補的是：§6.5（診斷改成
`code` + `args`，訊息文字退出規範）、§8.6.6.2（工具列的排列與捲動）、
§8.6.7.1（候選窗的多行／表格排版與 `max_width` 超出時的處置）、
§8.12（桌面端狀態列）、§8.13（無障礙朗讀）、§9.1.2（`alpha_layer`）、
§9.5.1（渲染端宣告未實作的動詞）、§9.5.2（`input_mode:toggle`）、
§9.6（`label_from: input_mode_pair`、`a11y_label` 與朗讀名的推導順序）、
§10 第 9 條（診斷比對的作用域）。

第四輪（Windows 端做出 TSF 與桌面候選窗之後回報的六個缺口）補的是：
§8.6.0（文字區塊的字體綁定 + `font` 欄位）、§8.6.4.1（item 內部間距與量測演算法）、
§8.6.7.2（`max_width` 溢出:不得丟棄候選、`effective_max` 的兩條例外、
直排橫排同一套）、§8.6.7.3（`anchor`）、§8.6.7.4（`backdrop`/`opacity`/`shadow`
不支援時的退化與「退化不得改變排版」）、§8.7（桌面端組字串畫在哪）、
§8.12 末段（確認 §11 的多欄與狀態列兩項已由 §8.6.7.1 與 §8.12 覆蓋）、
§10 第 26–33 條。

第二輪（S24U 實機量測回饋）補的是:§8.8.0（高度模型改為固定預算 ÷ Σweight）、
§8.8.0.1（`key_aspect` 描述的是參考格上那顆鍵）、§9.1.1（`for_schema` 拆成
資格與自動命中兩個欄位）、§9.2（`height_scale` 不再拿來補償列數）、
§9.3（間距記在元素數上、列高用 Σweight）、§9.4（標點用 keysym 的規則
限定在主佈局，符號面板應該用 `text`）。

第五輪（Android 端把九宮格的逐音節消歧做出來之後回報的形狀）補的是：
§1.1（桌面端「解析」與「渲染」是兩件事）、§8.6.6.3（`candidates.syllables`：欄位表、
與 §8.8.0 預算的關係、解析／生效兩階段、四條退化規則與**求值順序**、
**已定義但零端實作的清單**、桌面端的預期行為與日後要長成的形狀）、
§8.8.0 的 `total`、§9.3.1（`syllable_slots`）、§9.5（`syllables:toggle`）、
§9.5.1 兩端的宣告、§6.5.1 的三個 `syllables_*` code、
§10 第 9 條的作用域再切一刀（欄位診斷共用、退化診斷行動端專屬）、§10 第 34–39 條。
⚠ 這一輪補的東西裡，`on_demand`、`syllables:toggle` 與四則 `syllables_*` 診斷
**都還沒有任何一端實作**，§8.6.6.3.4 把這件事列成一張表，不要當成已可用。

---

## 附錄 A：最小可用主題

```yaml
format: rime-theme/1
id: minimal
appearance: light
palette:
  accent: "#2563EB"
candidates:
  item:
    highlight_background: "$accent"
```

其餘一切走預設值。這份檔案 **必須** 能在四端載入並產生可用的 UI。

## 附錄 B：最小可用佈局

```yaml
format: rime-layout/1
id: minimal-layout
layers:
  - id: main
    rows:
      - keys:
          - { label: "a", send: { keysym: "a" } }
          - { label: "b", send: { keysym: "b" } }
          - { icon: backspace, send: { keysym: "BackSpace" }, repeat: true }
```

`units` 自動推導為 `3.0`，`default_layer` 自動推導為 `main`。
