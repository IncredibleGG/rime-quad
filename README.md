# LuminaKey

**LuminaKey 輸入法** —— 基於 [librime](https://github.com/rime/librime) 的開源輸入法客戶端，
目標平台為 **Android** 與 **iOS**。

Android 是目前唯一有實作的一端，也是標竿實現（reference implementation）；
iOS 尚未開始，開工時對齊 Android 的行為。

> **2026-08-16：Windows 與 macOS 兩條桌面線已停止開發並從 main 移除。**
> 程式碼沒有消失，完整保留在標籤 `desktop-final-5fa5baa`
> （Windows 另有 `windows-final-24190704`）——
> `git checkout desktop-final-5fa5baa -- apple/ windows/` 就撿得回來。
> 移除的理由、被搬走的守門、以及哪幾條規範因此失去執行者，
> 記在 [`docs/coordination.md`](docs/coordination.md) 的 2026-08-16 那一則。

> 名字的由來與「為什麼識別碼要趁現在一起改」見 [`docs/decisions/product-name.md`](docs/decisions/product-name.md)。
> 產品名與各平台識別碼的**唯一來源**是 [`scripts/lib/product.env`](scripts/lib/product.env)，
> 腳本一律從那裡讀 —— 這一輪改名之所以痛，就是因為同一個 id 曾在九支腳本裡各寫死一份。
>
> 倉庫名仍是 `rime-quad`、R2 的發布路徑仍是 `rime/…`：兩者都被既有的推送腳本與
> **使用者裝置上的應用內升級**指著，改了會斷。這是刻意保留，不是漏改。

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
              │            │
      ┌───────┘      └─────┐
      ▼                    ▼
┌───────────┐        ┌───────────┐
│  Android  │        │    iOS    │
│ IMEService│        │ Keyboard  │
│  Compose  │        │ Extension │
│    JNI    │        │  SwiftUI  │
└───────────┘        └───────────┘
   軟鍵盤              軟鍵盤
   已實作              尚未開始
```

**UI 不共用，且不應該共用。** 兩端的輸入法宿主機制不同：

| 平台 | 宿主機制 | UI 實際型態 | 關鍵約束 |
|---|---|---|---|
| Android | `InputMethodService` | 整塊軟鍵盤 | librime 需 NDK 交叉編譯 + JNI |
| iOS | Keyboard Extension | 整塊軟鍵盤 | 記憶體額度僅數十 MB；沙盒需 App Group 傳配置 |

iOS 鍵盤擴展那一格決定了技術選型的下限：記憶體額度只有數十 MB，
而擴展被系統隨時可回收。Flutter / Electron / JVM 類方案在這一格不可行。

> 桌面兩端收掉之前，真正壓住下限的是 Windows 的 TSF DLL（被載入**每個**宿主
> 進程，崩潰即拖垮宿主），因此當時採「瘦 DLL + 獨立服務進程」的分離架構。
> 那個約束隨 Windows 線一起退場，但**結論沒有變**：核心層仍然是一層薄 C ABI，
> UI 仍然各端自己做。

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
```

> `apple/`（macOS）與 `windows/` 已於 2026-08-16 移除，見標籤
> `desktop-final-5fa5baa`。iOS 開工時的起點會是新的目錄，不是撿回 `apple/`
> 整棵 —— 那棵樹裡沒有任何 iOS 程式碼（`Package.swift` 宣告的是
> `platforms: [.macOS(.v11)]`），但 `LuminaKeyKit` 那 39 個檔裡有 37 個
> 完全不 import AppKit（ThemeParser、MiniYaml、CandidateLayout、StoreEngine、
> NetworkGate、KeyMapper…），連同 200 餘項單元測試，是最可能直接沿用的資產。

---

## 路線圖

- [x] **M0** 共用層定案：`rime_shell.h` ABI、主題與佈局格式規範
- [x] **M1** Android 跑通 —— **模擬器上實測可用**
      librime 1.17.0 交叉編譯（arm64-v8a + x86_64）→ JNI → Compose 鍵盤 → 拼音打出「你好」
- [ ] **M2** Android 消費 `core/themes` 與 `core/layouts`，主題可換
- [ ] **M3** iOS 鍵盤擴展（Swift；純邏輯層可從 `desktop-final-5fa5baa` 的
      `apple/LuminaKey/Sources/LuminaKeyKit` 撿回來當起點，UI 重打）
- [ ] **M4** 配置同步

> 原本的 M3（macOS IMKit）與 M5（Windows TSF）**已完成過並且發布過**，
> 於 2026-08-16 停止開發、從 main 移除。原本的 M4「iOS 鍵盤擴展（與 macOS
> 共用 Swift 層）」改寫成上面的 M3：那個「共用」的對象已經不在樹上了。

---

## 建置與驗證

**建置全部在 GitHub Actions 上**,不需要任何本機環境:

| 平台 | CI runner | workflow |
|---|---|---|
| Android | `ubuntu-latest` + NDK | `.github/workflows/build.yml` |
| iOS | `macos-latest` | 待建立(這條線尚未開始) |

> `.github/workflows/macos.yml` 與 `windows.yml` 已隨桌面兩端一起刪除。
> 現在 `.github/workflows/` 底下只有 `build.yml`、`native.yml`、
> `schema-store.yml` 三份。

**但 CI 給的是「編得出來」,不是「真的能用」。**

這個專案抓到的真 bug 幾乎都是靠實際安裝、點鍵、截圖比對輸入框內容才發現的
—— 按下去顏色回不來、重輸鍵是裝飾品、中英鍵不換佈局、鍵盤被拉伸。**這些編譯
全部成功。** Android 靠模擬器把那個迴圈跑進 CI(見 build.yml 的慢車道);
iOS 開工之後會遇到同一個問題 —— 模擬器跑得動鍵盤擴展,但記憶體額度與
真機的沙盒行為要真機才驗得到。

> 桌面兩端當年在 CI 上**跑不了**那個迴圈:macOS 的輸入法要由系統從
> `~/Library/Input Methods` 載入,CI runner 沒有登入的圖形工作階段;
> Windows 的 TSF 要註冊 COM in-proc server 並有真實輸入情境。
> 那是它們的驗證成本一直居高不下的原因之一。

所以每一端都要有兩層:

1. **CI 能做的**:單元測試、連結與符號檢查、以及 `tools/rime_console.cc` 那種
   **不經 UI 直接驅動 librime** 的核心驗證(純命令列,四端 runner 都跑得起來)。
   Android 上就是這一層把「核心與資料對不對」和「UI 對不對」分開的。
2. **只有人做得到的**:在真實裝置上實際用一遍。這一層無可取代。

---|---|---|
| Android | Linux/macOS + JDK 17 + Android SDK/NDK | ✅ 已具備，M1 即在此完成 |
| macOS | **一台 Mac** + Xcode | ❌ 尚無可用機器 |
| iOS | **一台 Mac** + Xcode（模擬器可測，上架需開發者帳號） | ❌ 尚無可用機器 |
| Windows | **一台 Windows** + MSVC（TSF 實務上無法用 mingw 交叉編譯） | ❌ 尚無可用機器 |

Android 之外的三端**無法在目前的建置環境上編譯或驗證**。程式碼可以先寫，
但依本專案「不驗證過的東西不算完成」的原則，M3–M5 在取得對應機器前
不會被標記為完成。

---

## 待決事項

- **授權條款**：librime 本身為 BSD-3-Clause（寬鬆），但 Weasel / Squirrel / Trime 等現成前端為 GPL 系。
  若計畫參考或移植其程式碼，需先確認相容性——這會反過來決定本專案的授權選擇。
- ~~**專案正式名稱**與 bundle id / package name~~ ——
  2026-08-09 定為 **LuminaKey**（`org.luminakey.*`），見 `docs/decisions/product-name.md`。
- **網域與商標查核**尚未做。名字定了不等於名字是安全的。
