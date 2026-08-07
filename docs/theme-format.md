# 主題與鍵盤佈局格式規範 v1

> 本文件是**規範性（normative）**文件。四端（Android / iOS / macOS / Windows）各自
> 實作解析器，唯一的一致性來源就是本文件。凡本文件未明確規定者，即為規範缺陷，
> 請提 issue 而非各自發明。
>
> 對應檔案：`core/themes/*.yaml`、`core/layouts/*.yaml`
> 參考實作：`android/app/src/main/java/org/rimequad/ime/theme/`

---

## 0. 用語

| 詞 | 意義 |
|---|---|
| **MUST / 必須** | 不遵守即為不合規實作 |
| **MUST NOT / 不得** | 同上 |
| **SHOULD / 應** | 有正當理由可偏離，但須在實作中留註記 |
| **MAY / 可** | 純選用 |
| **診斷（diagnostic）** | 解析過程產生的一則 `{severity, path, message, line}` 記錄 |
| **致命錯誤（fatal）** | 文件被整份拒絕，不產生任何結果物件 |
| **可回復錯誤（recoverable）** | 該欄位改用預設值，解析繼續，並產生一則 WARNING 診斷 |

---

## 1. 範圍與設計原則

### 1.1 兩種形態，一套配置

| | 行動端（Android / iOS） | 桌面端（macOS / Windows） |
|---|---|---|
| UI 型態 | 整塊自繪軟鍵盤 + 上方候選列 | 懸浮候選窗，無軟鍵盤 |
| 消費 `core/themes/` | 全部 | 除 `keyboard` 與 `feedback` 之外全部 |
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

### 5.7 v1 期間的一次性例外（鍵盤高度模型）

§8.8 的高度模型在 v1 發布後被改寫，`keyboard.height.*` 被 `key_aspect` /
`key_height` / `max_screen_ratio` 取代。依 §5.2 這應該遞增 major，
**本次刻意不遞增**，理由如下：

1. 舊模型是**缺陷**而非設計選擇 —— 它讓鍵高綁螢幕高、鍵寬綁螢幕寬，
   長寬比必然失控。保留它沒有任何價值。
2. 此時尚無任何第三方主題（客戶端版本 0.1.x），影響範圍為零。
3. 遞增 major 會讓所有 v1 文件被拒絕載入，代價遠大於收益。

補償措施：舊 `height:` 區塊被忽略時 **必須** 產生 INFO 診斷（§8.8.0.2），
使用者看得到「你的主題用的是舊高度模型」。

**這個例外不會再有第二次。** 客戶端進入 1.0 之後，任何語義變更一律遞增 major。

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
每則診斷 **必須** 含：嚴重度、YAML 路徑（如 `keyboard.key_styles.default.background`）、
可讀訊息、以及**行號**（若讀取層能提供）。

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
| `max_width` | length | `640` | 超出則換行／截斷，由實作決定 |
| `placement` | enum `below` \| `above` \| `auto` | `auto` | `auto` = 空間不足時翻面 |
| `offset_x` / `offset_y` | length −64–64 | `0` / `6` | 相對插入點的位移 |
| `follow_caret` | bool | `true` | `false` = 固定在螢幕角落 |
| `backdrop` | enum `none` \| `blur` \| `vibrancy` | `none` | 不支援的平台 **必須** 靜默退化為 `none`（INFO 級診斷） |
| `opacity` | ratio | `1.0` | |
| `shadow.show` | bool | `true` | |
| `shadow.radius` | length 0–64 | `18` | |
| `shadow.offset_x` / `offset_y` | length | `0` / `4` | |
| `shadow.color` | color | `#00000040` | |

`window` **可** 覆寫 §8.6.1–8.6.5 的任一子區塊。

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

### 8.8 `keyboard`（僅行動端；桌面端必須整段忽略）

| 欄位 | 型別 | 預設 |
|---|---|---|
| `background` | color | `#D0D0D0` |
| `key_aspect` | number 0.6–2.5 | `1.28` |
| `key_height.min` | length 20–200 | `40` |
| `key_height.max` | length 20–200 | `56` |
| `max_screen_ratio.portrait` | ratio 0.2–0.8 | `0.45` |
| `max_screen_ratio.landscape` | ratio 0.2–0.9 | `0.62` |
| `padding.left` / `.right` / `.top` / `.bottom` | length | `5` / `5` / `4` / `4` |
| `row_spacing` | length 0–32 | `12` |
| `key_spacing` | length 0–32 | `6` |
| `honor_bottom_inset` | bool | `true` |

#### 8.8.0 高度模型：由鍵寬推鍵高，不由螢幕高推鍵盤高

**規範 v1 初稿的模型是錯的，本節取代它。** 初稿以
`鍵盤高 = clamp(螢幕高 × ratio, min, max)` 為主，鍵高再由鍵盤高平分而來。
這個模型有一個致命缺陷：**鍵高綁在螢幕高度上，鍵寬卻綁在螢幕寬度上**，
兩者各自獨立變動，鍵的長寬比因此完全失控。在 20:9 的長螢幕上
（實測 S24U：ratio 0.33 撞上 `max: 300` → 鍵高 68dp，而鍵寬只有 35dp）
長寬比被拉到 1:1.94，使用者的原話是「感覺被拉伸了,然後沒有自適應」。

正確的因果方向是**鍵寬 → 鍵高 → 鍵盤高**：

```
# 1. 鍵寬:由可用寬度、欄數、鍵距決定。與螢幕高度無關。
key_w = (kb_width − padding.left − padding.right − key_spacing × (units − 1)) / units

# 2. 鍵高:由鍵寬乘上 key_aspect，再夾制到絕對上下界。
#    height_scale 來自佈局（§9.2），在夾制之後套用。
key_h = clamp(key_w × key_aspect, key_height.min, key_height.max) × height_scale

# 3. 鍵盤高度是**算出來的結果**，不是設定值。
h = key_h × rows + row_spacing × (rows − 1) + padding.top + padding.bottom

# 4. 安全網:鍵盤永遠不得超過螢幕的這個比例（摺疊機、平板橫放、超小螢幕）。
h_cap = avail × max_screen_ratio.<當前方向>
if h > h_cap:
    # 反推鍵高，讓鍵盤剛好塞得下；此時 key_aspect 讓位給安全網。
    key_h = (h_cap − padding.top − padding.bottom − row_spacing × (rows − 1)) / rows
    h = h_cap

total = h + candidates.bar.height + (honor_bottom_inset ? 系統底部 inset : 0)
```

`avail` = 當前方向下宿主視窗的可用高度（dp）。
`units` 與 `rows` 來自**當前 layer**（§9.3），所以：

* 注音大千是 11 欄，鍵寬比 QWERTY 的 10 欄窄，鍵高**跟著等比變窄**，
  兩份佈局的鍵長得一樣胖瘦。舊模型下 11 欄的鍵會比 10 欄的更瘦長。
* 鍵盤總高會隨佈局的列數改變（大千 5 列比 QWERTY 4 列高一列）。
  這是刻意的：列數不同本來就該不同高，硬壓成同高就會壓扁。

**`padding` 算在 `h` 之內**（`h` 已含 padding，見第 3 步），
`candidates.bar.height` 則是**外加**在 `h` 之上。兩者語義不同，最容易搞混。

#### 8.8.0.1 `key_aspect` 該設多少：實測基準

Gboard（Android 參考對象）在兩種螢幕上的實測值：

| 螢幕 | 鍵寬 | 鍵高 | 實際 aspect |
|---|---|---|---|
| 1080×2400 @420dpi（411.4 dp 寬） | 35.4 dp | 47.0 dp | 1.33 |
| 1440×3120 @505dpi（456.2 dp 寬） | 39.6 dp | 47.2 dp | 1.19 |

**注意這組數字說的事情，和「aspect 是常數」不一樣。**
Gboard 的鍵高在兩種螢幕上都是 47 dp（差 0.2 dp），鍵寬卻從 35.4 變成 39.6 ——
也就是說 Gboard 用的是**固定鍵高**，aspect 只是被動的結果。

本規範仍選擇 aspect 作為主控參數，而不是照抄固定鍵高，理由有二：

1. 固定鍵高無法處理欄數變化。11 欄的注音在固定鍵高下會明顯比 10 欄的 QWERTY 瘦長，
   而這正是初稿模型被詬病的同一個病灶，只是換了個地方發作。
2. 固定鍵高在平板上會產生極寬極扁的鍵（800 dp 寬的平板上鍵寬 76 dp、鍵高 47 dp）。

`key_height.min` / `max` 就是用來把 aspect 的結果拉回 Gboard 那條線的：
預設 `key_aspect: 1.28` 搭配 `key_height: {min: 40, max: 56}`，
在上表兩種螢幕上分別得到 44.4 dp 與 50.3 dp，夾住 Gboard 的 47 dp。
**在需要精確對齊某個參考鍵盤時，正確做法是收緊 `key_height` 的上下界，
而不是去動 `key_aspect`。**

#### 8.8.0.2 相容性與版本

本節改變了既有欄位的語義，依 §5.2 本應遞增 major。**本次刻意不遞增**，
並把它記為 v1 期間的一次性例外（見 §5.7）。移除的欄位：

| 移除 | 取代 |
|---|---|
| `height.portrait.ratio` / `height.landscape.ratio` | `max_screen_ratio.<方向>`（語義從「目標」變成「上限」） |
| `height.portrait.max` / `height.landscape.max` | `max_screen_ratio` + `key_height.max` |
| `height.portrait.min` / `height.landscape.min` | `key_height.min` |

解析器遇到舊的 `height:` 區塊 **必須** 忽略它並產生一則 INFO 診斷，
指出該主題使用的是已被取代的高度模型、且已改用預設的 aspect 模型。
**不得** 因此拒絕載入 —— 舊主題只是會長得跟作者預期不同，而不是壞掉。

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
| `for_schema` | string-list | `["*"]` | 見 §9.1.1 |
| `direction` | enum `ltr` \| `rtl` | `ltr` | `rtl` 時列內順序鏡射 |
| `default_layer` | string | 第一個 layer 的 id | 不存在 → F9 |
| `primary` | bool | `false` | 見 §9.5 的 `@primary` |
| `metrics` | 見 §9.2 | `{}` | |
| `layers` | list<layer> | — **必填** | 空 → F8 |
| `key_patches` | map<key-id, partial-key> | `{}` | §9.7 |

#### 9.1.1 `for_schema`

宣告本佈局適用於哪些 librime schema id。`"*"` 表示全部。

**切換演算法（規範性）。** 使用者切換 schema（`rs_select_schema`）後：

```
1. 找出所有 for_schema 不含 "*" 且含 <新 schema id> 的佈局 → 精確命中。
   有命中 → 切到其中在搜尋路徑裡最先找到的那一份。結束。
2. 無精確命中時，檢查**當前佈局**是否仍適用於新 schema
   （matches := for_schema 含 "*" 或含 <新 schema id>）。
   仍適用 → 什麼都不做。結束。
3. 否則 → 切回 primary 佈局（`primary: true` 者）。
```

**第 2、3 步是 v1 初稿漏掉的，不補會讓使用者卡死。** 具體場景：
使用者從注音切回拼音，`bopomofo-dachen` 的 `for_schema` 不含 `luna_pinyin`
所以第 1 步無命中；而 `qwerty` 是 `"*"`、依定義不算精確命中。
若照初稿「無命中則沿用當前佈局」，使用者就會**停在注音鍵盤上打拼音** ——
鍵面全是ㄅㄆㄇ，送出的卻是 ASCII，畫面與行為完全對不上，
而且他找不到任何一顆鍵可以離開。

> 使用者若曾為當前 schema **明確指定**過佈局，實作 **應** 記住該選擇並跳過
> 第 1 步。自動切換是便利機制，不該覆蓋使用者的明示意圖。

這是「一套配置四端共用」的實際兌現點：使用者裝了注音方案，
四端裡的兩個行動端會自動換上 `bopomofo-dachen`，不需要各自再設定一次。

### 9.2 `metrics`（佈局層級，覆寫主題）

| 欄位 | 型別 | 預設 | 說明 |
|---|---|---|---|
| `row_spacing` | length \| null | `null` | `null` = 用主題的值 |
| `key_spacing` | length \| null | `null` | 同上 |
| `height_scale` | number 0.5–2.0 | `1.0` | 乘在**鍵高**上（§8.8.0 第 2 步），鍵盤總高隨之改變 |

注音佈局比 QWERTY 多一列，`key_spacing` 調小、`height_scale` 調大是常見需求，
所以這個覆寫點必須存在於佈局側而非主題側。

### 9.3 `layer` 與 `row`

layer：

| 欄位 | 型別 | 預設 |
|---|---|---|
| `id` | string | — **必填**，layer 內唯一 |
| `label` | localized-string | `""` | 供 UI 顯示（如層切換選單） |
| `units` | number > 0 | 各 row `width` 總和的最大值 | 見下 |
| `rows` | list<row> | — **必填**，空 → F10 |

row：

| 欄位 | 型別 | 預設 |
|---|---|---|
| `weight` | number 0.1–4.0 | `1.0` | 相對列高 |
| `keys` | list<key> | — **必填**，空 → F10 |

**佈版演算法（規範性）：**

```
# 列高
usable_h = keyboard_height − padding.top − padding.bottom
           − row_spacing * (rows.count − 1)
row_h[i] = usable_h * weight[i] / Σ weight

# 列內
usable_w = keyboard_width − padding.left − padding.right
           − key_spacing * (keys.count − 1)
unit_w   = usable_w / units
key_w[j] = unit_w * width[j]
```

各鍵自左（`direction: rtl` 時自右）依序排列。
若 `Σ width < units`，剩餘空間留在該列末端；
若 `Σ width > units`，該列會溢出，實作 **必須** 等比壓縮該列
（`unit_w' = usable_w / Σ width`）並產生 WARNING。

> 需要置中的短列（如 QWERTY 的 `asdfghjkl`）**應** 使用顯式的
> `{ spacer: true, width: 0.5 }` 佔位鍵，而不是仰賴任何自動置中規則。
> 自動置中是隱式行為，四端會做出四種結果。

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

**誤用的後果很具體：** 把 `，` 寫成 `send: { text: "，" }` 看起來能動，
但使用者切到英文模式時它仍然吐出全形逗號，因為它從沒經過 librime 的
`punctuator`。正確寫法是 `send: { keysym: "comma" }`，讓 librime 依
`ascii_punct` 開關自己決定要吐 `,` 還是 `，`。

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
| `shape` | `is_full_shape ? "全" : "半"` |
| `variant` | `is_simplified ? "简" : "繁"` |
| `schema_name` | `rs_status.schema_name` |
| `schema_id` | `rs_status.schema_id` |

`label_from != none` 時 `label` 作為狀態不可用時的後備。
上表的中文字面是規範性的（四端一致），需要本地化時由客戶端資源覆蓋。

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
每一個 swipe 都必須是某個「另有他途」的捷徑。本 repo 三份佈局皆已遵守：

| swipe | 等效路徑 |
|---|---|
| 字母鍵上滑出數字 | 長按彈出盤、或 `numeric-symbol` 佈局 |
| 空白鍵左右滑動移動游標 | 直接點輸入框（宿主提供） |
| 退格鍵左滑清除 | 按住退格自動重複，直到清空 |
| 注音空白鍵上下滑翻頁 | 候選列的翻頁指示器 |

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

1. `core/themes/` 四份與 `core/layouts/` 三份全部解析成功，零 ERROR 診斷。
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
9. 診斷清單長度與內容在四端一致（同一份壞檔案，四端報一樣多則）。
10. `candidates.bar.toolbar.items` 缺席時，解析結果為 §8.6.6.1 的六項預設清單；
    主題若把 `items` 覆寫成不含 `schema:picker` 的清單，實作補回該項並發 INFO。
11. 空白鍵（同時有 `icon: space` 與 `label_from: schema_name`）在 schema 已載入時
    顯示方案名；schema_name 為空字串時退回圖示而非畫成空白鍵。
12. 系統字體縮放調到最大時，文字放大倍率恰為 `font_scale_max`（不是它的平方）。
13. 組字中按下 `send: {text: ...}` 的鍵：組字內容先上屏，標點接在其後，
    兩者皆不遺失（§9.4.1）。
14. 把 `keyboard.padding.top` 從 4 改成 20：鍵盤總高度**增加 16dp**，按鍵高度不變
    （新模型下鍵盤高是算出來的，padding 是加項；這與初稿相反）。
15. 同一份主題在 411 dp 寬與 456 dp 寬的螢幕上，鍵的**長寬比一致**
    （皆為 `key_aspect`，除非撞到 `key_height` 夾制）。這是初稿模型做不到的一條。
16. 同一份主題下，10 欄的 `qwerty` 與 11 欄的 `bopomofo-dachen`，
    鍵的長寬比相同（鍵較窄時鍵也較矮），而不是後者更瘦長。
17. 舊主題（含 `keyboard.height:` 區塊、無 `key_aspect`）仍能載入，
    產生恰好一則 INFO 診斷，且渲染採用預設 aspect 模型。

---

## 11. 尚未規範、已知的缺口

誠實列出，避免各端各自發明：

* **候選窗的多列／表格排版。** 桌面端在候選數多時常用兩欄或表格，
  目前只有 `orientation` 與 `max_width`，不足以描述。v2 議題。
* **主題預覽圖。** 主題商店需要縮圖，格式尚未定義 `preview` 欄位。
* **深色主題的自動生成。** 目前必須手寫 counterpart，沒有「由淺色推導深色」的機制。
* **工具列的外觀細節。** §8.6.6.1 已規定工具列的**內容**與必備項，
  但排列方式（靠左／平均分佈）、項目間距、是否可捲動仍未規範。
* **`text` 與 `keysym` 的混合送出**（例如一鍵送出多個 keysym 序列）。
  倉頡的簡碼、日文的濁音變換可能會需要。
* **RTL 的完整語義。** `direction: rtl` 目前只鏡射列內順序，
  沒有處理 popup 展開方向與 hint 位置。
* **`schema:picker` 選單本身的外觀。** 它現在是方案切換的唯一入口，
  但格式只規定「有這個 action」，沒規定選單長什麼樣、用哪些顏色欄位。
  這是 §8.6.6.1 補完之後浮出來的下一個同類缺口。
* **候選列與鍵盤之間的過場。** `motion.candidate_change_ms` 只描述候選項自身，
  沒描述「工具列 ⇄ 候選列」的切換，而那是使用者每次組字都會看到的動畫。

### 本節之外：v1 實作回饋已修補的項目

下列缺陷是 Android 端把 `bopomofo-dachen.yaml` 真正渲染出來時撞到的，
已在對應章節補齊,列此備查:§4.4.1（縮放重複套用）、§8.6.6.1（工具列）、
§8.8（padding 算在高度之內）、§8.8.1（active 的觸發條件）、
§9.1.1（schema 切換的退回規則）、§9.4（keysym 回落所需的 ABI）、
§9.4.1（`send.text` 撞上組字）、§9.5（`candidate:next/prev` 所需的 ABI）、
§9.6（鍵面解析順序、圖示退化表、swipe 為 OPTIONAL）。

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
