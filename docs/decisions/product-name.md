# 已決定:LuminaKey

**狀態:已決定(2026-08-09)。** 起因是使用者看到 Windows 安裝程式的標題
「安裝 - RIME 四端輸入法」:

> 「我們軟件名稱都沒 怎麼能叫四端 太難聽了」

**「四端」是開發期的工作代號**(四個平台),不是產品名。舊的英文代號同理 ——
Quad 也是「四」。兩個都描述的是**我們怎麼開發它**,不是**使用者拿它做什麼**。

---

## 1. 規範性命名表(四端必須一致,不可自行變體)

| 項目 | 值 |
|---|---|
| 產品名(英文) | `LuminaKey` |
| 產品名(中文顯示) | `LuminaKey 輸入法` |
| 識別碼字根 | `luminakey`(全小寫,無空格) |
| Android applicationId / namespace | `org.luminakey.ime` |
| macOS bundle id | `org.luminakey.inputmethod.LuminaKey` |
| macOS 設定 app bundle id | `org.luminakey.inputmethod.LuminaKey.Settings` |
| macOS TISInputSourceID | `org.luminakey.inputmethod.LuminaKey.Hant` / `.Hans` |
| macOS 使用者資料目錄 | `~/Library/Application Support/LuminaKey` |
| Windows 使用者資料目錄 | `%APPDATA%\LuminaKey` |

**這張表不是文件,是設定檔的投影。** 真正的來源是
[`scripts/lib/product.env`](../../scripts/lib/product.env),shell 與 python
兩邊各有一個十行的讀取器,`scripts/verify_product_ids.sh` 每次都會比對兩邊
逐字相同,並掃出「又有人把 id 寫死了一份」。

改名之所以痛,不是因為要改的地方多,是因為**沒有人知道到底有幾個地方**:
同一個 Android IME id 曾在九支腳本裡各寫死一份,寫法還各不相同(有的寫全 id、
有的只寫套件名、有的把它拼在字串中間)。**grep 找得到的只是字面寫出來的那些。**

## 2. 刻意**不改**的東西(不是漏改)

| 東西 | 現值 | 為什麼不改 |
|---|---|---|
| R2 發布路徑 | `rime/…` | 下載頁與**使用者裝置上的應用內升級**都指著它。改了 = 已發出去的連結 404、裝好的 app 升不上來 |
| R2 上的檔名字根 | `RimeQuad-latest.pkg`、`rime-latest.apk` … | 同上。等發布搬到 GitHub Releases 時一起換,那時是新舊並存、不會斷 |
| GitHub repo 名 | `rime-quad` | 牽動所有推送腳本與 CI,不是這一輪的事 |
| 底層引擎 | `librime` / RIME 方案 | **「Rime」講引擎時是正確的**,不是舊產品名。`rime_shell.h`(共用層名稱)、`rime.<前端>` 的 app_name 慣例、`assets/rime/` 的資料目錄同理 |
| librime-lua 沙盒的 C++ 符號 | `kRimeQuad*` / `rimequad_lua_*` | 定義在 `patches/` 裡。改了必須重建 librime,而稽核比對的是**已經建好的 `.a`** —— 改一半的下場是稽核說「沒有沙盒」而其實有 |
| 主題／佈局的 `author: "rime-quad"` | 未動 | 分不出它指的是倉庫名(不改)還是專案名(該改)。留著,等有人裁決 |

## 3. 為什麼「趁現在」是有時效性的

### 便宜的(隨時可改,使用者無感)

| 位置 | 改名前 |
|---|---|
| Windows 安裝程式標題 | `windows/installer/*.iss` 的 `AppName=RIME 四端輸入法` |
| macOS 顯示名 | `apple/…/Resources/*.lproj/InfoPlist.strings` 的 `CFBundleName` / `CFBundleDisplayName` |
| Android 顯示名 | `android/app/src/main/res/values*/strings.xml` 的 `app_name` |
| 各處 UI 文案、README、下載頁 | 到處 |

### 貴的(**發布出去之後就不能改**)

| 位置 | 改名前 | 改了會怎樣 |
|---|---|---|
| Android `applicationId` | `org.rimequad.ime`(`android/gradle.properties`) | **變成另一個 app**。使用者無法升級,要解除安裝重裝,**詞典與設定全失**。而且簽章金鑰與輪替鏈是綁在這個 id 上的 |
| macOS bundle id | `org.rimequad.inputmethod.RimeQuad` | 輸入法要重新加入輸入來源;使用者資料目錄改變 |
| macOS `TISInputSourceID` | `org.rimequad.inputmethod.RimeQuad.Hant` / `.Hans` | 同上,而且必須以 bundle id 為前綴 |
| Windows `AppId` | `{7A033CF7-CB91-408E-A653-EF639F4173DB}`(`.iss` 裡自己也標了「一旦發布出去就不能改」) | 舊版不會被覆蓋,「新增或移除程式」裡會出現兩筆 |
| Windows `CLSID_RimeTextService` | `windows/tsf/guids.h` | 舊的註冊留在登錄檔裡變成孤兒 |
| 使用者資料目錄 | `~/Library/Application Support/RimeQuad`、`%APPDATA%\RimeQuad` | 使用者詞典找不到了 —— 除非寫遷移 |
| 落地的檔名與魔術字串 | `rimequad-backup.json`(含 `kind`)、`rimequad-store.json`、`rimequad-layouts.json` | 使用者的備份被判成「不是備份」、裝過的方案清單變空 —— 而且**沒有錯誤訊息**,畫面上只是一片乾淨 |

**這一輪的使用者數接近零,所以改識別碼的成本也接近零。**
每多發一版、每多一個使用者,這個成本就往上跳一階,而且是不可逆的那種。
**所以這一輪連識別碼一起改,不只是顯示名。**

> ⚠ 上面最後一列(落地的檔名)是這次唯一**仍需遷移**的一格:既有裝置上還有
> 舊名字的檔案。四端的讀取端要**兩個名字都認得**,寫的時候只寫新名字。
> 沒做這件事的話,升級後使用者的備份會被判成 `NOT_A_BACKUP`、
> 「裝過哪些方案」會變成空清單 —— 兩者都不會有錯誤訊息。

## 4. 之後再做(這一輪刻意不做)

- **發布搬到 GitHub Releases**,順便把 R2 上的檔名字根一起換掉。
  現在換等於讓已經裝在使用者機器上的 app 升不上來。
  (順帶一提,那也是一次隱私改善:專屬子網域的 DNS 與 TLS SNI 都是明文。)
- **GitHub repo 改名**。
- **網域與商標查核。** 名字定了不等於名字是安全的:這是開源專案,
  但撞到既有輸入法會很麻煩。**目前沒有做過任何查核。**
- `core/themes/`、`core/layouts/` 的 `author:` 欄位要填什麼(見 §2 最後一列)。
