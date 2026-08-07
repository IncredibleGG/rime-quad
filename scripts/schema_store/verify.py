#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""方案市集 — 品質閘門：讓每個套件真的被 librime 部署過一次。

做法（刻意做成和行動端一樣的形狀）：
  shared_data_dir = 只有 opencc/（對應 APK 內建的那份）
  user_data_dir   = 該套件 + 遞迴展開的相依，攤平後的檔案 + default.custom.yaml
所以「少宣告一個相依」在這裡一定會炸，不會被 APK 內建資料掩蓋掉。

可續跑：每個套件的結果寫在 <work>/verify/<id>.json，已存在就跳過。
可平行：--jobs N（模擬器上各跑各的 process 與資料目錄）。
"""
import argparse, concurrent.futures as cf, io, json, os, re, shutil, subprocess, sys, threading

ap = argparse.ArgumentParser()
ap.add_argument("--work", required=True)
ap.add_argument("--adb", required=True)
ap.add_argument("--serial", required=True)
ap.add_argument("--binary", required=True)
ap.add_argument("--opencc", required=True)
ap.add_argument("--jobs", type=int, default=3)
ap.add_argument("--force", action="store_true")
ap.add_argument("--only", default="")
ap.add_argument("--no-push", action="store_true")
A = ap.parse_args()

DEV = "/data/local/tmp/rimestore"
VDIR = os.path.join(A.work, "verify")
os.makedirs(VDIR, exist_ok=True)

data = json.load(open(os.path.join(A.work, "packages.json")))
pkgs = data["packages"]
by_id = {p["id"]: p for p in pkgs}

_lock = threading.Lock()


class DeviceGone(Exception):
    pass


_OFFLINE = ("device offline", "device not found", "no devices/emulators",
            "device still connecting", "closed")
_dev_lock = threading.Lock()


def _raw(*args, timeout=1800):
    return subprocess.run([A.adb, "-s", A.serial] + list(args),
                          capture_output=True, text=True, timeout=timeout)


def adb(*args, timeout=1800):
    """模擬器是和其他 agent 共用的，隨時可能被重啟 —— 掉線就等它回來再試一次。"""
    r = _raw(*args, timeout=timeout)
    if r.returncode != 0 and any(s in (r.stderr or "") for s in _OFFLINE):
        with _dev_lock:
            subprocess.run([A.adb, "-s", A.serial, "wait-for-device"],
                           capture_output=True, timeout=900)
            # 開機完成前 /data 還沒 mount，資料會不見
            for _ in range(60):
                b = _raw("shell", "getprop sys.boot_completed", timeout=60)
                if (b.stdout or "").strip() == "1":
                    break
                subprocess.run(["sleep", "5"])
            if _raw("shell", f"ls {DEV}/pool/prelude", timeout=120).returncode != 0:
                push_base()
        r = _raw(*args, timeout=timeout)
        if r.returncode != 0 and any(s in (r.stderr or "") for s in _OFFLINE):
            raise DeviceGone(r.stderr)
    return r


def shell(cmd, timeout=1800):
    return adb("shell", cmd, timeout=timeout)


def closure(pid):
    seen, stack = set(), [pid]
    while stack:
        x = stack.pop()
        if x in seen or x not in by_id:
            continue
        seen.add(x)
        stack += by_id[x]["requires"]
    return sorted(seen)


# ──────────────────────────────────────────────────── 探針按鍵推導 ────────
# 不瞎編：從該方案自己的詞庫裡查「你好」實際的編碼。
#   dict.yaml 的內文是 TSV：<詞>\t<編碼>[\t<權重>]
#   多音節/多碼以空白分隔，speller 打字時直接連著打。
HANIHAO = "你好"


def probe_candidates(p):
    stage = {q: by_id[q]["stage_dir"] for q in closure(p["id"]) if q in by_id}

    def find(fn):
        for d in stage.values():
            fp = os.path.join(d, fn)
            if os.path.isfile(fp):
                return fp
        return None

    out = []
    for sc in p["schemas"][:4]:
        sp = find(sc["file"])
        if not sp:
            continue
        text = io.open(sp, encoding="utf-8", errors="replace").read()
        dicts = re.findall(r"^\s*dictionary:\s*([^\s#\"']+)\s*$", text, re.M)
        codes = []
        for dn in dict.fromkeys(dicts):
            for cand_fn in [dn + ".dict.yaml"]:
                dp = find(cand_fn)
                if not dp:
                    continue
                tables = [dp]
                head = io.open(dp, encoding="utf-8", errors="replace").read(4000)
                for m in re.finditer(r"^\s*-\s*([A-Za-z0-9_.\-]+)\s*$", head, re.M):
                    t = find(m.group(1) + ".dict.yaml")
                    if t:
                        tables.append(t)
                # 先找「你好」這個詞條；找不到就退而求其次，把「你」「好」
                # 兩個單字的編碼接起來 —— 一樣是從該方案自己的詞庫查出來的，
                # 不是憑空編的。粵拼這種詞庫裡沒有「你好」詞條的就靠這條。
                per_char = {"你": [], "好": []}
                for t in tables:
                    try:
                        with io.open(t, encoding="utf-8", errors="replace") as f:
                            for line in f:
                                if line.startswith(HANIHAO + "\t"):
                                    code = line.split("\t")[1].strip()
                                    if code:
                                        codes.append(code.replace(" ", ""))
                                elif line[:1] in per_char and line[1:2] == "\t":
                                    c = line.split("\t")[1].strip().replace(" ", "")
                                    # 多音字要多留幾個讀音：粵拼的「好」先出現的是
                                    # hou3（喜好），hou2（好的）在後面，只取第一個會漏。
                                    if c and c not in per_char[line[0]] \
                                            and len(per_char[line[0]]) < 3:
                                        per_char[line[0]].append(c)
                                if len(codes) >= 2:
                                    break
                    except OSError:
                        pass
                    if len(codes) >= 2:
                        break
                for a in per_char["你"]:
                    for b in per_char["好"]:
                        codes.append(a + b)
            if codes:
                break
        keys = list(dict.fromkeys(codes + ["nihao"]))
        # 注音大千鍵位：詞庫是 terra_pinyin，查到的是拼音，打不進去；
        # 大千鍵位的「你好」是 su3cl3（本專案已實測驗證過）。
        if p.get("bopomofo"):
            keys.insert(0, "su3cl3")
        for k in keys[:6]:
            if re.fullmatch(r"[!-~]{1,20}", k):
                out.append((sc["id"], k))
    return out[:12]


# ────────────────────────────────────────────────────────── 裝置端 ──────
def push_base():
    """只用 _raw，避免和 adb() 的重連邏輯互相遞迴。"""
    _raw("shell", f"rm -rf {DEV}")
    _raw("shell", f"mkdir -p {DEV}/pool {DEV}/run {DEV}/shared")
    r = _raw("push", A.opencc, f"{DEV}/shared/opencc")
    assert r.returncode == 0, r.stderr
    r = _raw("push", A.binary, f"{DEV}/rime_console")
    assert r.returncode == 0, r.stderr
    _raw("shell", f"chmod 755 {DEV}/rime_console")
    for p in pkgs:
        r = _raw("push", p["stage_dir"], f"{DEV}/pool/{p['id']}")
        assert r.returncode == 0, f"{p['id']}: {r.stderr}"
    print(f"已推送 {len(pkgs)} 個套件到模擬器", flush=True)


def make_run_dir(p, schemas, tag):
    run = f"{DEV}/run/{p['id']}{tag}"
    shell(f"rm -rf {run} && mkdir -p {run}/user")
    cps = " && ".join(f"cp {DEV}/pool/{q}/* {run}/user/ 2>/dev/null || true"
                      for q in closure(p["id"]) if q in by_id)
    shell(cps)
    # 上游 default.yaml 列的方案遠多於本套件提供的，未提供者部署會報錯 ——
    # 用 RIME 慣用的 default.custom.yaml patch 把 schema_list 限縮成待測方案。
    lines = "".join(f"    - schema: {s}\n" for s in schemas)
    yml = "patch:\n  schema_list:\n" + lines
    local = os.path.join(VDIR, f"_dcy.{p['id']}{tag}.yaml")
    io.open(local, "w", encoding="utf-8").write(yml)
    r = adb("push", local, f"{run}/user/default.custom.yaml", timeout=300)
    assert r.returncode == 0, f"push default.custom.yaml 失敗: {r.stderr}"
    return run


DEPLOY_RE = re.compile(r"^\[deploy\] (\w+)", re.M)
SCHEMA_RE = re.compile(r"^\[schema\] ([^\t]+)\t(.*)$", re.M)
COMMIT_RE = re.compile(r'^>>> COMMIT: "(.*)"$', re.M)


def run_console(run, keys, sel="1", schema="", timeout=1500):
    cmd = (f"cd {DEV} && ./rime_console {DEV}/shared {run}/user "
           f"{keys} {sel} {schema}")
    try:
        r = shell(cmd, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "", "TIMEOUT"
    return (r.stdout or "") + (r.stderr or ""), None


def deploy_test(p, schemas, tag=""):
    run = make_run_dir(p, schemas, tag)
    out, err = run_console(run, "-")
    # 原始輸出留檔，失敗時才有東西可查（glog 訊息都在裡面）
    io.open(os.path.join(VDIR, f"{p['id']}{tag}.deploy.log"), "w",
            encoding="utf-8", errors="replace").write(out)
    if err:
        return None, run, f"逾時（>{1500}s）"
    st = DEPLOY_RE.findall(out)
    ok = "SUCCESS" in st
    listed = {m[0] for m in SCHEMA_RE.findall(out)}
    if not ok:
        tail = "\n".join(l for l in out.splitlines()
                         if re.search(r"\bE\d{8}|error|failed|ERROR", l))[-1200:]
        return None, run, f"部署未回報 SUCCESS（{st or '無回報'}）\n{tail}"
    return listed, run, None


def verify(p):
    res = {"id": p["id"], "deployed": False, "schemas_ok": [], "probe": None,
           "notes": []}
    want = [s["id"] for s in p["schemas"]]
    if not want:
        res["notes"].append("此套件不提供方案，部署證據由相依它的套件之部署帶出")
        return res

    listed, run, err = deploy_test(p, want)
    if listed is not None and set(want) <= listed:
        res["deployed"] = True
        res["schemas_ok"] = want
    else:
        if err:
            res["notes"].append("整包部署失敗：" + err[:600])
        else:
            res["notes"].append(
                "整包部署成功但 rs_schema_list 少了：%s" % ",".join(sorted(set(want) - listed)))
        # 逐一測試，救回還能用的方案（部署很慢，所以只在整包失敗時才做）
        good = []
        for i, s in enumerate(want):
            l2, run2, e2 = deploy_test(p, [s], tag=f".s{i}")
            if l2 is not None and s in l2:
                good.append(s)
            else:
                res["notes"].append(f"方案 {s} 單獨部署仍失敗：{(e2 or '未列出')[:300]}")
            shell(f"rm -rf {run2}")
        if good:
            res["deployed"] = True
            res["schemas_ok"] = good
            run = make_run_dir(p, good, "")
        else:
            return res

    # ── 探針：真的打一次字
    for sid, keys in probe_candidates(p):
        if sid not in res["schemas_ok"]:
            continue
        out, err = run_console(run, keys, "1", sid, timeout=600)
        if err:
            continue
        cm = COMMIT_RE.findall(out)
        if cm and cm[-1] == HANIHAO:
            res["probe"] = {"schema": sid, "keys": keys, "expect": HANIHAO}
            break
    shell(f"rm -rf {run}")
    return res


def worker(p):
    path = os.path.join(VDIR, p["id"] + ".json")
    if os.path.exists(path) and not A.force:
        return json.load(open(path))
    res, last = None, None
    for attempt in range(3):
        try:
            res = verify(p)
            break
        except Exception as exc:
            last = exc
            # 幾乎都是模擬器被別的 agent 重啟造成的掉線；等它回來再重來一次。
            try:
                adb("shell", "true", timeout=900)
            except Exception:
                pass
    if res is None:
        res = {"id": p["id"], "deployed": False, "schemas_ok": [], "probe": None,
               "notes": [f"驗證程序自身出錯（重試 3 次仍失敗）: {last!r}"]}
    with _lock:
        json.dump(res, open(path, "w"), ensure_ascii=False, indent=1)
        print(f"  {p['id']:22s} deployed={res['deployed']} "
              f"schemas={len(res['schemas_ok'])}/{len(p['schemas'])} "
              f"probe={(res['probe'] or {}).get('keys', '-')}", flush=True)
    return res


targets = [p for p in pkgs if not A.only or p["id"] in A.only.split(",")]
if not A.no_push:
    push_base()
# 大套件先跑，尾巴才不會被單一慢件拖住
targets.sort(key=lambda p: -p.get("size", 0))

with cf.ThreadPoolExecutor(max_workers=A.jobs) as ex:
    list(ex.map(worker, targets))

# ── 沒有方案的套件：部署證據來自「有它在相依閉包裡、且成功部署過」的套件
results = {}
for p in pkgs:
    fp = os.path.join(VDIR, p["id"] + ".json")
    if os.path.exists(fp):
        results[p["id"]] = json.load(open(fp))
for p in pkgs:
    r = results.get(p["id"])
    if r and not p["schemas"]:
        carriers = [q["id"] for q in pkgs
                    if q["schemas"] and p["id"] in closure(q["id"])
                    and results.get(q["id"], {}).get("deployed")]
        if carriers:
            r["deployed"] = True
            r["via"] = carriers[:5]
            r["notes"].append("部署證據來自：" + ", ".join(carriers[:5]))
            json.dump(r, open(os.path.join(VDIR, p["id"] + ".json"), "w"),
                      ensure_ascii=False, indent=1)

ok = sum(1 for r in results.values() if r["deployed"])
print(f"\n通過部署驗證 {ok} / {len(results)}")
