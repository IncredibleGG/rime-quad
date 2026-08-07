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
  "format_version": 1,
  "generated_at": "2026-08-07T15:30:00Z",
  "base_url": "https://pub-xxxx.r2.dev/rime/schemas/",

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
      "description": "最常用的全拼方案，詞庫為繁體，可經 opencc 轉簡體。",

      "upstream": "https://github.com/rime/rime-luna-pinyin",
      "upstream_commit": "a1b2c3d",           // 打包時的上游 commit
      "license": "LGPL-3.0-or-later",

      "file": "luna-pinyin-a1b2c3d.zip",      // 相對於 base_url
      "size": 962144,
      "sha256": "…",                          // 行動端**必須**驗證

      // 這個套件提供哪些可選用的方案（即 schema_list 可以列的東西）。
      // 只作為詞庫相依而不該出現在切換清單裡的套件，此陣列為空。
      "schemas": [
        { "id": "luna_pinyin",    "name": "朙月拼音" },
        { "id": "luna_pinyin_tw", "name": "朙月拼音·臺灣正體" }
      ],

      "requires": ["prelude", "essay"],       // 其他 package 的 id

      // 打包時實際驗證過的證據。未通過驗證的套件不得進入索引。
      "verified": {
        "deployed": true,                     // librime 部署成功
        "probe": { "schema": "luna_pinyin_tw", "keys": "nihao", "expect": "你好" }
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
- `categories[].hidden` 為 true 者不在市集列表顯示（例如 `prelude`、`essay` 這類
  只作為相依的基礎元件），但仍可被 `requires` 指名。

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

## 5. 從 R2 遷移到 GitHub

目前測試階段索引與套件都放 R2。正式開源後改以 GitHub Releases 為來源時，
**索引格式不需要變動** —— 只需換掉 `base_url`，並在行動端提供來源設定。
`upstream` 與 `upstream_commit` 兩個欄位就是為了這一天而存在：
它們讓任何人都能獨立重建整份索引，而不必信任我們的鏡像。
