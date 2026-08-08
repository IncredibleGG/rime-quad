# macOS 端

RimeQuad 的 macOS 輸入法：`IMKServer` + `IMKInputController` + 自繪候選窗。

```
apple/
  RimeQuad/
    Package.swift              RimeQuadKit（純邏輯層）的 SwiftPM 宣告
    Sources/RimeQuadKit/       ⭐ 沒有 AppKit、沒有 librime、有單元測試
    Tests/RimeQuadKitTests/    105 項
    AppSources/                AppKit + InputMethodKit + librime 綁定
    Resources/                 Info.plist 樣板、選單圖示產生器
  scripts/
    build_macos.sh             librime + 5 個依賴 + librime-lua + rime_console
    build_app.sh               → apple/build/RimeQuad.app
    run_kit_tests.sh           單元測試 + 變異測試
    verify_app_bundle.sh       .app 結構與 IMKit 宣告
    verify_console.sh          核心層「打得出字」的斷言
    verify_data.sh             執行期資料齊全
    package_core.sh            核心層產物打包
```

---

## 1. 為什麼分成 RimeQuadKit 與 AppSources

**因為 CI 驗不了 UI。** `macos-latest` runner 沒有登入的圖形工作階段，
系統不會從 `~/Library/Input Methods` 載入輸入法，NSPanel 建得起來也不會出現在畫面上。

所以紀律是：**凡是算得出來的都不要留在 AppKit 那一側。**

| 在 `RimeQuadKit`（有測試） | 在 `AppSources`（沒有自動化） |
|---|---|
| RTS YAML 讀取、主題綁定、繼承合併、診斷 | 主題檔案的實際搜尋路徑 |
| `NSEvent` → keysym 的映射規則、修飾鍵狀態機 | 從 `NSEvent` 取值 |
| 候選窗排版（欄寬、落點、換行、超出處置） | 量字、畫圖、擺窗 |
| 上屏政策（`menu.count` 那三條） | 呼叫 `IMKTextInput` |
| 狀態列每一項顯示成什麼 | 畫狀態列 |

AppSources 裡剩下的東西都短到可以讀完。這不是潔癖，是唯一能在
「編得出來 ≠ 能用」這個前提下還保住品質的分法。

---

## 2. 安裝（只有人做得到）

CI 的 `rime-macos-app` artifact 是 **CI 產物，不是可散布的版本**：

* 只有跑 CI 的那一個架構（目前是 arm64），沒有做 universal binary；
* 只有 ad-hoc 簽章。散布需要 Developer ID + 公證；
* librime 的靜態庫沒有指定部署目標，實際最低系統版本可能高於
  `Info.plist` 宣告的 `LSMinimumSystemVersion`。

自己在 Mac 上裝來試：

```bash
tar xzf RimeQuad-app-arm64.tar.gz
mkdir -p ~/Library/Input\ Methods
rm -rf ~/Library/Input\ Methods/RimeQuad.app
cp -R RimeQuad.app ~/Library/Input\ Methods/
# 讓 TIS 看到它（第一次安裝之後要登出再登入才最保險）
open ~/Library/Input\ Methods/RimeQuad.app
```

然後在「系統設定 › 鍵盤 › 輸入來源」加入 **RimeQuad**。

使用者資料在 `~/Library/Application Support/RimeQuad/`。
**刻意不用 `~/Library/Rime`** —— 那是 Squirrel 的目錄，兩個輸入法共用同一份
使用者詞典與 `installation.yaml` 會互相踩，而使用者完全看不出是誰改的。

自訂主題放 `~/Library/Application Support/RimeQuad/themes/`，
它的優先序高於 `.app` 內建的那一份（規範 §2.3）。

---

## 3. ⚠ 沒有被自動化驗到的東西

**這一節是本文件最重要的一節。** CI 全綠**不代表能用**。
本專案已經有過「編譯成功、單元測試全過、發布關卡全綠，而使用者一裝上去
按鍵就永久變灰」的紀錄。

### CI 驗得到的

- 105 項純邏輯單元測試，外加 5 個變異測試（植入違規，斷言測試會紅）
- `rime_console` 不經 UI 直接驅動 librime：`nihao → 你好`、`su3cl3 → 你好`
- 四個方案都部署成功、執行期資料齊全
- `.app` 編得起來、bundle 結構完整、`Info.plist` 的 IMKit 宣告齊全
- 二進位裡真的有 `_OBJC_CLASS_$_RimeQuadInputController`（`@objc(...)` 有生效）
- 二進位真的連上 InputMethodKit，且 librime 是**靜態**連結
- `RimeQuad --self-check`：向真的 librime 問 `PhysicalKeys` 表裡每一個 keysym 名稱

### CI **驗不到**的（只有人在真 Mac 上跑才驗得到）

1. **輸入法到底載不載得起來。** TIS 只在有登入的圖形工作階段時掃描
   `~/Library/Input Methods`。plist 全部正確也可能因為簽章、隔離屬性
   （quarantine）、或 macOS 版本差異而載不起來。
2. **候選窗有沒有出現、長什麼樣。** 排版數字全部有測試，但「主題的顏色套上去
   好不好看」「窗有沒有蓋住游標」「多螢幕與 Retina 上的位置」一項都沒驗。
3. **在真的 app 裡打不打得出字。** 每個宿主 app 的 `IMKTextInput` 實作品質
   不同。已知會出事的類型：Electron（Slack / VS Code）、Java（JetBrains）、
   終端機、以及任何自繪文字框的 app。**這幾類必須各試一個。**
4. **`attributes(forCharacterIndex:lineHeightRectangle:)` 回報的插入點。**
   有些宿主回報 `NSRect.zero`，本實作會退回滑鼠位置 —— 那條退路從沒被真的走過。
5. **實體鍵盤佈局。** 映射邏輯有 Dvorak / AZERTY 的單元測試，但那是**餵假資料**：
   真正要驗的是「macOS 在那些佈局下送出的 `charactersIgnoringModifiers` 是不是
   我以為的那個字」。至少要在 Dvorak 與一個非拉丁佈局上實測。
6. **修飾鍵。** 「輕點 Shift 切中英」是所有 RIME 使用者的肌肉記憶，
   而它靠的是 `flagsChanged` 的 release 事件。狀態機有測試，
   真的按下去會不會動沒驗過。⌘C / ⌘V 有沒有被吃掉也沒驗過。
7. **VoiceOver。** 候選項有 `accessibilityLabel` 也有 `accessibilityPerformPress`，
   但「打開 VoiceOver 真的摸得到候選、輕點兩下真的選得到字」沒有任何自動化。
   Android 端已經證實**注入式的點擊驗不了朗讀器**（`adb input` 繞過無障礙輸入
   過濾），macOS 上同理。
8. **首次部署的等待。** Android 實測是 7–12 秒。桌面應該更快，但那段時間
   使用者按鍵會發生什麼（現在是被 `hasSession` 擋掉）沒有真的看過。
9. **深淺色跟隨。** 監聽的是 `AppleInterfaceThemeChangedNotification`，
   切換時候選窗會不會即時換主題沒驗過。
10. **每一顆能點的東西都真的做了它宣稱的事。** 狀態列的每一項、選單裡的
    每一個方案、候選窗的滑鼠點選。這是本專案抓到最多真 bug 的一類 ——
    **畫面完全正常、自動化全過**。

### 驗收時請這樣做

不要只問「打不打得出字」，要問「**每一顆鍵、每一個可點的東西，是不是都真的做了
它宣稱的事**」。並且逐一走過上面第 3 條的四類宿主 app。

---

## 4. 三個必須一起改的名字

`RimeQuadInputController` 同時出現在：

1. `AppSources/RimeQuadInputController.swift` 的 `@objc(RimeQuadInputController)`
2. `Resources/Info.plist` 的 `InputMethodServerControllerClass`
3. `scripts/verify_app_bundle.sh` 的斷言

少了第 1 個，Swift 會把類別名 mangle 成 `_TtC8RimeQuad24RimeQuadInputController`，
IMKit 的 `NSClassFromString` 找不到它 —— 症狀是**輸入法裝得起來、選單裡看得到、
但一個字都打不出來，而且完全沒有錯誤訊息**。第 3 條斷言就是為了擋這個。

---

## 5. 桌面端消費什麼

| `core/` 底下 | 用不用 |
|---|---|
| `core/themes/*.yaml` 的 `candidates` / `preedit` / `status_bar` / `typography` / `palette` / `metrics` / `accessibility` | **要用** |
| `core/themes/*.yaml` 的 `keyboard` / `feedback` / `candidates.bar` | 不用（規範 §1.1 的形態分野） |
| `core/layouts/*.yaml` | **完全不用**，也不進 bundle |
| `core/data/` | **要用**，librime 的執行期資料 |

⚠ **「不用」不等於「可以刪」。** 使用者的自訂佈局（`rimequad-layouts.json`）是
行動端資料，桌面端**必須原樣搬運、不得解析、不得清理**，否則跨裝置同步會把
使用者在手機上調好的鍵位洗掉。
