# 離線為預設：我們宣稱什麼、不宣稱什麼，以及你怎麼自己查

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


> 這份文件的目標讀者有兩種：**懷疑我們的人**，和**三個月後的我們**。
> 每一條主張後面都附上「你要怎麼自己驗」。沒有辦法驗的，就寫成缺口，
> 不寫成保證。

這個專案的定位是使用者的原話：

> 「我是無聯網，但是你要聯網就自己打開開關，關掉了以後又是無聯網了，
> 用戶自己選擇開關。**我們經得起審計。**」

「經得起審計」的意思不是「相信我們」，是「你不必相信我們」。所以底下每一節
都盡量把主張化約成**一行 grep** 或**一支可以重跑的腳本**。

---

## 0. 我們**不**宣稱的事

先寫這一節，是因為它比宣稱的部分更重要。

| 常見的誤解 | 事實 |
|---|---|
| 「這個 app 沒有網路權限」 | **假的。** `android.permission.INTERNET` 在權限清單上，而且是安裝時權限，執行期取消不掉。有方案市集與應用內升級就一定要有它。 |
| 「開關關掉 = 系統層級斷網」 | 不是。開關是**我們自己**在 `NetworkGate` 裡執行的政策，不是 Android 的網路隔離。它擋得住我們自己的程式碼，擋不住作業系統元件。 |
| 「連網紀錄是完整的網路稽核」 | 只涵蓋**經由 NetworkGate 發出**的連線。系統自動備份、Play 商店、電信商的連線都不經過我們，也不會出現在紀錄裡。 |
| 「Lua 沙盒 = 惡意方案跑不了任何東西」 | 不是。沙盒關掉的是「跳出行程、載入原生碼、亂讀寫檔案」這三條路。方案 lua 仍然拿得到 librime 的繫結，仍然讀寫得到你的詞庫。 |
| 「TLS 保護了你在下載哪個方案」 | **不保護。** 見 §3，這是目前最大的一個洞。 |

---

## 1. 連網面：單一出口

**主張：整個 app 只有 `android/app/src/main/java/org/luminakey/ime/net/NetworkGate.kt`
一個檔案碰得到網路 API。**

自己查：

```bash
grep -rn 'java\.net\|openConnection\|Socket(\|OkHttp\|Retrofit' \
     android/app/src --include=*.kt --include=*.java
```

結果必須只出現在 `NetworkGate.kt`（以及各檔案的**註解**裡——那些是刻意寫上去
說明「我們不用這些」的文字）。

守門的是 `scripts/audit_offline.sh` 第 1 項，發布前一定會跑到
（`scripts/release_check.sh` 第 0 關）。

**守門腳本自己也被反向測過**：`scripts/verify_audit_offline.sh` 會複製一棵樹、
真的把一個 `store/Sneaky.kt` 放進去、確認守門腳本會紅；另外 15 條違規（塞
WebView、加 okhttp、加 crashlytics、原生層開 socket、把 allowBackup 改回
true、開明文 HTTP、User-Agent 自報家門、多要一個權限、把沙盒註解掉……）
逐條植入逐條確認。其中一條是**正向對照**：只出現在註解裡的 `java.net`
不可以被誤判成違規——一支動不動就紅的守門腳本會被當成雜訊直接略過。

### 開關

- 預設是**關**，而且是 fail-closed：`NetworkGate.policy` 的初值是 `{ false }`。
  就算 `RimeApp.onCreate` 裡安裝政策的那一行被誤刪，行為是「完全離線」而不是
  「完全開放」。政策自己丟例外時也視為關（`isEnabled` 的 catch）。
- 每次連線前、以及**每一次轉址**都重新問一次開關。下載到一半把開關關掉，
  下一跳就不會發生。

自己查（不需要任何工具，一台裝置 + adb）：

```bash
adb logcat -s NetworkGate
```

開關關著時去逛方案市集，會看到 `拒絕連網（開關關閉）`，而**不會**有任何一行
`連線`。

### 連網紀錄

每一次**真的建立**的連線留一筆：時間、主機、原因、結果、位元組數。
轉址的每一跳各記一筆。

**刻意不記錄被開關擋下的嘗試。** 這不是疏漏：如果把被擋下的嘗試也記進去，
「開關從沒開過 → 紀錄是空的」這句話就不成立了，而那正是使用者驗證我們的方式。

紀錄裡**不含** URL 的路徑與查詢字串，只有主機名加一個我們自己給的用途標籤。
輸入內容一個字都不會進到這裡（輸入內容根本不會離開 librime 與鍵盤）。

---

## 2. User-Agent 與自報家門

送出的 User-Agent 是固定的 `Mozilla/5.0`，不含專案名稱、不含裝置型號。

理由與取捨都寫在 `NetworkGate.kt` 的 `USER_AGENT` 註解裡，重點是：**拿掉不會
變成「沒有 UA」**，`HttpURLConnection` 會自己補一個帶 Android 版本與裝置型號的
`Dalvik/2.1.0 (...)`，熵更高。

守門：`audit_offline.sh` 第 7 項（UA 不含 `SELF_ID_UA_PATTERN` 裡的任何一個詞——
見 `scripts/lib/product.env`，**改名時那一行要跟著加新名字**，否則守門的還在
擋舊名、新名字大搖大擺地走出去——且請求標頭不夾帶
`Build.*`）。

---

## 3. 流量特徵：**目前最大的缺口**

開關打開、只連我們自己的伺服器，連線內容有 TLS 保護。**但是連線本身的形狀
沒有保護**，而它洩漏得比想像中多。

### 3.1 SNI／DNS：一眼看出你在用哪個 app

方案與升級目前都放在 R2 的
`pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev`。這個主機名是**本專案專屬**的
——它出現在 DNS 查詢與 TLS 的 SNI 裡，兩者都是**明文**。

也就是說：我們花力氣把帶著產品名的字樣從 User-Agent 裡拿掉，避免被動觀察者
認出使用者，**但主機名把同一件事又講了一遍**，而且講得更早（連線建立之前）。
改 UA 改不了這件事，我們也沒有在說明裡假裝改得了
（見 `NetworkUi.kt` 的 `NetworkRequiredCard`）。

**可行的改善方向**（不是這一輪做得到的，寫下來讓它別被忘記）：把方案與升級
搬到一個**很多人共用**的主機（GitHub Releases → `objects.githubusercontent.com`）。
那不會讓連線消失，但「連上 githubusercontent」與「連上一個只有這個輸入法
會連的子網域」對觀察者的資訊量差了一個數量級。專案本來就打算把 R2 換成
正規的 GitHub Releases 發布——**那同時是一個隱私改善，不只是流程整理**。

### 3.2 傳輸大小：直接指出你下載了哪一個方案

實測（2026-08-08，市集索引裡的 34 個套件）：

| | |
|---|---|
| 套件數 | 34 |
| 最小 / 最大 | 8,649 B ／ 31,182,471 B |
| **在 ±2% 之內大小獨一無二的** | **21 / 34** |
| 其餘 13 個 | 落在 6 組「兩三個彼此接近」的小群 |

也就是說，一個只看得到加密位元組數的被動觀察者，多數情況下**可以判定你下載
的是哪一個輸入方案**——而輸入方案往往直接對應語言與地區（粵語、藏文、
中古漢語、日文…）。索引 `index.json` 本身是公開的，觀察者拿得到每個套件的
確切大小，比對只是一次查表。

補充幾點，免得低估：

- 我們送 `Accept-Encoding: identity`，所以回應大小**等於**檔案大小，比對是精確的。
  改成允許 gzip 也沒有用：同一個檔案壓出來的大小同樣是固定值，觀察者一樣算得到。
- 時間相關性同樣會洩漏：使用者一按下開關就下載，本地網路上看得到一個突發流量。
- 這一節講的是**被動觀察者**。主動的中間人另有 TLS 與 sha256 擋著（見 §5）。

### 3.3 現在做得到的事

老實說：**在這一層做不到有意義的緩解。** 有效的做法（把每個套件補齊到固定
大小的桶、或每次多抓幾個不需要的套件當誘餌）都要伺服器端配合，而 R2 是靜態
託管；客戶端單方面補位元組只會多花使用者的流量而騙不了任何人。

所以我們的處置是：

1. **寫在這裡**，並在「連網」頁面的說明裡照實告訴使用者（不宣稱做不到的事）。
2. 把 §3.1 的搬遷列為後續工作。
3. 使用者若在意，唯一真正有效的手段是把流量交給 VPN／Tor——那是使用者的
   選擇，我們既不代做也不假裝代做了。

---

## 4. 第三方方案的程式碼執行

方案市集下載的 zip 裡有 `.lua`，而 `.lua` 是**程式碼**。這一節講我們把
「下載 = 執行任意程式碼」縮到多小。

### 4.1 執行的時機：從「裝好就跑」改成「選到才跑」

上游 librime-lua 的 `src/modules.cc` 在**模組初始化**（`RimeSetup` 期間）就
`luaL_dofile(<user>/rime.lua)`。也就是說，只要檔案在使用者資料目錄裡，
使用者**還沒選到那個方案**，它的程式碼就已經跑過了。

`patches/librime-lua@sandbox.patch` 把它改成：第一個 lua 元件
（`lua_translator` / `lua_filter` / `lua_segmentor` / `lua_processor`）
被建立時才初始化。結果是：

| 動作 | `rime.lua` 會不會跑 |
|---|---|
| 下載 / 安裝 / 部署一個方案 | **不會** |
| 選到一個**不用** lua 的方案 | **不會** |
| 選到一個用 lua 的方案 | 會 |

這不是「lua 不再執行」——那樣雾凇與萬象整組都會壞。改的是**觸發條件**：
從「檔案存在」變成「使用者選了它」。

### 4.2 沙盒：兩層，允許清單制

**第一層**（`src/lib/lua.cc`，`luaL_openlibs()` 之後立刻裝）：
`os` / `io` / `debug` 三個標準庫改成**允許清單**——只留下方案真的用得到的，
其餘一律 `nil`。另外拔掉 `package.loadlib`、`package.cpath`、
`package.searchers[3]`（C 模組載入器）與 `[4]`，並強制 `load` / `loadfile`
只收文字 chunk（bytecode 可以破壞 VM 的記憶體安全，等於直接跑原生碼）。

> 為什麼是允許清單而不是移除清單：第一版是移除清單，而它**漏掉了
> `debug.getupvalue`**。有了它，第二層包起來的 `io.open` 可以被一行
> `debug.getupvalue(io.open, 1)` 原樣挖回來，整層形同虛設。移除清單還有第二個
> 沉默的失敗方式：Lua 升級後多出來的新函式預設是開的。

**第二層**（`src/modules.cc`，拿得到資料目錄之後、跑 `rime.lua` 之前）：
把 `io.open` / `io.lines` / `io.input` / `io.output` / `loadfile` / `dofile`
全部收斂到 RIME 的兩個資料目錄：

```
使用者資料目錄   可讀、可寫
共用資料目錄     只可讀        （隨 app 出貨的內建資料，方案不該改它）
其餘任何路徑     一律拒絕
```

`require` 的搜尋器（`package.searchers[2]`）也換成只看那兩個目錄的版本——
方案改得掉 `package.path`，改不掉那個 closure 裡的根目錄。

### 4.3 `io` 為什麼不整個拿掉（這是量出來的，不是猜的）

掃過市集索引裡**全部 38 個套件的 202 個 `.lua`**（含雾凇 rime-ice 29 個、
萬象 rime-moran 17 個），標準庫的實際用量是：

```
os.date(102)  os.time(22)  os.getenv(1)  os.clock(1)
io.open(6)    require(21)  package.config(1)
load(6，全部明確傳 't')    debug.getinfo(2)   collectgarbage(15)
```

`io.open` 那 6 處裡有 4 處在 `--[[ ]]` 註解區塊內，真的會執行的只有 4 處：

| 檔案 | 做什麼 | 落在哪 |
|---|---|---|
| `moran/lua/moran.lua` | 讀 `<user>/lua/zrmdb.txt`（輔助碼字典） | 使用者目錄 |
| `moran/lua/moran.lua` | 桌面回退 `/usr/share/rime-data/…` | **目錄外** |
| `ice/lua/cold_word_drop/logger.lua` | 附加寫 `runLog.txt` | 使用者目錄 |
| `ice/lua/cold_word_drop/processor.lua` | 寫 `*_words.lua`（降頻／隱藏詞） | 使用者目錄 |

所以：整個拿掉 `io` 會讓萬象少一本字典、雾凇的「降頻／隱藏詞」整組壞掉。
而真正會執行到的那幾處**全部落在使用者資料目錄裡**——收斂的邊界是量出來的。

唯一行為真的改變的是萬象那條 `/usr/share/rime-data/` 桌面回退路徑：它在
Android 上本來就不存在（上游對 `nil` 有處理），第二層把它從「找不到」變成
「明確拒絕」。

### 4.4 這一層**沒有**做的事

- **不是完整隔離。** 方案 lua 仍然拿得到 librime 的繫結（`rime_api`、
  `Config`、`UserDb`），那條路能改設定、能讀寫詞庫。這一層管的是**檔案系統**
  與**跳出行程**。真正的隔離要另一個行程加 seccomp，不在這一層。
- **不解 symlink。** 路徑正規化是字面的（解掉 `.` 與 `..`）。資料目錄裡若有
  一條指向外面的 symlink，這一層攔不住。目前那個目錄只有 app 自己寫，而
  `store/ArchiveGuard` 與打包端都拒絕 zip 裡的 symlink 與穿越路徑。
- `os.getenv` 留著（雾凇用它找 ibus 的設定路徑）。它讀得到環境變數。

### 4.5 怎麼驗（**讀 patch 說「看起來有擋」不算數**）

三層驗證，一層比一層貴：

| 腳本 | 驗什麼 | 需要 |
|---|---|---|
| `scripts/audit_offline.sh` 第 8 / 8b 項 | 沙盒還在原始碼裡、順序對、**而且出貨的 `librime.a` 裡真的有它** | 只要 grep |
| `scripts/verify_lua_sandbox.sh` | 把兩段沙盒裝進**真的 Lua 5.4.8**（librime-lua 出貨的那一份原始碼），39 條探針 × 3 個階段 | 一個 C 編譯器 |
| `scripts/verify_lua_deferral.sh` | 用**實際出貨的 `librime.a`** 在模擬器上跑，驗延後與沙盒 | 模擬器 |

`verify_lua_sandbox.sh` 的設計重點：

- 沙盒字串**不是抄的**，是把 patch 套到乾淨的原始碼上、再從套完的檔案裡抽出來。
  patch 改了而探針沒跟上，這裡就會紅。
- 三個階段：stage 0（什麼都不裝）、stage 1、stage 2。**stage 0 的期望值就是
  反向測試**——`os.execute` 在那裡必須真的執行外部指令、`io.popen` 必須真的讀到
  子行程的輸出、`package.loadlib` 必須真的載入 libc。萬一抽取失敗變成空字串，
  stage 1／2 的結果會和 stage 0 一樣，整批期望值立刻對不上。
- 四個**變異測試**：對沙盒各植入一個真違規（拿掉 io 允許清單、拿掉 debug
  允許清單、把路徑檢查改成恆真、不換掉 require 的搜尋器），確認探針會紅。
  不會紅的那一條代表探針根本沒在檢查它。

`verify_lua_deferral.sh` 的三個案例：只部署 → `rime.lua` 不可以跑；選一個不用
lua 的方案 → 不可以跑（而且「你好」照樣打得出來）；選一個用 lua 的方案 →
**必須**跑（這是反向測試：三個都「沒跑」的話，可能只是探針壞了）。

### 4.6 ⚠ patch 只有重建之後才會到使用者手上

`patches/` 底下的檔案要等 `scripts/build_native.sh` 重跑，才會進到
`third_party/prebuilt/<abi>/lib/librime.a`——**而那個 `.a` 才是使用者拿到的
引擎**。改了 patch 卻沒重建，原始碼上看起來安全、出貨的引擎完全沒變。

`audit_offline.sh` 第 8b 項就是為了讓這件事不可能沉默地發生：它直接對
`librime.a` 找沙盒的標記字串，找不到就紅。

---

## 5. 方案套件的完整性

現況（`docs/schema-store.md` §1 與 `store/SchemaStore.kt`）：

| 層 | 現在有什麼 |
|---|---|
| 傳輸 | TLS；不允許明文（`network_security_config.xml` 全面禁止，連 loopback 例外都沒有） |
| 套件內容 | 索引宣告 `sha256`，行動端**下載時邊算邊比**，不符即整包丟棄、**不解壓** |
| 大小 | 傳輸層有硬上限，不倚賴遠端宣告的 `size` |
| zip 本身 | `store/ArchiveGuard` 擋穿越路徑、symlink、副檔名白名單、深度上限 |
| **索引本身** | **沒有簽章。** ← 缺口 |

也就是說：**單一套件被掉包擋得住，整份索引被掉包擋不住。** 能改索引的人
（拿到 R2 憑證、或任何一個被使用者設成自訂索引的鏡像）可以同時改掉
`sha256` 與 zip，行動端比對得到一致的結果。

另外兩條同源的路：

- 索引裡的 `base_url` 由遠端決定。`NetworkGate.resolveUrl` 會照著它去解析，
  所以一份被改過的索引可以把下載導到**別的主機**。
- 套件的 `file` 欄位允許是完整 URL，同上。

已做的緩解（本輪）：`NetworkGate.download` **不再跟著轉址換主機**。轉址目標
與最初那個 URL 不同主機時直接中止並記一筆——「你連的是 A」不能被一次 302
變成「你其實連了 B」。這擋得住傳輸層的重導，擋不住 §5 說的索引本身被改。

**還沒做的：索引簽章。** 設計方向與交接寫在 `docs/coordination.md` §5
（要一把新的簽章金鑰、要改 `scripts/schema_store/mkindex.py` 與
`store/SchemaIndex.kt`，兩者都不在 sec 這條支線的檔案範圍裡）。
在那之前，這份文件的說法是：**索引的真確性目前倚賴「誰能寫入那個 bucket」**，
不是密碼學。

---

## 6. 資料落地面

- `android:allowBackup="false"`，而且有 `tools:replace`（相依函式庫蓋不掉）。
  實測方法與 `allowBackup=true` 時到底會被同步走什麼，寫在
  `scripts/audit_offline.sh` 的檔頭。
  **代價**：換手機時詞庫不會跟過去。誠實的替代方案是使用者自己匯出／匯入，
  那個功能**還沒有實作**。
- 沒有 crash reporter、沒有分析 SDK、沒有 WebView。守門在 `audit_offline.sh`
  第 2、3 項（我們**寫下來**的相依）與第 10 項（**已建置 APK 的 dex**）。

### 6.1 產物層的「單一連網出口」

第 1 項的 grep 只看 `android/app/src`。它擋得住我們自己多寫一個出口，擋不住
**傳遞相依**：`build.gradle.kts` 只寫了 androidx 那幾行，而 androidx 自己會拉
進別的東西。

所以第 10 項直接問 APK：某個網路型別，整個 dex 裡**有哪些類別引用它**？
`scripts/dex_network_refs.py` 把答案釘死，判準是**集合相等**（多一個少一個
都紅）。2026-08-08 在 `app-debug.apk`（未經 R8 縮減，所以是最寬的一份）上量到：

| 型別 | 引用者 |
|---|---|
| `java.net.HttpURLConnection` | **只有 `org.luminakey.ime.net.NetworkGate`** |
| `java.net.URLConnection` | **只有 `org.luminakey.ime.net.NetworkGate`** |
| `java.net.URL` | NetworkGate、`kotlin.io.TextStreamsKt`、`kotlinx.coroutines.internal.FastServiceLoader`、`okio.internal.ResourceFileSystem$Companion` |
| `java.net.Socket` | `androidx.core.net.TrafficStatsCompat`、`androidx.core.net.DatagramSocketWrapper`、`okio.Okio` 等 4 個 okio 類別 |
| `java.net.DatagramSocket` | `androidx.core.net` 的那兩組工具類 |
| `android.webkit.WebView` | `androidx.core.text.util.LinkifyCompat`（取 URL 正規表示式用，不會建立 WebView） |
| `java.net.ServerSocket` / `MulticastSocket` / `javax.net.ssl.SSLSocket` | **沒有任何引用者** |

**掃出來的東西裡有一個值得寫下來：APK 裡有 okio。** 它是
`androidx.datastore-preferences` 帶進來的（我們的 `build.gradle.kts` 上一個字
都沒提到它），而 okio 有 `Okio.source(Socket)` 這類 socket 輔助函式。
它**不會自己連線**——那是一個 I/O 函式庫，socket 要由呼叫端給——而我們的程式碼
一行都沒有呼叫它。但這正是「原始碼 grep 看不到、產物層才看得到」的例子，
所以它被釘進清單裡：哪天多一個 okio 類別碰到 socket，第 10 項就會紅。

「少一個也紅」是刻意的。`NetworkGate` 不再引用 `HttpURLConnection`，通常代表
連網搬去別的地方了，而那正是最該被發現的一種變化。升級相依讓某個工具類消失
是正常的；正常的事也要有人明確看過一次、改清單、在 commit message 裡說明。

### 6.2 轉址

`NetworkGate.download` 不跟換主機的轉址，也不跟 https → http 的降級。
判斷抽在 `NetworkGate.redirectAllowed`（純字串），由
`NetworkRedirectTest` 的 10 則測試守著——**測試自己一個 socket 都不開**，
因為這個專案的規矩是連測試都不准出現第二個連網出口。
那 10 則被反向測過：把規則改成 `return true` 之後有 5 則會紅。

---

## 7. 一次跑完

```bash
scripts/audit_offline.sh            # 守門（release_check.sh 第 0 關，CI 快車道）
scripts/verify_audit_offline.sh     # 守門自己的反向測試（16 條植入，CI 快車道）
scripts/verify_lua_sandbox.sh       # 沙盒探針（39 條 × 3 階段 + 4 個變異，CI 快車道）
scripts/dex_network_refs.py <apk>   # 產物層的引用者清單（audit 第 10 項會呼叫）
scripts/verify_lua_deferral.sh      # 真引擎（需要模擬器，**不在 CI 裡**）

cd android && ./gradlew :app:testDebugUnitTest   # 377 則，含 net/ 的 29 則
```

前三支加上 `dex_network_refs.py` 已經接進 `.github/workflows/build.yml` 的
快車道（`fast`）。`verify_lua_deferral.sh` 需要模擬器，目前**只在本機跑過**，
還沒有接進慢車道——見 §8。

---

## 8. 已知缺口一覽

| 缺口 | 嚴重度 | 現況 |
|---|---|---|
| 傳輸大小可辨識下載了哪個方案（34 個裡 21 個獨一無二） | 高 | §3.2，這一層無解，已寫進文件與 UI |
| SNI／DNS 暴露「你在用這個 app」 | 高 | §3.1，搬到 GitHub Releases 可大幅改善 |
| 索引沒有簽章 | 中 | §5，需要跨支線（金鑰＋打包端＋store） |
| Lua 沙盒不解 symlink | 低 | §4.4 |
| 方案 lua 仍可經由 librime 繫結讀寫詞庫 | 中 | §4.4，需要另一個行程才能解 |
| 詞庫沒有匯出／匯入，換機會丟 | 中 | §6 |
| 桌面三端（macOS / Windows / iOS）**完全沒有**這一層的等價物 | 高 | Windows 目前根本沒編 librime-lua；macOS 有編、有套同一份 patch，但沒有跑過本文件的任何一支驗證腳本 |
| `verify_lua_deferral.sh` 沒有接進 CI | 中 | 它要模擬器（慢車道）。目前只在本機的 emulator-5554 上跑過一次，17 項全過 |
| 只在 x86_64 模擬器上跑過真引擎驗證 | 低 | arm64-v8a 的 `librime.a` 是同一份原始碼、同一套 patch 建的，但**沒有在 arm64 裝置上實跑過** |
| 沒有人在真手機上用過改完的引擎 | 高 | 本輪全部是模擬器與單元測試。「編得出來 ≠ 能打出字」在這個專案發生過 |
