# 備份格式（匯出／匯入）

> ## ⚠ 2026-08-16：桌面兩端（Windows / macOS）已停止開發並從 main 移除
>
> 這份文件裡所有 `macOS` / `Windows` / 「桌面端」/「四端」的段落**刻意原樣保留**，
> 沒有逐句刪除。理由寫在 `docs/refuted-claims.tsv` 的開頭：用刪除讓文件變綠，
> 會把「當時量到的事實」跟「現在誰在做」一起抹掉，而前者不會因為不出貨而變假。
>
> 讀的時候請這樣換算：
>
> | 文件裡寫的 | 現在的意思 |
> |---|---|
> | 「四端」 | Android（唯一有實作的一端）＋ iOS（尚未開始） |
> | 「桌面端 / macOS / Windows」 | **已退場**，只剩歷史意義；程式碼在標籤 `desktop-final-5fa5baa` |
> | 「僅 macOS / Windows」的區塊 | **目前沒有任何實作者**，見下面各處標註 |
>
> 完整的移除清單與哪幾條規範因此失去執行者，記在 `docs/coordination.md` 的
> 2026-08-16 那一則。


> 四端共用。Android 端的實作在 `android/app/src/main/java/org/luminakey/ime/store/Backup*.kt`
> 與 `UserDbSnapshot.kt`。桌面兩端還沒接，§8 列了要做的事。

---

## 0. 為什麼有這份規範

`AndroidManifest.xml` 把 `android:allowBackup` 關掉了。那一行不是效能設定，是隱私邊界：
系統的自動備份由**系統元件代勞**，不經過我們的行程，因此不經過 `net/NetworkGate`、
連網紀錄一筆都不會有 —— 使用者照我們教的方法查會查到「乾淨」，但詞庫其實已經
上傳到某家公司的雲端。實測（`bmgr backupnow`，見 `scripts/audit_offline.sh`）確認被
帶走的包含使用者詞庫、一組跨重裝穩定的 UUID、「何時用過哪個方案」、以及連網紀錄本身。

代價是**使用者換手機就會失去所有詞庫與設定**。誠實的替代方案是他**自己匯出**：
主動、看得見、去處由他決定。這份文件定義那個檔案長什麼樣子。

三條設計原則，後面每一條規則都能回推到它們：

1. **看得懂。** 容器是 zip，內容是 LevelDB 目錄、YAML 與 JSON。使用者拿電腦打開來
   看得到裡面有什麼。備份不是黑盒子。
2. **只帶使用者的東西。** 身分識別、使用時間軸、稽核紀錄一律不進去（§4）。
   而且**刻意不帶的東西要寫在 manifest 裡**，讓人查得到我們漏掉了什麼、為什麼。
3. **壞掉要說。** 每一種失敗都有自己的碼與自己的一句話。「匯入失敗」不是訊息。

---

## 1. 容器

| 項目 | 值 |
|---|---|
| 容器 | ZIP（deflate；不支援 ZIP64） |
| 建議副檔名 | `.zip` |
| 建議 MIME | `application/zip` |
| 建議檔名 | `rime-backup-<yyyyMMdd>.zip`（只是建議，使用者可以改） |
| 清單檔 | `luminakey-backup.json`（容器根目錄） |

**辨識備份的方式是打開它找 `luminakey-backup.json`，不是看副檔名。**
Android 的 SAF 交回來的 Uri 沒有可信的檔名（DocumentsUI 給的是 `msf:1000000072`
這種不透明 id），桌面端使用者也會自己改名。認內容才可靠。

> ⚠ **改名的相容規則（2026-08-09，規範性）。**
> 舊名（改名前）：清單檔 `rimequad-backup.json`、`kind` 為 `rimequad-backup`。
> **讀取端必須兩個名字都認得**（先找新名字，找不到再找舊名；`kind` 同理）；
> **寫出端只寫新名字**。
>
> 少了這一條的下場是：使用者升級之後，他自己匯出的那份備份會被判成
> `NOT_A_BACKUP`。那個訊息會叫他去找一個壞掉的檔案，而檔案是好的、
> 壞掉的是我們。**這一類失敗不會有任何錯誤紀錄，畫面上只是一句「這不是備份」。**
>
> 舊名字的支援可以在確定沒有人手上還有舊備份之後移除，但**移除要是一個
> 明確的決定**，不是因為沒有人記得它存在。

容器內只有五個目錄，其餘一律不合法：

```
luminakey-backup.json
dict/          使用者詞典
schema/        已安裝方案的檔案 + 安裝帳本
config/        使用者改過的 *.custom.yaml
settings/      偏好
layout/        自訂鍵位
```

---

## 2. manifest：`luminakey-backup.json`

```jsonc
{
  "kind": "luminakey-backup",       // 固定字串，認身分用
  "format_version": 1,             // 見 §6
  "created_at": 1754697600,        // Unix 秒。只給人看，不參與任何判斷
  "producer": {                    // 全部只給人看，不得用來閘門任何行為
    "platform": "android",         // android / macos / windows / ios
    "app_version": "0.9.1",
    "app_version_code": 26080810,
    "rime_shell_abi": 1
  },

  "user_db": [
    { "name": "luna_pinyin",
      "encoding": "leveldb-dir",
      "root": "dict/luna_pinyin.userdb",
      "flushed": true }
  ],

  "schemas": [
    { "id": "luna_pinyin_tw", "name": "拼音（臺灣字形）", "package": null, "bundled": true },
    { "id": "double_pinyin",  "name": "雙拼",             "package": "double-pinyin", "bundled": false }
  ],
  "enabled_schemas": ["luna_pinyin_tw", "double_pinyin"],

  "omitted": ["network-log", "installation-id", "schema-access-time",
              "builtin-ledger", "sync-dir", "derived-binaries", "logs"],

  "files": [
    { "path": "dict/luna_pinyin.userdb/CURRENT", "size": 16,  "sha256": "…" },
    { "path": "config/default.custom.yaml",      "size": 240, "sha256": "…" }
  ]
}
```

### 2.1 `files` 是白名單，不是索引

**只有 `files` 列出來的路徑會被解出來。** 容器裡多出來的 entry 一律忽略
（不報錯 —— 那是未來版本新增的東西，前向相容）。這條規則同時解掉兩個問題：

* 不需要副檔名白名單。LevelDB 的檔案叫 `CURRENT`、`MANIFEST-000002`、`000005.ldb`，
  任何以副檔名為基礎的白名單都會誤殺它們（`store/ArchiveGuard.kt` 的那一份就會）。
* 攻擊者要塞東西進來，得同時改 manifest 與內容，而 `sha256` 會擋下改內容。

`sha256` 是**解壓後**內容的摘要，小寫十六進位。讀取端**必須**逐檔比對；
不符合就整包拒絕，一個位元組都不進使用者的資料目錄。

### 2.2 `schemas` 為什麼要帶 `package`

**方案 id 不是全域唯一的。** `double_pinyin` 同時存在於兩個套件，而且字集相反
（見 `docs/coordination.md` §5）。只認 id 的話，還原時可能把使用者的簡體雙拼詞庫
接到繁體雙拼上。`package` 為 `null` 且 `bundled` 為 `true` 代表它隨 App 內建。

---

## 3. 內容

### 3.1 `dict/` —— 使用者詞典

這是整份備份唯一無法重建的東西，其餘都可以重下載或重設。

**兩種載體，讀取端必須兩種都認得，寫出端可以只產生其中一種。**

> 過渡期的例外：某一端還沒做到「兩種都認得」時，**必須把讀不動的那幾本詞典
> 指名報給使用者**，不得安靜略過。安靜略過的長相是「匯入成功，但一本詞庫都
> 沒回來」，而使用者要到幾天之後才會發現。Android 現況見 §8。

| `encoding` | 形態 | 誰產生 |
|---|---|---|
| `rime-userdb-text` | librime 自己的 `<name>.userdb.txt` 文字快照 | 桌面端（規劃中） |
| `leveldb-dir` | 整個 `<name>.userdb/` 目錄原樣搬移 | Android（現況） |

#### `rime-userdb-text`（**正式的跨端格式**）

librime 的 `UserDictManager::Backup()` → `LevelDb::Backup()` →
`UserDbHelper::UniformBackup()` 產生的那份文字檔。它是首選，因為：

* **可合併。** `UserDictManager::Restore()` 走 `UserDbMerger`，同一個詞在兩邊都有時
  取較新的那一份。目錄搬移做不到這件事（見下）。
* **跨得過 db 實作與版本。** LevelDB 的目錄格式雖然可攜，但它是實作細節；
  文字快照是 librime 明確承諾的介面。
* **人看得懂。** 符合 §0 的第一條原則。

`root` 指向容器內的那個檔案，例如 `dict/luna_pinyin.userdb.txt`。

#### `leveldb-dir`（Android 現況）

`root` 指向容器內的目錄，例如 `dict/luna_pinyin.userdb`，底下是 LevelDB 的檔案。

**收哪些檔案：`CURRENT`、`MANIFEST-*`、`*.log`、`*.ldb`、`*.sst`，以及其他一切。**
只排除三個：`LOCK`（行程鎖，開檔時自己會建）、`LOG` 與 `LOG.old`
（LevelDB 自己的壓實除錯輸出）。

> ⚠ **`000003.log` 是寫入前紀錄（WAL），不是日誌。**
> 使用者最近打的字 —— 已經寫進 memtable、還沒被壓實成 `.ldb` 的那些 —— 全部只存在
> 那個檔案裡。一條看起來很合理的「排除所有 `*.log`」會產生一份大小幾乎不變、
> 匯入也成功的備份，而使用者最近學的詞全沒了，沒有任何錯誤訊息。
> Android 端有一條專門守這件事的測試（`BackupPlanTest`），並附反向測試。

**匯入時是整本換掉，不是合併。** 兩個 LevelDB 沒有辦法在不經過 librime 的情況下
合併；把檔案疊上去的結果是 `MANIFEST` 與 `.ldb` 對不起來的目錄，連開都開不起來。
所以 `leveldb-dir` 的匯入**必須**在動手前讓使用者確認一次。
`rime-userdb-text` 沒有這個限制。

#### `flushed`：這份詞庫證明得了自己是完整的嗎

librime 的 `Memory::OnCommit` 在使用者上屏之後**開一個交易**再把學到的詞寫進去，
而那個交易的內容住在記憶體裡的 `leveldb::WriteBatch`，要等到下一次
`FinishSession()` 或 `~UserDictionary` 才會落地。也就是說，**匯出當下直接複製目錄，
拿到的通常是「上一輪」的詞庫**。

`flushed: true` 代表匯出端在複製前確實讓引擎把待寫入的交易寫下去了。
`false` **不代表資料一定不完整**，只代表沒有證據。讀取端要據此對使用者多說一句話。

各端怎麼做到 `flushed: true`：

* **桌面端（建議）**：`RimeSyncUserData()`。它會 `CleanupAllSessions()` 之後跑
  `user_dict_sync`，順便就產生 `rime-userdb-text`。一步到位。
* **Android（現況）**：**建立一個 session、立刻銷毀它**。
  （`rime_shell.h` 從 ABI 2 起**已經有** `rs_sync_user_data()`，但 Android 的
  JNI 橋還沒把它接出來 —— 見 §8.2。）`UserDictionaryComponent` 有一個
  `hash_map<string, weak<Db>> db_pool_`，同一本詞典在整個行程裡只有一個 `Db` 物件，
  所以任何 `~UserDictionary` 的提交等於替所有人提交。
  這條路徑刻意不經過 `rs_select_schema()`：`Session` 建構時走
  `ConcreteEngine::InitializeComponents()` → `switcher_->CreateSchema()` 並直接
  `schema_.reset(...)`，**不經過 `ApplySchema()`**，因此不會呼叫
  `Switcher::SetActiveSchema()`，也就不會寫 `user.yaml` 的 `var/schema_access_time`
  與 `var/previously_selected_schema` —— 匯出不會偷偷在使用者機器上多記一筆
  「你何時用過哪個方案」，也不會改變他的鍵盤下次開起來是哪個方案。

`leveldb-dir` 的寫出端**還必須**確認複製期間目錄沒有變動（LevelDB 隨時可能在背景
壓實：產生新的 `.ldb`、刪掉舊的、換一份 `MANIFEST`）。作法是複製前後各取一次
「檔名＋大小＋修改時間」的指紋，不一致就重來，重試用完就**明白地失敗**。
產生一份壞掉的備份讓使用者三個月後才發現，比當場說「現在複製不了，請稍後再試」糟得多。

### 3.2 `schema/` —— 已安裝的方案

裡面是**已安裝套件放在 user_data_dir 裡的檔案**（相對路徑原樣保留），
加上安裝帳本 `schema/luminakey-store.json`。

**為什麼連檔案一起帶走，而不是只記一份清單。** 這個 App 的定位是「離線為預設」。
使用者換手機的當下很可能沒有開連網開關，甚至根本連不到我們的索引。
只記清單的備份在那個時刻等於一張無法兌現的收據，而他的詞庫沒有對應的方案
就是一堆打不開的資料。

隨 App 內建的方案**不進備份** —— 新機器上的 APK 自己帶著，而且可能是更新的版本。

### 3.3 `config/` —— 使用者改過的設定

只收 `*.custom.yaml`。非 `.custom.` 的那些是 librime 部署時從 shared 抄過去的產物
（帶 `customization:` 標記），新機器上會自己重新產生，搬過去只會與新版的隨附檔打架。

`default.custom.yaml` 的 `patch/schema_list` 就是「使用者啟用了哪些方案」。

### 3.4 `settings/prefs.json` —— 偏好

```jsonc
{ "format_version": 1, "values": { "keyboard_height_scale": 1.15, "candidate_count": 7 } }
```

`values` 直接對應 Android 的 `UserPrefs.toMap()`。

**「未設定」必須原樣保留。** 這個專案的偏好層規定 `null` = 「使用者沒設定過這一項」，
而不是「使用者選了預設值」；缺席的 key 一律不補。補了就等於把匯出當下的預設值凍進
備份，使用者換手機之後會被永遠釘在舊預設上，而且他從沒動過那一項。

**三個 key 刻意不進備份**（寫出端與讀取端都要擋）：

| key | 理由 |
|---|---|
| `network_enabled` | 這是**安全預設**不是喜好。允許它跟著備份走，等於讓「打開一個檔案」變成一個可以替使用者開啟連網的動作，而他可能正在一台還沒決定要不要信任的新手機上。新機器上要連網，請他自己再按一次那個開關。 |
| `offline_notice_seen` | 上面那個開關的第一次說明。開關回到預設，說明就該再出現一次。 |
| `onboarding_done` | 它記的是「這台裝置上曾經走完引導」，是這台裝置與系統輸入法設定的狀態，不是偏好。 |

桌面端沒有這三個 key 時忽略即可；**但不得新增一個「連網開關」到備份裡**。

### 3.5 `layout/luminakey-layouts.json` —— 自訂鍵位

原樣搬移。格式見 `keyboard/UserLayoutStore.kt` 的 `LayoutRemapJson`。
它存的是**操作**（swap / move）而不是快照，所以在新機器上套到新版的基礎佈局仍然成立。

> 實作提醒：這份檔案背後的儲存層是**單例而且在記憶體裡有快取**。匯入時直接覆蓋
> 檔案的話，畫面與輸入法會繼續用舊的那一份，而且變更旗標不變、永遠不會重載。
> 必須走那一層的公開 API 寫回去。

---

## 4. 刻意不進備份的東西

| 東西 | `omitted` 碼 | 理由 |
|---|---|---|
| 連網紀錄（`files/net/`） | `network-log` | 它的用途是讓使用者**稽核我們** —— 「這台裝置什麼時候連過網、為了什麼」。跟著備份跑到另一台機器上，它就不再是那台機器的證據，卻長得一模一樣；使用者會拿一份與現況無關的紀錄來判斷現在的行為。而且它本身就是一條上網時間軸。 |
| `installation.yaml` | `installation-id` | 裡面是 `installation_id`，一組跨重裝穩定的 UUID。帶到新機器上等於把兩台裝置釘成同一個身分 —— 那正是我們拒絕 Google 自動備份的理由之一。librime 在新機器上會自己生一個。 |
| `user.yaml` | `schema-access-time` | 記的是 `var/schema_access_time`（何時用過哪個方案），是使用行為的時間軸，不是設定。使用者真正在意的「用哪些方案」由 `default.custom.yaml` 帶著走。 |
| `builtin_introduced.json` | `builtin-ledger` | 「**這個安裝**曾經引入過哪些內建方案」的帳本，屬於安裝史。搬過去反而會讓新機器以為某個方案已經引入過而跳過它。 |
| `sync/` | `sync-dir` | librime 自己的同步目錄，按 `installation_id` 分子目錄，可能含別台裝置的 id。 |
| `*.table.bin`、`*.prism.bin`、`*.reverse.bin`、`build/` | `derived-binaries` | 部署產物。匯入之後一定要重新部署，這些會被重新產生，帶著走只是讓備份大上十倍。 |
| librime 的日誌 | `logs` | 除錯輸出，不是使用者資料。 |

**寫出端必須把用到的碼列進 `omitted`。** 這不是裝飾：使用者（或稽核者）要能從備份
本身看出我們刻意漏掉了什麼，而不必去讀原始碼。

---

## 5. 匯入流程（規範性）

1. 讀 `luminakey-backup.json`；找不到則退回舊名 `rimequad-backup.json`
   （見 §1 的改名相容規則）；兩個都讀不到 → `NOT_A_BACKUP`。
2. 版本判定（§6）。**先於任何欄位檢查。**
3. 逐檔解到暫存區並比對 `sha256`。任何一項不符 → 整包拒絕、清掉暫存、
   **使用者的資料目錄一個位元組都沒被動過**。
4. 取 `default.custom.yaml` 的**整份位元組快照**（回滾用；⚠ 必須在動手之前取）。
5. 搬進 user_data_dir。詞庫整本換掉；`schema/` 與 `config/` 依相對路徑覆蓋。
6. 偏好：以備份為準，但保留本機的 §3.4 那三個 key。
7. `enabled_schemas` **逐一確認這台機器上真的有那份 `*.schema.yaml`**
   （搜尋順序 user → shared）。找不到的**要從 schema_list 剔除**，否則 librime
   部署會直接失敗，使用者連鍵盤都打不開。被剔除的要以**方案名**（不是 id）
   列給使用者看，並告訴他裝回來之後詞庫會自動接上。
8. 重新部署。失敗 → 用第 4 步的快照還原 `default.custom.yaml`、再部署一次，
   並如實告訴使用者「東西拿回來了，但整理字詞失敗」。

---

## 6. 相容性規則

`format_version` 是一個整數，**只有破壞相容性的變更才遞增**。

以下**不**遞增：新增選用欄位、新增 `omitted` 的理由碼、新增一種 `encoding`、
在 `files` 裡多列檔案、新增一個容器目錄。

判定表（四端必須一致；Android 的實作是 `BackupFormat.verdictFor()`，
單元測試逐條釘住）：

| 條件 | 結果 | 使用者該做什麼 |
|---|---|---|
| `format_version` > 本端支援的最高版 | `TOO_NEW` | **升級 App**。訊息必須這樣說。 |
| `format_version` < 本端支援的最低版 | `TOO_OLD` | 沒有升級路徑，說清楚就好，不要「盡力而為」。 |
| 其餘 | `OK` | — |

三條讀取端的義務：

* **未知的欄位一律忽略**，不得視為錯誤。
* **未知的檔案一律忽略**，不得寫出來。
* **版本判定先於欄位檢查。** 一份未來版本的 manifest 對舊解析器來說「缺欄位」是必然的；
  若先報「格式壞掉」，使用者會去找一個不存在的壞檔案，而他真正該做的事是升級 App。

目前：`format_version = 1`，最低可讀 `1`。

---

## 7. 讀取端的安全檢查

備份可能來自任何地方（使用者的雲端硬碟、別人傳的檔案），一律以最壞情況為準：

1. **路徑穿越**：`..`、絕對路徑、磁碟機代號、反斜線、控制字元、連續斜線一律拒絕；
   解壓時再用正規化後的路徑確認一次落在目標目錄內（兩種機制互相獨立）。
2. **符號連結**：Unix mode 的 `S_IFLNK` 藏在 zip central directory 的
   external file attributes 裡，`java.util.zip.ZipEntry` 拿不到，要自己讀。發現即整包拒絕。
3. **解壓炸彈**：宣告的大小是攻擊者可控的欄位，所以除了檢查宣告值，
   **實際複製位元組時要硬性計數**，超過上限立刻中止並刪除半成品。
   Android 端的上限：單檔 64MB、整包 256MB。
4. **前綴白名單**：只接受 §1 的五個目錄。
5. **`files` 白名單 + `sha256`**：見 §2.1。

第 1–3 條與方案套件那條路徑（`docs/schema-store.md` §4）用的是同一套實作，
不重寫（重寫兩份必然分岔）。

---

## 8. 各端現況

| 端 | 匯出 | 匯入 | 備註 |
|---|---|---|---|
| Android | ✅ `leveldb-dir` | ⚠ 只讀得動 `leveldb-dir`；碰到 `rime-userdb-text` **會指名報給使用者**（`BackupFormat.READABLE_ENCODINGS`），不會安靜地少一本 | SAF（`ACTION_CREATE_DOCUMENT` / `ACTION_OPEN_DOCUMENT`），**不要求任何儲存權限**。整條往返 2026-08-09 在模擬器上實跑過（`scripts/verify_backup_roundtrip.sh`），含反向控制組；**SAF 的選檔對話框本身仍未被自動化驗過** |
| macOS | 未做 | 未做 | |
| Windows | 未做 | 未做 | |
| iOS | 未做 | 未做 | |

### 8.1 桌面端接手時建議的順序

1. 先做**匯入**。Android 使用者換到桌面、或桌面重灌，是最先會撞到的情境。
2. 匯出用 `RimeSyncUserData()` 產生 `rime-userdb-text`，一步同時解決
   「flush」與「跨端載體」兩個問題。

### 8.2 `rs_sync_user_data()`：ABI 已經有了，Android 還沒接

> **2026-08-09 更新。這一節原本寫「`rime_shell.h` 目前沒有暴露
> `RimeSyncUserData()`」——那句話已經不成立**：協調端加了，它在
> `core/include/rime_shell.h:131`（ABI 2）。**但 Android 這一端沒有接**：
> `android/app/src/main/cpp/jni_bridge.cc` 的 `kMethods[]` 裡沒有對應的項目，
> 所以 Kotlin 呼叫不到。§3.1 的 session 建銷法仍然是現況。

現況是**量過的**，不是推測的（`scripts/verify_backup_roundtrip.sh`，
2026-08-09 在模擬器上跑）：教三個詞 → 匯出（學習用的 session **刻意留著**，
交易還掛在記憶體裡）→ `pm clear` → 匯入 → 三個詞都回到候選第一名。

而且反向也驗過：把 `UserDbSnapshot.flushEngine()` 整支停掉之後，
**最後學到的那個詞會安靜地消失**（前兩個仍在——它們被後續的查詢順手提交了），
manifest 的 `flushed` 如實變成 `false`。也就是說 §3.1 那套 flush 是**承重的**，
不是保險。

⚠ 驗這件事的時候有一個陷阱，第一版就踩到了：**教完之後、匯出之前不可以再打
任何字**。`UserDictionary::Query` 開頭就 `FinishSession()`，而 `db_pool_` 讓同一本
詞典在行程內只有一個 `Db` 物件，所以**一次查詢**也會替所有人提交。把「確認它
學到了」排在匯出之前的話，flush 停掉仍然全綠 —— 一個永遠不會紅的驗證。

接上 `rs_sync_user_data()` 之後拿得到的東西（都還沒有）：

* `rime-userdb-text`（`*.userdb.txt`），也就是 §3.1 那個**跨端正式載體**。
  桌面端要跟 Android 交換詞庫的話，這是唯一該用的格式。
* 「flush 成功了嗎」變成引擎自己回答，而不是靠 librime 的實作細節推論。

要付的代價（所以它不是「加一行就好」）：`rs_sync_user_data()` **會銷毀所有
session**（librime 的 sync 以 `cleanup_all_sessions()` 開頭），而且是**非同步**的，
結果經由 `on_deploy` 回報、與部署共用同一支維護執行緒。所以 Android 接它的時候
要一併處理：IME 的 session 失效後重建、以及「同一時間只能有一個維護工作」。
`UserDbSnapshot.kt` 一個檔案改不完。
