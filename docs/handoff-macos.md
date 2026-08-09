# macOS 端交接

寫給接手 macOS 版的人。假設你沒看過這個專案。

---

## 0. 一分鐘版本

基於 [librime](https://github.com/rime/librime) 的開源輸入法,目標覆蓋
Android / iOS / Windows / macOS。**Android 已經是可用的產品**,其餘三端尚未開始。

主張是:**真正該跨平台的不是像素,是配置**。一套主題、一套鍵盤佈局描述,四端各自渲染。
(但這句話對桌面端只成立一半 —— 見 §3。)

- 程式碼:`https://github.com/IncredibleGG/rime-quad`(公開)
- **建置全部在 GitHub Actions**,`macos-latest` runner 內建 Xcode,不需要自備 Mac
- Android 的工具鏈與模擬器在 `lc@192.168.60.223:/home/lc/rime`(你不會用到,但那裡有可跑的參考實作)

**你是桌面端的第一個。** 這代表你會撞到共用層裡「為行動端設計、沒考慮桌面」的缺口
—— 這是預期中的,而且是你這一輪最有價值的產出。Windows 端會繼承你驗證過的規範。

---

## 1. 動手前必讀

| 檔案 | 為什麼 |
|---|---|
| **`docs/coordination.md`** | **四個會話唯一的溝通管道。**會話之間看不見彼此,只看得見 repo。檔案所有權、共用紀律、待裁決事項都在那裡,**動工前讀,有跨端影響的決定寫回去** |
| `core/include/rime_shell.h` | **你唯一該用的 API**。四端共用的 C ABI,檔頭有執行緒、記憶體、版本協商的完整約定 |
| `docs/architecture.md` | 四端各自怎麼接、按鍵映射、效能紅線 |
| `docs/theme-format.md` §8 | 主題規範。**你只需要 `candidates` 那部分**,`keyboard` 與佈局(§9)是行動端的 |
| `docs/handoff-windows.md` | Windows 端的交接。你們共用桌面形態,那份的 §5、§6 對你同樣成立 |

`core/src/rime_shell.cc` 是門面的實作,約 600 行純 C++,沒有平台相依,直接編進你的 app。

---

## 2. macOS 的架構

**宿主機制是 InputMethodKit**:`IMKServer` + `IMKInputController`。輸入法是一個獨立的
`.app`,安裝到 `~/Library/Input Methods`(或 `/Library/Input Methods`)。

**這是四端裡最好調試的一格**,而且這正是先做 macOS 的理由:

- **獨立進程**。崩潰不會拖垮宿主 app,可以直接下中斷點。
  (對照組:Windows 的 TSF DLL 被載入到每一個接受文字輸入的進程裡,你崩它就崩,
  所以那邊被迫做成「瘦 DLL + 獨立服務進程 + IPC」。)
- **UI 是懸浮候選窗**(`NSPanel`),不畫鍵盤。
- 需要簽章與公證才能散佈。

**與 iOS 的關係**:共用 Swift 技術棧與 `rime_shell` 綁定層,但 **UI 完全重寫** ——
一個是候選窗、一個是整塊軟鍵盤。**不要試圖抽象成同一套 View**,那是這類專案常見的死法。

---

## 3. 桌面端消費什麼、不消費什麼

| `core/` 底下 | macOS 用不用 |
|---|---|
| `core/layouts/*.yaml`(鍵盤佈局) | **完全不用**。那是軟鍵盤排版,桌面沒有軟鍵盤 |
| `core/themes/*.yaml` 的 `candidates` 區塊 | **要用**。候選窗的顏色、字級、排列、標籤格式 |
| `core/themes/*.yaml` 的 `keyboard` 區塊 | 不用 |
| `core/data/`(方案與詞庫) | **要用**,那是 librime 的執行期資料 |

⚠ **「不用」不等於「可以刪」。** 使用者的自訂佈局(`luminakey-layouts.json`)是行動端資料,
桌面端**必須原樣搬運、不得解析、不得清理**,否則跨裝置同步會把使用者在手機上調好的鍵位洗掉。

### 你會撞到的規範缺口(已知,§11 自己列著)

- **候選窗的多欄/表格排版**沒有定義。桌面候選數多時常用兩欄或表格,目前只有
  `orientation` 與 `max_width`,不足以描述。
- **狀態列/工具列的外觀**完全未規範。

**你是第一個撞到的人,所以規範由你擴充。** 但有一條紀律:

> **只有先動工的桌面端可以改 `docs/theme-format.md`。** 規範是四端共用的,兩邊各自改
> 就會分岔,而規範一分岔,「一套配置四端共用」這個主張就沒了。

擴充時請比照既有章節的寫法:欄位表(名稱/型別/預設值/哪些平台消費)、明確的錯誤處理約定、
以及可驗證的檢核項。規範寫得夠精確的標準是:**另一個人只讀規範就能在 C++ 上寫出行為一致的解析器**。

---

## 4. 按鍵映射

librime 吃的是 **X11 keysym**,不是字元、不是平台鍵碼。

modifier 遮罩的實際值(**必須查表,憑印象一定寫錯**,見 librime `src/rime/key_table.h`):

| 名稱 | 值 |
|---|---|
| `kShiftMask` | `1 << 0` |
| `kLockMask` | `1 << 1` |
| `kControlMask` | `1 << 2` |
| `kAltMask` | `1 << 3` |
| `kSuperMask` | **`1 << 26`**(不是 `1 << 6`) |
| `kReleaseMask` | `1 << 30` |

macOS 的來源是 `NSEvent.keyCode` + `charactersIgnoringModifiers`。難度中等
(Windows 的 `VK_*` + `GetKeyboardState` 更難)。

**關鍵**:映射受使用者的實體鍵盤佈局影響(QWERTY / Dvorak / 各國佈局),
**不可寫死成一張常數表**。

門面層已提供 `rs_keysym_by_name()` / `rs_keysym_name()`(純查表,不需初始化),
可用來把名稱轉 keysym,不必自己維護一份會腐爛的表。注意 librime 內部查不到時回傳
`XK_VoidSymbol`(0xffffff),門面層已正規化成 **0**。

---

## 5. Android 踩出來、對你同樣成立的教訓

**「選字」不等於「上屏」。** 拼音方案選字當下就 commit,注音方案選字後仍停在組字狀態。
判別條件是 `menu.count`:

```
count > 0                  → 還有段落待選,不可 commit
count == 0 && is_composing → 轉換完成待確認,呼叫 rs_commit_composition()
count == 0 && !is_composing→ 已結束
```

已用多音節輸入壓測驗證。**只測拼音永遠不會發現這件事。**

**commit 在 `rs_snapshot_acquire()` 當下就被消費,不是 release。** 所以「先 acquire 看狀態、
做點事、再 acquire 讀結果」會遺失第一次的 commit。紀律:**每個輸入事件只 acquire 一次**。

**`rs_deploy_callback` 不在呼叫端的執行緒上。** 來自 librime 的維護執行緒,可能在
`rs_deploy()` 早已返回之後才觸發。不可在回呼裡直接碰 UI。

**部署失敗時 `rs_last_error()` 是空字串。** librime 的 C API 不提供失敗原因。
「告訴使用者缺哪本詞典」只能自己預檢。

**首次部署要花時間。** 實測:Android 模擬器 7.2 秒、真機 12.5 秒。桌面應該更快但不會是零。
**這段等待要設計,不能假裝不存在。**

**效能紅線:從按鍵到候選更新,預算是一到兩幀。** 任何選型不滿足就淘汰。

---

## 6. 產品定位的硬約束

專案定位是 **離線為預設、無審查、經得起審計**。使用者原話:

> 「我是無聯網,但是你要聯網就自己打開開關,關掉了以後又是無聯網了。**我們經得起審計。**」

macOS 端要照同樣精神做:

1. **單一連網出口**,關閉時直接拒絕,fail-closed。Android 的參考實作在
   `android/.../net/NetworkGate.kt`,並有 `scripts/audit_offline.sh` 用 grep 守住
   「專案裡沒有第二個出口」。
2. **連網紀錄**:每次真的發生的連線記下時間、主機、原因、結果,使用者自己查。
   刻意**不記錄被開關擋下的嘗試** —— 記了的話「開關從沒開過所以紀錄是空的」這句話
   就不成立,而那正是使用者驗證我們的方式。
3. **不得宣稱做不到的事。** 先確認能做到,再寫進文案。

⚠ **第三方方案有程式碼執行能力。** librime-lua 已編進原生層,方案市集可下載第三方方案。
Android 端已沙盒化(移除 `os.execute`、`io.popen`、`package.loadlib`、危險的 `debug.*`),
但 `io.*` 仍開著。**macOS 移植必須套用同一份沙盒**:`patches/librime-lua@sandbox.patch`。
沒套等於第三方方案能在使用者機器上執行任意程式碼。

---

## 7. 怎麼驗證(這一節請認真看)

**CI 給你「編得出來」,給不了「真的能用」。**

`macos-latest` runner 沒有登入的圖形工作階段,系統不會從 `~/Library/Input Methods`
載入你的輸入法。所以 CI 上驗不了「候選窗有沒有出現」「在真的 app 裡打不打得出字」。

分兩層做:

**(a) CI 做得到的** —— 這一層價值最高,請優先建立:

`tools/rime_console.cc` 是一支**不經 UI 直接驅動 librime** 的命令列程式,餵按鍵、印候選、
印 commit。它在任何 runner 上都跑得起來。Android 就是靠它把「librime + 資料 + 門面」
和「平台 UI」分開驗證 —— **核心層綠而 UI 紅,問題就必在 UI**,省下大量瞎猜。

```bash
./scripts/run_console_test.sh --keys nihao --schema luna_pinyin_tw --expect 你好
```

再加上單元測試(主題解析、keysym 映射表、門面邏輯)、連結與符號檢查。

**(b) 只有人做得到的** —— 在真的 Mac 上裝起來實際用。

這一層無可取代。Android 這一路抓到的真 bug 幾乎都出自這裡:按下去顏色回不來、
重輸鍵是裝飾品、中英鍵不換佈局、鍵盤被拉伸。**這些編譯全部成功、單元測試全過。**

### 兩類最陰險的失敗,請特別防

**一、看得到但摸不到。** 「重輸」鍵呼叫的是「結束組字區」而不是清空,按下去什麼都沒發生;
中英鍵切了模式卻不換佈局,等於在九宮格上打英文。畫面完全正常,只有真的去按才知道。
**自動化只驗「打得出字」,驗不到「每顆鍵都真的做了它宣稱的事」。**

**二、會靜靜跳過自己的測試。** 發布關卡的升級測試曾因步驟順序寫反而被判定「略過」,
報出一片全綠;`LayoutEscapeTest` 的佈局清單寫死四份,12 份裡有 8 份從沒被檢查過,
而那幾份都真的有死路。**測試是綠的,因為它沒在測。**

寫測試時請自問:**它會不會在該紅的時候安靜地不跑?** 並實際植入一個違規驗證它會紅。

---

## 8. 現況

**已完成(Android)**:librime 交叉編譯、`rime_shell` 門面、拼音/注音/九宮格、
鍵盤與主題由 YAML 驅動、鍵盤類型選單、自定義鍵位、方案市集(34 個方案)、
離線開關與連網紀錄、應用內升級與金鑰輪替、325 項單元測試、16 項發布關卡。

**尚未開始**:macOS(你)、iOS、Windows。

完整待辦見 task 清單與 `docs/` 各文件末尾的「還沒解掉」段落。
