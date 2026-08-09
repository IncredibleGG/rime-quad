# 產品還沒有名字

**狀態:待使用者決定。** 2026-08-09 使用者看到 Windows 安裝程式的標題「安裝 - RIME 四端輸入法」:

> 「我們軟件名稱都沒 怎麼能叫四端 太難聽了」

**「四端」是開發期的工作代號**(四個平台),不是產品名。`RimeQuad` 同理 —— Quad 也是「四」。
兩個都描述的是**我們怎麼開發它**,不是**使用者拿它做什麼**。

---

## ⚠ 這件事有時效性:一半的東西現在改很便宜,以後改會讓使用者升不上去

### 便宜的(隨時可改,使用者無感)

| 位置 | 現值 |
|---|---|
| Windows 安裝程式標題 | `windows/installer/rimequad.iss` 的 `AppName=RIME 四端輸入法` |
| macOS 顯示名 | `apple/RimeQuad/Resources/*.lproj/InfoPlist.strings` 的 `CFBundleName` / `CFBundleDisplayName` |
| Android 顯示名 | `android/app/src/main/res/values*/strings.xml` 的 `app_name` |
| 各處 UI 文案、README、下載頁 | 到處 |

### 貴的(**發布出去之後就不能改**)

| 位置 | 現值 | 改了會怎樣 |
|---|---|---|
| Android `applicationId` | `org.rimequad.ime`(`android/gradle.properties`) | **變成另一個 app**。使用者無法升級,要解除安裝重裝,**詞典與設定全失**。而且簽章金鑰與輪替鏈是綁在這個 id 上的 |
| macOS bundle id | `org.rimequad.inputmethod.RimeQuad` | 輸入法要重新加入輸入來源;使用者資料目錄改變 |
| macOS `TISInputSourceID` | `org.rimequad.inputmethod.RimeQuad.Hant` / `.Hans` | 同上,而且必須以 bundle id 為前綴 |
| Windows `AppId` | `{7A033CF7-CB91-408E-A653-EF639F4173DB}`(`rimequad.iss` 裡自己也標了「一旦發布出去就不能改」) | 舊版不會被覆蓋,「新增或移除程式」裡會出現兩筆 |
| Windows `CLSID_RimeTextService` | `windows/tsf/guids.h` | 舊的註冊留在登錄檔裡變成孤兒 |
| 使用者資料目錄 | `~/Library/Application Support/RimeQuad`、`%APPDATA%\RimeQuad` | 使用者詞典找不到了 —— 除非寫遷移 |

**現在使用者數是零,所以改識別碼的成本也接近零。** 每多發一版、每多一個使用者,
這個成本就往上跳一階,而且是不可逆的那種。**要改就趁現在。**

---

## 需要的不只是一個字

- **名字本身**(中文 + 英文,英文那個會變成 bundle id / package 的字根)
- **網域**(如果要有官網或用它當 bundle id 的反轉字首)
- 商標衝突檢查 —— 這是開源專案,但名字撞到既有輸入法會很麻煩

## 定位提醒(取名時的素材)

這個產品的主張是**離線為預設、無審查、經得起審計**,對象是**全世界的麻瓜**。
名字最好不要:帶「Rime」以外的技術詞、暗示只支援中文、或聽起來像開發工具。
