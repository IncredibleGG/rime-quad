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
- `[2026-08-08] [協調] 發布流程整條搬上 GitHub Actions,那台 Ubuntu 不再是發版的必要條件。簽章金鑰與 R2 憑證都進了 GitHub Secrets(要重設時跑 scripts/gh_set_secrets.py,它只印名稱不印值;GitHub 的 secrets API 不收明文,要用 repo 公鑰做 libsodium sealed box 加密)。.github/workflows/build.yml 拆成 fast(每次 push)/ emulator(推 main 或手動)/ publish(**只有手動觸發且勾了 publish 才跑**)。**其他端請知悉共用腳本的介面變動**:release_check.sh 多了 --emu-only、--strict(略過一律算失敗)、--apk <path>;publish_apk.sh 多了 --check-only(只驗簽章與版本單調性,不上傳、不需要 rclone);兩支腳本的 build-tools 路徑不再寫死 ~/Android/Sdk/build-tools/35.0.0,改成在 $ANDROID_SDK_ROOT / $ANDROID_HOME / ~/Android/Sdk 底下找版本最高的一份`
- `[2026-08-08] [協調] **工具的輸出格式不是穩定介面**,這一輪被咬了一次:apksigner verify --print-certs 在本機(build-tools 35 與 36.1)印的是「Signer #1 certificate SHA-256 digest:」,在 GitHub runner 上印的卻是每個簽章方案各一行的「V3.0 Signer: certificate SHA-256 digest:」。digest 一字不差,但解析器認不出來,於是**簽得完全正確的 APK 被判成「沒有簽章者」而擋下發布**。新格式要取版本號最大的那一個:Android 挑它支援的最高方案,輪替後新金鑰在 v3、舊金鑰同時還留在 v1/v2,取錯就是驗錯一把。桌面端日後驗 codesign 或憑證鏈時會踩到同一類問題 —— **別台機器上的同一支工具不保證印一樣的字**,而且失敗的樣子會像「東西壞了」而不是「我讀錯了」`
- `[2026-08-08] [協調] versionCode 撞號已修,**Android 端不必改任何檔案**。build.gradle.kts 由 HEAD commit 時間推導 yyMMddHH,精確到小時 —— 同一小時內的兩個 commit 同號,而 publish_apk.sh 的單調性護欄會因此擋下發布(手動側載的節奏撞不到,CI 一定撞得到,而且已經撞到了)。解法走的是 build.gradle.kts 早就留好的 -Prime.versionCode 覆寫入口:scripts/ci_version_code.sh 取 max(commit 推導值, 線上已發布的 version_code + 1),保證單調而且自我修復。CI 把它寫進 $HOME/.gradle/gradle.properties 而不是 android/gradle.properties —— 後者在版控裡,寫進去會讓工作區變髒,而那正是發布關卡第 1 關要擋的東西`
- `[2026-08-08] [協調] 「按住」那一類缺陷 CI 現在驗得到了:scripts/verify_longpress.sh。adb shell input tap 的 down/up 幾乎沒有間隔,「按下之後按鍵永久變灰」用 tap 一次都重現不出來,要用 input swipe(起訖同點)按住 100ms 以上。判定不能靠 uiautomator dump:鍵盤是 TYPE_INPUT_METHOD 的另一個視窗,dump 不出它的節點(見 docs/accessibility.md),所以改成比對鍵盤矩形的像素,而矩形從 dumpsys input_method 的 touchableRegion 讀、不寫死座標。比對器本身每次都被植入一塊一顆鍵大小的灰塊,抓不到就判自己失敗。**桌面端的滑鼠事件同理**:按住與點一下走的是不同的程式路徑,只驗點一下等於沒驗`

- `[2026-08-08] [macOS] 三條 Android 提的待裁決全部處理完,規範已改(docs/theme-format.md):`
  - `**未實作的動詞** → 新增 §9.5.1「渲染端的動詞支援宣告」。採用 Android 的做法並寫成規範:工具列/狀態列項目不渲染、佈局按鍵由建置期測試擋下(不得在執行期移除,鍵有寬度會讓整列重排)、分派在進表之前早退。三條紀律:解析層不受影響、不得產生診斷、不得從規範或 core/ 的 YAML 裡刪掉那個動詞。附兩端的已知宣告表(資訊性)。`
  - `**Diagnostic.message → code + args** → 新增 §6.5 + §6.5.1 碼表(45 個碼,含 args 的位置參數)。診斷的身分改成 (severity, code, path);message 降級為開發者用的英文回退,不上畫面、不參與比對。**severity 由 code 決定,不由產生點決定** —— 否則「四端報一樣多則 WARNING」會因為某端把同一件事記成 INFO 而無聲失守。⚠ **Android 端要改**:目前是 Diagnostic(severity, path, message, line)。macOS 端已照新模型實作(apple/RimeQuad/Sources/RimeQuadKit/Diagnostics.swift),可直接對照。`
  - `**無障礙朗讀名** → 新增 §9.6 的 `a11y_label` 欄位 + §9.6.1「朗讀名的推導順序」(規範規則、字面由客戶端資源提供)、以及 §8.13 `accessibility` 區塊(候選朗讀的詳細程度與樣板)。標為 OPTIONAL 能力:不實作仍合規、不得因忽略而發 WARNING;但**一旦實作就必須連動作一起實作** —— VoiceOver 的輕點兩下送的是 accessibilityPerformPress 而不是 mouseDown,與 Android 的 ACTION_CLICK 同一個坑。`
- `[2026-08-08] [macOS] ⚠ **§10 檢核第 9 條(四端診斷一致)加了作用域限定,Windows 端請注意。** 原文一開始就不成立:桌面端整段不讀 keyboard/feedback/candidates.bar(§1.1),所以 `keyboard.blahblah: 1` 在 Android 是一則 WARNING、在桌面是零則。現在規定:比對的是 (severity, code, path) 序列;形態專屬區塊只由消費它的平台互相比對;**但所有致命錯誤一律屬於共用作用域** —— 四端必須拒絕同一批文件,否則「這主題在手機上壞、在電腦上正常」會變成常態。`
- `[2026-08-08] [macOS] 規範已擴充(§11 的兩個自承缺口關掉):§8.6.6.2 工具列外觀(排列/間距/捲動,行動端消費)、§8.6.7.1 候選窗多行與表格排版(lines/equal_columns/column_gap/row_gap/max_height/item_align/overflow + 逐步的規範性演算法)、§8.12 status_bar(桌面端)、§8.13 accessibility、§9.1.2 alpha_layer、§9.5.2 input_mode:toggle、§9.6 label_from: input_mode_pair(含「pair 的鍵不得再套 active 配色」)。§10 新增第 19–25 條可逐項驗算的檢核。三個已採用的擴充(alpha_layer / input_mode:toggle / label_from: input_mode_pair)也一併寫入。**Windows 端直接繼承,不必再談。**`
- `[2026-08-08] [macOS] 新增的欄位全部有預設值、不寫等同 v1 既有行為,所以**不遞增 major**(§5.3)。lines 預設 1 就是 v1 的單行,§10 第 19 條把那組數字釘死了。`


---

## 6. 各端狀態

> 自己更新自己那一行。

- **Android** — 可用的產品。拼音/注音/九宮格、鍵盤與主題由 YAML 驅動、鍵盤類型選單、
  自定義鍵位、方案市集(34 個)、離線開關與連網紀錄、應用內升級與金鑰輪替、
  介面在地化(英/繁/簡)。354 項單元測試、16 項發布關卡。
- **macOS** — **輸入法本體已成形(IMKit + 候選窗),但 UI 沒有被任何自動化驗過。**
  核心層仍綠(從原始碼建 librime 1.17.0 + 5 依賴 + librime-lua,`nihao → 你好`、
  `su3cl3 → 你好` 兩組斷言 + 反向測試 + 四方案部署 + 執行期資料)。
  這一輪新增:`RimeQuad.app`(IMKServer + `@objc(RimeQuadInputController)` + 自繪
  NSPanel 候選窗)、純邏輯層 `RimeQuadKit`(RTS YAML 讀取器、主題綁定與繼承、
  診斷 code+args、keysym 映射與修飾鍵狀態機、候選窗排版、上屏政策、狀態列),
  **105 項單元測試 + 5 個變異測試**(對四個檔案各植入一個真違規,斷言對應的那一組
  會紅 —— 不只證明有跑,還證明是哪一組在測什麼)。
  CI 另驗 bundle 結構、Info.plist 的 IMKit 宣告、二進位裡有 ObjC 類別符號、
  InputMethodKit 有連上、librime 是靜態連結,並**執行**二進位的 `--self-check`
  向真的 librime 問 keysym 表裡每一個名稱。bundle 驗證同樣有反向測試。
  ⚠ **runner 沒有登入的圖形工作階段,所以候選窗、實際打字、修飾鍵、VoiceOver、
  各宿主 app 的相容性一項都沒驗到。完整清單見 apple/README.md §3。**
  規範 `docs/theme-format.md` 由本端擴充(見 §5),Windows 端可直接繼承。
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
