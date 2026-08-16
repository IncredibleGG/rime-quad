# 架構：各端如何接上同一顆核心

> **2026-08-16：Windows 與 macOS 兩條桌面線已停止開發並從 main 移除。**
> 這份文件的 §4（macOS）與 §5（Windows）**刻意原樣保留**，標成「已退場」。
> 理由：它們記的是真的量到過的平台行為與踩過的坑，那些事實不會因為不出貨
> 而變成假的；而用刪除讓文件變乾淨，正是 `docs/refuted-claims.tsv` 開頭
> 那條規矩要擋的形狀。程式碼在標籤 `desktop-final-5fa5baa`。

本文記錄各平台的接入細節。這些是**踩過才知道**的知識，寫下來是為了避免在第二、第三端重新發現一次。

---

## 0. 分層與契約

```
librime (C++, 靜態連結)
   └── rime_shell.h / rime_shell.cc     ← 各端共用，唯一的 ABI 邊界
          ├── Android   JNI          →  Kotlin
          └── iOS       Swift C interop（尚未開始）
          ─────────────────────────────────────────
          （已退場：macOS Swift C interop、Windows 服務進程內直接呼叫）
```

跨越 `rime_shell.h` 的東西只有：POD 結構、`const char*`、整數。**沒有 C++ 型別、沒有例外、沒有回呼以外的控制反轉。** 這條紀律讓同一份實作能同時被 JNI 與 Swift 消費（桌面兩端還在時，MSVC 也消費同一份）。

### 記憶體契約（最容易出錯的地方）

`rs_snapshot_acquire()` 回傳的指標只在「下一次 acquire 或 release 之前」有效。實作上，本層在 acquire 當下就把 librime 的字串全部深拷貝到 session 自有的 storage，並**立刻**釋放 librime 的結構。

因此：

- 前端**必須**在 acquire/release 之間把資料複製成自己語言的型別（Kotlin data class、Swift struct）。
- 前端**不可以**把 `const char*` 存起來跨事件使用。
- 即使前端忘了 release，也不會洩漏 librime 的記憶體 —— 這是刻意的取捨，寧可多一次拷貝，也不要把跨語言的生命週期管理丟給 JNI 那一側。

### 選字不等於上屏（四端都會踩）

**不同方案的行為不一致，前端不可假設「選了候選字就會上屏」。**

實測（模擬器上以 `tools/rime_console.cc` 驗證）：

| 方案 | 選第 1 個候選之後 |
|---|---|
| `luna_pinyin_tw`（拼音） | 直接產生 commit，`is_composing` 轉為 false |
| `bopomofo_tw`（注音） | **仍停留在組字狀態**，`is_composing` 依然為 true，`preedit` 變成選中的「你好」，但沒有 commit |

注音方案需要一個明確的確認動作才會真正上屏。正確的前端邏輯是：

```
rs_select_candidate(...)
snapshot = rs_snapshot_acquire(...)
if (snapshot->status.is_composing)      // 還沒上屏
    rs_commit_composition(...)          // 明確確認
```

這個差異是在端到端測試中發現的 —— 原本的 `rime_shell.h` 根本沒有暴露 commit 操作，
只做拼音測試的話永遠不會察覺。`rs_commit_composition()` 就是為此補上的。

### commit 的消費語意

librime 的 `get_commit` 是**取出即清除**。本層在每次 `rs_snapshot_acquire()` 都會呼叫它，所以：

> **每個輸入事件只可 acquire 一次。** acquire 兩次而沒有處理第一次的 `commit_text`，那段待上屏的文字就永久遺失了。

---

## 1. 按鍵映射：桌面端最大的隱藏工作量

librime 吃的是 **X11 keysym**（見 librime `src/rime/key_table.h`），不是字元、不是平台鍵碼。

modifier 遮罩的實際值（**這些必須查表，憑印象一定寫錯**）：

| 名稱 | 值 | 備註 |
|---|---|---|
| `kShiftMask` | `1 << 0` | |
| `kLockMask` | `1 << 1` | Caps Lock |
| `kControlMask` | `1 << 2` | |
| `kAltMask` | `1 << 3` | 同 `kMod1Mask` |
| `kSuperMask` | `1 << 26` | **不是 `1 << 6`**，Win 鍵 / Command |
| `kReleaseMask` | `1 << 30` | 置位代表 key-up |

各端的來源鍵碼與難度：

| 平台 | 來源 | 難度 |
|---|---|---|
| **Android** | 鍵盤自繪，佈局 YAML 直接指定 keysym | **最低** —— 沒有映射問題 |
| **iOS** | 同上，鍵盤擴展自繪 | 最低 |
| ~~macOS~~（已退場） | `NSEvent.keyCode` + `charactersIgnoringModifiers` | 中 |
| ~~Windows~~（已退場） | TSF 收到的 `VK_*` + `GetKeyboardState` | 高 |

桌面兩端必須自行實作「原生鍵碼 → keysym」，且**這個映射受使用者的實體鍵盤佈局影響**（QWERTY / Dvorak / 各國佈局），不可寫死成一張常數表。這是先做 Android 的理由之一：行動端能完全繞開這個問題，讓我們先把核心與 UI 跑通，再回頭處理桌面端的鍵碼地獄。

> **2026-08-16 起這一節只剩歷史意義。** 剩下的兩端（Android / iOS）都是鍵盤
> 自繪、由佈局 YAML 直接指定 keysym，**沒有鍵碼映射這件工作**。
> 上面兩列標成已退場的內容保留著：如果哪天又要做桌面端，那是量過的結論，
> 不必再踩一次。

---

## 2. Android

### 宿主機制
`InputMethodService`。`onCreateInputView()` 回傳整塊軟鍵盤的 View（此處為 Compose）。上屏走 `InputConnection.commitText()`，組字預覽走 `setComposingText()`。

### 原生層
- librime 及其 5 個依賴（glog / yaml-cpp / leveldb / marisa-trie / opencc）以 **NDK 交叉編譯成靜態庫**，再與 `rime_shell.cc` + JNI glue 一起連成單一 `librime_shell.so`。
- **Boost 不需要編譯。** librime 的 CMakeLists 在非 Linux 平台走 `find_package(Boost 1.77.0)`（不帶 components），即 header-only。Android 下 `CMAKE_SYSTEM_NAME=Android`，CMake 的 `LINUX` 為假，自動走這條路徑。
- 必須用 **NDK/SDK 內建的 CMake 3.22.1**，不可用系統的 CMake 4.x —— CMake 4 移除了 `FindBoost` 模組，header-only 的 Boost 查找會直接失敗。

### 資料目錄
`shared_data_dir` 放隨附 schema（從 assets 解壓到 `filesDir`），`user_data_dir` 放使用者詞典與個人配置。兩者都必須可寫（librime 會在 `user_data_dir/build` 下產生編譯後的 schema）。

### 執行緒
`rs_*` 除 `rs_deploy` 外都必須在同一條執行緒序列化，實務上就是 IME 的主執行緒。**不要**從 Compose 的動畫或 recomposition 執行緒呼叫。部署回呼來自 librime 的維護執行緒，JNI 側需要 `AttachCurrentThread` 再切回主執行緒更新 UI。

---

### 實測踩到的兩個坑

**1. Compose 放進 InputMethodService 會直接閃退**

```
java.lang.IllegalStateException: ViewTreeLifecycleOwner not found from
  android.widget.LinearLayout{... android:id/parentPanel}
  at WindowRecomposer_androidKt.createLifecycleAwareWindowRecomposer
```

只把 `ViewTreeLifecycleOwner` / `ViewTreeViewModelStoreOwner` /
`ViewTreeSavedStateRegistryOwner` 掛在自己的 `ComposeView` 上**不夠**。
`setInputView()` 會把 view 塞進 IME 視窗的 `parentPanel`，而 Compose 建立
window recomposer 是從**視窗的 decor view 往下找**，不是從 ComposeView 往上找。
必須在 decor view 與 ComposeView 兩邊都掛。

任何要在 IME 裡用 Compose 的人都會踩到，與本專案無關。

**2. 鍵盤「畫出來了」不等於「可以打字了」**

`dumpsys input_method` 的 `mIsInputViewShown=true` 只代表輸入視圖已顯示。
若 IME 採非同步初始化（解壓資料、等待部署），此時鍵盤已經在畫面上、
但 session 還不存在，注入的按鍵會**原封不動落到宿主**——實測冷啟動時
輸入框拿到的是 `nihao1` 而不是「你好」。

自動化驗證必須等一個**應用層的就緒訊號**，不能只看 `mIsInputViewShown`。
`scripts/verify_rime_compose.sh` 的 `--ready-log <regex>` 就是為此而加。

**實測數據**（API 35 x86_64 模擬器，冷啟動）：
- 解壓 13MB / 54 個隨附資料檔：**84ms**
- `rs_init` → 首次部署完成：**7.2 秒**（編譯三本詞庫加 5.7MB 語言模型）

## 3. iOS

### 宿主機制
**Keyboard Extension（App Extension）**，不是一般的 App。整塊鍵盤自繪，與 Android 形態相同，但約束嚴苛得多。

### 硬約束
- **記憶體額度只有數十 MB**，超過直接被 jetsam 殺掉。這條否決了任何重量級 runtime（Flutter engine 等），也意味著詞庫載入策略要謹慎。
- 沙盒隔離：主 App 與鍵盤擴展是**兩個不同的進程、不同的容器**。`user_data_dir` 必須放在 **App Group** 共享容器內，否則在主 App 裡設定的東西鍵盤看不到。
- 「完全取用權限」（Full Access）未授權時，某些能力受限。設計上應讓核心輸入功能**不依賴** Full Access。
- App Store 審查對輸入法（尤其要求 Full Access 的）特別嚴格。

### 起點在哪裡
~~共用 Swift 技術棧與 `rime_shell` 綁定層，但 **UI 完全重寫**。~~

**2026-08-16 更新：那個「共用」的對象已經不在樹上了。** macOS 端收掉之後，
iOS 這條線沒有現成的同伴，但也不是從零：`desktop-final-5fa5baa` 標籤裡的
`apple/LuminaKey/Sources/LuminaKeyKit/` 有 39 個檔，其中 **37 個完全不 import
AppKit**（ThemeParser、MiniYaml、CandidateLayout、StoreEngine、ArchiveGuard、
ZipReader、NetworkGate、IPC、KeyMapper、SchemaPreflight…），`Package.swift`
的檔頭明說那一層「刻意不含 AppKit / InputMethodKit / librime」，
另有 200 餘項單元測試與 11 個變異測試。

⚠ 撿回來的時候要知道：**那棵樹裡沒有任何 iOS 程式碼**，連 Xcode 專案檔都沒有
（`git ls-files | grep -iE 'pbxproj|xcodeproj'` 零命中），`Package.swift` 宣告的是
`platforms: [.macOS(.v11)]`。它是「可以移植的純邏輯」，不是「已經跑過的 iOS 層」。

---

## 4. macOS ~~（已退場，2026-08-16）~~

> 這一節保留當歷史。macOS 端做完並發布過，於 2026-08-16 停止開發、
> 從 main 移除；程式碼在標籤 `desktop-final-5fa5baa`。
> 下面的內容是當時**實際做出來並且量過**的樣子，沒有改寫。


### 宿主機制
**InputMethodKit**：`IMKServer` + `IMKInputController`。輸入法是一個獨立的 `.app`，安裝到 `/Library/Input Methods` 或 `~/Library/Input Methods`。

### 特點
- 各端中**最好調試的**：獨立進程，崩潰不會拖垮別人，可以直接下中斷點。這是當時把它排在 Android 之後、iOS 與 Windows 之前的理由。
- UI 是懸浮候選窗（`NSPanel`），不畫鍵盤。
- 需要簽章與公證才能散佈。

---

## 5. Windows ~~（已退場，2026-08-16）~~

> 這一節保留當歷史。Windows 端做完並發布過，於 2026-08-16 停止開發、
> 從 main 移除；程式碼在標籤 `windows-final-24190704`
> 與 `desktop-final-5fa5baa`。
>
> ⚠ 下面「這一格決定了整個專案的技術選型下限」那一段，**當時是對的**，
> 但那個下限已經隨 Windows 線退場。現在壓住下限的是 iOS 鍵盤擴展的
> 記憶體額度（見 §3）。結論沒有變：核心層是一層薄 C ABI，UI 各端自己做。


### 宿主機制
**TSF（Text Services Framework）**，一個 COM in-proc server DLL。

### 這一格決定了整個專案的技術選型下限

> TSF 的 DLL 會被載入到**每一個**接受文字輸入的進程裡 —— 包括瀏覽器、Office、以及提權的系統進程。**你的 DLL 崩潰，宿主跟著崩潰。**

因此：

- 採 **瘦 DLL + 獨立服務進程** 的分離架構（與 Weasel 相同）：DLL 只做 TSF 協議與 IPC，librime 與候選窗渲染都在獨立的服務進程裡。
- DLL 側不可有重量級 runtime、不可有長時間阻塞、不可有未捕捉的例外。
- 需要 x64 與 arm64 兩份建置（32 位元宿主若要支援還需 x86）。
- 候選窗是服務進程開的獨立 top-level window，不是 DLL 畫的。

---

## 6. 效能紅線

**從按鍵事件到候選更新上屏，預算是一到兩幀。**

輸入法的體驗幾乎完全由延遲決定，卡一下就是「這輸入法不能用」。任何選型若無法滿足這條，直接淘汰 —— 這是 Electron / Flutter / JVM 類方案在桌面兩端出局的真正原因，而非偏好問題。

實務注意：
- 快照的深拷貝發生在每次按鍵，候選數通常 < 10，成本可忽略；但**不要**在這條路徑上做 I/O 或配置檔解析。
- 主題與佈局的解析只在載入時做一次，解析結果要快取成型別安全的物件，不可每次繪製都重讀 YAML。

---

## 7. 版本協商

`rime_shell.h` 定義了 `RIME_SHELL_ABI_VERSION`。前端啟動時應以 `rs_abi_version()` 比對自身編譯期的常數，不符即拒絕載入。

librime 自己的結構（`RimeContext` / `RimeStatus` 等）採 `data_size` 自我版本化，存取較新的欄位前**必須**用 `RIME_PROVIDED` / `RIME_STRUCT_HAS_MEMBER` 檢查，否則接上舊版 librime 會讀到越界記憶體。本層的 `pick_label()` 就是一個實例（`select_labels` 是 v0.9.2 之後才有的欄位）。
