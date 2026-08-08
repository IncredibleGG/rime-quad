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
- `[2026-08-08] [Android] 未實作的動詞不該出現在畫面上。emoji 是這一類:ActionVerb.EMOJI 在 Android 只有一行 log,而 §8.6.6.1 的規範性預設工具列把 emoji 列為預設項,於是 12 份主題全部長出一顆按了沒反應的鍵 → Android 的做法是不刪規範也不刪 YAML,改由渲染端宣告「本端不支援哪些 verb」:工具列項目不渲染,佈局按鍵改由建置期測試擋下。桌面端哪天實作了表情面板,它自己會回來。請 macOS 動 §9.5 時把這條寫進規範`
- `[2026-08-08] [Android] Diagnostic.message 目前是自由文字,要上畫面就得在地化,但 §6.2 要求「同一份壞檔案四端報一樣多則」,翻譯過的字面沒辦法拿來比對 → Android 提議改成穩定的 code + args(message 降級為開發者用的英文回退),UI 端才查表成當地語言。這會動到 §6.2 的診斷模型,桌面端還沒開始實作,現在改代價最低`
- `[2026-08-08] [Android] 無障礙朗讀名稱目前只能由 icon／verb／keysym 反推,CJK 佈局上「々」這種鍵反推不出合理讀法 → Android 先做本端對照表、不動格式；但格式長期可能需要一個選用的 per-key 朗讀名欄位,留給 macOS 撞到候選窗無障礙時一起定`

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
- **Windows** — 核心層已綠。`windows-latest`(windows-2025-vs2026 / MSVC 14.51)上以
  MSVC 從原始碼建 librime 1.17.0 + 5 個依賴,`tools/rime_console.cc` 斷言
  `nihao → 你好`(luna_pinyin_tw)與 `su3cl3 → 你好`(bopomofo_tw)兩組;
  比對錨定 `^>>> COMMIT: ` 的最後一行且完全相等(只 grep「你好」會放過「你好嗎」),
  並先 `tr -d '\r'` —— MSVC 的 CRT 寫的是 CRLF,留著 CR 會讓比對失敗成
  「你好 != 你好」。斷言以竄改過的日誌反向測過五種失敗都會紅。
  另斷言 keysym 正反查(`BackSpace → 0x00FF08`、未知鍵名 → 0)—— `rime_shell.cc`
  重宣告的那兩個私有符號靠 C++ mangling 對上,連得起來不等於接到對的函式。
  **產生器用 Ninja + vcvars,不要換回 Visual Studio 產生器**:VS 產生器的名字帶著
  VS 版本號,會把 CMake 版本與 runner 的 VS 版本綁死,第一版就是這樣掛掉的。
  **尚未開始:TSF、COM、候選窗、按鍵映射 —— 下一輪。** 另外 Windows 端目前
  **沒有編 librime-lua**,倚賴 lua_translator/lua_filter 的第三方方案會部署成功
  卻沒有候選;`windows/build.sh` 有一道守門,日後掛上 lua 而沒帶 sandbox patch 會擋下建置。
  只做 x64,arm64 未做。
- **iOS** — 未開始。
