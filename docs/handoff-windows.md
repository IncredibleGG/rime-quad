# Windows 端交接

寫給接手 Windows 版的人。假設你沒看過這個專案。

---

## 0. 一分鐘版本

這是一個基於 [librime](https://github.com/rime/librime) 的開源輸入法,目標覆蓋
Android / iOS / Windows / macOS。**Android 已經是可用的產品**,其餘三端尚未開始。

專案的主張是:

> 真正該跨平台的不是像素,是**配置**。一套主題、一套鍵盤佈局描述,四端各自渲染。

**但這句話對 Windows 只成立一半** —— 見 §3。

- 程式碼:`https://github.com/IncredibleGG/rime-quad`(公開)
- 建置機器:`lc@192.168.60.223` 的 `/home/lc/rime`(Android 的模擬器與工具鏈都在這)
- CI:GitHub Actions。**建置全部在雲端,不需要自備 Windows 機器** ——
  `windows-latest` runner 內建 MSVC。現有 workflow:`build.yml`(Android)、
  `native.yml`(librime 交叉編譯);Windows 的要你自己加。

⚠ **但 CI 給的是「編得出來」,不是「真的能用」。** TSF 需要註冊 COM in-proc server
並有真實的文字輸入情境,CI runner 上做不到。驗證要分兩層,見 §8。

---

## 1. 動手前必讀的三份

| 檔案 | 為什麼 |
|---|---|
| **`docs/coordination.md`** | **四個會話唯一的溝通管道。**會話之間看不見彼此,只看得見 repo。檔案所有權、共用紀律、待裁決事項都在那裡,**動工前讀,有跨端影響的決定寫回去** |
| `core/include/rime_shell.h` | **你唯一該用的 API**。四端共用的 C ABI,檔頭有執行緒、記憶體、版本協商的完整約定 |
| `docs/architecture.md` | 四端各自怎麼接、按鍵映射、效能紅線 |
| `docs/theme-format.md` | 主題與佈局的規範。**很長,但 §8(主題)是你要實作的部分** |

`core/src/rime_shell.cc` 是那層門面的實作,約 600 行,可以直接編進 Windows 的服務進程。它只用 librime 的 `rime_get_api()` 函式指標表,沒有平台相依。

---

## 2. Windows 的架構(這一格決定了整個專案的技術選型下限)

**宿主機制是 TSF(Text Services Framework)**,一個 COM in-proc server DLL。

> TSF 的 DLL 會被載入到**每一個**接受文字輸入的進程裡 —— 包括瀏覽器、Office、
> 以及提權的系統進程。**你的 DLL 崩潰,宿主跟著崩潰。**

所以架構必須是 **瘦 DLL + 獨立服務進程**(與 Weasel 相同):

```
每個宿主進程                        單一服務進程
┌──────────────────┐              ┌────────────────────────┐
│ TSF DLL(瘦)     │◄── IPC ────►│ rime_shell + librime    │
│ 只做協議與 IPC    │              │ 候選窗渲染(獨立視窗)   │
└──────────────────┘              └────────────────────────┘
```

- DLL 側不可有重量級 runtime、不可長時間阻塞、不可有未捕捉的例外。
- 需要 x64 與 arm64 兩份建置(32 位元宿主若要支援還需 x86)。
- 候選窗是服務進程開的獨立 top-level window,**不是 DLL 畫的**。

這也是為什麼 Electron / Flutter / JVM 類方案在這一格出局 —— 不是偏好問題。

---

## 3. Windows **不消費**佈局,只消費主題的一部分

這是最容易誤解的地方,先講清楚免得白做:

| `core/` 底下的東西 | Windows 用不用 |
|---|---|
| `core/layouts/*.yaml`(鍵盤佈局) | **完全不用**。那是軟鍵盤的排版,桌面端沒有軟鍵盤 |
| `core/themes/*.yaml` 的 `candidates` 區塊 | **要用**。候選窗的顏色、字級、排列、標籤格式 |
| `core/themes/*.yaml` 的 `keyboard` 區塊 | 不用 |
| `core/data/`(方案與詞庫) | **要用**,那是 librime 的執行期資料 |

⚠ **同步時「不用」不等於「可以刪」。** 使用者的自訂佈局(`rimequad-layouts.json`)
是行動端的資料,桌面端**必須原樣搬運、不得解析、不得清理**,否則跨裝置同步時
會把使用者在手機上調好的鍵位一次洗掉。

主題規範 §11 已經列出候選窗還缺的東西:多欄/表格排版、狀態列外觀。那些是你會
第一個撞到的缺口,**發現規範不夠請提出而不是自己擴充** —— 四端共用一份規範。

---

## 4. 按鍵映射:Windows 是四端中最難的一格

librime 吃的是 **X11 keysym**,不是字元、不是平台鍵碼。

modifier 遮罩的實際值(**這些必須查表,憑印象一定寫錯**,見 librime `src/rime/key_table.h`):

| 名稱 | 值 |
|---|---|
| `kShiftMask` | `1 << 0` |
| `kLockMask` | `1 << 1` |
| `kControlMask` | `1 << 2` |
| `kAltMask` | `1 << 3` |
| `kSuperMask` | **`1 << 26`**(不是 `1 << 6`) |
| `kReleaseMask` | `1 << 30` |

各端難度:

- **Android / iOS**:鍵盤自繪,佈局 YAML 直接指定 keysym。**沒有映射問題。**
- **macOS**:`NSEvent.keyCode` + `charactersIgnoringModifiers`。中等。
- **Windows**:TSF 收到的 `VK_*` + `GetKeyboardState`。**最難。**

Windows 這一格的關鍵:**映射受使用者的實體鍵盤佈局影響**(QWERTY / Dvorak /
各國佈局),**不可寫死成一張常數表**。

門面層已經提供 `rs_keysym_by_name()` / `rs_keysym_name()`(純查表,不需初始化),
可以用來把名稱轉成 keysym,不必自己維護一份會腐爛的表。注意 librime 內部查不到時
回傳 `XK_VoidSymbol`(0xffffff),門面層已正規化成 **0**。

---

## 5. 從 Android 學到、但對你同樣成立的教訓

這些都是實際踩過的,不是理論:

**「選字」不等於「上屏」。** 拼音方案選字當下就 commit,注音方案選字後仍停留在
組字狀態。判別條件是 `menu.count`:

```
count > 0                  → 還有段落待選,不可 commit
count == 0 && is_composing → 轉換完成待確認,呼叫 rs_commit_composition()
count == 0 && !is_composing→ 已結束
```

這條已在模擬器上用多音節輸入壓測驗證過。**只測拼音永遠不會發現這件事。**

**commit 在 `rs_snapshot_acquire()` 當下就被消費,不是在 release。** 所以
「先 acquire 看狀態、做點事、再 acquire 讀結果」會遺失第一次的 commit。
紀律是:**每個輸入事件只 acquire 一次**。

**`rs_deploy_callback` 不在呼叫端的執行緒上。** 它來自 librime 的維護執行緒,
可能在 `rs_deploy()` 早已返回之後才觸發。不可在回呼裡直接碰 UI。

**部署失敗時 `rs_last_error()` 是空字串。** librime 的 C API 根本不提供失敗原因。
「告訴使用者缺哪本詞典」只能靠前端自己預檢,不要指望問得到。

**效能紅線:從按鍵到候選更新,預算是一到兩幀。** 任何選型不滿足這條就淘汰。

---

## 6. 產品定位的硬約束(這會限制你的技術選擇)

專案定位是 **離線為預設、無審查、經得起審計**。使用者的原話:

> 「我是無聯網,但是你要聯網就自己打開開關,關掉了以後又是無聯網了。**我們經得起審計。**」

Android 端的做法(Windows 要照同樣的精神做):

1. **單一連網出口**(`android/.../net/NetworkGate.kt`),關閉時直接拒絕,
   fail-closed。有一支 `scripts/audit_offline.sh` 用 grep 守住「專案裡沒有第二個出口」。
2. **連網紀錄**:每一次真的發生的連線都記下時間、主機、原因、結果,使用者自己查。
   刻意**不記錄被開關擋下的嘗試** —— 記了的話「開關從沒開過所以紀錄是空的」
   這句話就不成立,而那正是使用者驗證我們的方式。
3. **不得宣稱做不到的事。** Android 上不能說「本 app 沒有網路權限」(那是假的,
   安裝時權限取消不了)。Windows 上同理:**先確認你能做到,再寫進文案。**

⚠ **第三方方案有程式碼執行能力。** librime-lua 已編進原生層,方案市集可下載
第三方方案。Android 端已經沙盒化(移除 `os.execute`、`io.popen`、`package.loadlib`
等),但 `io.*` 仍開著。**Windows 移植時必須套用同一份沙盒**,見
`patches/librime-lua@sandbox.patch`。沒套等於第三方方案能在使用者機器上執行任意程式碼。

---

## 7. 現況與待辦

**已完成(Android)**:librime 交叉編譯、`rime_shell` 門面、拼音/注音/九宮格、
鍵盤與主題完全由 YAML 驅動、鍵盤類型選單、自定義鍵位、方案市集(34 個方案)、
離線開關與連網紀錄、應用內升級與金鑰輪替、325 項單元測試、16 項發布關卡。

**尚未開始**:macOS、iOS、Windows。

**與 Windows 相關的已知缺口**:
- 主題規範沒有候選窗的多欄/表格排版(§11)
- 方案 id 不是全域唯一的(`double_pinyin` 同時存在於兩個套件,字集相反)
- 規範 §9.3 的 slack 間距未定義,四端會做出兩種結果(這條只影響行動端)

完整待辦見專案的 task 清單與 `docs/` 底下各份文件末尾的「還沒解掉」段落。

---

## 8. 怎麼驗證你做的東西

Android 端建立了一套「不經 UI 直接驗核心」的做法,Windows 可以照抄:

`tools/rime_console.cc` 編成執行檔,直接跑 librime,餵按鍵、印候選、印 commit。
它把「librime + 資料 + 門面」和「平台 UI」**分開驗證** —— 若核心層打得出字而
你的 UI 打不出,問題必在 UI。

```bash
./scripts/run_console_test.sh --keys nihao --schema luna_pinyin_tw --expect 你好
```

**一個一再重複的教訓**:自動化測試只驗「打得出字」,驗不到「每一顆鍵都真的做了
它宣稱的事」。這個專案已經抓到三個「看得到但摸不到」的問題(重輸鍵呼叫的是結束
組字而不是清空、中英鍵不換佈局、按下後顏色回不來),全部是使用者實際用才發現的。

還有一類更危險的:**會靜靜跳過自己的測試**。發布關卡的升級測試曾經因為順序寫反
而被判定「略過」,報出一片全綠;`LayoutEscapeTest` 的佈局清單寫死四份,12 份裡有
8 份從沒被檢查過,而那幾份都真的有死路。**測試是綠的,因為它沒在測。**
