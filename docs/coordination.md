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
- `[2026-08-08] [Windows] 驗證用的使用者目錄要明確指定方案。librime 把「上次選的方案」記在 <user>/user.yaml。Windows 的 verify_ime.sh 沿用 verify_console.sh 編好的使用者目錄以省下詞庫編譯時間,結果拿到的是上一支腳本最後選的注音,nihao 被打成「所噢草莓」。四端的驗證腳本若有共用使用者目錄的,同樣要明著選方案 ——「不指定」不是中性的。`

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
- **Windows** — 核心層已綠,**TSF 輸入法已寫出但沒有人在真 Windows 上用過**。

  第一個里程碑(核心層,協調端整理的那一段,保留):
  `windows-latest`(windows-2025-vs2026 / MSVC 14.51)上以 MSVC 從原始碼建
  librime 1.17.0 + 5 個依賴,`tools/rime_console.cc` 斷言
  `nihao → 你好`(luna_pinyin_tw)與 `su3cl3 → 你好`(bopomofo_tw)兩組;
  比對錨定 `^>>> COMMIT: ` 的最後一行且完全相等(只 grep「你好」會放過「你好嗎」),
  並先 `tr -d '\r'` —— MSVC 的 CRT 寫的是 CRLF,留著 CR 會讓比對失敗成
  「你好 != 你好」。斷言以竄改過的日誌反向測過五種失敗都會紅。
  另斷言 keysym 正反查(`BackSpace → 0x00FF08`、未知鍵名 → 0)—— `rime_shell.cc`
  重宣告的那兩個私有符號靠 C++ mangling 對上,連得起來不等於接到對的函式。
  **產生器用 Ninja + vcvars,不要換回 Visual Studio 產生器**:VS 產生器的名字帶著
  VS 版本號,會把 CMake 版本與 runner 的 VS 版本綁死,第一版就是這樣掛掉的。

  第二個里程碑(TSF,本輪):瘦 DLL(`rime_tsf.dll`,只做 TSF 協議 + 按鍵映射
  + IPC,**不含 librime**)加獨立服務進程(`rime_service.exe`,rime_shell +
  librime + 候選窗),兩者以具名管道通訊(DACL 只授權目前使用者的 SID)。
  按鍵映射不用常數表 —— 會產生字元的鍵一律問 `ToUnicodeEx(..., hkl)`,
  並以**真實**的德文/法文佈局在 CI 上驗證(`LoadKeyboardLayout`)。
  CI 分成兩個 job:`logic-x64`(不需 librime,約 3 分鐘)與 `core-x64`。
  新驗到的:四個 COM 匯出正好那四個、`rime_tsf.dll` 的相依正好是
  kernel32/user32/advapi32/ole32(守 `/MT`,沒有任何 CRT DLL)、
  58 個單元測試 815 個斷言 + 反向測試、以及**經由真的具名管道**驅動服務
  以 luna_pinyin_tw 打出「你好」。既有的 `rime_console` 核心驗證不回歸。

  **驗不到的(完整清單見 `windows/README.md`「沒有被驗證的部分」):**
  regsvr32 註冊、TSF 的 Activate 與組字、候選窗的樣子與位置、
  在記事本/瀏覽器/Office 裡真的打得出字、每一顆鍵是不是都做了它宣稱的事。
  **CI 綠不等於能用 —— 需要有人在真 Windows 上跑一遍。**

  已知缺口:只有 x64(arm64 未做)、TSF 不給純修飾鍵事件(所以按 Shift 切中英
  做不到)、沒有顯示屬性(組字底線)、沒有系統匣與安裝程式、
  **沒有編 librime-lua**(倚賴 lua_translator/lua_filter 的第三方方案會部署成功
  卻沒有候選;`windows/build.sh` 有一道守門,日後掛上 lua 而沒帶 sandbox patch
  會擋下建置)。
- **iOS** — 未開始。
