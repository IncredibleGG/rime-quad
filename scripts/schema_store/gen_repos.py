#!/usr/bin/env python3
"""列舉上游 repo 清單：rime 組織的 rime-* + rppi(官方套件索引)列出的第三方。

不硬編任何清單 —— 全部來自 GitHub API 與 rppi 的 index.json。
"""
import json, os, sys, urllib.request

WORK = sys.argv[1]
RPPI = "https://raw.githubusercontent.com/rime/rppi/master"


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "rime-schema-store"})
    with urllib.request.urlopen(req, timeout=30) as f:
        return json.load(f)


repos = {}


def ent(slug):
    return repos.setdefault(slug, {"repo": slug})


# ---------------------------------------------------------------- 1. rime 組織
page, org = 1, []
while True:
    batch = get(f"https://api.github.com/orgs/rime/repos?per_page=100&page={page}")
    if not batch:
        break
    org += batch
    page += 1

for r in org:
    if not r["name"].startswith("rime-"):
        continue
    e = ent("rime/" + r["name"])
    e["source"] = "org"
    e["github_license"] = (r.get("license") or {}).get("spdx_id")
    e["size_kb"] = r.get("size")
    e["default_branch"] = r.get("default_branch")

# ---------------------------------------------------------------- 2. rppi 索引
def walk(path):
    url = f"{RPPI}/{path}/index.json" if path else f"{RPPI}/index.json"
    try:
        idx = get(url)
    except Exception as exc:
        print(f"  [警告] 取不到 {url}: {exc}", file=sys.stderr)
        return
    for c in idx.get("categories", []):
        walk(f"{path}/{c['key']}" if path else c["key"])
    for rec in idx.get("recipes", []):
        e = ent(rec["repo"])
        e.setdefault("source", "rppi")
        e["rppi_path"] = path
        e["rppi_name"] = rec.get("name")
        e["rppi_schemas"] = rec.get("schemas")
        e["rppi_deps"] = rec.get("dependencies")
        e["rppi_rdeps"] = rec.get("reverseDependencies")
        e["rppi_labels"] = rec.get("labels")
        e["rppi_license"] = rec.get("license")
        if rec.get("branch"):
            e["branch"] = rec["branch"]


walk("")

out = sorted(repos.values(), key=lambda r: r["repo"])
os.makedirs(WORK, exist_ok=True)
with open(os.path.join(WORK, "repos.json"), "w") as f:
    json.dump(out, f, ensure_ascii=False, indent=2)

print(f"共 {len(out)} 個 repo")
for r in out:
    print(f"  {r['repo']:42s} src={r.get('source'):5s} rppi={r.get('rppi_path') or '-':22s} "
          f"labels={r.get('rppi_labels') or '-'}")
