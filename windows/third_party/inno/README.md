# `Default.isl` — Inno Setup 的英文原文，當作對照用的參考檔

## 為什麼它在版控裡

`windows/check_installer_messages.sh` 的工作是「Inno 內建的每一句話，我們
都覆寫成中文了嗎」。它需要一份**原文清單**當對照。CI 上那份就在 ISCC 旁邊
（`C:\Program Files (x86)\Inno Setup 6\Default.isl`），開發機（Linux）上沒有。

在此之前的做法是設 `REFERENCE_ISL=<某個路徑>`。2026-08-13 覆核時發現：

    $ bash windows/check_installer_messages.sh              # EXIT=1
    $ REFERENCE_ISL=/tmp/Default.isl bash …                 # EXIT=0

也就是說「這一關是綠的」這句話，靠的是一個**不在版控裡、8/9 隨手放在
`/tmp` 的檔案**，換一台機器、或那台機器重開機清掉 `/tmp` 就不成立。
一道要靠口耳相傳的環境變數才會綠的守門，和沒有守門差不多。

所以把它收進版控：**任何一台機器 clone 下來就跑得出同一個結果**。

## 這一份是什麼

| | |
|---|---|
| 版本 | 檔案自己的第一行寫著 `*** Inno Setup version 6.5.0+ English messages ***` |
| 大小 | 24 269 bytes（CRLF，原樣保存，見 `.gitattributes`）|
| sha256 | `b95385bf66ad61f40de6a8d2bbda3ada4d45bc7b0db1fa218a417e776be514fc` |
| 去掉 CR 之後的 sha256 | `14267c5b21dfeb33e1bc65455f7b59d8443132fea13f351e251a316b95b0178e` |
| `[Messages]` 裡的訊息 ID | 281 |

`check_installer_messages.sh` 釘的是**去掉 CR 之後**的那一個 —— 這樣就算
哪天 git 的換行正規化動到它，守門也不會為了一件無關的事亮紅燈。

## ⚠ 一件必須講清楚的事

這一份是**當時那台開發機上已經有的那一份**（`/tmp/Default.isl`，2026-08-09），
它的內容與 Inno Setup 6.5.0 的官方發行版**沒有在本機重新下載比對過**。
我們釘住的是「從今以後大家對照的是同一份」，不是「這一份等於官方那一份」。

真正算數的仍然是 **CI 上那一次**：`check_installer_messages.sh` 一律優先用
ISCC 旁邊的那一份，那是 runner 真的拿去編譯安裝程式的版本。這裡這一份
只在沒有 ISCC 的機器上當替身，而且腳本會明白地說它用的是替身。

## 怎麼更新

Inno 升版、或要換成官方發行版重新核對時：

1. 從一份 Inno Setup 6 的安裝目錄裡把 `Default.isl` 複製過來（就在 `ISCC.exe`
   旁邊），**保持 CRLF 原樣**；
2. 更新 `windows/check_installer_messages.sh` 裡的 `REF_ISL_SHA256`
   （值是 `tr -d '\r' < Default.isl | sha256sum`）；
3. 更新上表；
4. 跑 `windows/check_installer_messages.sh --self-check` —— 新版多出來的
   訊息會當場列出來，那些就是要補翻的。

## 授權

Inno Setup 由 Jordan Russell 與 Martijn Laan 發行，其授權允許重新散布。
這一份是**原文語言檔**，我們只拿它當對照表，不隨產品出貨 ——
安裝程式裡出貨的是 `windows/installer/luminakey.iss` 裡我們自己寫的
`[Messages]`。
