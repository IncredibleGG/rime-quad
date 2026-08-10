# 九宮格的產品模型：組字區、讀音欄、記憶、候選

> 這一份是**調查 + 規格**，不含程式碼變更。它回答使用者拿三星輸入法對照之後
> 提出的五個問題。
>
> 使用者的原話：
>
> > 「三星的輸入法他這邊就顯示得特別好，他是有記憶的，他會認為你想要打什麼，
> > 如果不對再選」
> > 「但是你顯示 PGM 就很奇怪？我感覺安卓的精髓還是沒做好，是產品沒調查明白
> > 還是咋回事？」
>
> **這份文件的價值在於「不猜」。** 每一條都標了它是怎麼查到的。查不到的集中在
> §6，沒有用推測填。§6 有六條，請把它當成這份文件的一部分讀，不是免責聲明。

## 證據等級的寫法

本文每一項結論後面括號裡的標記，意思固定：

| 標記 | 意思 |
|---|---|
| **〔實測〕** | 我在這一輪真的跑出來的。附指令或截圖檔名。 |
| **〔原始碼〕** | 讀 librime／本專案原始碼得到的。附檔案與行號或函式名。 |
| **〔競品資產〕** | 從競品 APK 解出來的設定檔。附路徑。 |
| **〔官方文件〕** | 廠商自己的公開說明頁。附網址。 |
| **〔使用者回報〕** | 只有使用者的截圖描述，我沒有獨立驗證。 |
| **〔查不到〕** | 查了，沒有可靠來源。見 §6。 |

## 這一輪動到了什麼（先講，方便別條線判斷有沒有被影響）

- **沒有下載任何檔案。** 用的是 2026-08-07 那一輪已經下載好的
  `build/competitors/*.apk`，以及**已經裝在** `rime_compete` AVD 上的競品。
- 啟動了 `rime_compete`（**port 5562**）。**沒有碰 emulator-5554 / 5556 的畫面或
  輸入法設定**，也沒有下過 `adb root`。
- 5562 上我把預設輸入法切成語燕、後來切成 Gboard。過程中我用
  `settings put secure enabled_input_methods` 覆寫了「已啟用輸入法」清單，
  **這是個錯誤**（它會把其他三個 IME 從清單裡刪掉）。已用 `ime enable` 把
  語燕／Gboard／Trime／fcitx5 四個全部補回並確認。沒有安裝或解除安裝任何 App。
- 5554 上只跑 `rime_console`（唯讀引擎測試），在 `/data/local/tmp/rime/` 底下建過
  `u_t9s`、`u_lp` 兩個測試用 user 目錄與幾個 `o_*.txt`、`memtest*.sh`，**已清除**。
- 截圖存在 `docs/reference/yuyan/t9study-*.png`（該目錄在 `.gitignore` 第 79 行，
  不進版控）。

實測用的競品是 **語燕 YuyanIme v4.3.3**（`com.yuyan.pinyin.release`）。
為什麼不是最新版：最新版在這台 AVD 上鍵盤被畫到螢幕外，見
`docs/competitive-review.md` §2.1。**語燕本身就是 Rime 做的**，所以它是我們
架構上最近的參照物 —— 它遇到的每一個問題我們都會遇到。

---

## 0. 摘要：六件事

1. **`PGM` 不是 bug，是我們把 librime 的 preedit 原樣送進宿主輸入框。**
   而 librime 的 preedit 照定義就是「你按了什麼」，它**不會**回推成規範拼音。
   回推成 `qin` 的是**另一個東西** —— 候選的 `comment`。見 §1。
2. **語燕在宿主輸入框裡什麼都不放。** 組字顯示在鍵盤自己的左上角，內容是
   `qin` / `ni'hao` —— 那是**高亮候選的 comment**，不是 preedit。見 §1。
3. **讀音欄不是常駐欄。** 語燕實測：沒打字時是標點，打字時變讀音，
   **讀音一旦選定又變回標點**。它是「組字中的收斂器」。使用者看到三星常駐，
   是因為他一直在組字中。見 §2。
4. **「有記憶」有三種，我們有一種、缺一種、另一種完全沒生效。**
   使用者詞典**有生效**（有實測證據）；「上次在這個模糊碼下選過哪個讀音」
   **完全沒有**；八股文**語言模型**（`grammar`）**根本沒有被編進來**，
   `__patch: - grammar:/hant?` 的那個 `?` 讓它靜靜地無效。見 §3。
5. **一頁 5 個是 librime 的預設值，語燕也是 5 —— 但語燕畫面上一次給 9 個。**
   因為它**不用 librime 的分頁 API 畫候選**。librime 有一組與 page_size 無關的
   候選迭代器，**我們的 ABI 沒有匯出它**。把 5 改成 9 只是把牆推遠一點。見 §5。
6. **使用者看到的「一排小字」和左欄是同一個功能的兩種擺法**，由主題決定，
   兩條路都真的會改寫輸入串。所以功能是有的 —— 問題是它**看起來不像可以按**。
   見 §2.3。

---

## 1. 組字區到底該顯示什麼？

### 1.1 我們現在顯示什麼，以及為什麼

`RimeInputMethodService.kt:1255`〔原始碼〕：

```kotlin
ic.setComposingText(snapshot.composition.preedit, 1)
```

引擎給的 preedit 就是 `PGM`〔實測〕：

```
$ ./rime_console shared u_t9s PGM 1 t9_pinyin
  [組字後] preedit="PGM" caret=3  schema=t9_pinyin(九宮格拼音) composing=1
  [組字後] 候選 5 個 (page 0, 高亮 0):
        1. 親  # qin
        2. 品  # pin
        3. 秦  # qin
        4. 琴  # qin
        5. 拼  # pin
```

（順帶一提：這五個候選與使用者截圖裡的 `1 亲qin 2 品pin 3 秦qin 4 琴qin`
**完全一致**。他那台是全新的使用者詞典狀態。這條在 §3 會再用到。）

### 1.2 根因：preedit 與 comment 是兩個不同的東西

librime `src/rime/gear/script_translator.cc`〔原始碼〕：

- `ScriptSyllabifier::GetPreeditString()` 組字串的方式是
  **`output += input_.substr(current_pos, next_pos - current_pos)`** ——
  它把**使用者實際輸入的字元**照抄，只在音節邊界插入 `delimiters.at(0)`。
  **它的設計目的就是「顯示你按了什麼」，不是「顯示引擎聽懂了什麼」。**
- 真正會回推成規範拼音的是 `ScriptTranslator::Spell()`（同檔 308 行）：
  ```cpp
  dict_->Decode(code, &syllables);
  result = boost::algorithm::join(syllables, string(1, delimiters_.at(0)));
  ```
  而 `Spell()` 的輸出被 `ScriptSyllabifier::GetOriginalSpelling()`（437 行）拿去
  當成候選的 **comment**，受 `translator/spelling_hints` 的音節數上限管。

**結論：把 `PGM` 換成 `qin` 不是設定問題。** 沒有任何 `preedit_format` 能做到，
因為 preedit 手上根本沒有那個資訊。要顯示 `qin`，就得**改用 comment 當組字顯示**。

好消息是那份資料我們早就拿得到了 —— 上面那段 `# qin`／`# pin` 就是。

### 1.3 語燕怎麼做（實測）

〔實測〕`docs/reference/yuyan/t9study-03.png`、`-05.png`：

| 按鍵 | 宿主輸入框 | 鍵盤左上角標籤 | 候選列 |
|---|---|---|---|
| （未打字） | 空 | 無 | 無（`t9study-00.png`） |
| 7 4 6 | **空** | `qin` | 琴 拼 秦 亲 品 勤 寝 擒 频 |
| 6 4 4 2 6 | **空** | `ni'hao` | 你好 你敢 米高 你搞 你干 米糕… |

兩件事：

1. **宿主輸入框全程是空的。** 截圖裡 `type here` 的 placeholder 一直在。
   語燕**不呼叫 `setComposingText`** —— 組字完全發生在鍵盤自己的畫面裡。
2. **那個標籤的內容是 comment，不是 preedit。** 證據是 `ni'hao` 中間那個 `'`：
   語燕的 `speller/delimiter` 是 `"''"`〔競品資產〕，所以
   `Spell()` 的 `delimiters_.at(0)` 正好是 `'`，`join` 出來就是 `ni'hao`。
   而且標籤內容永遠等於**第一個（高亮）候選**的讀音：
   候選是 琴(qin) 時標籤是 `qin`，點了 `pin` 之後第一候選變 拼(pin)、
   標籤同步變 `pin`（`t9study-04.png`）。

語燕的 schema 也印證了這個用法〔競品資產〕
（`assets/rime/build/t9_pinyin.schema.yaml`）：

```yaml
translator:
  always_show_comments: true
  dictionary: pinyin
  initial_quality: 1.2
  prism: t9_pinyin
  spelling_hints: 100
```

`always_show_comments: true` + `spelling_hints: 100` —— 它非常在意 comment 拿得到。
（我們是 `spelling_hints: 50`，夠用；`always_show_comments` 我們沒設。）

### 1.4 已確定的音節與未確定的部分怎麼區分？

**語燕不區分 —— 因為它的畫面上沒有「未確定的尾巴」這個概念。**〔實測〕

它永遠顯示一個**完整、讀得出來**的拼音串（`ni'hao`），那是引擎目前最佳猜測的
整條路徑。使用者點左欄的 `ni` 之後，串**還是**完整的拼音，只是第一個音節被釘住了。
音節之間用 `'` 分隔，如此而已。

我們現在的模型不一樣：`InlinePreedit`（`CandidateBarModel.kt`）把還沒確定的
代表字母**砍掉**，補一個 `⋯`，做出 `ni⋯`。那是在「preedit 裡混著拼音與代碼」
這個前提下的正確補救 —— 但如果組字顯示改成走 comment，**這個前提就不存在了**，
`⋯` 也就不需要了。

搜狗〔官方文件〕的說明頁只講「鍵盤上方會出現一行候選詞」，沒有提到在宿主
輸入框裡放拼音。三星的宿主輸入框顯示什麼 —— 〔查不到〕，見 §6。

### 1.5 規格

**S1-1　組字期間不要在宿主輸入框裡放九宮格代碼。**
兩個可接受的做法，擇一：

- **(A) 跟語燕：宿主輸入框全程不放組字。** 組字只顯示在鍵盤上。
  最乾淨，但要注意宿主 app 的游標與自動完成行為會少一個提示。
- **(B) 放，但放 comment 而不是 preedit。** 即高亮候選的讀音（`qin`／`ni'hao`）。

**建議 (B)**，理由：Android 的無障礙服務、密碼欄位偵測、以及部分輸入框的
「輸入中」樣式都靠 composing region；整個拿掉是一個影響面比看起來大的改動。
而且 (B) 對全拼／注音方案是**零變更**（它們的 preedit 本來就等於使用者按的東西，
comment 與 preedit 一致）。

**S1-2　組字顯示的資料來源統一成「高亮候選的 comment，取不到才退回 preedit」。**
退回這條必須有，因為 `spelling_hints` 有音節數上限、而且不是每個方案都給 comment。
退回時顯示 preedit 原樣 —— 對全拼／注音那就是正確答案。

**S1-3　音節分隔字元跟著方案走，不要寫死。**
用 `speller/delimiter` 的第一個字元（librime `Spell()` 就是這樣做的）。
我們的 `t9_pinyin` 是 `" '"`，第一個是**空白**，所以會得到 `ni hao`；
語燕是 `"''"`，得到 `ni'hao`。哪個好看是另一件事，但**規則要跟引擎一致**，
否則畫面上的分隔與引擎的切分會對不上。

---

## 2. 讀音欄是什麼東西？

### 2.1 它是「組字中的收斂器」，不是常駐欄（實測推翻了原本的假設）

〔實測〕語燕 v4.3.3，同一個鍵盤的三個時刻：

| 時刻 | 左欄內容 | 截圖 |
|---|---|---|
| 沒打字 | `,` `。` `?` `!` `…`（標點） | `t9study-00.png` |
| 打了 7 4 6 | **`qin` `pin` `si` `ri` `pi`** | `t9study-03.png` |
| 點了 `pin` 之後 | **變回標點** `,` `。` `?` `!` `…` | `t9study-04.png` |

第三行是關鍵：**讀音選定、輸入串不再有歧義之後，那一欄就退場了。**
所以它不是常駐欄，是「有歧義時才出現」的東西 —— 這一點我們原本的設計
（`T9Syllables` 檔頭寫的「消歧欄」）**本來就是對的**。

使用者覺得三星那一欄是常駐的，最可能的解釋是他截圖時一直在組字中。
三星是不是真的常駐 —— 〔查不到〕，見 §6。

### 2.2 選了讀音之後：真收斂，不是篩選

〔實測〕點 `pin` 前後的候選列：

| | 候選 |
|---|---|
| 點之前 | 琴 拼 秦 亲 品 勤 寝 擒 频 |
| 點 `pin` 之後 | 拼 品 频 **贫 聘 嫔 颦 牝 姘** |

粗體那六個**點之前根本不在列表裡**。這證明語燕不是把不合的候選藏起來，
而是**把輸入串改寫後讓引擎重新翻譯**。

搜狗〔官方文件〕的說法一致：「在選擇音節後，輸入界面上方候選詞會產生相應變化。」
（<https://shouji.sogou.com/bangzhu.php?pid=5&cid=53>）

**我們現在也是真收斂**（`T9Syllables.rewriteInput()` + `rs_set_input()`）。
這條路是對的，不用改。

### 2.3 但我們有兩套並存的機制，而使用者踩到的是弱的那一套

`KeyboardView.kt:163-176`〔原始碼〕：讀音欄的位置由**主題**決定，
`SyllablePlacement` 有 `keyboard_slot`（左欄）與 `above_candidates`（上方一排）。
註解自己寫著：

> 使用者的外觀設定是 iPhone 慣例，所以他會看到上方那一排，而不是左欄。

這就是使用者截圖裡「鍵盤上方一排小字 `qin  pin`」的來源。**它不是壞掉，
是另一種擺法。** 兩條路都送 `KeyboardEvent.SelectSyllable`，都會真的改寫輸入串
〔原始碼，`KeyboardView.kt` 的 `onPick` 與 `onSlot` 兩個呼叫點〕。

問題出在**別的地方**：

- 上方那一排是 `BasicText`，沒有鍵框、沒有背景、沒有按下狀態
  〔原始碼，`SyllableRow`〕。它**看起來就是一行說明文字**。
  使用者的原話是「鍵盤上方一排小字」—— 他沒有想到那可以按。
- 語燕與搜狗的讀音都畫成**鍵**（有框、佔一整格、在拇指構得到的地方）。
- 而且 `above_candidates` 這條路 `slotIds` 是空的，於是
  `disambiguating = slotIds.size >= MIN_SLOTS && hasReadings` 恆為 false
  〔原始碼，`KeyboardView.kt:181`〕→ `pin` 恆為 null → 候選列的
  `visibleIndices` 篩選那一套**在這個擺法下完全不會作用**。

也就是說：**同一個功能在兩種主題下的行為不只是位置不同，連候選列會不會跟著
收斂都不同。** 這是一個真實的不一致。

### 2.4 它與候選列的關係

一句話：**讀音欄是候選列的上游，不是它的過濾器。**

點讀音 → 改寫輸入串 → 引擎重新翻譯 → 候選列整個換掉。
候選列永遠只反映引擎當前的狀態，它自己不做任何篩選。

**這條規則一旦寫死，`T9Syllables.visibleIndices()` 那一整套篩選（以及檔頭那條
「高亮的那一個永遠不准被篩掉」的鐵律）就該退場。** 那是「引擎做不到收斂」
年代的補丁，現在引擎做得到了，兩套並存只會讓行為隨主題而變（見 §2.3）。

### 2.5 幾個、怎麼捲

| | 讀音欄的長度 | 出處 |
|---|---|---|
| 語燕 | **5**（左欄五格，實測沒看到捲動） | 〔實測〕`t9study-03.png` |
| 搜狗 | **左側 4 個常用音節**；展開候選頁下方另有「音節選擇區」列出**所有**組合 | 〔官方文件〕 |
| 三星 | 4 個，可捲 | 〔使用者回報〕 |
| 我們 | 左欄 3 格（第 3 格會讓給 `⋯`，實際 2 個 + 翻頁） | 〔原始碼〕`cn-t9-pinyin.yaml:182` |

搜狗那個「兩層」設計值得注意：**左側只給 4 個最可能的（快捷），完整清單放在
展開頁**。這比我們現在「3 格裡有 1 格是翻頁鍵」好 —— 我們實際上一次只給
使用者看 2 個讀音。

### 2.6 規格

**S2-1　讀音欄的語意是「組字中的音節選擇器」。**
沒有歧義（讀音候選 < 2）時不出現；選定後若後續已無歧義，就退回原本的鍵面。

**S2-2　它必須看起來像鍵。**
不論擺在左欄還是上方一排，都要有鍵框／背景／按下狀態，尺寸要是可點的目標。
`above_candidates` 那一排現在不符合。

**S2-3　候選列不做篩選。** 收斂一律走「改寫輸入串 → 引擎重譯」。
兩種擺法的行為必須一致。

**S2-4　讀音的格位數建議提到 4–5，並保留「更多」到展開頁。**
現在的 3 格（實際 2 個）太少。⚠ 這牽涉 `core/layouts/cn-t9-pinyin.yaml` 的
`syllable_slots`，是協調端的檔案，**只提案**。

---

## 3. 「有記憶」是指什麼？

使用者說的「有記憶、會認為你想打什麼」，拆開來至少是三件不同的事。
我逐一查了我們現在有沒有。

### 3.1 (a) 使用者詞典的詞頻 —— **有，而且真的在作用**

〔實測〕方法：在模擬器上用 `rime_console` 開一個全新的 user 目錄，比較排序。

```
# 全新 user 目錄，第一次打 PGM（九宮格）
1. 親  # qin    2. 品  # pin    3. 秦  # qin    4. 琴  # qin    5. 拼  # pin

# 在同一個 user 目錄，改用 luna_pinyin 打 qin 並反覆選字若干次之後，
# 再用 t9_pinyin 打 PGM：
1. 親  # qin    2. 秦  # qin    3. 琴  # qin    4. 勤  # qin    5. 品  # pin
```

排序變了：`勤` 從前五名外進來，`品`／`拼` 被擠下去。userdb 裡也看得到累積：

```
$ strings u_lp/luna_pinyin.userdb/*.ldb
  qin     c=4 d=3.92632 t=10
```

**兩個附帶結論：**

1. `enable_user_dict: true`〔原始碼，`t9_pinyin.schema.yaml`〕確實生效。
2. **九宮格與全鍵盤共用同一份使用者詞典。** 因為 `t9_pinyin` 的
   `dictionary: luna_pinyin`，userdb 檔名跟著詞典走，兩個方案都是
   `luna_pinyin.userdb`。**使用者在全鍵盤上養出來的習慣，切到九宮格照樣算數。**
   這是我們相對於「九宮格是寫死的 Java 邏輯」那類競品的一個真實優勢，
   目前沒有任何地方講給使用者聽。

**但也有壞消息：** 在同一個編碼下連選 8 次「勤」，`qin` 的排序**沒有翻轉**，
`親` 還是第一〔實測〕。原因是詞典本身給 `親` 的權重遠高於 8 次選取帶來的加成。
所以**使用者在短期內感覺不到「它記得我」是完全合理的** —— 這不是壞掉，
是這套加權的性質。

### 3.2 (b) 上次在同一個模糊碼下選過的讀音 —— **完全沒有**

`pinnedSyllable` 活在 Compose 的 `remember` 裡，而且
`LaunchedEffect(state.preedit) { pinnedSyllable = null }`〔原始碼，
`KeyboardView.kt:140-143`〕—— **組字一變就清掉，從來不落地。**

也就是說：使用者這次為 `746` 選了 `pin`，下次再打 `746`，我們照樣先給他 `qin`。

**我認為使用者說的「有記憶」最可能就是這件事。** 它比 (a) 明顯得多：
(a) 要累積很多次才看得出來，(b) 第二次就看得出來。而且它正是他那句
「他會認為你想要打什麼，如果不對再選」的字面意思 —— 先猜一個，猜錯你再改，
改過之後它記住。

三星是不是這樣做的 —— 〔查不到〕。但**這是我們可以做、而且成本最低的一項**：
一張 `模糊碼 → 上次選的讀音` 的小表就夠了，不需要動引擎。

### 3.3 (c) 語言模型 —— **沒有生效，而且是靜靜地沒生效**

這一條要講清楚，因為交接文件裡寫著「我們已經有八股文語言模型」，**那句話只對一半**。

**第一層：設定根本沒載進來。**
`t9_pinyin.schema.yaml` 結尾〔原始碼〕：

```yaml
__patch:
  # 使用八股文語言模型（rime-essay）
  - grammar:/hant?
```

那個 **`?` 是「可選」的意思** —— 檔案不存在就跳過。而
`core/data/shared/` 裡**沒有 `grammar.yaml`**〔實測，`ls` 與 `find` 都沒有〕。
所以這一行從來沒有做過任何事，而且不會有任何錯誤訊息。

**第二層：就算把 `grammar.yaml` 補上也沒用。**
`grammar` 在 librime 裡是一個**元件**，要由外掛註冊。
`src/rime/gear/poet.cc:74-76`〔原始碼〕：

```cpp
inline static Grammar* create_grammar(Config* config) {
  if (auto* grammar = Grammar::Require("grammar")) {
    return grammar->Create(config);
  }
  ...
```

全 librime 原始碼裡**只有這一處**提到 `"grammar"` 這個元件名 —— 沒有任何地方
註冊它。註冊它的是 **librime-octagram** 外掛，而我們
`third_party/librime/plugins/` 底下**只有 `lua`**〔實測〕。
於是 `Grammar::Require()` 回 nullptr，`grammar_` 是空的。

**第三層：後果是什麼。**
`src/rime/gear/grammar.h`〔原始碼〕：

```cpp
const double kPenalty = -13.815510557964274;   // log(1e-6)
return entry_weight +
       (grammar ? grammar->Query(context, entry_text, is_rear) : kPenalty);
```

沒有 grammar 時，**每一個候選都加同一個常數**。也就是說
**上下文對排序的貢獻是零** —— 前面打了什麼，完全不影響後面怎麼排。

**第四層：那 `essay.txt` 是幹嘛的？**
它**有**在用，但角色完全不同。`luna_pinyin.dict.yaml:33` 有
`use_preset_vocabulary: true`〔原始碼〕，而
`src/rime/dict/dict_settings.cc:60` 的
`kDefaultVocabulary = "essay"`〔原始碼〕—— 也就是說 `essay.txt` 是在
**編譯詞典時**併進去的**靜態字詞頻率**。

靜態字頻 ≠ 上下文語言模型。前者讓「的」永遠排前面，後者讓「你」後面更容易接
「好」。**我們有前者，沒有後者。**

### 3.4 規格

**S3-1　先把 (b) 做出來，它的投資報酬率最高。**
一張持久化的 `模糊碼 → 上次選定的讀音序列` 表。命中時**預先套用**，
讀音欄仍然照常出現讓使用者改（這正是「如果不對再選」）。
要點：這是**前端的記憶**，不進 librime 的 userdb，因此不影響詞庫匯出格式。

**S3-2　不要宣稱我們有語言模型，直到 octagram 真的編進來。**
`docs/` 與 UI 文案裡凡是講「八股文語言模型」的地方都要改成「八股文字頻」。

**S3-3　`grammar:/hant?` 這一行要嘛補齊要嘛刪掉，不要留著。**
一行「看起來有做、實際不會做任何事、而且不會報錯」的設定，比沒有更糟 ——
它讓後來的人以為這件事已經解決了。⚠ 這是 `core/data/schemas/` 的檔案，只提案。

**S3-4　把「九宮格與全鍵盤共用使用者詞典」講給使用者聽。**（§3.1）

---

## 4. 選了一個讀音之後，候選應該長什麼樣？

### 4.1 兩家都給乾淨的

〔實測〕語燕點 `pin` 之後：拼 品 频 贫 聘 嫔 颦 牝 姘 —— **9 個全是 pin，沒有一個 qin**。

〔實測〕我們的引擎也做得到。`t9_pinyin` 是雙編碼方案，小寫拼音直接餵得進去：

```
$ ./rime_console shared u_t9s qin 1 t9_pinyin
  [組字後] preedit="qin" caret=3  schema=t9_pinyin(九宮格拼音) composing=1
  [組字後] 候選 5 個 (page 0, 高亮 0):
        1. 親    2. 秦    3. 琴    4. 勤    5. 欽
```

乾淨。所以**這是產品選擇，不是能力問題** —— 使用者在提問裡也已經自己驗過了。

### 4.2 答案：乾淨的

理由不是好看，是**可預期**：讀音欄的意思是「我告訴你這一段念 pin」。
如果候選裡還混著 qin，那句話就沒有被兌現，使用者下一次就不會再相信那一欄。

### 4.3 順帶一件事：選定之後不要再在每個候選旁邊印讀音

使用者截圖裡候選是 `1 亲qin  2 品pin  3 秦qin  4 琴qin` —— 每個候選都掛著讀音。
**語燕的候選列不印 comment**〔實測，`t9study-03.png` 只有漢字〕，即使它的 schema
設了 `always_show_comments: true` —— 那個 comment 被拿去畫左上角的組字標籤了（§1.3）。

我們現在是印的。主題**有**一個開關〔原始碼，`core/themes/default-light.yaml`
的 `candidates.comment.show`〕，預設 `true`，但它只有 `true` / `false` 兩態 ——
沒有「看情況」。建議：

- **有歧義時**（讀音 ≥ 2）：印，它幫助使用者理解為什麼候選是混的。
- **讀音已選定 / 只有一個讀音時**：不印。此時每個候選的讀音都一樣，
  印出來是同一個字重複 N 次的雜訊。

### 4.4 規格

**S4-1　選定讀音後，候選列只含該讀音的結果**（由引擎重譯保證，不由前端篩選）。

**S4-2　候選的讀音 comment 是條件顯示的**：讀音仍有歧義才顯示。
現有的 `candidates.comment.show` 是布林，要擴成三態
（`auto | always | never`，預設 `auto`；`true`/`false` 沿用為 `always`/`never`
以免既有主題壞掉）。⚠ 這是主題格式，屬〔四端〕規範 §8.6。

---

## 5. 一頁幾個候選、怎麼翻頁？

### 5.1 5 是哪裡來的

〔原始碼〕`core/data/shared/default.yaml`：

```yaml
menu:
  page_size: 5
```

〔原始碼〕而 librime 的**內建預設值也是 5**（`src/rime/schema.cc:32-34`、
`schema.h:40`）：

```cpp
config_->GetInt("menu/page_size", &page_size_);
if (page_size_ < 1) { page_size_ = 5; }
```

### 5.2 競品是多少

| | page_size | 畫面上一次看得到 | 出處 |
|---|---|---|---|
| **我們** | 5 | 5，**而且沒有翻頁入口** | 〔原始碼〕+〔使用者回報〕 |
| **語燕** | **5**（`default.yaml` 裡**完全沒有 `menu:` 區段**，用 librime 預設） | **9** + 展開後 40 個以上 | 〔競品資產〕+〔實測〕 |
| **搜狗** | — | 一行 + 【更多】展開全部；右側有翻頁鈕 | 〔官方文件〕 |
| **三星** | — | 5 + 一顆「…」 | 〔使用者回報〕 |

**語燕那一行是這一節的重點：它的 page_size 明明是 5，畫面上卻一次給 9 個，
展開更是幾十個。**

### 5.3 為什麼語燕做得到

因為 **librime 的候選列不是只能分頁取**。`third_party/librime/src/rime_api.h:458-468`
〔原始碼〕：

```c
Bool (*candidate_list_begin)(RimeSessionId, RimeCandidateListIterator*);
Bool (*candidate_list_next)(RimeCandidateListIterator*);
void (*candidate_list_end)(RimeCandidateListIterator*);
Bool (*candidate_list_from_index)(RimeSessionId, RimeCandidateListIterator*, int);
```

這組迭代器**與 `page_size` 無關**，可以一路取到底（`from_index` 還能懶載入）。
`page_size` 只影響 librime 自己的分頁 API 與「按數字鍵選第幾個」。

**我們的 `core/include/rime_shell.h` 沒有匯出這組 API**〔原始碼〕，只有：

```c
bool rs_select_candidate(rs_session s, int32_t index_on_page);
bool rs_change_page(rs_session s, bool backward);
```

所以我們的候選列**在結構上**就只能一次看 `page_size` 個。

### 5.4 而且我們連翻頁的入口都沒有

`CandidateBarModel.kt` 的 `Pager` 註解已經寫得很清楚〔原始碼〕：

> `rs_change_page` 早就在 ABI 裡、`KeyboardEvent.Page` 也早就在 IME service 裡有
> 處理，但畫面上沒有任何東西會送出它 —— 整條路從頭到尾沒有人走過。

所以使用者說「只有 5 個，下一頁就沒了」是**兩個問題疊在一起**：一頁只有 5 個，
而且沒有下一頁的按鈕。

### 5.5 規格

**S5-1　不要只把 `page_size` 從 5 改成 9。** 那只是把牆推遠一點：
使用者一樣會撞到「就這些了」，只是晚一點。而且 9 之後還會有人說 9 不夠。

**S5-2　正解是候選列改用候選迭代器、不分頁。**
需要 `core/` 在 `rime_shell.h` 補上 `candidate_list_*` 的門面
（回報給協調端，見 §7）。這同時解掉桌面端要的「展開候選網格」，
以及 `Pager` 註解裡提到的「總頁數在 ABI 裡不存在」。

**S5-3　在 S5-2 落地之前的過渡：`page_size` 提到 9，並且把翻頁鍵接上。**
⚠ `core/data/shared/default.yaml` 是協調端的檔案，**這裡只提案，不改**。
提 9 的理由：規範 §8.6.6.3.6 本來就寫 9（現況與規範不符，
`CandidateBarModel.kt` 的註解已經記了這件事），而九宮格一列放得下 9 個
（語燕實測就是 9 個）。

**⚠ 但 S5-3 有一個必須一起處理的副作用：**
`page_size` 同時決定「數字鍵選第幾個候選」的範圍
（`selector.cc:259-263`〔原始碼〕）。九宮格上數字鍵是被 speller 吃掉的，
不受影響；但**全鍵盤與桌面端會受影響**。改之前要確認四端的數字選字行為。

---

## 6. 這幾條我查不到（請當成文件的一部分）

1. **三星中文輸入法的任何內部行為。** 閉源、沒有公開技術文件。
   我手上只有使用者的兩張截圖描述。因此以下全部是**未知**：
   它的讀音欄是不是真的常駐、它的宿主輸入框顯示什麼、它的「記憶」是
   §3 的 (a)(b)(c) 哪一種、它的 page_size 是多少。
   **§2 與 §5 裡搜狗的行為不能拿來替三星背書** —— 它們是兩家公司。
2. **百度輸入法：完全沒查。** 沒有可用的公開技術文件，也沒有安裝
   （要下載 APK = 下載檔案，本輪沒有取得許可，我沒有做）。
3. **Gboard 的拼音九鍵：沒有實測。** 模擬器上的 Gboard 只有英文佈局
   （`docs/reference/gboard/t9study-no-chinese.png`），加中文要下載語言包。
   同上，我沒有下載。
4. **語燕最新版（v20260530.10）的 T9 行為：沒測。** 實測用的是 v4.3.3
   （2024-08-23）。最新版在這台 AVD 上鍵盤畫到螢幕外，見
   `docs/competitive-review.md` §2.1。兩年的差距，行為可能已經不同。
5. **語燕究竟怎麼把九宮格鍵位送進 Rime。** 它出貨的 built schema
   〔競品資產〕`speller/alphabet` 是完整的大小寫英文字母，**而且沒有 `algebra` 段**
   （prism 是預先編好的 `.prism.bin`）。所以「一鍵三字母」是在哪一層展開的，
   從資產推不出來。我**沒有反編譯它的 Java 程式碼**。
6. **我沒有在真機／模擬器上重現使用者那張截圖。**
   §1、§2.3 對他畫面的解釋是**從原始碼推的**（`setComposingText` + 主題的
   `above_candidates`），與他的描述完全吻合，但我沒有實際跑我們的 APK 去證實。
   要證實得切換共用模擬器的預設輸入法，會影響別條線。
   **這是這份文件裡最需要有人補的一條。**

---

## 7. 提案（依優先序）

分三類標注：**〔本端〕** = Android；**〔協調端〕** = `core/`，只能提案；
**〔四端〕** = 要寫進規範。

| # | 提案 | 誰的檔案 | 依據 |
|---|---|---|---|
| **P0-1** | 組字顯示改走「高亮候選的 comment」，取不到才退回 preedit | 〔本端〕`RimeInputMethodService.kt` | §1 |
| **P0-2** | 讀音欄的兩種擺法行為要一致；`above_candidates` 那一排要畫成鍵 | 〔本端〕`KeyboardView.kt` +〔四端〕§8.6.6.3 | §2.3 |
| **P0-3** | 拿掉 `visibleIndices` 的候選篩選，收斂一律走引擎重譯 | 〔本端〕`T9Syllables.kt` | §2.4 |
| **P0-4** | 候選列補上翻頁入口（`Pager` 已經寫好，缺的是按鈕） | 〔本端〕 | §5.4 |
| **P1-1** | 記住「這個模糊碼上次選的讀音」，下次預先套用 | 〔本端〕 | §3.2 |
| **P1-2** | `candidates.comment.show: auto`，讀音選定後不再逐候選印讀音 | 〔本端〕+〔四端〕 | §4.3 |
| **P1-3** | `rime_shell.h` 補 `candidate_list_*` 迭代器門面 | 〔協調端〕 | §5.3 |
| **P1-4** | `menu/page_size` 5 → 9（過渡；⚠ 連帶影響數字選字，四端要一起確認） | 〔協調端〕 | §5.5 |
| **P2-1** | 讀音格位 3 → 4 或 5 | 〔協調端〕`cn-t9-pinyin.yaml` | §2.5 |
| **P2-2** | `grammar:/hant?` 補齊或刪掉；文案別再說「語言模型」 | 〔協調端〕+ docs | §3.3 |
| **P2-3** | 把「九宮格與全鍵盤共用使用者詞典」講給使用者聽 | 〔本端〕UI 文案 | §3.1 |

**明確不建議做的：**

- **不要為了「候選要乾淨」而在前端篩選候選。** 那是我們已經走過的路，
  它的失敗形狀是「畫面收斂了、空白鍵送出的還是原本那個」——
  `T9Syllables` 檔頭那條「高亮永遠不准被篩掉」的鐵律就是那次留下的疤。
  引擎已經做得到真收斂，回頭走篩選是退步。
- **不要把 `page_size` 當成候選數量的旋鈕。** 見 S5-1。

---

## 8. 可重現的操作紀錄

```bash
# ── 引擎層（emulator-5554，唯讀，不需要 root）──────────────────────────
adb -s emulator-5554 shell 'cd /data/local/tmp/rime && \
  rm -rf u_t9s && mkdir u_t9s && cp user/default.custom.yaml u_t9s/ && \
  ./rime_console shared u_t9s PGM 1 t9_pinyin'      # → preedit="PGM"
adb -s emulator-5554 shell 'cd /data/local/tmp/rime && \
  ./rime_console shared u_t9s qin 1 t9_pinyin'      # → preedit="qin"，候選乾淨

# ⚠ rime_console 的 stdout 接到管線（`| grep`）時會 segfault；
#   要取內容請先重導到檔案再 pull。這本身可能是一條值得查的缺陷。

# ── 競品實測（emulator-5562，專用 AVD，不影響 5554/5556）──────────────
RIME_AVD=rime_compete RIME_EMU_PORT=5562 ./scripts/emu.sh start   # 16 秒開機
adb -s emulator-5562 shell 'ime set com.yuyan.pinyin.release/com.yuyan.imemodule.service.ImeService'
adb -s emulator-5562 shell 'am start -n dev.rime.imetest/.MainActivity'
# 1080x2400：PQRS=(312,2018)  GHI=(312,1837)  MNO=(766,1837)  ABC=(539,1657)
# 左欄第 2 格（pin）=(107,1741)   重輸=(976,1837)   展開 ∨=(1022,1518)
adb -s emulator-5562 exec-out screencap -p > shot.png

# ⚠ 不要用 `settings put secure enabled_input_methods` 切輸入法 ——
#   它會覆寫整份清單，把其他 IME 從「已啟用」裡刪掉。用 `ime enable` / `ime set`。

# ── 競品資產（不需連網，APK 是上一輪下載好的）─────────────────────────
unzip -o -q build/competitors/yuyanIme_2026053010_release.apk \
  'assets/rime/build/t9_pinyin.schema.yaml' 'assets/rime/build/default.yaml'
```

截圖：`docs/reference/yuyan/t9study-00.png`（未打字）、`-03.png`（打 746）、
`-04.png`（點 pin 之後）、`-05.png`（打 64426）、`-06.png`（展開候選網格）、
`docs/reference/gboard/t9study-no-chinese.png`。
（`docs/reference/` 在 `.gitignore` 第 79 行，不進版控。）
