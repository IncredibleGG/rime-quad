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
