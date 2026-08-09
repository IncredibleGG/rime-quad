"""真正的重複鍵檢查:用 yaml 的節點樹,只在**同一個 mapping** 裡比對。

⚠ 為什麼需要它:`yaml.safe_load` 對重複鍵是「後者覆蓋前者」,不會出聲,
  而 GitHub Actions 會**整份檔案拒收**,run 的名字變成檔案路徑。
  聯集式的合併衝突解法(兩邊各自新增)最容易產生這種東西 ——
  這一輪就是 `push:` 底下併出了兩個 `branches:`。
"""
import sys
import yaml


class DupKeyError(Exception):
    pass


def check(path):
    with open(path, encoding="utf-8") as f:
        root = yaml.compose(f)
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


rc = 0
for p in sys.argv[1:]:
    b = check(p)
    print("%-40s 重複鍵 %d" % (p.split("/")[-1], len(b)))
    for x in b:
        print("    ⚠", x)
        rc = 1
sys.exit(rc)
