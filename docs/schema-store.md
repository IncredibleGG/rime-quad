# 方案市集：索引格式與導入流程

使用者要能一鍵導入市面上既有的 RIME 方案，也能導入自己的。本文定義**索引格式**
（伺服器側打包與行動端導入之間的契約）與**導入流程**。

---

## 0. 為什麼不是「一份下載清單」

三件在本專案已經實際踩到、或必然會踩到的事，決定了這個格式的形狀：

1. **相依性是真的。** 注音方案（`rime-bopomofo`）的 translator 指定的詞典是
   `terra_pinyin` 而**不是** `luna_pinyin`，另外還需要 `stroke` 做筆畫反查。
   我們自己組資料集時就中過這一發。索引若不能表達相依，使用者點「導入注音」
   會得到一個部署失敗的輸入法。

2. **部署要花時間。** 實測：三本詞庫加 5.7MB 語言模型，在 20 核主機的模擬器上
   耗時 **7.2 秒**。大詞庫方案在真機上可能數十秒。導入必須是背景作業 + 進度回報，
   不能卡住鍵盤。

3. **使用者自帶檔案是不可信輸入。** zip 內含 `../../` 就能寫出 app 沙盒
   （zip slip）。這條必須在解壓前擋掉，不是「之後再說」。

---

## 1. 索引檔

位置：`<base_url>/index.json`。行動端啟動時取得，可快取，以 `format_version` 判斷相容性。

```jsonc
{
  "format_version": 1,        // 破壞性變更才動，見 §1.1
  "format_minor": 1,          // 加欄位動這個。舊讀取端看不到，行為不變
  "generated_at": "2026-08-07T15:30:00Z",
  "base_url": "https://pub-xxxx.r2.dev/rime/schemas/",

  // 語言分組表（BCP 47）。顯示名放在索引裡而不是 app 裡，理由與 categories 相同：
  // 新增一種語言不該要求使用者先更新 app。
  "languages": [
    { "tag": "zh-Hant-TW", "name": "中文（臺灣正體）", "order": 1 },
    { "tag": "und",        "name": "其他（未標記語言）", "order": 99 }
  ],

  // 隨 APK 出貨的內建方案。它們不是從市集裝的，所以不在 packages 裡，
  // 但選單一樣要分組。uid 用保留命名空間 @builtin（見 §1.2）。
  "builtin_schemas": [
    { "id": "luna_pinyin_tw", "uid": "@builtin/luna_pinyin_tw",
      "name": "朙月拼音·臺灣正體", "language": "zh-Hant-TW",
      "language_source": "curated" }
  ],

  // 同一個 schema id 由多個提供者給出。行動端據此在安裝前就講得出
  // 「你已經有一個叫這個名字的方案了」。見 §1.2。
  "schema_id_collisions": [
    { "schema": "double_pinyin", "providers": ["double-pinyin", "ice"] }
  ],

  // 語言標記的涵蓋率。「還有幾個方案是未知」要能回答（§1.3）。
  "language_coverage": {
    "total": 101, "tagged": 96, "not_a_language": 5, "unknown": 0,
    "coverage_pct": 100.0,
    "by_source": { "upstream": 3, "curated": 19, "derived": 79 }
  },

  "categories": [
    { "id": "mandarin",  "name": "華語",       "order": 1 },
    { "id": "topolect",  "name": "方言",       "order": 2 },
    { "id": "other",     "name": "其他語系",   "order": 3 },
    { "id": "essential", "name": "基礎元件",   "order": 9, "hidden": true }
  ],

  "packages": [
    {
      "id": "luna-pinyin",                    // 穩定識別碼，kebab-case，不可變更
      "name": "朙月拼音",
      "category": "mandarin",
      "description": "最通用的全拼，不知道選什麼就選這個。詞庫是繁體，可切簡體輸出。",

      "recommended": true,                    // 選填。UI 把它排前面並加推薦標記
      "recommended_layout": "qwerty",         // 軟鍵盤佈局，見下
      "layout_note": "輸入碼為英文字母，QWERTY 佈局可用",   // 選填

      "upstream": "https://github.com/rime/rime-luna-pinyin",
      "upstream_commit": "a1b2c3d",           // 打包時的上游 commit
      "license": "LGPL-3.0-or-later",

      "file": "luna-pinyin-a1b2c3d.zip",      // 相對於 base_url
      "size": 962144,
      "sha256": "…",                          // 行動端**必須**驗證

      // 這個套件提供哪些可選用的方案（即 schema_list 可以列的東西）。
      // 只作為詞庫相依而不該出現在切換清單裡的套件，此陣列為空。
      //
      // ⚠ `id` **不是全域唯一的**（§1.2）。唯一鍵是 `uid`。
      //   `id` 仍然保留，因為 librime 的 schema_list 用的就是裸 id。
      "schemas": [
        { "id": "luna_pinyin",
          "uid": "luna-pinyin/luna_pinyin",   // 全域唯一，見 §1.2
          "name": "朙月拼音",
          "language": "zh-Hant",              // BCP 47；判不出來是 "und"，見 §1.3
          "language_source": "derived" },     // upstream / curated / derived / unknown
        { "id": "luna_pinyin_tw",
          "uid": "luna-pinyin/luna_pinyin_tw",
          "name": "朙月拼音·臺灣正體",
          "language": "zh-Hant-TW",
          "language_source": "curated" }
      ],

      "requires": ["prelude", "essay"],       // 其他 package 的 id

      // 選填。裝了這個套件會覆蓋掉誰的哪些檔案（§1.2「攤平命名空間」）。
      // 有這個欄位就代表安裝順序會改變使用者拿到的東西，UI 必須在安裝前說。
      "conflicts": [
        { "package": "ice",
          "files": ["double_pinyin.schema.yaml"],
          "schemas": ["double_pinyin"] }
      ],

      // 打包時實際驗證過的證據。未通過驗證的套件不得進入索引。
      "verified": {
        "deployed": true,                     // librime 部署成功
        // 輸入探針：打包時在模擬器上真的送了這串按鍵、真的上屏了 expect。
        // kind = "exact" 打出「你好」（編碼從該方案自己的詞庫查出來）
        //      = "typed" 打出了 expect 這串字（雙拼／形碼的「你好」編碼沒辦法
        //                從詞庫直接讀出來，退到這一級仍證明整條路是通的）
        "probe": { "schema": "luna_pinyin_tw", "keys": "nihao",
                   "expect": "你好", "kind": "exact" }
      }
    }
  ]
}
```

### 欄位規則

- **`id` 一旦發布不可變更** —— 它是使用者裝置上記錄「已安裝什麼」的鍵。
- **`requires` 是套件 id，不是方案 id。** 相依解析在套件層級做，遞迴展開，需偵測循環。
- **`sha256` 行動端必須驗證。** 不符即整包丟棄，不可「先解壓再說」。
- **`verified.deployed` 為 false 或缺漏的套件不得進索引。** 這是本設計的核心品質閘門：
  寧可少收一個方案，也不要讓使用者點下去才發現壞的。
- **用到 `lua_*` 元件的套件，光有 `verified.deployed` 不算數，必須另外有
  `verified.probe`。** 理由是這一類的失效方式和別人不同：缺少 librime-lua、
  或 `lua/` 沒被正確打包時，**部署仍然回報 SUCCESS**，只是引擎少了
  translator／filter，使用者按下去沒有任何候選。deploy-only 閘門看不到這種
  假成功，只有「真的打出字」才看得到。
- `categories[].hidden` 為 true 者不在市集列表顯示（例如 `prelude`、`essay` 這類
  只作為相依的基礎元件），但仍可被 `requires` 指名。

- **`recommended_layout` 必填，行動端據此決定要顯示哪個軟鍵盤佈局。**
  目前可用的值：

  | 值 | 用於 | 判斷依據 |
  |---|---|---|
  | `qwerty` | 倉頡、五筆、粵拼、各式拼音等 | 方案的 `speller/alphabet` 是純 ASCII 字母 |
  | `bopomofo-dachen` | 注音 | `speller/alphabet` 是大千鍵位 `1qaz2wsx…` |

  這條不是裝飾。**注音用 QWERTY 佈局是打不出東西的** —— 使用者導入之後看到一個
  按不出注音符號的鍵盤，比當初沒收錄這個方案更糟。

- **`layout_note` 選填**，用來說明鍵面不理想的情況（例如某方案的 `speller/alphabet`
  含 QWERTY 上沒有的字元，兩種佈局都不完全適用）。有這個欄位代表「需要新佈局」，
  是待辦事項而不是缺陷；行動端可以照樣顯示，但應把說明傳達給使用者。

- **`recommended` 選填**，每個分類標一到兩個。上游光拼音類就十幾個變體，一般
  使用者分不出差別，分不出就等於選不下去。UI 應把推薦款排前面並加標記。

- **`precompiled` 選填**，代表該套件的 zip 內附有 librime 已編譯好的 `build/` 產物，
  行動端解壓後 **不需要在裝置上重編詞庫**。實測（模擬器，luna_pinyin + stroke）：

  | 情況 | 部署耗時 |
  |---|---|
  | 只有原始檔，從頭編譯 | 5.08 s |
  | 附帶 `build/` 產物 | **1.08 s** |
  | 只附 `build/`、連 `.dict.yaml` 都不放 | 1.03 s |

  格式如下，三個值都是**防呆用的**：

  ```jsonc
  "precompiled": {
    "librime": "1.17.0",                // 產生產物時的 librime 版本
    "table_format": "Rime::Table/4.0",  // 見 librime src/rime/dict/table.cc
    "prism_format": "Rime::Prism/4.0"
  }
  ```

  **行動端必須比對自己連結的 librime 版本與格式字串，不符就當作沒有預編譯產物、
  照常在裝置上部署。** 理由是實測出來的失效模式：

  - 產物版本不符**但原始檔還在** → librime 自己回退重編，部署照樣成功（實測 3.04 s）。
  - 產物版本不符**且沒有原始檔** → 部署直接失敗，訊息是
    `neither X.dict.yaml nor X.table.bin exists.`，使用者沒有自救途徑。

  所以：**附預編譯產物的套件，原始的 `.dict.yaml` 必須一起附**（多佔的空間就是
  保險費），除非行動端能保證版本一致。

- **`description` 是寫給使用者「選」的，不是技術規格。** 寫「這適合誰」，
  不要抄上游 README。
  ❌「基於 Rime 的全拼輸入方案，支援模糊音與自定義短語」
  ✅「最通用的全拼，不會拼音以外的輸入法就選這個。詞庫是繁體，可切簡體。」

---

## 1.1 版本與相容性規則

索引有**兩個**版本號，語義不同：

| 欄位 | 什麼時候動 | 舊讀取端會怎樣 |
|---|---|---|
| `format_version` | **破壞性**變更：既有欄位的意義改了、必填欄位被拿掉 | **整份拒收**，市集變成空的 |
| `format_minor` | 加欄位、加選填值 | 看不到新欄位，行為與以前**完全相同** |

**讀取端的規則（規範性）：**

1. `format_version` 不等於自己支援的版本 → 拒收整份索引，並告訴使用者更新 app。
2. `format_minor` **不得**作為拒收依據。不認得就當作沒有。
3. 不認得的鍵一律忽略，不得視為錯誤。
4. 單一套件缺欄位或壞掉，只讓那一個套件出局，其餘照常顯示
   （唯一例外是 `sha256`：沒有它就沒辦法驗完整性，那個套件必須出局）。

**為什麼要把「加欄位」與「破壞相容」分成兩個號碼**，而不是「反正遞增就好」：
現行已出貨的 Android 讀取端寫的是 `format_version != 1 → 整份拒收`
（注意是 `!=` 不是 `>`）。也就是說**任何一次 major 遞增，都會讓所有已安裝
的 app 同時失去整個市集** —— 使用者看到的不是「有新功能」，是「方案一個都沒有」。
本輪加的 `uid` / `language_source` / `conflicts` 因此一律走加欄位，
`format_version` 維持 1。`scripts/schema_store/test_store.py` 有一條測試
（`test_MUTATION_bumping_major_breaks_every_shipped_app`）把這個後果釘住。

真的必須做破壞性變更時，**不可以就地換掉 `index.json`**。做法是：

- 新格式發到另一個檔名（例如 `index-v2.json`），舊的 `index.json` 繼續產生；
- 等到舊 app 的存量降到可以接受，再停掉舊檔；
- 停掉之前先讓舊 app 的錯誤訊息有意義（現在的訊息是「請更新 app」，可用）。

隨 APK 出貨的 `core/schema-languages.json` 適用同一組規則，並且已經有一個
必須遵守的事實：現行讀取端要求 `format_version == 1`，所以 `schemas_by_uid`
是**加**上去的，原本以裸 id 為鍵的 `schemas` 原封不動。

---

## 1.2 方案的唯一識別碼（uid）

**`schema id` 不是全域唯一的。** 這不是理論隱憂，是索引裡實際存在的事實。
以目前收錄的 34 個套件實測，共有 **9 個**撞號的 schema id：

| schema id | 提供者 | 性質 |
|---|---|---|
| `double_pinyin` | `double-pinyin` / `ice` | **內容相反**：前者繁體詞庫，後者簡體 |
| `double_pinyin_abc` | `double-pinyin` / `ice` | 內容相反（繁／簡） |
| `double_pinyin_flypy` | `double-pinyin` / `ice` | 內容相反（繁／簡） |
| `double_pinyin_mspy` | `double-pinyin` / `ice` | 內容相反（繁／簡） |
| `pinyin_simp` | `pinyin-simp` / `wubi86-jidian` | 都是簡體，但**詞庫不同** |
| `radical_pinyin` | `ice` / `radical-pinyin` | schema 與 dict 兩個檔都不同 |
| `bopomofo_tw` | `@builtin` / `bopomofo` | 同一份上游，來源不同 |
| `luna_pinyin` | `@builtin` / `luna-pinyin` | 同一份上游，來源不同 |
| `luna_pinyin_tw` | `@builtin` / `luna-pinyin` | 同一份上游，來源不同 |

清單維護在 `scripts/schema_store/data/known_collisions.yaml`，
`test_store.py` 拿真實語料雙向比對（多一個、少一個都會紅）。

### 形狀

```
uid = <package id> "/" <schema id>

    double-pinyin/double_pinyin      ← 繁體
    ice/double_pinyin                ← 簡體
```

- 分隔符取 `/`：套件 id 是 kebab-case、schema id 是 librime 的檔名主幹
  （`<id>.schema.yaml`），兩邊都不可能含 `/`，所以切**最後一個** `/` 就能
  無歧義還原，不需要跳脫。
- 套件 id 形狀：`^[a-z0-9][a-z0-9._-]*$`；schema id：`^[A-Za-z0-9][A-Za-z0-9._+-]*$`。
- 兩個保留命名空間（以 `@` 開頭，撞不到合法的套件 id）：

  | 命名空間 | 用於 | 例 |
  |---|---|---|
  | `@builtin/<schema id>` | 隨 APK 出貨的內建方案 | `@builtin/t9_pinyin` |
  | `@local/<來源>/<schema id>` | 使用者自帶的 zip／yaml | `@local/mine_v2/mine` |

  `<來源>` 由行動端決定（Android 目前是 `"local:" + 檔名主幹`），
  非 `[A-Za-z0-9._-]` 的字元一律換成 `_`。同一份檔案重匯入得到同一個 uid，
  那本來就是「重裝」。

### ⚠ uid 識別的是「誰提供了這個方案」，不是「執行期哪個方案在跑」

librime 執行期只看得到一個攤平的 `user_data_dir` 與一份 `schema_list`，
兩者都用**裸的** schema id。所以 uid 解決帳本層的歧義，**不解決**底下這個
更嚴重的問題：

> 兩個套件提供同名的檔案，解壓到同一個目錄就是互相覆蓋，**誰後裝誰贏**。

目前 34 個套件裡有 **14 個路徑**由多個套件提供，牽涉 7 個套件：

| 路徑 | 提供者 | 被蓋掉會怎樣 |
|---|---|---|
| `default.yaml` | `ice` / `moran` / `prelude` | 預設方案清單與全域設定 |
| `key_bindings.yaml` | `moran` / `prelude` | **所有**方案的按鍵綁定 |
| `punctuation.yaml` | `moran` / `prelude` | **所有**方案的標點 |
| `symbols.yaml` | `moran` / `prelude` | **所有**方案的符號表 |
| `recipe.yaml` | `ice` / `moran` / `radical-pinyin` | 上游的配方描述（無執行期影響） |
| `lua/search.lua` | `ice` / `radical-pinyin` | 兩者的反查腳本 |
| `double_pinyin*.schema.yaml`（4 個） | `double-pinyin` / `ice` | **雙拼從繁體變簡體** |
| `pinyin_simp.schema.yaml` / `.dict.yaml` | `pinyin-simp` / `wubi86-jidian` | 換掉詞庫 |
| `radical_pinyin.schema.yaml` / `.dict.yaml` | `ice` / `radical-pinyin` | 換掉方案與詞庫 |

注意 `key_bindings.yaml` / `punctuation.yaml` / `symbols.yaml` 這一組：
它們被所有方案 `__include`，被蓋掉影響的是使用者裝的**每一個**方案，
而不只是撞號的那一個。這一層 schema id 撞號完全看不到。

索引的 `conflicts` 欄位就是為了讓這件事在**安裝前**講得出來。
行動端最低限度要做到：安裝前告知、安裝後把「誰的檔案現在真的在磁碟上」
記進帳本。徹底的解法（每個套件各自的子目錄）需要 librime 的
`user_data_dir` 支援多來源，不在本輪範圍。

---

## 1.3 語言標記與來源分級

每個方案帶一個 BCP 47 標記（`language`）與一個**來源分級**（`language_source`）。
分級存在的理由是「這個標記可以信到什麼程度」要看得出來：

| `language_source` | 意思 | 目前筆數 |
|---|---|---|
| `upstream` | 完全來自上游 metadata（rppi 的分類路徑） | 3 |
| `curated` | 用到我們自己維護的判定資料，但沒有用到啟發式 | 19 |
| `derived` | 用到啟發式（方案名的地區字樣、詞庫的繁簡字集探針） | 79 |
| `unknown` | 判不出來，`language` 必為 `"und"` | 0 |

有多個成分時取**最不可靠**的那一個（`unknown` > `derived` > `curated` > `upstream`）。
例如「rppi 說這是 zh（upstream）＋ 字集探針判出 Hant（derived）」記成 `derived`。

**規則：**

- 判不出來一律 `"und"`，**不猜**。分錯類比沒分類更糟 ——
  使用者會以為清單裡沒有他要的東西。
- `"und"` 有兩種，報告裡分開算，因為處置方式不同：
  - **本來就不是語言**（IPA 音標、`moran_charset` 這種工具方案）：
    在判定資料裡明寫 `not_a_language: true`，這是正確答案，不是缺口；
  - **查不到**：`language_source` 為 `unknown`，這是缺口，要補資料。
- `language_coverage.unknown` 就是「還有幾個方案是未知」。目前是 **0**
  （101 個方案：96 個有標記、5 個明確不是語言）。
  `build_schema_store.sh` 以 `--max-unknown 0` 把它釘住，多一個就擋下建置。
- 讀取端遇到不認得的標記或 `"und"`，**不得把方案藏起來** ——
  歸入「其他」分組即可。分組沒那麼準是小事，方案從選單裡消失是大事。

**判定資料在版控裡，不在程式的 if-else 裡**：
`scripts/schema_store/data/languages.yaml`。每一筆人工判定都必須附 `why`，
沒寫理由的會在載入時直接 die。逐條依據另外產生成 `languages.json`，
與 `index.json` 一起發布，任何人都可以複查每一個標記的來歷。

判定資料裡的方案層級鍵可以是 uid 或裸 id；但只要那個 id 出現在
`known_collisions.yaml` 的 `content-differs` 清單裡，就**必須**用 uid ——
否則等於對兩個內容相反的方案下同一個判斷。`test_store.py` 會擋。

隨 APK 出貨的 `core/schema-languages.json` 有兩張表：`schemas_by_uid`（完整，
新讀取端用）與 `schemas`（裸 id，舊讀取端用）。後者的收錄規則是
**判定不同的撞號 id 不收**（目前是四個 `double_pinyin*`，繁 vs 簡）；
判定相同的照收（`pinyin_simp`、`radical_pinyin` 兩邊都是 `zh-Hans`）——
為了「內容不同」丟掉一個確定的正確答案，只會讓讀取端退回字面啟發式，
那才是真的會分錯。撞號的其他後果（佈局、已安裝判斷、檔案覆蓋）由 uid 與
`conflicts` 承接，不歸這張表管。這條規則是自我修正的：哪天其中一邊換了字集，
判定就會不同，那個 id 自動從表裡消失。

---

## 2. 套件 zip 的內容規則

zip 內是**扁平的檔案集合**，解壓後直接落進 `user_data_dir`：

```
luna_pinyin.schema.yaml
luna_pinyin_tw.schema.yaml
luna_pinyin.dict.yaml
pinyin.yaml
LICENSE                  ← 必須隨附
UPSTREAM.txt             ← 必須隨附：上游 URL、commit、打包時間
```

- **不得包含目錄穿越路徑**（`..`、絕對路徑、符號連結）。打包端要保證，行動端仍要再驗一次。
- opencc 詞典（`.ocd2`）由 APK 內建提供，套件**不應**重複打包。

### 攤平是預設，不是規則

真正的規則是：**entry 名稱必須等於 librime 解析該檔案時用的相對路徑**。
多數方案的引用不帶斜線（`dictionary: luna_pinyin`），解析出來就是根目錄的
`luna_pinyin.dict.yaml`，所以看起來像「扁平」。但只要引用帶了斜線，
librime 就是**照路徑**找檔案，攤平會直接讓部署失敗。實際踩過的四種：

| 子目錄 | 用途 | 觸發 |
|---|---|---|
| `lua/`（與根目錄的 `rime.lua`） | librime-lua 的執行期腳本 | 方案用了 `lua_translator` / `lua_filter` / `lua_processor` / `lua_segmentor` |
| 詞庫子目錄，例如 `cn_dicts/`、`en_dicts/` | 上游把詞庫分目錄放 | `import_tables: - cn_dicts/8105` |
| `opencc/` | 上游自帶的 opencc 設定與詞表（`.json`／`.txt`，**不含 `.ocd2`**） | 方案用到 APK 沒內建的簡繁／emoji 轉換 |
| `build/` | librime 預編譯產物（`*.table.bin`、`*.prism.bin`、`*.reverse.bin`） | 套件宣告了 `precompiled` |

前兩條都是實測撞出來的，不是推測：

- 攤平 `cn_dicts/8105.dict.yaml` 之後，雾凇拼音部署直接失敗：
  `source file '.../cn_dicts/8105.dict.yaml' does not exist.`
- 攤平 `lua/`：部署**照樣回報 SUCCESS**，但 `lua_translator` 一 require 就找不到
  檔案，按下去沒有任何候選。librime-lua 的 `package.path` 寫死了
  `<user>/lua/?.lua;<user>/lua/?/init.lua;<shared>/lua/?.lua;<shared>/lua/?/init.lua`，
  而 `rime.lua` 只從資料目錄的**根**載入。這是最陰險的一種 ——
  所以 §1 的品質閘門對用到 lua 的套件不接受「只有 deployed」。

**放寬的只有「路徑」，安全規則一項都沒放寬：**

- entry 名稱正規化後仍必須落在目標目錄內；仍然拒絕 `..`、絕對路徑、符號連結。
- 目錄深度上限 **3 段**（`a/b/c.yaml`），打包端與行動端都檢查。
- 副檔名白名單：`.yaml`／`.yml`／`.txt`／`.json`／`.lua`／`.gram`／`.ocd2`／`.md`
  與無副檔名的 `LICENSE`。**`.bin` 不在白名單**（見行動端 `ArchiveGuard` 的註解）。
- `.lua` 是**會被執行的程式碼**，白名單放它進來是有代價的決定。市集套件靠
  sha256 + 伺服器側「實際部署 + 輸入探針」把關；使用者自帶的 zip 沒有這些憑據，
  由 `ArchiveGuard` 的其餘各條與 librime 自己的 Lua state 邊界承接。

---

## 3. 導入流程（行動端）

```
選擇套件
  → 遞迴展開 requires，扣掉已安裝的
  → 顯示「將下載 N 個套件，共 X MB」讓使用者確認
  → 逐一下載 → 驗 sha256 → 驗 zip 路徑安全 → 解壓到 user_data_dir
  → 把新方案加進 default.custom.yaml 的 schema_list patch
  → rs_deploy()                     ← 背景執行，經 rs_deploy_callback 回報
  → 部署成功 → 標記已安裝；失敗 → 回滾（見下）
```

### 部署期間的 UI

`rs_deploy()` 進行中 `rs_status.is_disabled` 為 true，且**舊 session 會失效**
（見 `rime_shell.h` 的 `rs_session_alive`）。鍵盤必須：

- 顯示進度而非停在空白或假死狀態
- 部署完成後**重建 session**，不可沿用舊的

### 失敗回滾

部署失敗時，必須把 `default.custom.yaml` 的 `schema_list` 還原成導入前的狀態並重新部署，
否則使用者會卡在一個「每次啟動都部署失敗」的狀態，且沒有自救途徑。
解壓出來的檔案可以留著（無害），但**不可留在 schema_list 裡**。

---

## 4. 使用者自帶檔案

入口：SAF 檔案選取器，接受 `.zip` 與單一 `.yaml`。

**安全檢查（缺一不可）：**

1. **路徑穿越**：解壓前逐項檢查 entry 名稱，含 `..` 區段、以 `/` 開頭、
   或正規化後跳出目標目錄者，整包拒絕。
2. **符號連結**：zip 內的 symlink 一律拒絕。
3. **解壓炸彈**：限制單檔與總解壓大小、entry 數量上限。
4. **副檔名白名單**：只接受 `.yaml`、`.txt`、`.dict.yaml`、`.ocd2`、`LICENSE` 之類，
   拒絕可執行檔。

通過後流程與市集導入相同（解壓 → 加進 schema_list → 部署 → 失敗則回滾），
差別只在沒有 sha256 可驗、也沒有相依資訊 —— 因此部署失敗的機率較高，
錯誤訊息必須明確告訴使用者「缺少哪一個詞典」，而不是只說「部署失敗」。

---

## 4.1 安裝紀錄（`rimequad-store.json`）的 v1 → v2 遷移

使用者手上**已經裝了東西**。加 uid 不能讓他們的方案消失。

### 為什麼遷移是無損的、而且不需要連網

v1 的帳本本來就是**以套件為單位**存的：頂層每一筆是一個套件，方案掛在它下面。
「這個方案是哪個套件裝的」v1 已經有了，只是沒有寫成一個可以當鍵用的字串。
所以遷移只是把它拼出來：

```
uid = <套件 id>/<方案 id>                      （source == "store"）
uid = @local/<清理過的來源>/<方案 id>          （source == "local"）
```

不必問伺服器、不必有網路（本專案離線為預設，這一點是硬要求），
也沒有猜錯的可能。參考實作與測試：`scripts/schema_store/registry.py`、
`test_store.py` 的 `TestRegistryMigration`。

### 規則

1. `format_version` 由 1 改為 2，其餘欄位**原封不動**。
2. 每個 `schemas[]` 元素加上 `uid`。已經有 `uid` 的不動 → 遷移**冪等**。
3. uid 產不出來的（id 形狀不合法）**不得丟掉那筆紀錄** ——
   磁碟上的檔案還在，抹掉帳本只會製造孤兒。標記它、讓 UI 有機會說話。
4. 讀取端遇到沒有 `uid` 的舊帳本，要能就地遷移並寫回（寫回一樣走
   「先寫暫存再改名」，寫到一半斷電不能讓使用者永久卡住）。

### 遷移**修不好**的一件事

如果使用者先裝 `double-pinyin` 再裝 `ice`，`double_pinyin.schema.yaml`
早就被後者蓋掉了。帳本兩筆都在，磁碟上只剩一份，而帳本記不得是誰的。
遷移只能**指出**（`registry.migrate()` 回傳 `file_conflicts`），不能還原；
要還原只能重新解壓其中一個套件。這是 uid 出現得太晚的既成代價，
寫出來比假裝已經修好要好。

---

## 5. 從 R2 遷移到 GitHub

目前測試階段索引與套件都放 R2。正式開源後改以 GitHub Releases 為來源時，
**索引格式不需要變動** —— 只需換掉 `base_url`，並在行動端提供來源設定。
`upstream` 與 `upstream_commit` 兩個欄位就是為了這一天而存在：
它們讓任何人都能獨立重建整份索引，而不必信任我們的鏡像。
