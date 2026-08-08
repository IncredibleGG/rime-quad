# 跨端協調

四個會話同時在這個專案上工作。這份文件是**唯一的溝通管道** —— 會話之間看不見彼此,
只看得見 repo。**動工前讀,有跨端影響的決定寫回來。**

| 角色 | 負責 | 工作目錄 | 分支 |
|---|---|---|---|
| **協調** | 合併、裁決規範、發版 | `/home/lc/rime` | `main` |
| **Android** | `android/` | `/home/lc/rime-android` | `android` |
| **macOS** | `apple/`(macOS 部分) | `/home/lc/rime-macos` | `macos` |
| **Windows** | `windows/` | `/home/lc/rime-windows` | `windows` |

---

## 1. 工作目錄:每個端一個 worktree

**不要三個會話擠在 `/home/lc/rime`。** 今天已經發生過:整個目錄 rsync 蓋掉別人未提交的
工作、`git add` 過寬把別人的半成品一起提交、變更被整批還原後要靠冪等腳本補回。
那還只是同一個會話裡的 agent 之間,獨立會話會更糟 —— 它們連對方存在都不知道。

第一次進來時建立自己的 worktree:

```bash
cd /home/lc/rime
git worktree add /home/lc/rime-<你的端> -b <你的端>    # 已存在就 git worktree add /home/lc/rime-<端> <端>
cd /home/lc/rime-<你的端>
```

之後**只在自己的 worktree 裡工作**。`/home/lc/rime` 是協調端的,不要動。

推送到 GitHub 用自己的分支:

```bash
/home/lc/github-push.sh /home/lc/rime-<你的端> rime-quad <你的端>
```

CI 的 workflow 記得在 `on.push.branches` 加上你的分支,否則推了不會跑。

合併回 `main` 由協調端做。你覺得可以合了就寫進下面的「狀態」欄。

---

## 2. 檔案所有權

| 路徑 | 誰能改 |
|---|---|
| `android/` | Android |
| `apple/` | macOS(iOS 之後也在這) |
| `windows/` | Windows |
| `core/include/rime_shell.h`、`core/src/` | **協調端**。要加 ABI 就提出,不要自己加 —— 四端都在用 |
| `docs/theme-format.md` | **只有 macOS 端**(第一個桌面端,候選窗規範由它擴充)。其餘一律回報 |
| `core/layouts/`、`core/themes/` | Android(行動端的佈局);桌面端只讀 |
| `core/data/`、`scripts/schema_store/` | 協調端 |
| `scripts/` 其餘 | 誰做的誰維護,新增用不撞名的檔名 |
| `docs/handoff-*.md`、本檔 | 大家都可以補,但**只加不刪別人的段落** |

**跨這條線之前先寫進 §5**,不要先做再說。

---

## 3. 對所有端都成立的紀律

這些是 Android 端一路踩出來的,寫在這裡免得每一端各踩一次。

**「編得出來」不等於「能打出字」。** 這個專案有過編譯成功、單元測試全過、發布關卡
全綠,而使用者一裝上去按鍵就永久變灰。每一端都要分兩層驗證:

- **CI 驗得了的**:單元測試、連結與符號檢查、以及 `tools/rime_console.cc` 那種
  **不經 UI 直接驅動 librime** 的核心驗證。核心層綠而 UI 紅 → 問題必在 UI,不必猜。
- **只有人做得到的**:在真裝置上實際用一遍。無可取代。

**小心會靜靜跳過自己的測試。** 發布關卡的升級測試曾因步驟順序寫反而被判「略過」,
報出一片全綠;`LayoutEscapeTest` 的佈局清單寫死四份,12 份裡有 8 份從沒被檢查過,
而那幾份都真的有死路。**寫測試時自問:它會不會在該紅的時候安靜地不跑?**
並實際植入一個違規驗證它會紅。

**小心「看得到但摸不到」的功能。** 已經抓到四個:重輸鍵呼叫的是結束組字而不是清空、
中英鍵切了模式卻不換佈局、按下後顏色回不來、工具列的 emoji 鍵什麼都不做。
共同點是**畫面完全正常、自動化全過**。驗收要問「每顆鍵是不是都真的做了它宣稱的事」,
不能只問「打不打得出字」。

**`adb shell input tap` 這類注入可能太快。** 變灰那個 bug 用 tap 重現不出來,要用
`input swipe` 模擬按住 100ms 以上。**「模擬器測不出來」的結論都要重新懷疑一次。**

**離線定位是硬約束。** 單一連網出口、連網紀錄、開關預設關閉,而且**不得宣稱做不到
的事**(不能說「本 app 沒有網路權限」—— 那是假的)。第三方方案有程式碼執行能力,
移植 librime-lua 時**必須套用 `patches/librime-lua@sandbox.patch`**。

---

## 4. 共用層的既知契約(細節見 `core/include/rime_shell.h` 檔頭)

- **「選字」不等於「上屏」**。判別條件是 `menu.count`:`> 0` 還有段落待選不可 commit;
  `== 0 && is_composing` 才呼叫 `rs_commit_composition()`。
- **commit 在 `rs_snapshot_acquire()` 當下就被消費**,不是 release。
  紀律:**每個輸入事件只 acquire 一次**。
- **`rs_deploy_callback` 不在呼叫端的執行緒上**,不可在回呼裡直接碰 UI。
- **部署失敗時 `rs_last_error()` 是空字串**,librime 不提供原因,要自己預檢。
- **modifier 遮罩**:`kSuperMask` 是 `1 << 26` 不是 `1 << 6`。查 librime 的
  `src/rime/key_table.h`,不要憑印象。

---

## 5. 待裁決 / 跨端事項

> 格式:`[日期] [提出者] 事項 → 狀態`

- `[2026-08-08] [協調] alpha_layer / input_mode:toggle / label_from: input_mode_pair 三個擴充已採用並實作,但還沒寫進 docs/theme-format.md → 待 macOS 端動規範時一併寫入(語義見 task #43)`
- `[2026-08-08] [協調] 候選窗的多欄/表格排版、狀態列外觀,規範 §11 自承未定義 → macOS 端撞到時擴充,Windows 繼承`
- `[2026-08-08] [協調] 方案 id 不是全域唯一(double_pinyin 同時存在於兩個套件,字集相反)→ 任何拿 schema id 當唯一鍵的地方都要重新檢查`
- `[2026-08-08] [macOS] 改了共用的 scripts/collect_data.sh(原屬協調端)→ 已完成,經使用者指示。詞庫檢查的抽取器 grep -oP '...\K...' 是 GNU/PCRE 專屬,BSD grep(macOS)直接 exit 2 拒絕,而 2>/dev/null + || true + process substitution 三層消音把證據抹掉,於是它在 macOS/BSD 上**每次都印「所有 schema 引用的詞庫都齊全」而完全沒有檢查**(已在真 macOS 上以兩本刻意缺席的詞庫重現)。改用 awk,並加上「抽到 0 個引用就 die」。⚠ Windows 端也跑這支,行為改變請知悉:產出資料不變,只是檢查真的會檢查了。注意 \K 是 PCRE 專屬,只拿掉 -P 不能修。`
- `[2026-08-08] [macOS] Android 的 CI 目前是紅的,且與 macOS 無關:build.yml 的「建 host 版 opencc」步驟需要 third_party/librime/deps/opencc 的**原始碼**,但 third_party/librime/ 是 gitignore 的,該 workflow 又沒有任何步驟去取得它 → CMake Error: source directory does not exist。在我第一個 commit 之前就已經是紅的(run #4)。Android 端請自行確認,我沒有動 android/。`
- `[2026-08-08] [macOS] CMake 4 的已知問題只中了一半:Android 的 build_native.sh 註記「CMake 4 移除 FindBoost **且** 對舊的 cmake_minimum_required 下限更嚴」。macOS runner 是 cmake 4.4.0,實測只有前者成立 —— 補一份 25 行的 header-only FindBoost 墊片(apple/scripts/build_macos.sh 的 create_findboost_shim)就過了,下限問題在這組釘死的依賴版本上沒有重現。Windows 端不必為此釘 CMake 3.x。`

- `[2026-08-08] [Windows] 候選窗規範的缺口,實作桌面候選窗時撞到的。我沒有改 docs/theme-format.md（規範所有權在 macOS 端），以下照 §2 回報：`
  1. **`§8.6.7 max_width` 的溢出行為寫的是「換行／截斷,由實作決定」。**
     那正好是最需要一致的地方。Windows 目前的行為是**橫排時截斷**(並記錄
     被丟掉幾個候選)、**直排時不截字讓窗變寬**;而且第一個候選一定放進去,
     就算它自己就超過 max_width —— 給一個空的候選窗比太寬更難理解。
     兩端各自決定的話,同一份主題在 macOS 與 Windows 上會看到不同數量的候選。
  2. **標籤↔候選文字↔註解之間的間距沒有欄位。** `§8.6.4` 只有 item 之間的
     `spacing`,item **內部**三段之間沒有。Windows 暫用 `metrics.spacing`。
  3. **`§8.6.7 follow_caret: false` 說「固定在螢幕角落」,沒說哪一個角。**
     Windows 暫取右下。
  4. **桌面候選窗的字型家族沒有欄位。** `§8.6.1–8.6.3` 只有 `size` 與 `color`。
     Windows 暫用系統 UI 字型(`SPI_GETNONCLIENTMETRICS`),只套規範裡有的字級。
  5. **`§8.6.7` 的 `backdrop` / `opacity` / `shadow.*` 在 Windows 上要用
     分層視窗才做得到,而規範沒有說不支援時該怎麼退化**(只有 `backdrop`
     那一列寫了「必須靜默退化為 none」)。Windows 目前這三項都還沒實作。
  6. `§11` 已列的多欄/表格排版與狀態列外觀,Windows 端確認同樣撞到:
     中/英、簡/繁的狀態指示目前**完全沒有畫**,因為沒有規範可依。
  `→ 待 macOS 端動規範時一併裁決。在那之前 Windows 端刻意只用規範已寫下的欄位、
     取規範預設值、不讀主題檔 —— 做滿等於自己發明一套。`
- `[2026-08-08] [Windows] 產品決定待確認:TSF 的輸入法設定檔目前註冊在 langid 0x0404(zh-Hant-TW)底下,因為內建方案是 luna_pinyin_tw / bopomofo_tw。要不要另外註冊一份 0x0804(zh-Hans)是產品決定,不是技術限制 —— 多一份 profile 就多一個 GUID。→ 待裁決`
- `[2026-08-08] [Windows] 只動了 windows/ 與 .github/workflows/windows.yml。workflow 的 on.push.branches 加了 windows 分支(原本只有 main,推到自己的分支不會跑 CI)。沒有動 core/、docs/theme-format.md、android/、apple/。`
- `[2026-08-08] [Windows] 給其餘三端參考:core/include/rime_shell.h 的 rs_modifier 與 librime 的遮罩是兩套東西(kSuperMask 1<<26 是後者,由 core/src/rime_shell.cc 的 to_rime_mask 轉換)。Windows 端在 windows/tests/test_keymap.cc 放了一條斷言把自己那份重寫的 Mod 位元與 rime_shell.h 的 RS_MOD_* 逐位釘在一起,順便釘住 RIME_SHELL_ABI_VERSION == 1。core/ 那邊若要動 ABI,這條會紅。`
- `[2026-08-08] [Windows] ⚠ 給其餘三端(尤其是 macOS/iOS 會做常駐進程的):**進入點若是寬字元版本,glog 會在初始化時空指標解參考。** glog 的 ProgramInvocationShortName() 在 Windows/MSVC 上走 const_basename(__argv[0])(deps/glog/src/utilities.cc 的 HAVE___ARGV 分支),而 __argv 只有在 CRT 以窄字元進入點啟動時才會被填 —— 用 wmain 的話 CRT 只填 __wargv,__argv 是 NULL。症狀是「一啟動就 0xC0000005,堆疊在 glog 深處」,與進入點看起來毫無關聯,花了五輪 CI 才查到(run #16–#20)。tools/rime_console.cc 用的是 main,所以它一直是綠的。Apple 端不走這條路徑,但**任何新的、會連結 librime 的執行檔都要注意進入點**,而且不要相信「rime_console 過了所以引擎沒問題」——引擎沒問題,是宿主的進入點有問題。`
- `[2026-08-08] [Windows] 驗證用的使用者目錄要明確指定方案。librime 把「上次選的方案」記在 <user>/user.yaml。Windows 的 verify_ime.sh 沿用 verify_console.sh 編好的使用者目錄以省下詞庫編譯時間,結果拿到的是上一支腳本最後選的注音,nihao 被打成「所噢草莓」。四端的驗證腳本若有共用使用者目錄的,同樣要明著選方案 ——「不指定」不是中性的。`

---

## 6. 各端狀態

> 自己更新自己那一行。

- **Android** — 可用的產品。拼音/注音/九宮格、鍵盤與主題由 YAML 驅動、鍵盤類型選單、
  自定義鍵位、方案市集(34 個)、離線開關與連網紀錄、應用內升級與金鑰輪替、
  介面在地化(英/繁/簡)。354 項單元測試、16 項發布關卡。
- **macOS** — 核心層已綠。`macos-latest`(macos-26-arm64 / cmake 4.4.0)上從原始碼建
  librime 1.17.0 + 5 個依賴 + librime-lua(已套沙盒 patch),`tools/rime_console.cc`
  斷言 `nihao → 你好`(luna_pinyin_tw)與 `su3cl3 → 你好`(bopomofo_tw)兩組。
  斷言錨定 `^>>> COMMIT: "…"$` 完全相等並要求結束碼 0 —— 未錨定會被 dump() 印的
  中途 commit 騙過去;另有一個**反向測試**步驟故意餵錯的預期值,斷言不判失敗就讓 CI 紅。
  另驗四個方案都部署成功、執行期資料齊全。核心產物打包上傳(apple/scripts/package_core.sh)。
  **尚未開始:IMKit、候選窗、Swift 綁定 —— 下一輪。CI 沒有圖形工作階段,驗不了 UI。**
- **Windows** — 核心層已綠,TSF 輸入法已寫出但**沒有人在真 Windows 上用過**。
  瘦 DLL(`rime_tsf.dll`,只做 TSF 協議 + 按鍵映射 + IPC,不含 librime)
  加獨立服務進程(`rime_service.exe`,rime_shell + librime + 候選窗),
  兩者以具名管道通訊(DACL 只授權目前使用者的 SID)。
  按鍵映射不用常數表 —— 會產生字元的鍵一律問 `ToUnicodeEx(..., hkl)`,
  並以**真實**的德文/法文佈局在 CI 上驗證(`LoadKeyboardLayout`)。
  CI 兩個 job:`logic-x64`(不需 librime,約 3 分鐘)與 `core-x64`,run #21 全綠。
  驗到的:編得起來、四個 COM 匯出正好那四個、`rime_tsf.dll` 的相依正好是
  kernel32/user32/advapi32/ole32(守 `/MT`,沒有任何 CRT DLL)、
  58 個單元測試 815 個斷言 + 反向測試、既有的 `rime_console` 核心驗證不回歸、
  以及**經由真的具名管道**驅動服務以 luna_pinyin_tw 打出「你好」。
  **驗不到的(清單見 `windows/README.md`「沒有被驗證的部分」):**
  regsvr32 註冊、TSF 組字、候選窗的樣子、在記事本/瀏覽器裡真的打得出字。
  **CI 綠不等於能用 —— 需要有人在真 Windows 上跑一遍。**
  已知缺口:只有 x64、沒有修飾鍵事件(TSF 不給)、沒有顯示屬性、
  沒有系統匣與安裝程式、沒有 librime-lua。
- **iOS** — 未開始。
