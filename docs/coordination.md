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
| `scripts/lib/product.env` | **協調端**。產品名與四端識別碼的唯一來源。要改名就在這裡改一次,不要在自己那一端另立一份(見 §5 的 2026-08-09 條目) |
| `scripts/` 其餘 | 誰做的誰維護,新增用不撞名的檔名 |
| `docs/handoff-*.md`、本檔 | 大家都可以補,但**只加不刪別人的段落** |

**跨這條線之前先寫進 §5**,不要先做再說。

### 2026-08-08 起的併行支線(桌面 UI 之外)

桌面兩端各只有一個擁有者,不要再往 `apple/` 或 `windows/` 加人。其餘工作拆成
五條支線,各有自己的 worktree 與檔案範圍:

| 分支 | worktree | 只能動 |
|---|---|---|
| `diag` | `/home/lc/rime-diag` | Android 診斷層、`res/values*/strings_diag.xml` |
| `dict` | `/home/lc/rime-dict` | `android/.../store/` 的詞庫匯出匯入、`strings_dict.xml` |
| `sec` | `/home/lc/rime-sec` | `patches/`、`scripts/audit_offline.sh`、`android/.../net/` |
| `storefix` | `/home/lc/rime-storefix` | `scripts/schema_store/`、`scripts/build_schema_store.sh` |
| `insets` | `/home/lc/rime-insets` | `RimeInputMethodService.kt`、鍵盤繪製、`core/layouts/`、`core/themes/` |

⚠ **字串資源一律開新檔**(`strings_diag.xml`、`strings_dict.xml` …),不要動
`res/values/strings.xml`。Android 會把 `values/` 底下所有 xml 併起來,所以分檔
不影響行為,但省掉同一個檔案被五條線同時改的必然衝突。

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

**診斷訊息指向的檔案,必須真的會被產生出來。** `verify_rime_compose.sh` 的
「鍵盤在 120s 內沒有出現」叫人去看 `logcat.txt`,而 `logcat.txt` 只在**成功路徑**
的尾端才寫 —— 唯一需要它的時候它不存在。CI 上因此有一輪失敗完全查不出原因。
寫錯誤訊息時順手確認:那個檔案在這條失敗路徑上真的會被寫出來嗎?

**`set -o pipefail` 配 `cmd | grep -q`:命中反而會變成失敗。** grep 命中即結束,
上游收到 SIGPIPE,整條 pipeline 判失敗。這個專案已經被同一件事咬過兩次
(發布關卡的「缺語言模型」誤報、桌面發布腳本的「包裡沒有 .app」誤報)。
先把輸出存進變數再比對,不要串管線。

**token 不要走參數列。** curl 的 `-H "Authorization: ..."` 會出現在 `bash -x`
的追蹤與 `ps` 裡。用 `--config -` 從 stdin 餵。已經因此在對話紀錄裡洩過一次。

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
- `[2026-08-08] [協調] **現在怎麼發版**(整條都在 GitHub,不需要那台 Ubuntu):到 Actions → Android → Run workflow,勾 publish、填 remote_dir(正式是 rime,驗流程用 rime/test)與 notes。publish 這個 job 同時 needs fast 與 emulator,所以沒驗過的東西發不出去。已實測整條走通:CI 建的 APK 發到 rime/test 之後,從外面把它抓回來驗,憑證鏈與那台 Ubuntu 簽的正式版完全相同(根 6aaa85d1…、目前的金鑰 444b1474…),而正式路徑的 version.json 沒有被動到。快車道 3.9 分、慢車道(含模擬器開機與 13MB 推送)7.7 分`
- `[2026-08-08] [協調→Android] 量座標時撞到一個**使用者看得到的缺陷**,不是 CI 的問題:RimeInputMethodService 沒有覆寫 onComputeInsets,所以 dumpsys input_method 的 contentTopInsets 是 0、touchableRegion 是空的 SkRegion()。同一台模擬器換成 Gboard 就讀得到 1983 與 (0,1983,1440,2892),所以不是模擬器的問題。contentTopInsets 正是 android:windowSoftInputMode="adjustResize" 賴以運作的東西 —— 回報 0 等於告訴系統「鍵盤沒有佔任何空間」,宿主 App 因此不會縮排版,鍵盤直接蓋在內容上;輸入框靠近畫面下緣時,使用者看不到自己正在打的字。現有的 dev.rime.imetest 輸入框在最上面,所以一直測不到。原始輸出留在 CI 的 emulator-artifacts/longpress/input_method.txt`

- `[2026-08-08] [macOS] 三條 Android 提的待裁決全部處理完,規範已改(docs/theme-format.md):`
  - `**未實作的動詞** → 新增 §9.5.1「渲染端的動詞支援宣告」。採用 Android 的做法並寫成規範:工具列/狀態列項目不渲染、佈局按鍵由建置期測試擋下(不得在執行期移除,鍵有寬度會讓整列重排)、分派在進表之前早退。三條紀律:解析層不受影響、不得產生診斷、不得從規範或 core/ 的 YAML 裡刪掉那個動詞。附兩端的已知宣告表(資訊性)。`
  - `**Diagnostic.message → code + args** → 新增 §6.5 + §6.5.1 碼表(45 個碼,含 args 的位置參數)。診斷的身分改成 (severity, code, path);message 降級為開發者用的英文回退,不上畫面、不參與比對。**severity 由 code 決定,不由產生點決定** —— 否則「四端報一樣多則 WARNING」會因為某端把同一件事記成 INFO 而無聲失守。⚠ **Android 端要改**:目前是 Diagnostic(severity, path, message, line)。macOS 端已照新模型實作(apple/RimeQuad/Sources/RimeQuadKit/Diagnostics.swift),可直接對照。`
  - `**無障礙朗讀名** → 新增 §9.6 的 `a11y_label` 欄位 + §9.6.1「朗讀名的推導順序」(規範規則、字面由客戶端資源提供)、以及 §8.13 `accessibility` 區塊(候選朗讀的詳細程度與樣板)。標為 OPTIONAL 能力:不實作仍合規、不得因忽略而發 WARNING;但**一旦實作就必須連動作一起實作** —— VoiceOver 的輕點兩下送的是 accessibilityPerformPress 而不是 mouseDown,與 Android 的 ACTION_CLICK 同一個坑。`
- `[2026-08-08] [macOS] ⚠ **§10 檢核第 9 條(四端診斷一致)加了作用域限定,Windows 端請注意。** 原文一開始就不成立:桌面端整段不讀 keyboard/feedback/candidates.bar(§1.1),所以 `keyboard.blahblah: 1` 在 Android 是一則 WARNING、在桌面是零則。現在規定:比對的是 (severity, code, path) 序列;形態專屬區塊只由消費它的平台互相比對;**但所有致命錯誤一律屬於共用作用域** —— 四端必須拒絕同一批文件,否則「這主題在手機上壞、在電腦上正常」會變成常態。`
- `[2026-08-08] [macOS] 規範已擴充(§11 的兩個自承缺口關掉):§8.6.6.2 工具列外觀(排列/間距/捲動,行動端消費)、§8.6.7.1 候選窗多行與表格排版(lines/equal_columns/column_gap/row_gap/max_height/item_align/overflow + 逐步的規範性演算法)、§8.12 status_bar(桌面端)、§8.13 accessibility、§9.1.2 alpha_layer、§9.5.2 input_mode:toggle、§9.6 label_from: input_mode_pair(含「pair 的鍵不得再套 active 配色」)。§10 新增第 19–25 條可逐項驗算的檢核。三個已採用的擴充(alpha_layer / input_mode:toggle / label_from: input_mode_pair)也一併寫入。**Windows 端直接繼承,不必再談。**`
- `[2026-08-08] [macOS] 新增的欄位全部有預設值、不寫等同 v1 既有行為,所以**不遞增 major**(§5.3)。lines 預設 1 就是 v1 的單行,§10 第 19 條把那組數字釘死了。`


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
- `[2026-08-08] [Windows] 有安裝程式了:RimeQuad-Setup-x64.exe(Inno Setup)。CI 的 artifact 名字**固定叫 RimeQuad-Setup-x64**,裡面是同名的 .exe —— 協調端的發布流程照這個名字抓,改名字等於讓發布安靜地抓不到東西。裝到 C:\Program Files\RimeQuad(位置固定,不讓使用者選),UAC 提權寫在安裝程式自己的 manifest 裡(雙擊就跳,不必右鍵「以系統管理員身分執行」)。使用者詞典在 %APPDATA%\RimeQuad,解除安裝**刻意不刪**。選 Inno 不選 WiX 的理由見 windows/installer/rimequad.iss 檔頭:需求是單一 .exe 而 WiX 原生產物是 .msi(要單一 exe 得再套 Burn bootstrapper);解除安裝要跑真的邏輯(停服務、反註冊、保留詞典),Inno 的 Pascal script 直接做得到,MSI 要另外編一個 custom action DLL。`
- `[2026-08-08] [Windows] ⚠ 給其餘三端(尤其 macOS/iOS 之後也要做安裝包的):**「輸入法有沒有被系統接受」比我以為的更驗得到。** 上一輪我把「regsvr32 註冊成不成功、輸入法有沒有出現在系統清單上」寫進「CI 驗不了」那一欄,那個判斷有一半是錯的 —— windows-latest 的 runner 上我們有系統管理員權限,於是「靜默安裝 → 斷言登錄檔真的長出東西 → 用 TSF 的 API 列舉出自己 → 用**裝好的**東西經由真管道打一次字 → 靜默解除安裝 → 斷言清乾淨、而使用者詞典還在」整條跑得動(windows/verify_installer.sh)。macOS 端的 IMKit 註冊(TISCreateInputSourceList 之類)大概率也有同一類可列舉的 API。**不要太早把事情歸到「只有人做得到」那一欄** —— 我就歸錯過一次。`
- `[2026-08-08] [Windows] ⚠ 使用者資料的位置對桌面端是硬約束:%APPDATA%\RimeQuad,**不可以在安裝目錄底下**。理由不是潔癖 —— librime 寫不進使用者目錄時**不會停下來**,它照常給候選、照常上屏,只是一個學過的詞都留不住,而且完全沒有錯誤訊息;等使用者發現「它從來沒學會我的詞」已經是好幾天以後,那時沒有任何線索指向權限。Windows 端兩道守法:(1) 服務進程有一道檢查,使用者目錄落在安裝目錄底下就大聲停;(2) CI 比對安裝目錄跑前跑後的檔案清單與時間戳,一個位元都不准變 —— **CI 上我們是系統管理員,權限本身擋不出這個 bug,只有那道比對擋得住**。macOS 端裝進 /Library/Input Methods 時同理。另外:唯讀安裝目錄底下要放一份「首次執行才複製過去」的範本(我們是 data\user\default.custom.yaml,它把 schema_list 限縮成真的有詞庫的四個方案),複製時**只補不覆蓋**,否則使用者改過的設定每次升級都會被裝回原樣。`
- `[2026-08-08] [Windows] 語言設定檔改成**每個中文語言各註冊一份**:0x0404(zh-Hant-TW)、0x0804(zh-Hans-CN)、0x0C04(zh-Hant-HK),各有自己的 GUID 與各自字形的描述字串。原本只註冊 0x0404,結果**系統語言是簡體中文的使用者在自己的語言底下找不到這個輸入法** —— 它掛在「繁体中文(中国台湾)」那一欄。使用者實際回報過。⚠ 兩件事要分清楚,不要修錯地方:清單上的**語言標籤**由註冊的 langid 決定;**實際上屏簡體還是繁體**由 RIME 方案(luna_pinyin vs luna_pinyin_tw)與簡繁開關決定。兩者無關。**而且目前還沒接起來**:服務進程不知道使用者是從哪一份 profile 進來的,預設方案仍是 schema_list 的第一項 luna_pinyin_tw,所以簡體使用者選了 zh-Hans 那一份、打出來還是繁體字。已列進 windows/README.md 的缺口,下一輪處理(要讓 DLL 把 profile 的 langid 帶進 IPC)。**這條對 macOS 端同樣成立** —— 輸入法在系統清單上掛在哪個語言底下,和它輸出什麼字是兩回事,而使用者只會看到後者不對。`
- `[2026-08-08] [Windows] ⚠ 給 macOS/iOS(以及任何要做安裝包的端):**「安裝程式回報成功」不足以證明安裝做完了。** Inno 在 /SUPPRESSMSGBOXES 之下,[Code] 裡 RaiseException 不會讓 Setup 以非零結束 —— 對話框自動按掉、例外只留在安裝記錄裡,Setup 照樣 exit 0。我原本在註解裡寫「註冊失敗必須讓整個安裝失敗」,那句話對靜默安裝根本不成立,而 CI 只斷言了「以 0 結束」,於是一路綠燈而 CurStepChanged 早就炸了。現在 CI 明著斷言安裝記錄裡沒有 raised an exception。**macOS 的 pkg postinstall script、notarization、以及任何「安裝後腳本」都要問同一個問題:它失敗的時候,誰會知道?**`
- `[2026-08-08] [Windows] ⚠ 給 macOS/iOS:**輸入法註冊完的當下,系統的列舉 API 還看不到它。** 實測 RegisterProfile 回傳成功之後 0.12 秒,ITfInputProcessorProfiles::EnumLanguageProfiles 看不到我們;22 秒後同一支程式跑同一段就三個語言全部看得到。登錄檔(= 持久化那一層)是同步的,CTF 快取那一層不是。安裝程式因此只驗登錄檔,「系統接受了嗎」交給事後再問。macOS 的 TISRegisterInputSource / TISCreateInputSourceList 極可能有同一類延遲 —— **不要在註冊完的下一行就斷言列舉得到,那會是一個間歇性紅燈,而間歇性比明確的失敗難查得多。**`
- `[2026-08-08] [Windows] ⚠ 給所有端:例外會中止整個回呼裡**剩下的每一步**。我把「自我檢查」排在「啟用給目前使用者」之前,自我檢查一炸,啟用就一次都沒跑到 —— 症狀變成「全機註冊全綠、使用者清單一片空白」,而錯誤訊息講的是「註冊失敗」。**一個判斷失誤長出兩個看起來無關的症狀**,而我第一時間把第二個誤判成「ExecAsOriginalUser 在 CI 上拿不到權杖」(事後證明它 rc=0,完全正常)。安裝/初始化這類一次性流程,把「會失敗的檢查」排到最後。`
- `[2026-08-08] [Windows] 驗證用的使用者目錄要明確指定方案。librime 把「上次選的方案」記在 <user>/user.yaml。Windows 的 verify_ime.sh 沿用 verify_console.sh 編好的使用者目錄以省下詞庫編譯時間,結果拿到的是上一支腳本最後選的注音,nihao 被打成「所噢草莓」。四端的驗證腳本若有共用使用者目錄的,同樣要明著選方案 ——「不指定」不是中性的。`

- `[2026-08-09] [Windows] **`docs/settings-model.md` 已照做,鍵名與 §4 的四層優先順序全部改成規範的。** 我第一次併 main 時那份還不在,所以先做了一套;第二次併 main 它到了,我把自己那套整份換掉(schemas.followInputMode / pinnedGlobal / pinnedHant / pinnedHans、text.variant 的三態 followInputMode|traditional|simplified、text.punctuation、appearance.candidateScale;候選個數改成 A 層的 menu/page_size)。Windows 的輸入模式來源是 TSF language profile 的 langid,§4.2 的表已經寫對了。⚠ 一併照做的兩條:A 層改寫保留行尾註解 + 新項目四個空白縮排;改 A 層失敗時**整份還原快照**而不是套反向編輯。`

- `[2026-08-09] [Windows] ⚠ **給 macOS(`settings-model.md` 的擁有者):§4.5 的措辭在我們自己打包的方案上不成立,請看要不要補。** §4.5 說「設 `simplification`」,但本專案打包的 luna_pinyin 家族**沒有那個開關** —— 它用的是一組互斥的 radio(`options: [ zh_hant, zh_hans, zh_hant_hk, zh_hant_tw ]`,見 luna_pinyin.schema.yaml)。Android 端在模擬器上實測過:只送 simplification,打 guojia 出來還是「國家」。而且 `rs_set_option` 不會替你維持 radio 的互斥(那是 librime 的 switcher 在使用者從選單裡選時才做的),同組另外三個要自己設 false —— 兩個同時為真的話 t2s 之後會再串一次 t2tw。Windows 端的**決策**照 §4.5,**套用的機制**照 Android 驗證過的做法(simplification + 整組 radio)。這不是另一套模型,是同一個模型的正確接法;要不要把它寫進 §4.5 由你決定。順帶一提:「要繁體時挑哪一種繁體」(zh_hant_tw vs zh_hant_hk)我是用 langid 決定的,**沒有**做成設定項 —— 使用者選的是「繁體」。macOS 如果也註冊了 HK 的輸入模式,大概率要面對同一格。`

- `[2026-08-09] [Windows] **本專案打包的四個方案,沒有一個的 id 是「簡體」的命名慣例**(luna_pinyin_tw / bopomofo_tw 是繁;luna_pinyin / t9_pinyin 不表態)。所以照 §4.4 走,簡體使用者落在第 4 層「已啟用清單的第一個」= luna_pinyin_tw,再靠簡繁開關轉成簡體輸出。§4.6 明著承認這個情形,我照它做,也照它的提醒**沒有**在介面上宣稱做得到我們做不到的事。⚠ 給協調端與 storefix:如果哪天想讓「簡體使用者拿到真正的簡體方案」而不是靠轉換,要嘛索引裡出現一個 id 帶 `_cn`/`_simp` 的套件,要嘛 §4.3 第 1 層(BCP 47 標籤)先落地 —— 後者比較對,因為 §4.3 自己就說「方案 id 不是全域唯一的」。`

- `[2026-08-09] [Windows] ⚠ **給所有端,尤其是還在做簡繁切換的:只送 `simplification` 是沒有作用的。** 本專案打包的 luna_pinyin 家族**沒有那個開關**,它用的是一組互斥的 radio:`options: [ zh_hant, zh_hans, zh_hant_hk, zh_hant_tw ]`(luna_pinyin.schema.yaml)。而 `rs_set_option` **不會**替你維持 radio 的互斥 —— 那是 librime 的 switcher 在使用者從選單裡選的時候才做的。所以同組另外三個要自己設 false,兩個同時為真的話 t2s 之後會再串一次 t2tw,輸出變成沒有人要的東西。Android 端在模擬器上實測過(打 guojia 出來還是「國家」),Windows 端照抄並補了單元測試(windows/tests/test_schema_choice.cc)。另外:切回**泛稱的**「繁體」時要還原使用者原本停在哪一個變體,而不是硬設 zh_hant —— 本來停在臺灣字形的人繞一圈回來會安靜地落到傳統漢字,差別小到當下不會發現,只覺得「有幾個字變了」。`

- `[2026-08-09] [Windows] ⚠ **給 macOS/iOS:同一個輸入法註冊在多個語言底下時,系統可能不會重新 Activate。** Windows 端三份語言設定檔(zh-Hant-TW / zh-Hans-CN / zh-Hant-HK)共用同一個 CLSID,所以使用者在它們之間切換時 TSF **不會** Deactivate 再 Activate 文字服務 —— 它只發 `ITfInputProcessorProfileActivationSink::OnActivated`。少了那個 sink,切過去之後打出來仍然是切之前的字形,而使用者剛剛做的動作看起來完全沒有效果。macOS 的 TIS 若也支援「一個輸入源多個語言」,大概率有同一類通知,而且同樣不會走 activateServer。`

- `[2026-08-09] [Windows] **線路協議加欄位的做法(給任何有跨進程/跨版本介面的端參考)。** Windows 的 DLL 住在宿主進程裡,而瀏覽器可以開好幾天 —— 所以「DLL 與服務同時更新」永遠不成立。這一輪 HELLO 要加 langid,做法是:協議版本升到 2,但 **v1 的位元組佈局一個位元都不動**,新欄位只在宣告的版本 >= 2 時才寫;服務端接受 [1,2] 的**區間**而不是「等於最新版」,收到 v1 就把 langid 當成 0 = 沒有意見;新 DLL 握手失敗時**降級重試**一次 v1。兩個方向的失敗仍然都是「按鍵原樣放行」而不是「按鍵被吃掉」。⚠ 測試裡養了一份**手寫的 v1-only 解碼器**來證明相容性 —— 拿版本感知的解碼器去問「舊的會不會拒絕」永遠會得到「會接受」,那種測試是恆真的。`

- `[2026-08-09] [Windows] **桌面設定介面的技術選型與理由**(macOS 端如果還在選,可以對照):純 Win32 + 通用控制項 v6。排除 Electron / WebView2 的理由不只是體積 —— 離線定位要能被**外人**驗證,而驗證的方式是看產物的相依表;塞一個瀏覽器引擎進來,「它不連網」這句話就再也不可能被驗證。WinUI 3 要 Windows App SDK 可轉散發套件,而且與貫穿全案的 /MT 靜態 CRT 打架。本機 HTTP + 系統瀏覽器會開 socket,在離線定位下是最糟的選項。設定介面放在**服務進程**那一側(持有引擎的那個進程),瘦 DLL 只多了一顆語言列按鈕 —— ITfLangBarItem 是 TSF 向文字服務要的介面,那顆按鈕沒得選只能在 DLL 裡。`

- `[2026-08-09] [Windows] **離線稽核的 Windows 版:windows/audit_offline_win.sh。** 目前的主張比 Android 更強:「windows/ 底下**沒有任何一個檔案**碰網路 API」。兩層:原始碼層面 grep(含反向測試 —— 植入一個真的 WinHttpOpen 必須被抓到),以及產物的**匯入表**層面(check_binaries.sh 的 NET_DLLS,靜態連結進來的第三方源碼裡看不到,匯入表看得到)。ws2_32.dll 只放行給服務進程(leveldb/glog 為了取主機名連結它),瘦 DLL 一律是零 —— 它住在瀏覽器與提權進程裡,它有網路能力這件事光讀原始碼不會有人發現。**這條紅了不代表壞了,代表要先做完檔頭寫的三件事**(單一出口、fail-closed 的開關、只記真的發生過的連線)。`

- `[2026-08-09] [Windows] **方案市集這一輪沒有做,理由與下一輪的形狀。** Windows 上沒有可用的現成 zip 解壓路徑:我們的相依裡沒有 zlib(librime 的五個相依一個都不含 DEFLATE);Windows 內建的 Compression API 只有 XPRESS/MSZIP/LZMS,**沒有 raw DEFLATE**;Shell 的 zip folders 可以解壓但會**繞過我們自己的 ArchiveGuard**,而 zip slip 的防護必須是我們的(docs/schema-store.md §4 列成「缺一不可」)。所以正確做法是自己寫一份 inflate —— 純邏輯,可以在 Ubuntu 上對 Python 產生的 deflate 串流測試,而且每個 entry 都驗 CRC32(inflate 有 bug 會被抓到,而不是安靜地寫出壞詞庫)。整塊是「zip 中央目錄 + inflate + SHA-256 + WinHTTP + 安裝與回滾 + 市集 UI」六件,不是一輪塞得下的量,**做一半的下載按鈕比沒有更糟**。已先落地的地基(有測試、沒接上):common/net_policy.cc(Android NetworkGate/NetworkLog 的移植)、common/schema_list_patch.cc(加進 schema_list 與回滾用同一支)。⚠ **給 storefix 支線:如果索引裡的套件可以改成 stored(不壓縮)或另附一個非 zip 的格式,Windows 端這一塊會小掉一大半。** 不過那會動到 docs/schema-store.md 與已經發布的索引,所以先問。`

- `[2026-08-09] [Windows] ⚠ **給 dict 支線:docs/backup-format.md 併 main 的時候還不在**,所以 Windows 端的設定介面這一輪沒有做詞庫匯出匯入。那份出來之後我照做,不會自己發明一套格式。`
- `[2026-08-08] [storefix] 動了原屬協調端的 scripts/schema_store/、scripts/build_schema_store.sh、docs/schema-store.md，以及重新產生 core/schema-languages.json —— 經使用者指示。沒有動 android/、apple/、windows/、docs/theme-format.md，也沒有動 core/ 的其他檔案。`

- **[2026-08-08] [storefix] 方案 id 不是全域唯一 —— 掃過索引裡真實的 34 個套件，撞號共 9 個 id，不是原先以為的一個。**

  | schema id | 提供者 | 性質 |
  |---|---|---|
  | `double_pinyin` | `double-pinyin` / `ice` | **內容相反**：繁體詞庫 vs 簡體 |
  | `double_pinyin_abc` | `double-pinyin` / `ice` | 內容相反（繁／簡） |
  | `double_pinyin_flypy` | `double-pinyin` / `ice` | 內容相反（繁／簡） |
  | `double_pinyin_mspy` | `double-pinyin` / `ice` | 內容相反（繁／簡） |
  | `pinyin_simp` | `pinyin-simp` / `wubi86-jidian` | 都是簡體，但**詞庫不同** |
  | `radical_pinyin` | `ice` / `radical-pinyin` | schema 與 dict 兩個檔都不同 |
  | `bopomofo_tw` | `@builtin` / `bopomofo` | 同一份上游，來源不同 |
  | `luna_pinyin` | `@builtin` / `luna-pinyin` | 同一份上游，來源不同 |
  | `luna_pinyin_tw` | `@builtin` / `luna-pinyin` | 同一份上游，來源不同 |

  唯一鍵定為 `uid = <套件 id>/<方案 id>`，保留命名空間 `@builtin/…`（內建方案）與
  `@local/<來源>/…`（使用者自帶）。規範見 docs/schema-store.md §1.2。
  清單維護在 `scripts/schema_store/data/known_collisions.yaml`，測試拿真實語料
  **雙向**比對 —— 上游多一個撞號會紅，少一個也會紅（清單不會腐爛成「以前有人看過一次」）。

- **[2026-08-08] [storefix] ⚠ 比 id 撞號更嚴重的一層：檔案撞名。**
  套件 zip 解壓到同一個 `user_data_dir`，同名檔案就是互相覆蓋，**誰後裝誰贏，
  而畫面上什麼都不會說**。真實資料裡有 **14 個路徑**由多個套件提供，牽涉 7 個套件。
  最不明顯的是 `key_bindings.yaml` / `punctuation.yaml` / `symbols.yaml`
  （`moran` vs `prelude`）與 `default.yaml`（`ice` / `moran` / `prelude`）——
  這幾個被所有方案 `__include`，被蓋掉影響的是使用者裝的**每一個**方案，
  不只是撞號的那一個。使用者可見的症狀：裝了雾凇之後，原本的繁體雙拼靜靜變成簡體。
  索引因此新增選填的 package 層級 `conflicts` 欄位（誰會蓋掉誰的哪些檔案），
  讓行動端在**安裝前**就講得出來。徹底解法（每個套件各自的子目錄）需要 librime
  支援多來源的 user_data_dir，不在本輪範圍。

- **[2026-08-08] [storefix] 索引格式加了欄位，`format_version` 維持 1，新增 `format_minor`。四端都請照這條規則。**
  理由：現行 Android 讀取端寫的是 `format_version != 1 → 整份拒收`（是 `!=` 不是 `>`），
  所以**任何一次 major 遞增都會讓所有已出貨的 app 同時失去整個市集**——
  使用者看到的不是「有新功能」，是「一個方案都沒有」。規則寫進 docs/schema-store.md §1.1：
  major 只在破壞性變更時動，且必須併發雙檔過渡；加欄位走 minor；
  讀取端**不得**拿 minor 當拒收依據、不認得的鍵一律忽略。
  `test_store.py` 的 `test_MUTATION_bumping_major_breaks_every_shipped_app` 把這個後果釘住。

- **[2026-08-08] [storefix→Android] 交接：store 的消費端要怎麼改。⚠ 我沒有動 `android/` 底下任何檔案（那是 dict 支線的檔案），以下是「要做什麼」。**
  索引與隨 APK 出貨的對照表都已經帶著新欄位，Android 不改也不會壞（欄位是加上去的），
  但下列每一條在撞號的 9 個 id 上都會給出錯誤答案：

  1. **`StoreSchemaRef` 加 `uid: String?`**（`store/SchemaIndex.kt`）。索引的
     `packages[].schemas[]` 與 `builtin_schemas[]` 都有了。舊索引沒有 → `null`，
     就地以 `pkg.id + "/" + schema.id` 補上（那正是伺服器側的算法）。
     同時新增的 `schemas[].language_source`（`upstream` / `curated` / `derived` /
     `unknown`）可以拿來決定要不要在 UI 上說「這個分類是推測的」。
  2. **`InstalledRegistry` 帳本升到 `FORMAT_VERSION = 2`。** 遷移**無損且不需連網**
     （離線為預設是硬約束）：v1 本來就以套件為單位存，
     `uid = pkg.id + "/" + schema.id`；`source == "local"` 走
     `@local/<清理過的來源>/<schema>`。規則與**冪等性**要求見 docs/schema-store.md §4.1，
     參考實作 `scripts/schema_store/registry.py`，測試
     `test_store.py::TestRegistryMigration`（7 條，含「跑第二次不可弄壞資料」與
     「壞紀錄不可丟掉，否則磁碟上的檔案變孤兒」）。Kotlin 端照著寫，兩邊行為要一致。
  3. **`InstalledRegistry.layoutForSchema(schemaId)` / `noteForSchema(schemaId)`
     目前是 `firstOrNull { 任何套件有這個 id }`** —— 使用者同時裝了 `double-pinyin`
     與 `ice` 時，它回的是「帳本裡先出現的那個套件」的佈局，不是使用者正在用的那個。
     改成以 uid 查。只拿得到裸 id 時（librime 的 `schema_list` 用的就是裸 id），
     若該 id 有多個提供者要有**明確的決勝規則** —— 建議「最後一次安裝的優先」，
     因為磁碟上贏的就是它 —— 不要靠 `firstOrNull` 的偶然順序。
  4. **`SchemaIndex.languageTags()` / `InstalledRegistry.languageTags()` 回傳
     `Map<裸 id, tag>`** —— 撞號的 id 在這裡會被後者覆蓋前者。改讀 uid 表。
     隨 APK 出貨的 `core/schema-languages.json` 已經加了 `schemas_by_uid`（96 筆，完整）；
     原本以裸 id 為鍵的 `schemas`（83 筆）**一個字都沒動** —— 這次的 diff 是純新增，
     不改 app 也不會有任何行為變化。裸 id 表的收錄規則仍是「判定不同的撞號 id 不收」
     （目前 4 個 double_pinyin*），`pinyin_simp` / `radical_pinyin` 兩邊判定相同照收：
     為了「內容不同」丟掉一個確定的正確答案，只會讓讀取端退回字面啟發式，那才是真的會分錯。
     `SchemaLanguages.parse` 的 `format_version == 1` 檢查不必改。
  5. **`SchemaIndex.packageProviding(schemaId)`** 同樣是「第一個提供者」，撞號時挑錯。
  6. **新的 `conflicts` 欄位要上 UI**：安裝前告知「這個套件會覆蓋掉 X 的這幾個檔案」。
  7. **自帶檔案匯入的 id**（`SchemaStore.kt` 的
     `"local:" + displayName.substringBeforeLast('.')`）是使用者輸入衍生的，
     可能含 `/` 或空白。走 `@local` 命名空間前要把非 `[A-Za-z0-9._-]` 的字元換成 `_`
     （見 `scripts/schema_store/uid.py` 的 `local_uid()`）。

- **[2026-08-08] [storefix] 語言標記（BCP 47）的判定資料全部搬進版控**：
  `scripts/schema_store/data/languages.yaml`（原本散在 `languages.py` 的 dict 裡）。
  每一筆人工判定都必須附 `why`，沒寫理由的在載入時直接 die。
  索引新增 `language_source` 分級：`upstream`（上游 metadata，即 rppi 的分類路徑）/
  `curated`（我們自己標的）/ `derived`（啟發式：方案名字樣、詞庫繁簡探針）/ `unknown`；
  多成分時取最不可靠的那一個。逐條依據另外產生 `languages.json`，與 `index.json`
  一起發布，任何人都可以複查每一個標記的來歷。
  目前 101 個方案（市集 97 + 內建 4）：有標記 96、明確「本來就不是語言」5
  （IPA 兩套與三個工具方案）、**未知 0**；來源分佈 upstream 3 / curated 19 / derived 79。
  `build_schema_store.sh` 以 `--max-unknown 0` 釘住 —— 上游哪天加了判不出來的東西，
  建置會擋下來，而不是讓一個 `und` 悄悄混進選單的「其他」分組
  （使用者會以為清單裡沒有他要的東西）。

- **[2026-08-08] [storefix] 共用腳本介面變動，其他端請知悉。**
  `scripts/build_schema_store.sh` 新增 `--phase test`（撞號與語言標記測試，39 項，
  含 8 條植入違規的反向測試），並在 `index` 階段之後自動重新產生測試語料
  `scripts/schema_store/data/corpus.json`（真實 34 個套件的脫水版，22KB，進版控，
  讓測試在沒有 891MB 上游 clone 的環境也跑得動）。
  `index` 階段現在同時寫 `core/schema-languages.json`（多了 `schemas_by_uid`）與該語料；
  `upload` 階段多傳一個 `languages.json` 並一樣用 curl 驗對外網址與 Content-Length。
  測試在缺少現場資料時會 skip，但收尾一定印出醒目的「以下 N 項沒有跑」，
  且 `--require-live` 會讓 skip 直接算失敗 —— 這個專案有過「測試安靜地跳過自己
  卻報一片全綠」的前科。
  CI 另開一條 `.github/workflows/schema-store.yml`（`on.push.branches: [main, storefix]`），
  只跑那 41 項測試 —— 它靠版控裡的語料，不需要上游 clone 也不需要模擬器，約 1 分鐘。
  **沒有動 build.yml**（那是 Android 那條，避免撞在同一個檔案上）。
- `[2026-08-08] [insets] ⚠ **「RimeInputMethodService 沒有覆寫 onComputeInsets、鍵盤蓋住宿主內容」那條(§5 協調端 2026-08-08 那則)實測是誤判,請不要照著加覆寫。** 三個數字沒有一個支持那個結論:(1) `contentTopInsets` 是**輸入法視窗內**的座標,本專案的輸入法視窗是 `gr=BOTTOM (fillxwrap)`、高度剛好等於鍵盤,輸入區就在它最上面,所以 0 是正確答案;Gboard 量到 1983 是因為它的視窗是整片螢幕(`frame=[0,76][1440,3120]`),同一個欄位在兩種視窗形狀下本來就不會是同一個數字。(2) `touchableRegion` 只在 `TOUCHABLE_INSETS_REGION` 時才填,`dumpsys input_method` 印空的是預期;真正生效的可觸區在 `dumpsys window`,實測 `SkRegion((0,2081,1440,3120))`,非空且等於鍵盤 frame。(3) Android 11 起宿主拿到的 ime inset 由 WindowManager 從**鍵盤視窗 frame** 推導,`contentTopInsets` 那條舊路已經不參與排版。實測(1440x3120):宿主收到的 ime inset = 1039 px = 鍵盤視窗高度,下緣輸入框的底停在 y=2081 = 鍵盤的頂,零重疊。**而照著那個推論加覆寫會真的做出那個缺陷** —— 植入 `contentTopInsets = decorView.height` 之後 inset 掉到 152 px、輸入框被蓋掉 887 px。程式碼一行未改,改成留下 `scripts/verify_insets.sh` 擋它`
- `[2026-08-08] [insets] ⚠ **給所有會用 dev.rime.imetest 當靶的人:`android:windowSoftInputMode="adjustResize"` 在 targetSdk 35 上是沒有作用的,不能拿它當判準。** `SOFT_INPUT_ADJUST_RESIZE` 自 API 30 起棄用,Android 15 又強制 edge-to-edge,於是不自己消費 insets 的 Activity 連標題都畫到狀態列底下,下緣的輸入框當然被鍵盤蓋住 —— **換成 Gboard 一模一樣**(已實測兩者並列)。拿那個畫面去指控輸入法會指控錯對象,上面那則誤判就是這樣來的。靶要能驗這件事,必須自己裝 `OnApplyWindowInsetsListener` 消費 `Type.ime()`。`scripts/build_testapp.sh` 加了選用的 `--ez bottom true`(下緣輸入框 + 監聽器 + 把量到的 ime inset 寫進標題 content-desc);不帶 extra 時行為與改動前完全相同,既有腳本不受影響`
- `[2026-08-08] [insets→協調/Android] **請幫忙接一個 JNI 綁定:`rs_highlight_candidate`。** `core/include/rime_shell.h:147` 已經有它,規範 §9.5 也改成以它為準,但 `android/app/src/main/cpp/jni_bridge.cc` 與 `android/.../core/RimeCore.kt` 都沒有對應的 `nativeHighlightCandidate`(`rime_shell_stub.cc` 也要補一份),所以 `candidate:next` / `candidate:prev` 在 Android 仍列在 `VerbSupport.UNIMPLEMENTED` 裡。那三個檔案不屬於本支線,照 §2 回報不自行跨界。⚠ 接的時候用 `rs_highlight_candidate`,**不要**寫成 `rs_select_candidate(i+1)` —— 後者會選定候選(依方案可能直接上屏),規範 §9.5 明文點名這個寫法是錯的。目前沒有任何佈局或主題用到這兩個動詞,所以不是使用者可見的缺陷,只是一個到期沒關的洞`
- `[2026-08-08] [insets→協調] **CI 沒有接我的東西,請在合併時一併處理。** `.github/workflows/build.yml` 的 `on.push.branches` 只有 main,而且 `emulator` job 只在 main 或手動時跑 —— `scripts/verify_insets.sh` 需要模擬器,所以就算把 `insets` 加進 branches 也只會跑 `fast`。六條支線同時往那兩行加分支是必然衝突,所以本支線刻意不動 build.yml。建議合併後把這一行加進 emulator job(在 verify_longpress 之後,兩者都要模擬器與已安裝的 IME):`bash scripts/verify_insets.sh --ime org.rimequad.ime/.RimeInputMethodService --apk <apk>`。它自帶反向對照,不會報一個沒驗到的綠`
- `[2026-08-08] [insets] 給四端參考的兩個「驗證層自己在說謊」的樣本,形狀都會重演:(1) `verify_layout.sh` 的組字區判準原本只看**最後一步**的 `cs`,於是選字上屏之後組字區消失,**最正常的中文輸入序列被判成「librime 沒真的參與」** —— 指控的正好是相反的事實。判準要看「整輪有沒有任何一步出現過組字區」。桌面端日後驗候選窗/上屏時同一個坑成立:**終態不等於全程**。(2) `RepoFixtures.themeIds` 還是手寫的四個 id(`layoutIds` 早就改成掃目錄了),十二份主題有八份從未被 ThemeParserTest / MiniYamlTest 載入過。已用同一個植入兩邊對測證實:改成掃目錄後紅,換回四個 id 後 **BUILD SUCCESSFUL**`
- `[2026-08-08] [Android/diag] **Android 的診斷已改成 code + args,照 §6.5 / §6.5.1 實作完成。** 診斷的身分是 `(severity, code, path)`,`Diagnostic.developerMessage` 只是英文回退(不上畫面、不參與比對)。`DiagnosticCode` 與 macOS 的 `Diagnostics.swift` 逐項對照過,45 個規範碼一個不少。三件桌面端可能會撞到的事:`
  1. `**severity 是 code 上的函式,產生點不能選。** `Diagnostics.add()` 沒有 severity 參數,而且有一條測試**掃原始碼**確認 `diag.warn(...)` / `Diagnostic(Severity.X, ...)` 這種形狀一個都不剩。這條規則當場抓到一個真的缺陷:`input_mode:<未知>` 被記成 `diag.error(... "F10" ...)`,也就是致命錯誤 —— 一顆鍵上的一個錯字讓**整份佈局載不起來**,使用者看到的是鍵盤整個換掉。§6.2 的致命清單沒有這一條,§6.3 明寫那是 WARNING。已修。`
  2. `**同一個 `(severity, code, path)` 只留一則。** 重複的那一則沒帶新資訊給使用者,卻會讓 §10 第 9 條的序列比對無聲失守。也當場抓到一個:`LayoutParser` 為了先取 `auto_for_schema` 再檢查它含不含 `"*"`,對同一個節點呼叫了兩次 `stringList()`,型別錯時就是兩則一模一樣的 WARNING。已修。**桌面端請自查同一個形狀**:任何「先取值、再拿同一個 cursor 取一次來判斷」的地方都會產生它。`
  3. `**去重之後,path 必須夠細。** 同一層三列寬度都不對、兩個必備工具列項都被刪掉,原本都共用一個 path,去重會把它們併掉。Android 改成 `layers[i].rows[j]` 與 `...toolbar.items[k]`。四端要一致,否則診斷數還是對不上。`

- `[2026-08-08] [Android/diag] ⚠ **給 macOS(規範所有權):§6.5.1 的碼表漏了 9 種規範正文要求發診斷的情況。** §6.3 與 §9.7 明寫「產生 WARNING」,但碼表裡沒有對應的一格,於是實作只能二選一:硬塞進最接近的既有 code(語義走樣),或自己取名(四端各取一個名字,§10 第 9 條的比對就永遠對不上)。Android 選第三條路:照下表實作並標成 `provisional`,一條測試盯著「程式碼裡多出來的 code 恰好等於登記過的暫定碼」——**macOS 把哪一條寫進規範,那條測試就會紅**,提醒把旗標拿掉。請直接採用或改名,改名也請在這裡回一聲,Android 會跟著改:`
  | 提議的 code | 規範依據 | args |
  |---|---|---|
  | `unknown_keysym` | §6.3「佈局:無法解析的 `keysym` 名」 | `[name]` |
  | `unknown_modifier` | §9.6 `send.modifiers` 的未知修飾鍵名 | `[name]` |
  | `unknown_swipe_direction` | §9.6 `swipe` 底下的未知方向 | `[direction]` |
  | `mutually_exclusive` | §6.3 的三條互斥規則(send/tap、repeat/long_press、keysym/text) | `[ignored, winner]` |
  | `send_incomplete` | `send` 既沒有 `keysym` 也沒有可用的 `text` | `[]` |
  | `row_width_mismatch` | §6.3「某 row 的 `width` 總和 ≠ `units`,差距 > 0.01」 | `[sum, units, layer-id]` |
  | `key_patch_no_target` | §9.7「patch 的 id 找不到 → 忽略 + WARNING」 | `[key-id]` |
  | `action_target_missing` | `layer:` / `layer_once:` / `layer_lock:` 指向不存在的層 | `[raw, target]` |
  | `auto_for_schema_wildcard` | §9.1.1:`auto_for_schema` 只比對具名方案 | `[]` |
  `另外還有一個 `user_remap_unapplicable`(使用者自訂鍵位套不上)是 **Android 專屬**,桌面端沒有自訂鍵位,不必進規範;列出來只是因為它也在同一個 enum 裡。`
  `⚠ §6.3 還有一列「`style` 指向主題沒有的 key style → 改用 `default` + WARNING」,Android 目前**根本沒有產生這則診斷**(style 是在繪製期才解析的)。這一格四端現在都空著,一併請規範裁決。`

- `[2026-08-08] [Android/diag] ⚠ **`type_mismatch` 的 args 本身是要翻譯的東西,規範沒有講。** §6.5.1 說它的參數是 `[expected, found]`,而這兩個位置放的是「映射／序列／純量」這類術語。產生端若直接塞 `"a mapping"`,中文使用者看到的就是「這裡應該是 a mapping」—— 訊息翻好了,洞開在參數上,而且外面看起來一切正常。Android 的做法是產生端只放**穩定代號**(`mapping` / `sequence` / `scalar` / `null` / `string-list` / `localized-string`),字面由各端自己的資源提供。建議規範把這組代號寫成規範性的詞彙表,否則 macOS 塞的是英文句子、Android 塞的是代號,args 雖然不參與比對,畫面上會是兩種東西。順帶一提:對使用者不要說「純量」,這個 app 的使用者是麻瓜 —— Android 的英文寫的是 “a single value”。`

- `[2026-08-08] [Android/diag] **§6.2「超出致命清單者一律為可回復」被當真了,有一個行為變更。** 「layer 缺 `id`」原本是致命的(記成 F8),但 F1–F10 裡沒有這一條。改成:丟掉那一層 + `entry_dropped` WARNING;全部丟光時才補 F8。這讓一份佈局裡的一個壞層不會拖垮整份文件。桌面端不讀佈局文件,所以不影響四端比對;但**「往致命清單裡自己加一條」這個動作本身四端都要避免** —— 那會讓四端拒絕不同的檔案,而 §10 第 9 條明寫致命錯誤一律屬於共用作用域。`

- `[2026-08-08] [Android/diag] ⚠ **給所有在 Android 端開字串檔的支線(dict / sec / insets):`StringCatalogTest` 不會檢查你的檔案。** 它的路徑寫死 `values*/strings.xml`,所以任何 `strings_<支線>.xml` 的「三份語系形狀一致」完全沒有人守 —— 少一條翻譯就是那一句在該語言下靜靜顯示英文。diag 這條線自己補了一份 `DiagnosticStringsTest`(key 集合、位置參數集合、每一份樣板都填得起來、英文那份不得出現漢字)。其餘支線請照做,或者由某一條線把 `StringCatalogTest` 改成掃 `strings*.xml`(那會改到共用測試,誰做誰在這裡說一聲,免得四份同時改)。`
  `另外實測到一個更細的坑:譯者把 `%2$s` 打成 `%2s` 時,Java 的 Formatter **不會丟例外**,它當成「寬度 2 的無編號轉換」然後安靜地取了下一個參數 —— 畫面上該出現第二個參數的地方出現第一個,長度標點語氣全都正常。「位置參數集合相同」與「填得起來」兩條都放它過。要另外守一條「每一個 % 都必須帶編號」。`

- `[2026-08-08] [Android/diag] `docs/theme-format.md` 現在是 Android 單元測試的**宣告輸入**(`android/app/build.gradle.kts` 的 `tasks.withType<Test>`)。`DiagnosticCodeSpecTest` 直接讀 §6.5.1 的碼表跟 `DiagnosticCode` 逐項比對(抄一份常數表會腐爛,而腐爛的方式正好是「規範改了、測試還是綠的」)。不宣告成輸入的話,macOS 端改了碼表 Android 這邊會判 UP-TO-DATE。**規範本身一個字都沒有動。**`
- `[2026-08-08] [dict] **新增 `docs/backup-format.md`（匯出／匯入的四端格式）。這個檔案由 dict 支線維護**，其餘端要擴充請照 §2 回報。容器是 zip、清單是 `rimequad-backup.json`、內容分 `dict/ schema/ config/ settings/ layout/` 五個目錄。三件事其餘端接手前一定要先讀：(1) 使用者詞典有**兩種載體**，`rime-userdb-text`（librime 自己的 `*.userdb.txt`，**正式的跨端格式**，可合併）與 `leveldb-dir`（整個目錄搬走，Android 現況，只能覆蓋不能合併）；讀取端**必須兩種都認得**。(2) 版本規則只有 TOO_NEW / TOO_OLD / OK 三種，而且**版本判定必須先於欄位檢查** —— 否則未來版本的備份會被報成「檔案壞了」，使用者去找一個不存在的壞檔案，而他該做的是升級 App。(3) `files` 是白名單 + 逐檔 sha256，容器裡沒被列到的東西一律不落地。`
- `[2026-08-08] [dict → 協調] ⚠ **請在 `rime_shell.h` 加一個 `bool rs_sync_user_data(void)`**（對應 librime 的 `RimeSyncUserData()`）。這不是方便性需求，是正確性需求：librime 的 `Memory::OnCommit` 在使用者上屏之後**開一個交易**才寫入剛學到的詞，而那個交易住在記憶體裡的 `leveldb::WriteBatch`，要等下一次 `FinishSession()` 或 `~UserDictionary` 才落地。也就是說**「使用者剛剛打的那些字」通常只存在於記憶體**，匯出時直接複製 `*.userdb/` 拿到的是上一輪的詞庫 —— 能開、能用、大小差不多，只是少了最近的學習成果，而且沒有任何錯誤訊息。Android 端目前的替代做法是「建一個 session、立刻銷毀」（`UserDictionaryComponent` 的 `db_pool_` 讓同一本詞典在行程內只有一個 `Db` 物件，所以任何 `~UserDictionary` 的提交等於替所有人提交），驗證不到、只能記進 manifest 的 `flushed` 欄位。有了 `rs_sync_user_data()` 之後：flush 變成確定的，而且順帶產出 `*.userdb.txt`（跨端正式格式）。**桌面兩端要做匯出時會撞到同一件事**，不是 Android 專屬問題。`
- `[2026-08-08] [dict → Android] ⚠ **`app/src/test/java/org/rimequad/ime/StringCatalogTest.kt` 把檔名寫死成 `strings.xml`**（`File(resRoot, "$dir/strings.xml")`）。專案規矩是「新字串開 `strings_<支線>.xml`」，於是**這一輪四條支線新增的字串一個都沒有被那支測試檢查過** —— 少一個 key 的下場是那一句在該語言下靜靜地顯示英文，畫面不壞、build 不紅。這正是「測試會安靜地跳過自己」。建議改成掃 `values*/strings*.xml`（我沒有動那個檔案，它不在本支線的範圍內）。dict 支線暫時自己帶了一份 `store/DictStringsTest.kt` 只檢查自己那一份，並實測「繁體少一個 key」會紅。`
- `[2026-08-08] [dict → Android] `AndroidManifest.xml` 的 `allowBackup` 註解最後一行寫「⚠ 匯出/匯入功能目前尚未實作，見 audit_offline.sh 檔頭的未竟事項」。**這句話現在不成立了**（進階頁已經有匯出／匯入）。我沒有改 manifest（不在本支線範圍，而且它是多條線都會碰的檔案），請 manifest 的所有者順手改掉並指向 `docs/backup-format.md`。`
- `[2026-08-08] [dict] 跨界一處，先報備：`home/SettingsPages.kt` 的 `AdvancedPage` 加了 8 行（一個 import、一個小標題、一次 `BackupSection()` 呼叫）。UI 本體全部住在 `store/BackupSection.kt`，所以那個多人共用的檔案只被動到那麼多。另外 `.github/workflows/build.yml` 的 `on.push.branches` / `on.pull_request.branches` 加了 `dict`，並把單行的 `[main]` 改成一行一項的區塊寫法 —— 四條支線都要加自己那一行，區塊寫法讓 git 有機會自動合併，單行陣列一定衝突。`
- `[2026-08-08] [dict] 備份**刻意不帶**三個偏好，其餘端請照辦（`docs/backup-format.md` §3.4）：`network_enabled`（這是安全預設不是喜好 —— 允許它跟著備份走，等於讓「打開一個檔案」變成一個可以替使用者開啟連網的動作）、`offline_notice_seen`、`onboarding_done`。同理刻意不帶 `installation.yaml`（跨重裝穩定的 UUID）、`user.yaml`（`var/schema_access_time`＝何時用過哪個方案）、以及連網紀錄本身。理由與 `allowBackup=false` 是同一套。`
- `[2026-08-08] [sec] **librime-lua 的沙盒 patch 大改,而且 third_party/prebuilt 的 .a 已一併重建並提交。** patches/librime-lua@sandbox.patch 現在同時改三個檔:src/lib/lua.cc(第一層改成**允許清單**——舊的移除清單漏了 debug.getupvalue,有了它第二層包起來的 io.open 可以被一行挖回來)、src/modules.cc(**新增第二層**:io/loadfile/dofile/require 收斂到 RIME 的兩個資料目錄,使用者目錄可寫、共用目錄唯讀;並把 rime.lua 從「模組初始化」延後到「第一個 lua 元件被建立」)、src/lua_gears.h(延後的掛勾)。⚠ **arm64-v8a 與 x86_64 的 librime.a 都重新建過並提交**(patch 不重建就只是躺在 patches/ 裡,原始碼看起來安全而出貨的引擎完全沒變)。這是二進位檔,若有人同時動它會是硬衝突,請先跟我對一下。`
- `[2026-08-08] [sec→macOS/Windows] **桌面端要重建 librime-lua。** patch 從只改 lua.cc 變成改三個檔,macOS 端若已經編過 librime-lua,不重建就會停在舊的沙盒(而舊的沙盒有 debug.getupvalue 那個洞)。Windows 端目前沒編 lua,windows/build.sh 那道「掛上 lua 但沒帶 sandbox patch 就擋下建置」的守門仍然有效,但**它只檢查 patch 檔存在**,建議日後改成呼叫 scripts/verify_lua_sandbox.sh —— 那支只需要一個 C 編譯器,不需要 librime、不需要裝置,在 Windows 的 Git Bash 上也跑得動。`
- `[2026-08-08] [sec→協調/Android] **索引簽章沒有做,而它需要跨三個支線。** 現況:單一套件的 sha256 行動端有驗(下載時邊算邊比,不符即整包丟棄不解壓),但**整份 index.json 沒有任何真確性保證** —— 能寫入那個 bucket 的人可以同時改掉 sha256 與 zip。另外索引的 base_url 與套件的 file 都是遠端給的,可以把下載導去別的主機。本輪只補了傳輸層那一半(NetworkGate 不再跟著轉址換主機,也不跟 https→http 降級),剩下的要:(1) 一把新的簽章金鑰(⚠ 遺失就再也發不出可驗證的索引,備份規則同 APK 金鑰);(2) scripts/schema_store/mkindex.py 產生 detached 簽章(協調端的檔案);(3) store/SchemaIndex.kt 驗簽並 fail-closed(Android 的檔案)。minSdk 26 → **不能用 Ed25519**(java.security 要 API 33),用 SHA256withECDSA / P-256。細節見 docs/offline-threat-model.md §5。`
- `[2026-08-08] [sec→協調] **把發布從 R2 搬到 GitHub Releases 同時是一個隱私改善,不只是流程整理。** 目前方案與升級都在 pub-d6a54d2e….r2.dev —— 那是本專案專屬的子網域,而 DNS 查詢與 TLS 的 SNI 都是明文。我們花力氣把 rimequad-android 從 User-Agent 拿掉,主機名卻把同一件事又講了一遍,而且講得更早(連線建立之前)。換成 objects.githubusercontent.com 那種很多人共用的主機,對被動觀察者的資訊量差一個數量級。`
- `[2026-08-08] [sec] **流量大小會洩漏使用者下載了哪一個方案,而且這一層無解。** 實測索引裡 34 個套件:在 ±2% 之內大小獨一無二的有 **21 個**,其餘 13 個落在 6 組兩三個一群的小群。索引是公開的,觀察者拿得到每個套件的確切大小,比對只是查表——而輸入方案往往直接對應語言與地區。有效的緩解(補位元組到固定桶、抓誘餌套件)都要伺服器端配合,R2 是靜態託管做不到。處置是**寫進文件與 UI 照實說**,不假裝改得了。四端都適用。`
- `[2026-08-08] [sec] **動了 .github/workflows/build.yml 兩處**(這是共用檔,先報一下):on.push.branches 加了 sec;fast job 在「發布關卡」之後、「簽章與單調性關卡」之前插了兩個步驟(scripts/verify_lua_sandbox.sh、scripts/verify_audit_offline.sh)。兩步都是純新增,沒有動既有步驟。`
- `[2026-08-08] [sec] **`set -o pipefail` 配 `cmd | grep -q` 咬了第三次,而且這次的失敗方向相反、更危險。** §3 已經記過兩次(誤報失敗)。這次是 `strings librime.a | grep -q <沙盒標記>`:一顆**確實帶著沙盒**的 .a 被判成沒有沙盒——因為 grep -q 命中就結束,strings 還在寫 18MB,收到 SIGPIPE 以 141 結束。**同一個寫法在小檔案上是對的**(輸出塞得進 pipe buffer 就不會 SIGPIPE),所以它會在你用小檔案測試時通過,換成真的檔案才壞。修法:用 `grep -c`(它會把輸入讀完)把數字存進變數再比對,或整段讀進變數用 `case` 比對。四端的腳本都請掃一遍這個寫法。`
- `[2026-08-08] [sec] 新增的驗證腳本(誰都可以拿去用):scripts/verify_lua_sandbox.sh(把兩段沙盒從 patch 套用後的原始碼抽出來,裝進 librime-lua 出貨的那份 Lua 5.4.8,39 條探針 x 3 個階段 + 4 個變異測試;stage 0 的期望值就是反向測試——沒有沙盒時 os.execute 必須真的執行外部指令)、scripts/verify_audit_offline.sh(對 audit_offline.sh 植入 16 條真違規,確認它會紅、而且紅在對的那一項;含一條正向對照:只出現在註解裡的 java.net 不可以被誤判)、scripts/dex_network_refs.py(在**已建置的 APK** 上驗單一出口:java.net.HttpURLConnection 的引用者必須正好是 org.rimequad.ime.net.NetworkGate,集合相等,多一個少一個都紅)、scripts/verify_lua_deferral.sh(模擬器,驗 rime.lua 真的沒有在部署時被執行)。`
- `[2026-08-08] [sec] 順帶量到一件事:**APK 裡有 okio**,是 androidx.datastore-preferences 帶進來的,我們的 build.gradle.kts 一個字都沒提到它。它有 Okio.source(Socket) 這類 socket 輔助函式(不會自己連線,socket 要呼叫端給,而我們沒有呼叫)。這就是「原始碼 grep 看不到、產物層才看得到」的例子,已釘進 dex_network_refs.py 的清單。**桌面三端的相依也建議用同一個角度掃一次**:看的不是你寫了什麼,是最後包進去了什麼。`
- `[2026-08-08] [macOS] **Windows 端回報的六個候選窗規範缺口全部關掉了,可以開始讀主題檔了。** docs/theme-format.md 新增/改寫:`
  1. `**§8.6.7.2 `max_width` 溢出** —— 取消「由實作決定」。裁決是 **不得丟棄候選**:一頁有幾個候選由方案的 page_size 決定,序號標籤與使用者按的數字鍵一一對應,丟掉第 5 個之後他按 5 仍然會選到那個看不見的字。`shrink` 縮欄寬 + 尾端加 `…`;`clip` 裁的是**像素不是候選**,且不得改變落點。另外 `max_width` **不是硬上界**:9a(shrink 縮到 item.min_width 還放不下 → 窗跟著變寬)與 9b(第一個候選一定看得見)兩條例外,都不產生診斷(§10 第 4b 條的但書)。**直排橫排走同一套** —— 依 orientation 分岔的溢出行為不合規。`
  2. `**§8.6.4.1 item 內部間距** —— 新增 `label_gap` / `comment_gap` / `comment_gap_v`,前兩者的**預設值就是 `metrics.spacing`**,所以 Windows 目前的行為已經合規、不必改。附規範性的量測演算法(空段不留空隙、min_width 夾在含 padding 之後)。這一節同時關掉 §11 的「量測與排版是分開的兩件事」。`
  3. `**§8.6.7.3 `anchor`** —— `follow_caret: false` 落在哪一個角。新欄位,**預設 `bottom_trailing` 就是 Windows 目前取的右下**。語義用 leading/trailing 而不是左右(留給 RTL),螢幕取「含插入點的那一個」並有三段回落,offset 在此模式下是**向內的邊距**。`
  4. `**§8.6.0 字體綁定** —— §8.4 一直有具名字體堆疊,但沒說哪一塊文字用哪一個,Windows 因此整個候選窗退回系統 UI 字型。現在規定 label→`fonts.label`、text→`fonts.candidate`、comment→`fonts.comment`、preedit→`fonts.preedit`、status_bar→`fonts.ui`,並新增選用的 `font` 欄位(值是 `typography.fonts` 的**鍵名**,不是家族名)。**桌面端不得改用系統 UI 字型當預設**——`$system` 代號已經是那個意思,走代號才能讓「有指定」與「沒指定」是同一條程式路徑。`
  5. `**§8.6.7.4 退化** —— `backdrop`/`opacity`/`shadow` 做不到時的行為寫死了,而且**退化不得改變排版**(陰影不佔空間,§10 第 19–22 條那幾組數字在四端必須相同)。診斷是 INFO、每個欄位每次載入最多一則、且**不參與 §10 第 9 條的比對**(它們是平台能力相依的)。`opacity` 下界改成 0.05 —— 0 的候選窗等於這個輸入法壞了。`
  6. `**§11 的多欄與狀態列** —— 確認已被 §8.6.7.1 與 §8.12 覆蓋,§8.12 末段寫了一段給第三個桌面端:中/英、簡/繁的指示就是 `source: input_mode_pair` 與 `source: variant`,字面規範性、四端一致;`status_bar.show` 預設 false 是刻意的,但**「預設關閉」不是「可以不做」**。桌面端剩下的狀態列缺口只有「`source` 全是文字、沒有圖示」,在補上之前不要自己發明。`
  `另外順手關掉 §11 的「preedit 在桌面端有兩個地方可以畫」:§8.7 新增裁決 —— 組字串**必須**交給宿主(marked text / TSF composition string),`preedit.show` 在桌面端的語義變成「候選窗裡是否再畫一份」,**桌面端預設 false**(行動端仍為 true)。§10 新增第 26–33 條可逐項驗算的檢核。`
- `[2026-08-08] [macOS] ⚠ **「輸入模式 ↔ 方案 ↔ 簡繁」四端已經各錯一次,規則寫進 docs/settings-model.md §4。** macOS 註冊了 .Hant/.Hans 兩個輸入模式但兩個都載入繁體方案(使用者選簡體、打 hao le 得到「號」);Windows 只註冊 0x0404 一個 langid,連選都沒得選。兩者都是**畫面完全正常、自動化全過**,錯的只有打出來是哪一種字。裁決:(a) 輸入模式的字集認不出來時回 unspecified,**不要預設繁體**;(b) 方案的字集**語言標籤優先**,沒標籤才用命名慣例,`luna_pinyin` 的預設輸出是繁體所以「含 pinyin 就是簡體」這種規則不可以加;(c) 挑方案的優先順序是 為此模式釘的 → 全域釘的 → 字集相符的第一個 → 清單第一個,而且**釘的方案即使字集不符也照做**(靠 simplification 補,不偷偷換掉使用者選的東西);(d) `simplification` **無條件設**,對不存在的 switch 呼叫 rs_set_option 是安全的,判斷「這個方案有沒有這個開關」要解析第三方 YAML、猜錯的機會更大;(e) **RIME 的 simplification 是單向的(繁→簡)**,所以「繁體模式 + 只裝了簡體方案」做不到,介面上不得宣稱做得到。⚠ Windows 端要註冊 0x0804 才有這條規則可用,那個「待裁決」現在有答案了:**要註冊**,否則簡體使用者連選都沒得選。`
- `[2026-08-08] [macOS] **設定介面的資訊架構寫成 docs/settings-model.md,四端共用,以 Android 現況為基準。** 七頁:輸入方案/外觀/文字/我的詞庫/方案市集/連網/進階 ——「鍵盤」在桌面端改叫「輸入方案」(桌面沒有軟鍵盤可選),「手感」**整頁拿掉**(震動、按鍵音、長按延遲都是軟鍵盤專屬,做一頁全灰的比不做更糟),多一頁「詞庫」。文件同時釘死**設定存在哪裡**的三層:A=`default.custom.yaml` 的 patch(librime 自己讀的東西一律放這裡,換別的 RIME 前端也有效)、B=各端自己的偏好檔(鍵名共用)、C=session 選項(由 B 推導,不落地)。⚠ 兩條介面紀律建議四端都做成 CI 斷言:**每一項設定都要有一句白話**、**不得把 YAML 欄位名搬到畫面上**(macOS 端把它做成型別強制 + 禁用字掃描,這種規則靠 code review 記住一定會漏)。`
- `[2026-08-08] [macOS] **使用者詞庫的格式定了,四端互通,見 docs/settings-model.md §5。** 走 RIME 既有的自訂短語機制:`<user>/custom_phrase.txt` 是 TSV(詞/編碼/權重),掛載用 `<schema>.custom.yaml` 的 `table_translator@custom_phrase` + `db_class: stabledb`。**沒有走 librime 的 userdb**,因為 rime_shell.h 的 ABI 沒有匯出它的增刪介面(要加得由協調端動 core/)。純文字的三個好處:四端都做得到、使用者看得懂改得動也備份得了、換去鼠鬚管或小狼毫照樣有效。⚠ 兩條容易做錯的:CRLF 必須讀得懂(Windows 端匯出的帶 \r,留著會讓權重欄變成「1\r」而解析失敗,錯誤訊息卻像使用者打錯字);`<schema>.custom.yaml` **只有在第一行有我們的標記時才可以覆寫**,使用者自己寫的那份裡常有他調了很久的按鍵綁定。Android 端的 task #39 可以直接讀同一個檔案。`
- `[2026-08-08] [macOS] ⚠ **「裝在正確的地方」現在驗得到了,Windows 端的等價做法應該也成立。** 我們(和 Windows)都犯過同一個錯:驗了 bundle 結構與宣告,但**全部是在打包好的檔案上驗的**,沒有一關問過「放到正確位置之後系統認不認」。apple/scripts/verify_pkg.sh 分三層:pkgutil --expand 檢查安裝路徑與 postinstall → `installer -pkg ... -target CurrentUserHomeDirectory` **真的裝一次**並斷言檔案落點 → `lsregister -dump` 與 `TISCreateInputSourceList` 問系統認不認。**第三層在 runner 上查不到**,原因是 TIS 掃描 ~/Library/Input Methods 的時機綁在有登入的圖形工作階段,而 runner 是背景工作階段;腳本把原因印出來但不判失敗 —— 試過並知道為什麼也是結論。前兩層是實打實的新覆蓋。`
- `[2026-08-08] [macOS] ⚠ **給四端:「圖示是一塊空白方框」有兩個獨立的原因,查一個會漏另一個。** (a) 系統設定的輸入來源清單顯示的是 **app 圖示**(`CFBundleIconFile` / `.icns`),不是輸入法的選單列圖示(`tsInputModeMenuIconFileKey` / `.tiff`)——我們原本只有後者,而且 bundle 裡根本沒有 `.icns`。(b) 那份 `.tiff` 是手寫 IFD 產生的,缺 `RowsPerStrip`(tag 278)與 `PlanarConfiguration`(tag 284):多數解碼器讀得出來,**ImageIO 讀不出來**,而讀不出來的樣子就是一塊空白,沒有錯誤訊息。改成產生器只產 PNG(格式簡單,zlib 就寫得出來),`.icns` 與多解析度 `.tiff` 交給系統自帶的 `iconutil` 與 `tiffutil`。教訓與「工具的輸出格式不是穩定介面」是同一類:**自己手寫二進位格式的代價,會在很久以後以「看起來像沒裝好」的樣子出現。**`
- `[2026-08-08] [macOS] ⚠ **給四端:沒有在地化顯示名的輸入法,清單裡顯示的是它的 id。** 真機看到的是 `org.rimequad.inputmethod.RimeQuad.Hans`。macOS 的解法是 `Resources/<lang>.lproj/InfoPlist.strings`,而**鍵就是輸入模式的 id 本身**(TIS 拿 id 去查在地化字串)。所以那個 id 現在同時出現在 Info.plist、三份 .strings、以及 RimeQuadKit 的 InputModeBinding 三個地方,verify_pkg.sh 把它們釘在一起。Windows 的語言列名稱走的是資源 DLL 裡的字串資源,機制不同但**失敗的樣子一樣**:使用者看到一個沒有人敢點的東西。`
- `[2026-08-08] [macOS] 只動了 apple/、.github/workflows/macos.yml、docs/theme-format.md、docs/settings-model.md(新檔)。沒有動 android/、windows/、core/、scripts/。`
- `[2026-08-08] [macOS] ⚠ **更正我自己前一條的結論:`TISCreateInputSourceList` 在 GitHub runner 上**查得到**輸入法。** 我原本推論「TIS 只掃描有登入的圖形工作階段,而 runner 是背景工作階段,所以這一層驗不了」,並把它寫成不判失敗的參考資訊。**實測結果相反**:裝進 runner 的 `~/Library/Input Methods` 之後,TIS 列出 321 個輸入來源,其中三個是我們的,而且**在地化名稱解得出來**(`FOUND …Hans — RimeQuad (Simplified)`)。所以它現在是硬關卡,而且直接驗到真機回報的兩個缺陷:「輸入法沒出現在清單裡」(結束碼 4)與「清單顯示的是 bundle id」(結束碼 3 —— 名稱等於 id)。**教訓給四端:「這個驗不了」在寫進文件之前要先跑一次。** 我這一輪差點把一條驗得到的東西登記成驗不到的,而那種登記會一直被後面的人相信。`
- `[2026-08-08] [macOS] ⚠ **給四端的一個驗證工具自壞案例。** run_kit_tests.sh 的變異測試突然報「四個變異打中了別的地方」,而錯的那四個每次不太一樣。原因不在變異:那支腳本開了 `pipefail`,而判定式是 `printf '%s' "$OUT" | grep -q "$group"` —— `grep -q` 找到第一個符合就結束,printf 吃到 SIGPIPE,整條管線的結束碼變成 141,於是 `if ! ...` 恆為真。輸出小的時候 printf 在 grep 結束前就寫完,所以它一路正常;測試從 105 項長到 207 項、輸出超過管線緩衝區之後才開始發作。**症狀看起來完全像是被驗的東西壞了。** 凡是 `set -o pipefail` 的腳本,不要把 printf/cat 接進 `grep -q` 或 `head`,改用 herestring。Android 與 Windows 的關卡腳本請自己檢查一遍。`

- `[2026-08-09] [rename-shared] ⚠ **產品定名 LuminaKey,識別碼一起改。四端都要動,而且要動得一致。** 完整決策與「哪些刻意不改」見 `docs/decisions/product-name.md`;**值的唯一來源是 `scripts/lib/product.env`**(shell 與 python 各有一個十行讀取器,`scripts/verify_product_ids.sh` 每次比對兩邊逐字相同)。共用層這一輪已經改完:`scripts/`、`core/`、`docs/`、`README`、`.github/workflows/build.yml`。**`android/`、`apple/`、`windows/` 由各自那條線改,而且改完之前共用層的幾支腳本會是紅的**(見下面「合併順序」)。`

  規範性命名表(**不可自行變體**):

  | 項目 | 值 |
  |---|---|
  | 產品名(英文) | `LuminaKey` |
  | 產品名(中文顯示) | `LuminaKey 輸入法` |
  | 識別碼字根 | `luminakey` |
  | Android applicationId / namespace | `org.luminakey.ime` |
  | macOS bundle id | `org.luminakey.inputmethod.LuminaKey` |
  | macOS 設定 app bundle id | `org.luminakey.inputmethod.LuminaKey.Settings` |
  | macOS TISInputSourceID | `org.luminakey.inputmethod.LuminaKey.Hant` / `.Hans` |
  | macOS 使用者資料目錄 | `~/Library/Application Support/LuminaKey` |
  | Windows 使用者資料目錄 | `%APPDATA%\LuminaKey` |
  | 備份清單檔 / `kind` | `luminakey-backup.json` / `luminakey-backup` |
  | 安裝帳本 | `luminakey-store.json` |
  | 自訂鍵位 | `luminakey-layouts.json` |
  | Android SharedPreferences | `luminakey-store` / `luminakey-current-keyboard` / `luminakey-update` |
  | `<schema>.custom.yaml` 的標記 | `luminakey-managed` |
  | librime `app_name` 字根 | `rime.luminakey`(慣例同 rime.weasel / rime.squirrel) |

  **刻意不改**(不是漏改,改了會斷):R2 路徑 `rime/…` 與 R2 上的檔名字根(應用內升級指著它)、GitHub repo 名 `rime-quad`、librime 與 RIME 方案(「Rime」講引擎時是**正確**的)、`patches/` 裡 librime-lua 沙盒的 C++ 符號(改了要重建 librime,而稽核比對的是已建好的 `.a`)。

  ⚠ **落地檔名的相容條款(規範性)。** 舊名字的檔案還在既有裝置上。`docs/backup-format.md` §1 與 `docs/schema-store.md` §4.1 已寫進規則:**讀取端兩個名字都要認得(先新後舊),寫出端只寫新名字。** 漏掉這一條的下場是升級後備份被判成 `NOT_A_BACKUP`、「裝過哪些方案」變成空清單 —— **兩者都沒有錯誤訊息**,畫面上只是一片乾淨。

  ⚠ **稽核樣式也要跟著改名。** `audit_offline.sh` 第 7 項擋「UA 自報家門」,樣式原本是 `rime|quad|ime`。改名之後 `luminakey-android` 這種 UA **不會**被那個樣式抓到 —— 守門的還在擋舊名,新名字大搖大擺地走出去,而稽核照樣全綠。現在樣式從 `product.env` 的 `SELF_ID_UA_PATTERN` 讀,而且 `verify_audit_offline.sh` 新舊名各植入一條。**這是改名時最容易漏、而且漏了完全沒有徵狀的一類。四端的守門腳本請自己掃一遍有沒有同型的東西。**

  **合併順序(重要)。** 共用層的腳本現在照**新**的 id 去找東西,所以在 `android/` 改名落地之前,`scripts/audit_offline.sh`(找 `main/java/org/luminakey/ime/net/NetworkGate.kt`)與 `scripts/verify_audit_offline.sh`(往同一個套件路徑植入違規)會紅在「找不到檔案 / 植入失敗」。**那是預期中的紅,不是新缺陷**,而且刻意讓它紅得大聲——反過來設計成「找不到就跳過」的話,四條線各自綠燈合併之後就沒有人會發現識別碼其實對不上了。建議協調端把 `rename-android` 與 `rename-shared` **一起**合併。`apple/` 與 `windows/` 沒有這個耦合:`publish_desktop.sh` 改成從產物檔名**量出**它實際叫什麼,新舊名都吃得下(白名單在 `product.env` 的 `DESKTOP_APP_BASES`)。

  **給各端的具體待辦:**
  - **Android**:`applicationId` / `namespace` / 套件目錄 → `org.luminakey.ime`;`BackupFormat.MANIFEST_NAME` / `KIND` / `LAYOUT_ENTRY` / `REGISTRY_ENTRY`、`InstalledRegistry.FILE_NAME`、`UserLayoutStore.FILE_NAME`、三個 `PREFS` 常數 → 上表的值,**讀取端要同時認舊名**;`strings.xml` 的 `app_name`。服務類別名要不要跟著改由 Android 決定,決定了請回報 —— 共用層是從 `ANDROID_IME_SERVICE` 一行推出 IME id 的。
  - **macOS/iOS**:bundle id、`TISInputSourceID`、`InfoPlist.strings` 的顯示名、使用者資料目錄、Swift 模組名(`docs/settings-model.md` 已寫成 `LuminaKeyKit`)。**資料目錄改名要寫遷移**,否則使用者的詞典找不到了。
  - **Windows**:安裝程式 `AppName`、安裝目錄、`%APPDATA%` 目錄、TSF profile 的顯示名。⚠ 這條線目前在修「裝得起來但打不出字」,改名**之後另外處理**,不要插隊。
  - 三端改完之後 CI artifact 名字若跟著變,請同步 `product.env` 的 `CI_ARTIFACT_*` 與 `DESKTOP_APP_BASES`。

  **沒有驗到的**:這一輪只做了靜態驗證(全部改過的 shell 腳本 `bash -n`、`downloads_server.py` 實際起得來並回 200、`verify_product_ids.sh` 自帶反向測試)。**沒有裝過任何一份改名後的建置**,所以「改名後輸入法還註冊得上、還打得出字」一次都沒有被驗過。`

- `[2026-08-09] [協調] ⚠ **更正 §5 上面那則 `[insets→協調]` 建議指令裡的 IME id(只加註,原段落一字未動)。** 那一行寫的是 `org.rimequad.ime/.RimeInputMethodService`,改名之後已經不對;要加進 `build.yml` 的 emulator job 時請用 **`org.luminakey.ime/.RimeInputMethodService`**,或更好的做法 —— `. scripts/lib/product.sh` 之後用 `"$RS_ANDROID_IME_ID"`,那樣下一次改名不必再回來翻一遍文件。同一個形狀在 `docs/emulator.md` 有兩處(第 8 節與 `verify_ime.sh` 的範例),那兩處寫的是 `dev.rime.ime/…`,**一個從來沒有存在過的套件名**,已經改成正確值並加上取值寫法。`scripts/verify_product_ids.sh` 現在盯著「文件裡每一個 `…/RimeInputMethodService` 都必須等於 product.env 推出來的 IME id」(coordination.md 除外 —— 這份文件只加不刪,所以用這一則附註更正)。`

- `[2026-08-09] [協調] **六個落地識別碼在 Android 那一側補齊了,而且讀取端同時認舊名。** `rename-shared` 把規範寫進 `docs/backup-format.md` §1 與 `docs/coordination.md` §5(值是 `luminakey-*`),`rename-android` 判斷「四端共用格式要改就一起改」所以刻意沒改 —— 兩邊各自有道理,但併起來之後**沒有任何一道關卡會發現**。採用規範值(macOS 那側已經照規範做完,含 `LegacyDataMigration` 與 17 條 self-test,退回去的成本明顯較高):
  `BackupFormat.MANIFEST_NAME` / `KIND` / `REGISTRY_ENTRY` / `LAYOUT_ENTRY`、`InstalledRegistry.FILE_NAME`、`UserLayoutStore.FILE_NAME` 全部改成 `luminakey-*`,並各自多一個 `LEGACY_*` 常數。
  **寫出端只寫新名字,讀取端先新後舊**:manifest 依序試兩個檔名、`kind` 接受兩個值、容器內的 entry 兩個路徑都找、磁碟上的兩份帳本讀不到新名就退回舊名**並在下一次寫入時遷移過去**(寫成功之後才刪舊的那一份,不會同時留下兩份各說各話的帳本)。容器裡用舊名寫出去的檔案落地時會換成現在的名字(`BackupFormat.landingPath()`,純函式)。
  新增 `BackupLegacyNameTest`(10 條):**一份舊名容器與一份新名容器各匯入一次,兩份都必須成功**,再加一條反向對照(名字兩個都不是的容器仍然必須是 `NOT_A_BACKUP`)—— 少了那一條,前兩條在「什麼 zip 都當成備份」的實作下也會綠。實測植入(把三個 `LEGACY_*` 從清單裡拿掉)之後,舊名那幾條紅、新名那條照樣綠。`:app:testDebugUnitTest --rerun-tasks` 462 項、0 失敗 0 略過。`

- `[2026-08-09] [協調] ⚠ **`scripts/verify_product_ids.sh` 在上面那六項全部不一致的情況下 6/6 全綠、exit 0 —— 它正是自己檔頭警告的那個樣子。** 原因:第 3 項只 `find "$ROOT/scripts"`(不含 `apple/scripts/`),第 4 項只掃 `core/ docs/ tools/ README.md .github/workflows/build.yml` —— **`android/` 與 `apple/` 不在任何一項的掃描範圍內**。已改:
  · 第 3 項掃 `scripts/`、`android/`、`apple/` 的 `.sh` 與 `.py`,三個掃描根各植入一個違規證明每一個根都真的走到了;`apple/` 的建置與驗證腳本走一張**帶理由的允許清單**(它們由 `apple/scripts/verify_names.py` 逐條盯著),而且清單指到不存在的檔案時會紅,不會活得比它的對象久。
  · 第 4 項的舊名掃描加上 `android/app/src`、`android/testdata` 與 `apple/` 的 `Sources`/`AppSources`/`SettingsSources`;`apple/` 的 `Tests/` 與 `scripts/` 仍由 `verify_names.py` §9 的 MUST_KEEP 表管(那張表比「同一行寫舊名」更嚴),而第 3 項會斷言那支腳本還在。
  · **新增第 6 項:20 列落地識別碼逐一比對 product.env**(12 列現行值 + 8 列相容宣告),比對的是**整段宣告**而不是只有值。反向測試逐列把宣告改成另一個名字,要求「比植入前**多**紅的正好是那一列」。它當場抓到一個真的重複:`KeyboardChoice.kt` 把同一個 SharedPreferences 名字在兩個 object 裡各寫了一遍(已合併成一個檔案層級常數)。
  **植入驗證**:把六個常數逐一改回舊值,每一次 `exit 1` 且指名那一列;還原後 11 項全過、`exit 0`。`

- `[2026-08-09] [協調→macOS] **下載頁不再靠人記得翻轉 `MACOS_APP_BUNDLE`。** `scripts/downloads_server.py` 原本用 `product.MACOS_APP_BUNDLE`(`RimeQuad.app`)產生 macOS 手動安裝指令,而 `product.env` 的註解要求「發布過一版之後才手動改這一行」—— 靠人記得翻轉一個常數是不可靠的,而漏掉的症狀正是那段註解自己寫的:**使用者照做四步,系統一聲不吭地不載入它**。現在:`publish_desktop.sh` 從壓縮包的**內容**量出 `.app` 的名字(不是從檔名猜,而且包裡出現兩個 `.app` 會擋下來),寫成 R2 上的 `macos/app-bundle-latest.txt`;下載頁讀那個檔案,讀不到才退回 `product.env`(那代表 R2 上還是改名前的那一份,而那個常數記的正好就是它)。發布的最後一關比對「下載頁講的名字」與「包裡實際的名字」,不一致就不准發,而且**反面**也擋:指令裡不可以同時出現第二個 `.app`。
  ⚠ 順帶修掉一個會讓那一關永遠紅的東西:`[A-Za-z0-9._-]+\.app` 會把 `com.apple.quarantine` 裡的 `com.app` 當成一個 app 名字 —— 結尾要有 `\b`。**一個永遠紅的關卡會被關掉**,所以這不是小事。
  另外:`downloads_server.py` 的 `INSTALL` 那一段**從來沒有被 render 過**(`card()` 有 `install=` 參數,但沒有任何一處傳它),所以那段舊名字其實還沒有出現在頁面上。現在接上了,macOS 那張卡片會列出四步,而且用的是壓縮包的實際檔名。`

- `[2026-08-09] [協調] **沒有驗到的**:(1) 這一輪一樣**沒有裝過任何一份改名後的建置** —— 「改名後輸入法還註冊得上、還打得出字」仍然一次都沒有被驗過,`docs/emulator.md` 改對的那個 IME id 也還沒有在模擬器上實跑過。(2) `apple/` 的改動只有註解與 `// 舊名` 標記,**沒有編譯過 Swift**(這台是 Ubuntu);`apple/scripts/verify_names.py --self-test` 全過,涵蓋了它 self-test 會動到的那幾行。(3) macOS 發布那條路徑**沒有真的發布過一次** —— 新增的兩段(量 `.app` 名字、比對下載頁)是用 tar 清單與指令字串在本機對測的,不是在一次真的 `publish_desktop.sh macos <run_id>` 裡跑過。(4) `windows/` 一個字都沒動。`

---

## 6. 各端狀態

> 自己更新自己那一行。

> **2026-08-08 發布現況**(三端都在 R2 上,commit `98e52d8`):
> Android `rime/rime-latest.apk`(versionCode 26080810)、
> macOS `rime/macos/RimeQuad-latest.tar.gz`(arm64、ad-hoc 簽章)、
> Windows `rime/windows/RimeQuad-windows-x64-latest.zip`(x64)。
> 桌面兩端由 `scripts/publish_desktop.sh` 打包 —— CI artifact 本身不含執行期
> 資料,直接給使用者會是「裝得起來、一個字都打不出來」。
> **桌面兩端沒有任何人按過一顆鍵。**

- **Android** — 可用的產品。拼音/注音/九宮格、鍵盤與主題由 YAML 驅動、鍵盤類型選單、
  自定義鍵位、方案市集(34 個)、離線開關與連網紀錄、應用內升級與金鑰輪替、
  介面在地化(英/繁/簡)。354 項單元測試、16 項發布關卡。
- **macOS** — **輸入法本體 + 視覺化設定介面 + `.pkg` 安裝檔。使用者已在真 Mac 上
  打出字,但這一輪新增的 UI 一頁都沒有被自動化開啟過。**
  這一輪:六頁設定介面(輸入方案/外觀/文字/市集/連網/進階,IA 見
  `docs/settings-model.md`;第七頁「詞庫」寫好了但**刻意沒上架**,
  因為 verify_user_dict.sh 證明 librime 讀不到我們寫的詞)、
  IMKit 選單當入口、`.pkg` 一鍵安裝、
  三種語言的輸入來源顯示名與圖示(修真機回報的「顯示成 bundle id + 空白方框」)、
  輸入模式↔方案↔簡繁綁對(修真機回報的「選簡體卻打出繁體」)、
  離線守門的單一出口 + 連網紀錄、方案市集(索引/相依/sha256/壓縮檔守門/安裝回滾)、
  使用者詞庫(TSV,四端互通)。
  **207 項單元測試 + 10 個變異測試**;CI 另驗 bundle 結構、IMKit 宣告、
  ObjC 類別符號、靜態連結、`--self-check`、**`.pkg` 真的裝到正確位置**、
  **系統真的認得這個輸入法**(TISCreateInputSourceList,連在地化名稱都驗)、
  **單一連網出口**(含反向測試)。
  ⚠ **設定視窗、跨行程部署、市集的真實下載、switcher 熱鍵一項都沒被自動化碰過。**
  完整清單見 `apple/README.md` §3。
  規範 `docs/theme-format.md` 由本端擴充(見 §5),Windows 端的六個缺口已全部關閉。
- **Windows** — **有安裝程式,也有設定介面了。**
  使用者已在真 Windows 上裝起來,輸入法出現在語言列上 —— TSF 註冊是通的。

  第四個里程碑(本輪):**修掉「簡體使用者選了簡體輸入法卻打出繁體字」**
  (DLL 把 langid 帶進 IPC,服務據此挑方案與字形;協議升到 v2 但與 v1 相容,
  舊 DLL 連新服務照樣能用,新 DLL 連舊服務會降級重試),
  以及**設定介面**(純 Win32,住在服務進程,瘦 DLL 只多一顆語言列按鈕):
  方案排序與預設方案、簡繁/標點/候選數/候選字級、重新部署(有進度與結果)。
  入口三個:語言列按鈕、系統匣圖示、接通 librime 內建的 Ctrl+`。
  **刻意沒做**:方案市集(理由見 §5)、連網分頁(沒有東西會用到那個開關,
  那就是一顆死鍵)、候選窗主題(等規範)、詞庫匯出匯入(等 backup-format.md)、
  全域熱鍵(衝突了使用者不會知道是我們幹的)。
  新的驗證:120 個單元測試案例 1106 個斷言、離線稽核(原始碼 + 匯入表,
  含反向測試)、以及對**裝好的**那份 rime_service.exe 斷言 langid → 方案
  (含一條反向測試:en-US 必須沒有意見,否則「永遠回傳 luna_pinyin」也會全過)。
  設定模型照 `docs/settings-model.md`(第二次併 main 時它到了,我把自己
  先做的那一套整份換掉,鍵名與 §4 的四層優先順序全部改成規範的)。
  ⚠ **設定介面沒有一個像素、沒有一次點擊被驗證過** —— 完整清單見
  windows/README.md「這一輪新增、而且一項都沒有被驗過的」。

  第三個里程碑(安裝程式,本輪):`RimeQuad-Setup-x64.exe`(Inno Setup)。
  雙擊跳 UAC、裝到 `C:\Program Files\RimeQuad`、安裝時完成 COM 與 TSF 註冊、
  執行期資料(`core/data/shared` + `data/user` 範本)一起裝、
  「新增或移除程式」有項目、解除安裝停服務+反註冊+清乾淨
  **但保留 `%APPDATA%\RimeQuad` 的使用者詞典**。
  語言設定檔改成三份(zh-Hant-TW / zh-Hans-CN / zh-Hant-HK),
  原本只有 TW 那一份,簡體使用者在自己的語言底下找不到它。

  新的 CI job `install-x64` 在一台乾淨的 runner 上靜默安裝、斷言、解除安裝、
  再斷言。**已經跑綠過**(run #40)。**斷言到的東西**(完整清單見 `windows/README.md`):
  CLSID / InprocServer32 的**精確路徑** / ThreadingModel、
  CTF 底下**三個** langid 的設定檔、能力類別 6 類 1+5×3 筆、
  `ITfInputProcessorProfiles::EnumLanguageProfiles` 對每一個 langid 都列舉得到我們、
  ARP 那一筆、資料目錄解析、
  **用裝好的服務與裝好的詞庫經由真管道打出「你好」**、
  **跑完之後安裝目錄一個位元都沒變**(使用者資料沒寫進 Program Files)、
  解除安裝後登錄檔全清而 `%APPDATA%\RimeQuad` 還在。
  四道反向測試:裝之前 check 必須紅、故意刪 InprocServer32 後必須紅、
  解除安裝後必須紅、安裝包少了執行期資料必須出不了貨。

  **仍然驗不到的:** 在記事本/瀏覽器/Office/市集 App 裡真的打得出字、
  `ActivateEx` 與組字、候選窗的樣子與位置、高 DPI、
  使用者的語言列上到底看不看得到它(還取決於他的語言清單)、
  「每一顆鍵是不是都做了它宣稱的事」。**CI 綠仍然不等於好用。**

  已知缺口:只有 x64;**簡體使用者選 zh-Hans 那一份打出來仍是繁體字**
  (langid 沒有帶進 IPC,預設方案還是 luna_pinyin_tw);
  `enable-user` 不會替使用者把中文加進 Windows 的語言清單;
  TSF 不給純修飾鍵事件;沒有顯示屬性;沒有系統匣;沒有編 librime-lua。

  前兩個里程碑(核心層、TSF)的細節保留在下面。
- **iOS** — 未開始。
