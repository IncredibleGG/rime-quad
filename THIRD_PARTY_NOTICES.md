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

---

## RIME 方案資料（執行期資源）

以下資料由 `scripts/collect_data.sh` 組裝進 `core/data/shared`，隨 APK 散佈。
**全部為 LGPL-3.0**，與本專案的 GPL-3 相容。

| 來源 | 提供 | 授權 |
|---|---|---|
| [rime-prelude](https://github.com/rime/rime-prelude) | `default.yaml`、標點、符號、按鍵綁定 | LGPL-3.0 |
| [rime-essay](https://github.com/rime/rime-essay) | `essay.txt` 語言模型 | LGPL-3.0 |
| [rime-luna-pinyin](https://github.com/rime/rime-luna-pinyin) | 拼音方案與詞庫 | LGPL-3.0 |
| [rime-bopomofo](https://github.com/rime/rime-bopomofo) | 注音方案 | LGPL-3.0 |
| [rime-terra-pinyin](https://github.com/rime/rime-terra-pinyin) | 注音方案所依賴的詞庫 | LGPL-3.0 |
| [rime-stroke](https://github.com/rime/rime-stroke) | 筆畫反查詞庫 | LGPL-3.0 |

OpenCC 的 `.ocd2` 詞典由本專案以 host 版 OpenCC 從其原始碼樹產生，
授權同 OpenCC 本體（Apache-2.0）。

### 九宮格拼音（`t9_pinyin.schema.yaml`）—— 本專案自撰，非第三方

`core/data/schemas/t9_pinyin.schema.yaml` 由本專案撰寫，授權同本專案（GPL-3.0-or-later）。
它共用 rime-luna-pinyin 的詞典，本身不含任何第三方資料。

字母分組（ABC／DEF／GHI／JKL／MNO／PQRS／TUV／WXYZ）出自 **ITU-T E.161** 電話鍵盤
標準，是通用事實而非任何人的著作。

**沒有引用 [YuyanIme](https://github.com/gurecn/YuyanIme)（BSD-3）的任何程式碼、
映射表或 algebra 規則。** 該專案只被當成「九宮格應為一套 RIME 方案而非前端硬湊」
這個結論的旁證。若日後真的取用其內容，必須回到本檔案登記並保留其著作權聲明。

**注意**：`core/data/shared` 與 `core/data/user` 是 `collect_data.sh` 的產物，
不納入版本控制。修改資料組成請改腳本，不要手改產出目錄。

---

<!-- BEGIN schema-store (由 scripts/build_schema_store.sh 產生，勿手改) -->

## 方案市集散布的第三方方案

以下方案**不隨 APK 散布**，是使用者從方案市集（`scripts/build_schema_store.sh`
產生的索引）自行選擇下載的。每一個套件的 zip 內都附有該上游庫原本的授權檔，
`UPSTREAM.txt` 記錄來源 URL 與 commit，任何人都能自行重建、核對。

**授權是逐一讀該庫的授權檔判定的，不是照抄 GitHub 的標記。**
結果並非全部都是 LGPL-3：

- `Apache-2.0`：1 個套件
- `CC-BY-4.0 AND ODbL-1.0`：1 個套件
- `GPL-3.0-only`：12 個套件
- `GPL-3.0-or-later`：1 個套件
- `LGPL-3.0-only`：13 個套件
- `MIT`：1 個套件

沒有授權檔的上游庫一律**不收錄** —— 無法確認散布條件，也無法滿足
`docs/schema-store.md` §2「zip 必須附 LICENSE」。

| 套件 | 名稱 | 分類 | 授權 | 打包時的 commit |
|---|---|---|---|---|
| [essay](https://github.com/rime/rime-essay) | 語料庫（繁） | 基礎元件 | `LGPL-3.0-only` | `e9b1a37` |
| [essay-simp](https://github.com/rime/rime-essay-simp) | 語料庫（簡） | 基礎元件 | `LGPL-3.0-only` | `dc06d4c` |
| [prelude](https://github.com/rime/rime-prelude) | 基礎配置 | 基礎元件 | `LGPL-3.0-only` | `082425e` |
| [array](https://github.com/rime/rime-array) | 行列 30 | 華語 | `GPL-3.0-only` | `557dbe3` |
| [bopomofo](https://github.com/rime/rime-bopomofo) | 注音 | 華語 | `LGPL-3.0-only` | `6085c9a` |
| [c2h6-pinyin](https://github.com/lotem/rime-c2h6-pinyin) | 乙烷拼音 | 華語 | `LGPL-3.0-only` | `25497f1` |
| [cangjie](https://github.com/rime/rime-cangjie) | 倉頡五代 | 華語 | `LGPL-3.0-only` | `52d90a1` |
| [combo-pinyin](https://github.com/rime/rime-combo-pinyin) | 宮保拼音 | 華語 | `GPL-3.0-only` | `862894a` |
| [double-pinyin](https://github.com/rime/rime-double-pinyin) | 雙拼 | 華語 | `GPL-3.0-only` | `01a1328` |
| [luna-pinyin](https://github.com/rime/rime-luna-pinyin) | 朙月拼音 | 華語 | `LGPL-3.0-only` | `56b934b` |
| [pinyin-simp](https://github.com/rime/rime-pinyin-simp) | 袖珍簡拼 | 華語 | `Apache-2.0` | `0c6861e` |
| [quick](https://github.com/rime/rime-quick) | 速成 | 華語 | `LGPL-3.0-only` | `5dcdb9e` |
| [radical-pinyin](https://github.com/mirtlecn/rime-radical-pinyin) | 部件拆字 | 華語 | `GPL-3.0-only` | `87e73d7` |
| [scj](https://github.com/rime/rime-scj) | 快速倉頡 | 華語 | `GPL-3.0-or-later` | `cab5a08` |
| [stenotype](https://github.com/rime/rime-stenotype) | 打字速記法 | 華語 | `GPL-3.0-only` | `bef9308` |
| [stroke](https://github.com/rime/rime-stroke) | 五筆畫 | 華語 | `LGPL-3.0-only` | `3a4b0f4` |
| [terra-pinyin](https://github.com/rime/rime-terra-pinyin) | 地球拼音 | 華語 | `LGPL-3.0-only` | `8a2c895` |
| [wubi](https://github.com/rime/rime-wubi) | 五筆·86 | 華語 | `LGPL-3.0-only` | `152a0d3` |
| [zhengma](https://github.com/lotem/rime-zhengma) | 鄭碼 | 華語 | `GPL-3.0-only` | `bc5873c` |
| [zrm](https://github.com/bigshans/rime-zrm) | 自然碼＋輔助碼 | 華語 | `GPL-3.0-only` | `4ef9a1d` |
| [ipa](https://github.com/rime/rime-ipa) | 國際音標 | 其他語系 | `LGPL-3.0-only` | `22b7171` |
| [cantonese](https://github.com/rime/rime-cantonese) | 粵語拼音（粵拼） | 方言 | `CC-BY-4.0 AND ODbL-1.0` | `c99b16e` |
| [dieghv](https://github.com/kahaani/dieghv) | 潮州話拼音 | 方言 | `GPL-3.0-only` | `1709bb7` |
| [jyutping](https://github.com/rime/rime-jyutping) | 粵拼（傳統版） | 方言 | `LGPL-3.0-only` | `ee5e72b` |
| [middle-chinese](https://github.com/rime/rime-middle-chinese) | 中古漢語拼音 | 方言 | `GPL-3.0-only` | `582e144` |
| [naamning-jyutping](https://github.com/leimaau/naamning_jyutping) | 南寧白話 / 平話 | 方言 | `MIT` | `531c0c0` |
| [sautungva](https://github.com/AlfredLouis00/rime-Sautungva) | 湘語·邵東話 | 方言 | `GPL-3.0-only` | `61081b1` |
| [soutzoe](https://github.com/rime/rime-soutzoe) | 蘇州吳語 | 方言 | `GPL-3.0-only` | `beeaeca` |
| [wugniu](https://github.com/rime/rime-wugniu) | 上海吳語 | 方言 | `GPL-3.0-only` | `2818f48` |

### 與本專案授權的相容性

本專案本體是 GPL-3.0-or-later。上表的方案資料是**執行期由使用者下載的資料**，
不與本專案的程式碼連結，也不編進發行二進位檔，因此不構成衍生作品。
即便如此，上表所有授權（LGPL-3.0、GPL-3.0、Apache-2.0、MIT、CC-BY-4.0、
ODbL-1.0、CC0-1.0）都與 GPL-3 相容或更寬鬆，散布上沒有衝突。

**CC-BY-4.0 與 ODbL-1.0（rime-cantonese）需要標示出處** —— 這一條由套件內附的
`LICENSE` 與 `UPSTREAM.txt` 滿足，行動端在方案詳情頁也應顯示 `license` 欄位。

<!-- END schema-store -->
