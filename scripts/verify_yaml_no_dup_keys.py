"""真正的重複鍵檢查:用 yaml 的節點樹,只在**同一個 mapping** 裡比對。

⚠ 為什麼需要它:`yaml.safe_load` 對重複鍵是「後者覆蓋前者」,不會出聲,
  而 GitHub Actions 會**整份檔案拒收**,run 的名字變成檔案路徑。
  聯集式的合併衝突解法(兩邊各自新增)最容易產生這種東西 ——
  這一輪就是 `push:` 底下併出了兩個 `branches:`。
"""
import glob
import os
import sys

import yaml


class DupKeyError(Exception):
    pass


def check(path):
    with open(path, encoding="utf-8") as f:
        root = yaml.compose(f)
    if root is None:
        raise DupKeyError("整份檔案是空的")
    bad = []

    def walk(node, trail):
        if isinstance(node, yaml.MappingNode):
            seen = {}
            for k, v in node.value:
                key = getattr(k, "value", None)
                if key in seen:
                    bad.append("%s/%s(第 %d 行與第 %d 行)" % (
                        "/".join(trail) or "<根>", key,
                        seen[key] + 1, k.start_mark.line + 1))
                else:
                    seen[key] = k.start_mark.line
                walk(v, trail + [str(key)])
        elif isinstance(node, yaml.SequenceNode):
            for i, v in enumerate(node.value):
                walk(v, trail + ["[%d]" % i])

    walk(root, [])
    return bad


# ⚠ 沒帶參數時這支以前掃 0 個檔案、印 0 行、以 0 結束 —— 一支永遠是綠的
#   守門比沒有更糟。實際發生過:`windows.yml` 裡留著一整組
#   `<<<<<<< / ======= / >>>>>>>`(0935f1a),而「改完驗一次」照著文件
#   跑成 `python3 scripts/verify_yaml_no_dup_keys.py`,回 rc=0。
#   現在沒帶參數就掃 .github/workflows/ 底下全部的 yml。
def targets(argv):
    if argv:
        return argv
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    found = sorted(glob.glob(os.path.join(root, ".github", "workflows", "*.yml"))
                   + glob.glob(os.path.join(root, ".github", "workflows", "*.yaml")))
    if not found:
        print("!! .github/workflows/ 底下一個 yml 都沒有 —— 不當成通過", file=sys.stderr)
        sys.exit(2)
    return found


rc = 0
for p in targets(sys.argv[1:]):
    name = p.split("/")[-1]
    # ⚠ 解析不了要當成失敗印出來,不能讓它以 traceback 的形式飛出去:
    #   合併衝突標記留在檔案裡時,GitHub 是**整份拒收**,而症狀是
    #   「那條車道安靜地沒有跑」——最需要看清楚的就是這一種。
    try:
        b = check(p)
    except (yaml.YAMLError, DupKeyError) as e:
        print("%-40s ⚠ 解析失敗(GitHub 會整份拒收)" % name)
        for line in str(e).strip().split("\n"):
            print("    ", line)
        rc = 1
        continue
    print("%-40s 重複鍵 %d" % (name, len(b)))
    for x in b:
        print("    ⚠", x)
        rc = 1
sys.exit(rc)
