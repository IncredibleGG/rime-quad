# 第三方元件授權

本專案本體採 **GPL-3.0-or-later**（見 `LICENSE`）。

以下是編譯進發行二進位檔的第三方元件。所有授權均已逐一核對其原始碼樹中的授權檔，
**沒有任何一項與 GPL-3 衝突**。

| 元件 | 用途 | 授權 | 授權檔位置 |
|---|---|---|---|
| [librime](https://github.com/rime/librime) | 輸入法核心引擎 | BSD-3-Clause | `third_party/librime/LICENSE` |
| [glog](https://github.com/google/glog) | 日誌 | BSD-3-Clause | `third_party/librime/deps/glog/COPYING` |
| [LevelDB](https://github.com/google/leveldb) | 使用者詞典儲存 | BSD-3-Clause | `third_party/librime/deps/leveldb/LICENSE` |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 配置與 schema 解析 | MIT | `third_party/librime/deps/yaml-cpp/LICENSE` |
| [marisa-trie](https://github.com/s-yata/marisa-trie) | 詞庫索引 | BSD-2-Clause **或** LGPL-2.1-or-later | `third_party/librime/deps/marisa-trie/COPYING.md` |
| [OpenCC](https://github.com/BYVoid/OpenCC) | 簡繁轉換 | Apache-2.0 | `third_party/librime/deps/opencc/LICENSE` |
| [Boost](https://www.boost.org/) | 僅使用標頭檔 | BSL-1.0 | 隨下載的原始碼樹 |

## 需要注意的兩點

**1. marisa-trie 是雙授權。** 我們選用 **BSD-2-Clause** 這一支，因此不承擔
LGPL-2.1 的相關義務（例如允許使用者替換該函式庫的重新連結要求）。此選擇需在
發行說明中明確標示，不可含糊帶過。

**2. OpenCC 是 Apache-2.0，與 GPL-2 不相容、但與 GPL-3 相容。**
這是本專案選擇 GPL-3 而非 GPL-2 的實質理由之一 —— 若日後有人提議改用 GPL-2，
這條會直接擋下來。

## 關於參考現有 RIME 前端

本專案的四端 UI 為**自行撰寫**，並非 fork 自任何現有前端。

RIME 生態中的既有前端（Weasel、Squirrel、Trime、Hamster 等）多為 GPL 授權。
若日後要移植或參考其**程式碼**（而非僅參考其建置腳本的做法或公開文件），
必須先確認授權相容性並在此檔案中登記。目前**沒有**任何來自這些專案的程式碼。
