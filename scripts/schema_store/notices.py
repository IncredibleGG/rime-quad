#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""依 index.json 更新 THIRD_PARTY_NOTICES.md 的方案市集章節。

只改兩個標記之間的區塊，其餘內容原樣保留（這個檔案有別的 agent 也在動）。
"""
import collections, io, json, sys

IDX, NOTICES = sys.argv[1], sys.argv[2]
BEGIN = "<!-- BEGIN schema-store (由 scripts/build_schema_store.sh 產生，勿手改) -->"
END = "<!-- END schema-store -->"

d = json.load(open(IDX))
pkgs = sorted(d["packages"], key=lambda p: (p["category"], p["id"]))
catname = {c["id"]: c["name"] for c in d["categories"]}

by_lic = collections.Counter(p["license"] for p in pkgs)

rows = "".join(
    "| [{id}]({up}) | {name} | {cat} | `{lic}` | `{commit}` |\n".format(
        id=p["id"], up=p["upstream"], name=p["name"],
        cat=catname.get(p["category"], p["category"]),
        lic=p["license"], commit=p["upstream_commit"])
    for p in pkgs)

body = f"""{BEGIN}

## 方案市集散布的第三方方案

以下方案**不隨 APK 散布**，是使用者從方案市集（`scripts/build_schema_store.sh`
產生的索引）自行選擇下載的。每一個套件的 zip 內都附有該上游庫原本的授權檔，
`UPSTREAM.txt` 記錄來源 URL 與 commit，任何人都能自行重建、核對。

**授權是逐一讀該庫的授權檔判定的，不是照抄 GitHub 的標記。**
結果並非全部都是 LGPL-3：

{"".join(f'- `{k}`：{v} 個套件' + chr(10) for k, v in sorted(by_lic.items()))}
沒有授權檔的上游庫一律**不收錄** —— 無法確認散布條件，也無法滿足
`docs/schema-store.md` §2「zip 必須附 LICENSE」。

| 套件 | 名稱 | 分類 | 授權 | 打包時的 commit |
|---|---|---|---|---|
{rows}
### 與本專案授權的相容性

本專案本體是 GPL-3.0-or-later。上表的方案資料是**執行期由使用者下載的資料**，
不與本專案的程式碼連結，也不編進發行二進位檔，因此不構成衍生作品。
即便如此，上表所有授權（LGPL-3.0、GPL-3.0、Apache-2.0、MIT、CC-BY-4.0、
ODbL-1.0、CC0-1.0）都與 GPL-3 相容或更寬鬆，散布上沒有衝突。

**CC-BY-4.0 與 ODbL-1.0（rime-cantonese）需要標示出處** —— 這一條由套件內附的
`LICENSE` 與 `UPSTREAM.txt` 滿足，行動端在方案詳情頁也應顯示 `license` 欄位。

{END}"""

txt = io.open(NOTICES, encoding="utf-8").read()
if BEGIN in txt and END in txt:
    pre = txt[:txt.index(BEGIN)]
    post = txt[txt.index(END) + len(END):]
    txt = pre + body + post
else:
    txt = txt.rstrip() + "\n\n---\n\n" + body + "\n"
io.open(NOTICES, "w", encoding="utf-8").write(txt)
print(f"THIRD_PARTY_NOTICES.md 已更新（{len(pkgs)} 個套件，{len(by_lic)} 種授權）")
