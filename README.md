# Rime 四端客戶端（工作名稱，待定）

基於 [librime](https://github.com/rime/librime) 的開源輸入法客戶端，目標覆蓋 **Android / iOS / Windows / macOS** 四端。

Android 為標竿實現（reference implementation），其餘三端對齊其行為。

---

## 這個專案想解決什麼

RIME 生態每一端都已經有成熟前端（Weasel / Squirrel / Trime / Hamster），
但它們各自為政：**主題格式互不相容、鍵盤佈局無法共用、配置無法同步**。
換一台裝置就要重新調一次外觀與佈局。

本專案的主張是：

> 真正該跨平台的不是像素，是**配置**。
> 一套主題、一套鍵盤佈局描述、一套 schema，四端各自渲染。

---

## 架構

```
┌─────────────────────────────────────────────────────────┐
│  共用層（C / C++，四端完全相同）                          │
│                                                          │
│   librime  ──►  rime_shell.h   薄 C ABI 門面              │
│                 · session 生命週期                        │
│                 · 按鍵 → 狀態快照（composition/menu/status）│
│                 · schema 切換、選項開關、部署               │
│                                                          │
│   core/themes/*.yaml    主題（顏色、字型、圓角、間距）      │
│   core/layouts/*.yaml   鍵盤佈局（僅行動端消費）           │
└─────────────────────────────────────────────────────────┘
              │            │            │            │
      ┌───────┘      ┌─────┘      ┌─────┘      ┌─────┘
      ▼              ▼            ▼            ▼
┌───────────┐  ┌───────────┐ ┌──────────┐ ┌──────────┐
│  Android  │  │    iOS    │ │  macOS   │ │ Windows  │
│ IMEService│  │ Keyboard  │ │  IMKit   │ │   TSF    │
│  Compose  │  │ Extension │ │ SwiftUI  │ │ Win32 +  │
│    JNI    │  │  SwiftUI  │ │          │ │ Direct2D │
└───────────┘  └───────────┘ └──────────┘ └──────────┘
   軟鍵盤          軟鍵盤        候選窗        候選窗
```

**UI 不共用，且不應該共用。** 四端的輸入法宿主機制根本不同：

| 平台 | 宿主機制 | UI 實際型態 | 關鍵約束 |
|---|---|---|---|
| Android | `InputMethodService` | 整塊軟鍵盤 | librime 需 NDK 交叉編譯 + JNI |
| iOS | Keyboard Extension | 整塊軟鍵盤 | 記憶體額度僅數十 MB；沙盒需 App Group 傳配置 |
| macOS | InputMethodKit（獨立 .app 進程） | 懸浮候選窗 | 需簽章與公證 |
| Windows | TSF（COM in-proc DLL） | 懸浮候選窗 | DLL 被載入**每個**宿主進程，崩潰即拖垮宿主 |

Windows 那一格決定了技術選型的下限：TSF DLL 必須極瘦極穩，
因此採 **瘦 DLL + 獨立服務進程** 的分離架構（同 Weasel 的做法）。
Flutter / Electron / JVM 類方案在這一格與 iOS 鍵盤擴展格皆不可行。

## 效能紅線

候選窗／鍵盤的更新延遲是這類產品的全部體驗。
**從按鍵事件到候選更新上屏，預算為一到兩幀。** 任何選型若無法滿足，直接淘汰。

---

## 目錄

```
core/include/rime_shell.h    四端共用的 C ABI 門面（librime 之上）
core/themes/                 主題定義
core/layouts/                鍵盤佈局定義
docs/architecture.md         各端接入細節、IPC、鍵碼映射
docs/theme-format.md         主題與佈局格式規範
android/                     Android 端（標竿實現）
apple/                       iOS + macOS（共用 Swift 技術棧）
windows/                     Windows TSF 端
```

---

## 路線圖

- [ ] **M0** 共用層定案：`rime_shell.h` ABI、主題與佈局格式規範
- [ ] **M1** Android 跑通：librime NDK 交叉編譯 → JNI → Compose 鍵盤 → 能上屏
- [ ] **M2** Android 消費 `core/themes` 與 `core/layouts`，主題可換
- [ ] **M3** macOS（IMKit + SwiftUI 候選窗），驗證共用層在桌面形態成立
- [ ] **M4** iOS 鍵盤擴展（與 macOS 共用 Swift 層，重打 UI）
- [ ] **M5** Windows TSF（瘦 DLL + 服務進程）
- [ ] **M6** 配置同步

---

## 待決事項

- **授權條款**：librime 本身為 BSD-3-Clause（寬鬆），但 Weasel / Squirrel / Trime 等現成前端為 GPL 系。
  若計畫參考或移植其程式碼，需先確認相容性——這會反過來決定本專案的授權選擇。
- **專案正式名稱**與 bundle id / package name。
