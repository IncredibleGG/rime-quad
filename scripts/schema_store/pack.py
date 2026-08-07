#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""方案市集 — 打包階段：把每個套件攤平成一個 zip（規範 §2）。

  * 扁平檔案集合（zip entry 名稱不含 "/"）
  * 必附 LICENSE 與 UPSTREAM.txt
  * 不得有目錄穿越路徑或符號連結
  * 不重複打包 opencc 的 .ocd2（APK 內建）
"""
import datetime, hashlib, io, json, os, shutil, subprocess, sys, zipfile

WORK = sys.argv[1]
DIST = sys.argv[2]
STAGE = os.path.join(WORK, "stage")
NOW = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)

data = json.load(open(os.path.join(WORK, "packages.json")))
pkgs = data["packages"]
excluded = data["excluded"]

os.makedirs(DIST, exist_ok=True)
shutil.rmtree(STAGE, ignore_errors=True)
os.makedirs(STAGE)

# 和行動端 ArchiveGuard.ArchiveLimits 的白名單保持一致。打包端先擋一次，
# 行動端仍然會再擋一次（規範 §2「打包端要保證，行動端仍要再驗一次」）。
ALLOWED_EXT = {"yaml", "yml", "txt", "json", "lua", "gram", "ocd2", "md"}
ALLOWED_BARE = {"LICENSE", "UPSTREAM.txt"}

CACHE = os.path.join(WORK, "packed.json")
cache = json.load(open(CACHE)) if os.path.exists(CACHE) else {}
reused = 0

kept = []
for p in pkgs:
    sid, d = p["id"], p["dir"]
    if not p["files"]:
        excluded.append({"id": sid, "repo": p["repo"], "stage": "pack",
                         "reason": "沒有任何可散布的資料檔（內容不是 RIME 執行期資料，"
                                   "例如只有 plum recipe 或 opencc/ 子目錄下的詞典）"})
        continue

    st = os.path.join(STAGE, sid)
    os.makedirs(st)
    # 讓 zip 可重現：所有檔案的 mtime 一律設成上游 commit 的時間，
    # 這樣同一個 commit 重跑會產出位元相同的 zip、相同的 sha256，
    # 行動端不會因為我們重跑一次流水線就被迫重新下載。
    ts = int(subprocess.run(["git", "log", "-1", "--format=%ct"], cwd=d,
                            capture_output=True, text=True).stdout.strip() or 0)
    names = []
    for fn in p["files"]:
        src = os.path.join(d, p["paths"][fn])
        if os.path.islink(src) or not os.path.isfile(src):
            continue
        # 規範 §2：zip 內原則上是扁平的，唯一的例外是 lua/（以及 opencc/、
        # build/）。lua 之所以不能攤平，是因為 librime-lua 的 package.path
        # 寫死了 <data_dir>/lua/?.lua —— 攤平之後 require 就找不到檔案。
        dst = os.path.join(st, fn)
        if "/" in fn:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        os.utime(dst, (ts, ts))
        names.append(fn)

    # LICENSE：規範要求 zip 內必附。多份授權檔就合併，並標明各自出處。
    parts = []
    for lf in p["license_files"]:
        txt = io.open(os.path.join(d, lf["file"]), encoding="utf-8",
                      errors="replace").read()
        parts.append(f"===== {lf['file']}  ({lf['spdx']}) =====\n\n{txt}")
    io.open(os.path.join(st, "LICENSE"), "w", encoding="utf-8").write(
        "\n\n".join(parts))
    names.append("LICENSE")

    spdx = " AND ".join(dict.fromkeys(l["spdx"] for l in p["license_files"]))
    if p["or_later"] and spdx in ("LGPL-3.0", "GPL-3.0", "LGPL-2.1", "GPL-2.0", "AGPL-3.0"):
        spdx += "-or-later"
    elif spdx in ("LGPL-3.0", "GPL-3.0", "LGPL-2.1", "GPL-2.0", "AGPL-3.0"):
        spdx += "-only"
    p["license"] = spdx

    io.open(os.path.join(st, "UPSTREAM.txt"), "w", encoding="utf-8").write(
        "# 本套件由 rime 專案的 scripts/build_schema_store.sh 自上游打包而成。\n"
        "# 內容未經修改，僅從上游庫挑出 librime 執行期需要的檔案並攤平。\n"
        f"package_id:   {sid}\n"
        f"upstream_url: {p['upstream']}\n"
        f"upstream_commit: {p['commit']}\n"
        f"license:      {spdx}  (見同捆的 LICENSE)\n"
        f"packed_at:    {NOW.isoformat().replace('+00:00', 'Z')}\n"
        f"requires:     {' '.join(p['requires']) or '(無)'}\n"
        f"files:        {len(names) + 1}\n\n"
        + "".join(f"  {n}\n" for n in sorted(names + ["UPSTREAM.txt"])))
    names.append("UPSTREAM.txt")

    for extra in ("LICENSE", "UPSTREAM.txt"):
        os.utime(os.path.join(st, extra), (ts, ts))

    # ── 打包
    # UPSTREAM.txt 內含 packed_at，所以 zip 沒辦法真正位元可重現。改成：
    # 上游 commit 與檔案清單都沒變就沿用既有的 zip，讓 sha256 保持穩定 ——
    # 否則每跑一次流水線，所有裝置都會被迫重新下載一次。
    zpath = os.path.join(DIST, f"{sid}-{p['commit'][:7]}.zip")
    sig = [p["commit"]] + sorted(f"{n}:{os.path.getsize(os.path.join(st, n))}"
                                 for n in names if n != "UPSTREAM.txt")
    prev = cache.get(sid)
    if prev and prev.get("sig") == sig and os.path.exists(zpath):
        reused += 1
    else:
        if os.path.exists(zpath):
            os.remove(zpath)
        subprocess.run(["zip", "-X", "-q", "-9", zpath] + sorted(names),
                       cwd=st, check=True)
    cache[sid] = {"sig": sig, "zip": os.path.basename(zpath)}

    # ── 產出自檢：路徑安全（規範 §2）。
    # entry 名稱以 librime 實際解析到的相對路徑為準 —— 攤平是常態，但
    # `import_tables: cn_dicts/8105`、`opencc_config`、librime-lua 的
    # package.path 都是**照路徑**找檔案的，那幾類必須原樣保留。
    # 深度上限 3 段是配合行動端 ArchiveGuard 的 maxDepth=4，留一格餘裕。
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            n = info.filename
            assert "\\" not in n, f"{sid}: zip 內有反斜線 {n}"
            assert n.count("/") <= 2, f"{sid}: zip 內路徑過深 {n}"
            assert not n.startswith(("/", "..")) and ".." not in n.split("/"), \
                f"{sid}: zip 內有穿越路徑 {n}"
            assert "." not in n.split("/") and "" not in n.split("/"), \
                f"{sid}: zip 內有可疑路徑段 {n}"
            # lua 只會出現在這兩個位置 —— librime-lua 就只從這兩個位置載入。
            assert not n.endswith(".lua") or n == "rime.lua" \
                or n.startswith("lua/"), f"{sid}: lua 檔不在 lua/ 也不是 rime.lua: {n}"
            assert n.rsplit(".", 1)[-1].lower() in ALLOWED_EXT or n in ALLOWED_BARE, \
                f"{sid}: 副檔名不在白名單 {n}"
            assert not (info.external_attr >> 16) & 0o120000 == 0o120000, \
                f"{sid}: zip 內有符號連結 {n}"
            assert not n.endswith(".ocd2"), f"{sid}: 不該打包 .ocd2"
        assert "LICENSE" in z.namelist() and "UPSTREAM.txt" in z.namelist()

    h = hashlib.sha256()
    with open(zpath, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    p["zip"] = os.path.basename(zpath)
    p["size"] = os.path.getsize(zpath)
    p["sha256"] = h.hexdigest()
    p["stage_dir"] = st
    kept.append(p)

json.dump(cache, open(CACHE, "w"), ensure_ascii=False, indent=1)
data["packages"] = kept
data["excluded"] = excluded
data["packed_at"] = NOW.isoformat().replace("+00:00", "Z")
json.dump(data, open(os.path.join(WORK, "packages.json"), "w"),
          ensure_ascii=False, indent=1)

print(f"打包 {len(kept)} 個套件 -> {DIST}（其中 {reused} 個沿用既有 zip，sha256 不變）")
for p in sorted(kept, key=lambda x: -x["size"]):
    print(f"  {p['id']:22s} {p['size']/1e6:8.2f} MB  {p['license']:22s} {p['zip']}")
