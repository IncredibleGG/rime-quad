# Android 模擬器與自動化驗證環境

本文件說明如何在無人值守(CI / agent)的情況下,用 Android 模擬器驗證我們的輸入法
APK 是否「真的能用」——不只是能安裝,而是能被系統認得、能被設為預設輸入法、
軟鍵盤能彈出、按鍵能把字送進輸入框。

所有指令都在 **Linux 建置機** 上執行,不需要圖形介面。

---

## 1. 環境現況

| 項目 | 值 |
| --- | --- |
| 主機 | Ubuntu 26.04 x86_64,20 核 / 91 GB RAM |
| JDK | OpenJDK 17 |
| Android SDK | `/home/lc/Android/Sdk`(`$ANDROID_SDK_ROOT`) |
| Emulator | `emulator` 37.1.11 |
| System image | `system-images;android-35;google_apis;x86_64`(rev 9,Android 15 / API 35) |
| AVD | `rime_test` |
| 硬體加速 | **KVM 可用**(見下節) |

### KVM 狀態

```console
$ ls -l /dev/kvm
crw-rw----+ 1 root kvm 10, 232 ... /dev/kvm

$ getfacl /dev/kvm
user::rw-
user:lc:rw-      # ← 透過 ACL 直接授權給 lc,不需要加入 kvm 群組
group::rw-

$ $ANDROID_SDK_ROOT/emulator/emulator -accel-check
accel:
0
KVM (version 12) is installed and usable.
```

使用者 `lc` **不在** `kvm` 群組裡,但 `/dev/kvm` 上有一條 ACL(`user:lc:rw-`)直接把讀寫權
限給了 `lc`,所以硬體加速完全可用,不需要 `sudo usermod -aG kvm`。

> 如果日後換機器而 `-accel-check` 失敗,請先確認 `/dev/kvm` 是否存在、以及目前使用者
> 能不能開啟它。若使用者既不在 `kvm` 群組也沒有 ACL,需要管理者執行
> `sudo usermod -aG kvm <user>` 後重新登入。沒有 KVM 時模擬器會退回純軟體模擬,
> 開機時間會從十幾秒變成好幾分鐘,自動化基本上不堪用。

### 為什麼選這個 system image

* **`google_apis`**(不是 `google_apis_playstore`):Play Store 版的映像檔 **不能 root**、
  而且 `adb shell` 受限,會擋掉自動化。`google_apis` 版可以 `adb root`,而且仍然帶有
  完整的設定 App(輸入法設定介面、語言與輸入設定)供人工檢查。
* **不是 `default`(AOSP)**:AOSP 映像檔沒有完整的輸入法設定介面,也沒有內建鍵盤可
  當對照組。`google_apis` 內建 Gboard,正好拿來當「已知可用的輸入法」做冒煙測試。

---

## 2. AVD 設定

AVD 建立指令(只需執行一次;檔案在 `~/.android/avd/`,不進 git):

```bash
export ANDROID_SDK_ROOT=$HOME/Android/Sdk
"$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/avdmanager" create avd \
  -n rime_test \
  -k "system-images;android-35;google_apis;x86_64" \
  -d pixel_6
```

建立後對 `~/.android/avd/rime_test.avd/config.ini` 做了以下調整(原檔備份為
`config.ini.orig`):

| 設定 | 值 | 原因 |
| --- | --- | --- |
| `hw.ramSize` | `4096M` | 預設 1536M 對 API 35 太小,容易 OOM |
| `hw.cpu.ncore` | `6` | 加快開機與編譯後的首次啟動 |
| `disk.dataPartition.size` | `6144M` | 預設 800M,裝幾個 APK 與詞庫就滿了 |
| `vm.heapSize` | `512M` | 同上 |
| **`hw.keyboard`** | **`no`** | **關鍵**:設成 `yes` 會讓系統認為有實體鍵盤,**軟鍵盤就不會彈出**,IME 測試會整組失效 |
| `hw.gpu.mode` | `swiftshader_indirect` | 無頭環境沒有 GPU,用軟體 GL |
| `hw.audioInput` / `hw.audioOutput` | `no` | 遠端主機沒有音效裝置 |
| `hw.camera.*` | `none` | 加快開機 |
| `firstboot.*Snapshot` / `fastboot.forceFastBoot` | `no` | 每次乾淨開機,確保驗證結果可重現 |
| `showDeviceFrame` | `no` | 無頭不需要外框 |

---

## 3. 腳本

全部放在 `scripts/`,都可以直接執行,不需要先 `cd` 到專案根目錄
(腳本會自己從 `$BASH_SOURCE` 推導專案路徑)。

### `scripts/emu.sh` — 模擬器控制

```bash
./scripts/emu.sh start [--cold]   # 無頭啟動,並「等到真的開機完成」才返回
./scripts/emu.sh stop             # 乾淨關閉
./scripts/emu.sh status           # 顯示狀態(未執行時離開碼為 1)
./scripts/emu.sh install <apk>…   # adb install -r -g -t
./scripts/emu.sh shot <out.png>   # 截圖
./scripts/emu.sh ime-list         # 列出全部 / 已啟用 / 目前預設的輸入法
./scripts/emu.sh ime-enable <id>  # ime enable + ime set,並驗證真的設定成功
./scripts/emu.sh logcat [tag]     # 抓日誌(給 tag 就只看那個 tag)
./scripts/emu.sh shell …          # 轉發 adb shell
./scripts/emu.sh adb …            # 轉發 adb(已帶 -s <serial>)
```

`start` 的等待邏輯**不是固定 sleep**,而是依序輪詢:

1. `emulator-5554` 出現在 `adb devices` 且狀態為 `device`
2. `adb shell getprop sys.boot_completed` 等於 `1`
3. `adb shell pm path android` 有回應(package manager 真的可用)

同時會監看模擬器行程,行程一死就立刻報錯,不會傻等到逾時。開機完成後會關閉三種動畫
(讓截圖穩定)並解除鎖定畫面。

可用環境變數:`ANDROID_SDK_ROOT`、`RIME_AVD`、`RIME_EMU_PORT`、`RIME_BOOT_TIMEOUT`、
`RIME_EMU_GPU`、`RIME_RUN_DIR`。要同時跑多台模擬器就改 `RIME_EMU_PORT`(例如 5556)。

### `scripts/build_testapp.sh` — 測試靶 APK

產生一個極小的 APK(`dev.rime.imetest`),畫面上只有一個 `EditText`,開啟時強制彈出軟鍵盤。
用途是給驗證流程一個**穩定、與 Google 內建 App 無關**的輸入目標。

重點:它**不用 Gradle**,直接呼叫 build-tools 的 `aapt2` / `d8` / `zipalign` / `apksigner`,
所以離線也能建,幾秒就好,不會跟主專案的 Gradle 設定互相干擾。

```bash
./scripts/build_testapp.sh     # → build/imetest/rime-imetest.apk
```

輸入框的 `content-description` 是 `rime_test_input`,方便 uiautomator 定位。

### `scripts/verify_ime.sh` — 端到端驗證

這是**主要的驗收入口**。IME id 完全參數化,換成我們自己的輸入法就能直接重用。

```bash
# 冒煙測試(用內建 Gboard 確認整條鏈路本身沒壞)
./scripts/verify_ime.sh

# 驗證我們自己的輸入法
./scripts/verify_ime.sh \
  --ime dev.rime.ime/.RimeInputMethodService \
  --apk android/app/build/outputs/apk/debug/app-debug.apk \
  --text "nihao"
```

主要選項:

| 選項 | 說明 |
| --- | --- |
| `--ime <pkg>/<service>` | 待測 IME id(預設 Gboard) |
| `--apk <path>` | 驗證前先安裝這個 APK(可重複) |
| `--text <str>` | 要注入的文字(預設 `hello.rime`) |
| `--expect <str>` | 預期輸入框內容(預設同 `--text`) |
| `--target testapp\|settings` | 輸入目標,預設 `testapp` |
| `--out <dir>` | artifact 輸出目錄(預設 `build/verify`) |
| `--no-start` | 不自動啟動模擬器 |
| `--restore-ime` | 結束後把預設輸入法還原 |

驗證步驟與各步的判定依據:

| 步驟 | 判定依據 |
| --- | --- |
| 1 模擬器就緒 | `sys.boot_completed=1` 且 `pm` 可用 |
| 2 安裝 APK | `adb install` 離開碼 |
| 3 系統認得 IME | `ime list -a -s` 中有完全相符的一行 |
| 4 設為預設 | `settings get secure default_input_method` 等於待測 id |
| 5 開啟輸入畫面 | `am start -W` |
| 6 **軟鍵盤真的彈出** | `dumpsys input_method` 的 `mIsInputViewShown=true`,且 `mCurId` 等於待測 id;再交叉檢查 `dumpsys window` 裡的 `InputMethod` 視窗 `package=` 是待測 package |
| 7 截圖 | `01-keyboard.png` |
| 8 清空輸入框並注入文字 | `input keyevent KEYCODE_DEL` ×N,再 `input text` |
| 9 **文字真的進入輸入框** | `uiautomator dump` 後解析 XML,取 `focused=true` 的 `EditText` 的 `text` 屬性比對 |
| 10 keyevent | 送 `KEYCODE_DEL`,確認內容長度變短 |
| 11 截圖 | `02-typed.png` |
| 12 logcat | 檢查有無 `FATAL EXCEPTION` / `ANR` |

任何一步失敗都會以離開碼 `1` 結束,並印出明確訊息說明卡在哪。artifact 一律保留在
`--out` 目錄,方便事後查。

---

## 4. 典型用法

```bash
# 一鍵:啟動 + 驗證 + 關閉
./scripts/emu.sh start
./scripts/verify_ime.sh --no-start --ime <我們的 IME id> --apk <我們的 APK>
./scripts/emu.sh stop
```

CI 裡建議 `verify_ime.sh` 不帶 `--no-start`(它會自己確保模擬器就緒),
最後再 `emu.sh stop`。

---

## 5. 效能實測

在本機(KVM 開啟、`-gpu swiftshader_indirect`、無頭)實測:

| 項目 | 耗時 |
| --- | --- |
| 首次冷開機(要初始化 userdata) | **22 秒** |
| 之後每次 `emu.sh start`(`-no-snapshot`,乾淨開機) | **16–17 秒**(重複多次都穩定) |
| `verify_ime.sh` 完整 12 步 | **約 17 秒** |
| **啟動 + 完整驗證 + 關閉** | **約 35 秒** |
| APK 安裝(12 KB 測試靶) | < 0.1 秒 |
| 截圖 | < 1 秒 |

有 KVM 的情況下速度完全足以放進 CI。**沒有 KVM 的話這些數字大約會惡化一個數量級**
(冷開機通常要 5–10 分鐘),屆時要考慮改用 snapshot 或改跑實體裝置。

---

## 6. 已驗證的內容

用系統內建的 Gboard(`com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME`)
把整條鏈路實際跑通,並且**人工檢視過截圖確認畫面上真的有鍵盤、文字真的進到輸入框**:

* `ime list -a` 列得出來 → `ime enable` / `ime set` 成功 → `default_input_method` 確實改變
* 開啟輸入畫面後 `mIsInputViewShown=true`,`InputMethod` 視窗屬於該 IME 的 package
* 截圖上是完整的 QWERTY 軟鍵盤(含候選字列)
* `input text "hello.rime"` 後,uiautomator 讀回輸入框內容為 `hello.rime`,截圖也看得到
* `KEYCODE_DEL` 後內容變成 `hello.rim`
* 兩種輸入目標(自建 `testapp`、系統設定搜尋框 `settings`)都通過
* 錯誤路徑也測過:餵一個不存在的 IME id,腳本以離開碼 1 失敗並印出可行動的訊息

---

## 7. 已知限制與注意事項

1. **`hw.keyboard` 必須是 `no`。** 若之後有人重建 AVD 而忘了這一項,軟鍵盤不會彈出,
   `verify_ime.sh` 會在第 6 步失敗。這是最容易踩的坑。
2. **不要換成 `google_apis_playstore` 映像檔**:不能 root,而且會擋掉部分自動化。
3. **`uiautomator dump` 讀空輸入框時會回報 hint 文字**(例如 `type here`),不是真的有殘留。
   腳本已針對這點做處理,只印 INFO 不判失敗。
4. **候選字 / 組字(composing)尚未驗證。** 目前只驗證了「直送字元」的路徑
   (`input text` 走的是 `commitText`)。RIME 真正的價值在拼音組字與候選字選擇,
   那條路徑需要模擬**實體按鍵事件**而不是 `input text`,見下一節。
5. 模擬器日誌在 `.emulator/emulator-<port>.log`,驗證 artifact 在 `build/verify/`,
   兩者都已加入 `.gitignore`,不會進版控。
6. 同時只跑一台模擬器時 serial 固定是 `emulator-5554`;要並行請用 `RIME_EMU_PORT`。
7. 這台機器沒有 passwordless sudo,所有 Android 元件都只裝在 `~/Android/Sdk`,
   不需要 root 權限。安裝新的 SDK 套件請用 `sdkmanager`,並注意同一時間只能有一個
   `sdkmanager` 行程(會搶 SDK 鎖)。

---

## 8. 接上我們自己的 IME 時要注意什麼

1. **IME id 的格式**是 `<applicationId>/<service 的完整或相對類名>`。
   相對類名要以 `.` 開頭(例如 `dev.rime.ime/.RimeInputMethodService`)。
   填錯的話 `verify_ime.sh` 第 3 步就會擋下來,並印出系統實際看得到的清單。

2. **要被 `ime list -a` 看見**,APK 必須具備:
   * `<service>` 宣告 `android:permission="android.permission.BIND_INPUT_METHOD"`
   * `<intent-filter>` 有 `<action android:name="android.view.InputMethod" />`
   * `<meta-data android:name="android.view.im" android:resource="@xml/method" />`
     且該 `method.xml` 至少有一個 `<subtype>`

   少任何一項,系統都會靜默地忽略這個 IME —— 裝得起來但清單裡看不到。

3. **`ime enable` 之外別忘了 `ime set`。** 只 enable 不 set,鍵盤不會變成預設,
   `emu.sh ime-enable` 兩件事都做了,而且會回讀 `default_input_method` 確認。

4. **驗證組字流程要改用 keyevent,不是 `input text`。**
   `adb shell input text` 走的是 `commitText`,會**繞過 IME 的組字邏輯**,
   所以它只能證明「輸入框收得到字」,不能證明「RIME 的拼音引擎有在運作」。
   要驗證組字,請改送實體按鍵:

   ```bash
   ./scripts/emu.sh shell input keyevent KEYCODE_N KEYCODE_I KEYCODE_H KEYCODE_A KEYCODE_O
   ./scripts/emu.sh shot /tmp/candidates.png   # 檢查候選字列是否出現「你好」
   ./scripts/emu.sh shell input keyevent KEYCODE_1   # 選第一個候選字
   ```

   之後應該在 `verify_ime.sh` 之上再加一個 `verify_rime_compose.sh`,
   斷言「送出 `nihao` 五個 keyevent 後,輸入框內容等於 `你好`」。
   這才是真正端到端的 RIME 驗證。

5. **抓我們自己的日誌**:`./scripts/emu.sh logcat <TAG>`。
   建議在 native 層(librime JNI)統一用一個好認的 tag,例如 `RimeJNI`,
   這樣模擬器上出事時能一行指令抓到。

6. **首次啟動要部署 RIME 資料目錄**(schema、詞庫)。這通常發生在 IME service 第一次
   `onCreate`,可能耗時數秒到數十秒。`verify_ime.sh` 第 6 步的等待上限是 20 秒,
   若我們的部署比這久,要調大該迴圈或在測試前先觸發一次部署。

7. **`adb install -g`** 已經幫忙自動授權 runtime 權限,所以不用擔心權限對話框擋住畫面。
