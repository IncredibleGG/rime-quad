"""這個 workflow 會不會為**這條分支**跑到那一步。

⚠ 為什麼需要它:verify_syllables.sh --check-ci 以前只對 build.yml 做
  `grep -q -- "--plant X"`。**字串在檔案裡 ≠ 那一步會執行** —— 中間隔著
  兩道各自獨立、而且都不出聲的閘門:

    1. `on: push: branches:` 沒列這條分支 → 整份 workflow 一件都不會觸發。
    2. 列了,但那個 --plant 所在的 job 自己的 `if:` 不認這條分支 → 那個 job
       整個被跳過,而 **skipped 的 job 在 checks 上是灰色的勾**,
       和「跑過而且通過」在任何摘要上看起來一樣。

  這一支的存在理由和 --check-ci 本身完全相同,只是往上移了一層:
  上一次是「檔頭宣告了、workflow 沒接」,這一次是「workflow 接了、分支沒接」。

用法:
  python3 scripts/ci_branch_gate.py --workflow .github/workflows/build.yml \
      --branch t9hole --needle 'verify_syllables.sh --plant tap-swallowed' ...

  退出碼:0 = 每一根針都會在這條分支上跑到;1 = 有閘門擋著(訊息會指名是哪一道);
          2 = 這支工具自己判斷不了(看不懂的 if:、檔案壞了)。**2 不可以當成綠。**

  --self-test 用內建的小 workflow 證明三種紅(沒接線 / 分支不在清單 / job 的 if
  不認這條分支)真的分得出來,而且各自紅在對的那一道閘門上。
"""
import argparse
import re
import sys

import yaml

MARK_OK = "PASS"
MARK_NOT_WIRED = "FAIL[not-wired]"
MARK_PUSH = "FAIL[push-branches]"
MARK_JOB_IF = "FAIL[job-if]"


class Unsupported(Exception):
    """看不懂的 if:。這必須是 exit 2(工具判斷不了),不可以靜靜地當成會跑。"""


# ─────────────────────────── GitHub 運算式的小直譯器 ───────────────────────────
#
# 只認 if: 實際用得到的那一小塊語法。**認不得的一律丟 Unsupported** ——
# 「看不懂就當成會跑」會把這一關變成裝飾品,而那正是它要擋的東西。
_TOKEN = re.compile(
    r"\s*(?:"
    r"(?P<str>'(?:[^']|'')*')"
    r"|(?P<op>&&|\|\||==|!=|\(|\)|!|,)"
    r"|(?P<name>[A-Za-z_][A-Za-z_0-9.\-]*)"
    r")"
)

_FUNCS0 = {"always": True, "success": True, "failure": False, "cancelled": False}


def _tokenize(expr):
    pos, out = 0, []
    while pos < len(expr):
        if expr[pos].isspace():
            pos += 1
            continue
        m = _TOKEN.match(expr, pos)
        if not m or m.end() == pos:
            raise Unsupported("第 %d 個字元起看不懂:%r" % (pos, expr[pos:pos + 20]))
        pos = m.end()
        if m.group("str") is not None:
            out.append(("str", m.group("str")[1:-1].replace("''", "'")))
        elif m.group("op") is not None:
            out.append(("op", m.group("op")))
        else:
            out.append(("name", m.group("name")))
    return out


class _Parser:
    def __init__(self, tokens, ctx):
        self.t = tokens
        self.i = 0
        self.ctx = ctx

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else (None, None)

    def eat(self, kind, val=None):
        k, v = self.peek()
        if k == kind and (val is None or v == val):
            self.i += 1
            return v
        return None

    def parse(self):
        v = self.p_or()
        if self.i != len(self.t):
            raise Unsupported("運算式剩下沒解析完的部分:%r" % (self.t[self.i:],))
        return v

    def p_or(self):
        v = self.p_and()
        while self.eat("op", "||") is not None:
            r = self.p_and()
            v = _truthy(v) or _truthy(r)
        return v

    def p_and(self):
        v = self.p_eq()
        while self.eat("op", "&&") is not None:
            r = self.p_eq()
            v = _truthy(v) and _truthy(r)
        return v

    def p_eq(self):
        v = self.p_unary()
        while True:
            if self.eat("op", "==") is not None:
                v = _eq(v, self.p_unary())
            elif self.eat("op", "!=") is not None:
                v = not _eq(v, self.p_unary())
            else:
                return v

    def p_unary(self):
        if self.eat("op", "!") is not None:
            return not _truthy(self.p_unary())
        return self.p_primary()

    def p_primary(self):
        k, v = self.peek()
        if k == "op" and v == "(":
            self.i += 1
            inner = self.p_or()
            if self.eat("op", ")") is None:
                raise Unsupported("括號沒有收尾")
            return inner
        if k == "str":
            self.i += 1
            return v
        if k == "name":
            self.i += 1
            nk, nv = self.peek()
            if nk == "op" and nv == "(":
                return self.p_call(v)
            return self.lookup(v)
        raise Unsupported("預期一個值,拿到 %r" % (self.peek(),))

    def p_call(self, name):
        self.i += 1  # 吃掉 '('
        args = []
        if not (self.peek() == ("op", ")")):
            args.append(self.p_or())
            while self.eat("op", ",") is not None:
                args.append(self.p_or())
        if self.eat("op", ")") is None:
            raise Unsupported("函式 %s( 沒有收尾" % name)
        if not args and name in _FUNCS0:
            return _FUNCS0[name]
        if name == "contains" and len(args) == 2:
            return str(args[1]) in str(args[0])
        if name == "startsWith" and len(args) == 2:
            return str(args[0]).startswith(str(args[1]))
        if name == "endsWith" and len(args) == 2:
            return str(args[0]).endswith(str(args[1]))
        raise Unsupported("不支援的函式:%s(%d 個參數)" % (name, len(args)))

    def lookup(self, name):
        if name in ("true", "false"):
            return name == "true"
        if name in self.ctx:
            return self.ctx[name]
        # inputs.* 在 push 事件底下不存在 = 空字串(GitHub 的行為)。
        if name.startswith("inputs."):
            return ""
        raise Unsupported("不認得的變數:%s" % name)


def _truthy(v):
    if isinstance(v, bool):
        return v
    if isinstance(v, str):
        return v != ""
    return bool(v)


def _eq(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return _truthy(a) == _truthy(b)
    return str(a) == str(b)


def evaluate(expr, ctx):
    """把 if: 的字串當成 GitHub 運算式算成 True/False。看不懂就丟 Unsupported。"""
    e = expr.strip()
    if e.startswith("${{") and e.endswith("}}"):
        e = e[3:-2]
    return _truthy(_Parser(_tokenize(e), ctx).parse())


# ─────────────────────────── workflow 的最小剖析 ───────────────────────────
def _scalar(node):
    return node.value if isinstance(node, yaml.ScalarNode) else None


def _map_get(node, key):
    if not isinstance(node, yaml.MappingNode):
        return None
    for k, v in node.value:
        if getattr(k, "value", None) == key:
            return v
    return None


def parse_workflow(path):
    """→ (push_branches 或 None, [ {id, if, needs, start, end} ])。行號是 1 起算。"""
    with open(path, encoding="utf-8") as f:
        root = yaml.compose(f)
    if root is None:
        raise Unsupported("%s 是空的" % path)

    on_node = _map_get(root, "on")
    if on_node is None:
        # YAML 1.1 會把裸的 on 當布林 key 解析,但 compose 拿到的節點 value
        # 仍是原字串;真的找不到才是這裡。
        raise Unsupported("%s 裡沒有 on:" % path)
    push = _map_get(on_node, "push")
    branches = None
    if push is not None:
        bn = _map_get(push, "branches")
        if isinstance(bn, yaml.SequenceNode):
            branches = [_scalar(x) for x in bn.value]

    jobs_node = _map_get(root, "jobs")
    if not isinstance(jobs_node, yaml.MappingNode):
        raise Unsupported("%s 裡沒有 jobs:" % path)
    jobs = []
    for k, v in jobs_node.value:
        needs_node = _map_get(v, "needs")
        if isinstance(needs_node, yaml.SequenceNode):
            needs = [_scalar(x) for x in needs_node.value]
        elif isinstance(needs_node, yaml.ScalarNode):
            needs = [needs_node.value]
        else:
            needs = []
        if_node = _map_get(v, "if")
        jobs.append({
            "id": k.value,
            "if": _scalar(if_node),
            "needs": needs,
            "start": k.start_mark.line + 1,
            "end": v.end_mark.line + 1,
        })
    return branches, jobs


def job_runs(jobs, job_id, ctx, _seen=None):
    """這個 job 會不會執行(把 needs: 鏈一起算進去)。→ (bool, 擋住它的 job id)"""
    _seen = _seen or set()
    if job_id in _seen:
        raise Unsupported("needs: 有環,繞到 %s" % job_id)
    _seen = _seen | {job_id}
    byid = {j["id"]: j for j in jobs}
    j = byid.get(job_id)
    if j is None:
        raise Unsupported("找不到 job %s" % job_id)
    if j["if"] is not None and not evaluate(j["if"], ctx):
        return False, job_id
    for n in j["needs"]:
        ok, who = job_runs(jobs, n, ctx, _seen)
        if not ok:
            return False, who
    return True, None


def find_needle_lines(path, needle):
    """針出現在哪些**不是註解**的行上。

    註解裡的字串不算接線 —— 舊的 grep 版本連 `# … --plant X …` 這種說明文字
    都會當成「接上了」,而說明文字正是這個坑的來源。
    """
    hits = []
    with open(path, encoding="utf-8") as f:
        for i, line in enumerate(f, 1):
            if line.lstrip().startswith("#"):
                continue
            if needle in line:
                hits.append(i)
    return hits


def check(path, branch, needles, out=sys.stdout):
    """→ (失敗數, 有沒有踩到判斷不了的東西)"""
    branches, jobs = parse_workflow(path)
    ctx = {
        "github.event_name": "push",
        "github.ref": "refs/heads/%s" % branch,
        "github.ref_name": branch,
        "github.ref_type": "branch",
    }
    failures = 0

    if branches is None:
        out.write("%s on: push: 底下沒有 branches: —— 每一條分支都會觸發\n" % MARK_OK)
    elif branch in branches:
        out.write("%s on: push: branches: 有列 %s\n" % (MARK_OK, branch))
    else:
        out.write("%s on: push: branches: 沒有列 %s(只有:%s)—— "
                  "推上去整份 workflow 一件都不會觸發。\n"
                  % (MARK_PUSH, branch, ", ".join(branches)))
        failures += 1

    for needle in needles:
        lines = find_needle_lines(path, needle)
        if not lines:
            out.write("%s %s —— build.yml 裡沒有這一步(註解不算)。\n"
                      % (MARK_NOT_WIRED, needle))
            failures += 1
            continue
        owners = set()
        for ln in lines:
            for j in jobs:
                if j["start"] <= ln <= j["end"]:
                    owners.add(j["id"])
        if not owners:
            out.write("%s %s 出現在第 %s 行,卻不屬於任何 job —— 這支工具的行號對應壞了。\n"
                      % (MARK_NOT_WIRED, needle, lines))
            failures += 1
            continue
        ok_any, blockers = False, []
        for jid in sorted(owners):
            ok, who = job_runs(jobs, jid, ctx)
            if ok:
                ok_any = True
                out.write("%s %s ← job「%s」會為 %s 執行\n" % (MARK_OK, needle, jid, branch))
                break
            blockers.append((jid, who))
        if not ok_any:
            for jid, who in blockers:
                expr = next(j["if"] for j in jobs if j["id"] == who)
                out.write("%s %s ← job「%s」的 if: 不認 %s,整個 job 會被跳過"
                          "(而 skipped 在 checks 上長得像通過)。\n"
                          "        擋住它的是 job「%s」的 if:%s\n"
                          % (MARK_JOB_IF, needle, jid, branch, who,
                             " ".join((expr or "").split())))
            failures += 1
    return failures


# ────────────────────────────────── 自我測試 ──────────────────────────────────
_WF_TMPL = """
name: t
on:
  push:
    branches:
%(branches)s
jobs:
  fast:
    runs-on: ubuntu-latest
    steps:
      - run: ./scripts/verify_syllables.sh --plant narrow-scope
  emulator:
    needs: fast
    if: >-
      github.event_name == 'workflow_dispatch' ||
      (github.event_name == 'push' &&
       (github.ref == 'refs/heads/main' ||
        github.ref == 'refs/heads/%(emubr)s'))
    runs-on: ubuntu-latest
    steps:
      - run: |
          # 註解裡寫 --plant ghost 不算接線
          bash ./scripts/verify_syllables.sh --plant bad-slot-ids
"""


def _self_test():
    import io
    import os
    import tempfile

    fails = []

    def case(name, cond):
        print(("✓ " if cond else "✗ ") + name)
        if not cond:
            fails.append(name)

    tmp = tempfile.mkdtemp()
    seq = [0]

    def wf(branches, emubr):
        seq[0] += 1
        p = os.path.join(tmp, "wf-%d.yml" % seq[0])
        with open(p, "w", encoding="utf-8") as f:
            f.write(_WF_TMPL % {
                "branches": "".join("      - %s\n" % b for b in branches),
                "emubr": emubr,
            })
        return p

    needles = ["--plant narrow-scope", "--plant bad-slot-ids"]

    def run(path, branch):
        buf = io.StringIO()
        n = check(path, branch, needles, out=buf)
        return n, buf.getvalue()

    # 1. 兩道閘門都通:綠。
    n, txt = run(wf(["main", "t9"], "t9"), "t9")
    case("兩道閘門都通 → 0 條紅", n == 0)
    case("  而且沒有出現任何 FAIL 標記", "FAIL" not in txt)

    # 2. 分支不在 on: push: branches: → 必須紅,而且紅在 push-branches 上。
    n, txt = run(wf(["main"], "t9"), "t9")
    case("分支不在 branches: → 紅", n > 0)
    case("  紅的是 push-branches 那一道", MARK_PUSH in txt)

    # 3. 分支在清單裡、但 job 的 if: 不認它 → 必須紅,而且紅在 job-if 上。
    n, txt = run(wf(["main", "t9"], "other"), "t9")
    case("job 的 if: 不認這條分支 → 紅", n > 0)
    case("  紅的是 job-if 那一道", MARK_JOB_IF in txt)
    case("  而 push-branches 那一道是綠的(不會張冠李戴)", MARK_PUSH not in txt)
    case("  快車道那一根針仍然是綠的", "PASS --plant narrow-scope" in txt)

    # 4. 接線根本不在 → not-wired。
    p = wf(["main", "t9"], "t9")
    with open(p, encoding="utf-8") as f:
        kept = [ln for ln in f if "--plant narrow-scope" not in ln]
    with open(p, "w", encoding="utf-8") as f:
        f.writelines(kept)
    n, txt = run(p, "t9")
    case("拆掉一條接線 → 紅", n > 0)
    case("  紅的是 not-wired 那一道", MARK_NOT_WIRED in txt)

    # 5. 只寫在註解裡的接線不算數(舊的 grep 版本會把它當成接上了)。
    p = wf(["main", "t9"], "t9")
    buf = io.StringIO()
    n = check(p, "t9", ["--plant ghost"], out=buf)
    case("只出現在註解裡的 --plant ghost → 紅", n > 0)
    case("  紅的是 not-wired 那一道", MARK_NOT_WIRED in buf.getvalue())

    # 6. 運算式直譯器本身。
    ctx = {"github.event_name": "push", "github.ref": "refs/heads/t9",
           "github.ref_name": "t9", "github.ref_type": "branch"}
    case("== 成立", evaluate("github.ref == 'refs/heads/t9'", ctx) is True)
    case("== 不成立", evaluate("github.ref == 'refs/heads/x'", ctx) is False)
    case("|| 短路", evaluate("github.ref == 'refs/heads/x' || github.ref_name == 't9'", ctx))
    case("&& 成立", evaluate("github.event_name == 'push' && github.ref_name == 't9'", ctx))
    case("! 反轉", evaluate("!(github.event_name == 'push')", ctx) is False)
    case("workflow_dispatch 的 inputs 在 push 底下是假",
         evaluate("github.event_name == 'workflow_dispatch' && inputs.publish", ctx) is False)
    case("contains()", evaluate("contains(github.ref, 'heads')", ctx))
    case("always()", evaluate("always()", ctx))
    try:
        evaluate("github.event.head_commit.message == 'x'", ctx)
        case("不認得的變數要丟 Unsupported", False)
    except Unsupported:
        case("不認得的變數要丟 Unsupported", True)
    try:
        evaluate("fromJSON('[1]')[0] == 1", ctx)
        case("不支援的語法要丟 Unsupported", False)
    except Unsupported:
        case("不支援的語法要丟 Unsupported", True)

    print()
    if fails:
        print("✗ 自我測試失敗 %d 條:%s" % (len(fails), "; ".join(fails)))
        return 1
    print("✓ 自我測試全過")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--workflow")
    ap.add_argument("--branch")
    ap.add_argument("--needle", action="append", default=[])
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return _self_test()
    if not a.workflow or not a.branch:
        ap.error("--workflow 與 --branch 都是必要的")
    try:
        n = check(a.workflow, a.branch, a.needle)
    except Unsupported as e:
        sys.stderr.write("!! 這支工具判斷不了:%s\n"
                         "   判斷不了不可以當成綠 —— 請把 if: 寫回看得懂的形狀,"
                         "或把這一種語法補進 ci_branch_gate.py。\n" % e)
        return 2
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
