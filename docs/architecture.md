# 架構：四端如何接上同一顆核心

本文記錄各平台的接入細節。這些是**踩過才知道**的知識，寫下來是為了避免在第二、第三端重新發現一次。

---

## 0. 分層與契約

```
librime (C++, 靜態連結)
   └── rime_shell.h / rime_shell.cc     ← 四端共用，唯一的 ABI 邊界
          ├── Android   JNI          →  Kotlin
          ├── iOS/macOS Swift C interop
          └── Windows   服務進程內直接呼叫
```

跨越 `rime_shell.h` 的東西只有：POD 結構、`const char*`、整數。**沒有 C++ 型別、沒有例外、沒有回呼以外的控制反轉。** 這條紀律讓同一份實作能同時被 JNI、Swift 與 MSVC 消費。

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

## 1. 按鍵映射：四端最大的隱藏工作量

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
| **macOS** | `NSEvent.keyCode` + `charactersIgnoringModifiers` | 中 |
| **Windows** | TSF 收到的 `VK_*` + `GetKeyboardState` | 高 |

桌面兩端必須自行實作「原生鍵碼 → keysym」，且**這個映射受使用者的實體鍵盤佈局影響**（QWERTY / Dvorak / 各國佈局），不可寫死成一張常數表。這是先做 Android 的理由之一：行動端能完全繞開這個問題，讓我們先把核心與 UI 跑通，再回頭處理桌面端的鍵碼地獄。

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

## 3. iOS

### 宿主機制
**Keyboard Extension（App Extension）**，不是一般的 App。整塊鍵盤自繪，與 Android 形態相同，但約束嚴苛得多。

### 硬約束
- **記憶體額度只有數十 MB**，超過直接被 jetsam 殺掉。這條否決了任何重量級 runtime（Flutter engine 等），也意味著詞庫載入策略要謹慎。
- 沙盒隔離：主 App 與鍵盤擴展是**兩個不同的進程、不同的容器**。`user_data_dir` 必須放在 **App Group** 共享容器內，否則在主 App 裡設定的東西鍵盤看不到。
- 「完全取用權限」（Full Access）未授權時，某些能力受限。設計上應讓核心輸入功能**不依賴** Full Access。
- App Store 審查對輸入法（尤其要求 Full Access 的）特別嚴格。

### 與 macOS 的關係
共用 Swift 技術棧與 `rime_shell` 綁定層，但 **UI 完全重寫** —— 一個是軟鍵盤，一個是候選窗。不要試圖抽象成同一套 View。

---

## 4. macOS

### 宿主機制
**InputMethodKit**：`IMKServer` + `IMKInputController`。輸入法是一個獨立的 `.app`，安裝到 `/Library/Input Methods` 或 `~/Library/Input Methods`。

### 特點
- 四端中**最好調試的**：獨立進程，崩潰不會拖垮別人，可以直接下中斷點。這是把它排在 Android 之後、iOS 與 Windows 之前的理由。
- UI 是懸浮候選窗（`NSPanel`），不畫鍵盤。
- 需要簽章與公證才能散佈。

---

## 5. Windows

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
