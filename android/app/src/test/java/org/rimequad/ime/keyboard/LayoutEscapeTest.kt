package org.rimequad.ime.keyboard

import org.junit.Assert.assertTrue
import org.junit.Test
import org.rimequad.ime.theme.ActionVerb
import org.rimequad.ime.theme.KeyAction
import org.rimequad.ime.theme.RepoFixtures

/**
 * 「進得去、出不來」的通用防線。
 *
 * 真機回報的缺陷（九宮格按 ABC 之後回不去）不是某一顆鍵寫錯，而是一整類問題：
 * 只要有一份佈局的某個層，**看得見的鍵**裡沒有任何一條路徑走得回起點，
 * 使用者就被鎖死了。逐條列舉「九宮格→英數→九宮格」這種路徑永遠會漏掉下一個，
 * 所以這裡改成把導覽鍵當成一張圖來走：
 *
 *   從每一份佈局的預設層出發，用**只有 tap 觸發**的導覽鍵（`layer` /
 *   `layer_once` / `layer_lock` / `switch_layout`）能走到的每一個狀態，
 *   都必須走得回出發點。
 *
 * 刻意**不**採計 `long_press`、`swipe`、`double_tap` 與 popup 裡的動作：
 * 藏在手勢裡的回程等於沒有回程 —— 使用者卡住的當下不會知道要長按哪裡。
 * （schema:picker 現在也救得回來，見 RimeInputMethodService.selectSchema，
 * 但那是安全網，不能拿來當正常的回程。）
 */
class LayoutEscapeTest {

    /** 走幾步。目前最長的一條合法路徑是 3 步，留一點餘裕。 */
    private val depth = 4

    private val navVerbs = setOf(
        ActionVerb.LAYER, ActionVerb.LAYER_ONCE, ActionVerb.LAYER_LOCK, ActionVerb.SWITCH_LAYOUT
    )

    @Test
    fun everyReachableLayerCanGetBackToWhereItStarted() {
        for (start in RepoFixtures.layoutIds) {
            val origin = replay(start, emptyList()).state()
            val reachable = reachableFrom(start, origin)
            for ((state, path) in reachable) {
                if (state == origin) continue
                assertTrue(
                    "佈局 $start：走 ${describe(start, path)} 之後停在 $state，" +
                        "接下來 $depth 步之內沒有任何看得見的鍵能回到 $origin —— 這是死路",
                    canReturnTo(start, path, origin)
                )
            }
        }
    }

    /** `layer:<id>` 指向本佈局不存在的層 = 那顆鍵是啞的，也是卡死的來源之一。 */
    @Test
    fun everyLayerReferenceResolvesInsideItsOwnLayout() {
        val repo = FixtureRepo()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value!!
            val known = layout.layers.map { it.id }.toSet()
            for (layer in layout.layers) {
                for (action in navActionsIn(layout.layer(layer.id)!!.rows.flatMap { it.keys })) {
                    val arg = action.arg ?: continue
                    if (action.verb == ActionVerb.SWITCH_LAYOUT) {
                        assertTrue(
                            "$id/${layer.id}: switch_layout 指向不存在的佈局 '$arg'",
                            arg == "@primary" || arg == "@previous" ||
                                RepoFixtures.layoutIds.contains(arg)
                        )
                    } else {
                        assertTrue(
                            "$id/${layer.id}: ${action.raw} 指向本佈局沒有的層 '$arg'",
                            known.contains(arg)
                        )
                    }
                }
            }
        }
    }

    /* ── 圖的走訪 ─────────────────────────────────────────────────────── */

    private fun LayoutHost.state(): String = "${layout?.id}/$layerId"

    /** 只認 tap；藏在手勢裡的回程不算回程。 */
    private fun navActionsIn(keys: List<org.rimequad.ime.theme.LayoutKey>): List<KeyAction> =
        keys.mapNotNull { it.tap }.filter { navVerbs.contains(it.verb) }

    private fun navActions(h: LayoutHost): List<KeyAction> =
        navActionsIn(h.layout?.layer(h.layerId)?.rows.orEmpty().flatMap { it.keys })

    /** 每次都從頭重放：`@previous` 有歷史，只記 (佈局, 層) 會騙自己。 */
    private fun replay(startLayout: String, path: List<Int>): LayoutHost {
        val h = LayoutHost(FixtureRepo())
        h.ensureLoaded()                 // 使用者的起點永遠是 primary
        h.switchLayout(startLayout)
        for (i in path) {
            val actions = navActions(h)
            apply(h, actions[i])
        }
        return h
    }

    private fun apply(h: LayoutHost, a: KeyAction) {
        val arg = a.arg ?: return
        when (a.verb) {
            ActionVerb.LAYER -> h.setLayer(arg)
            ActionVerb.LAYER_ONCE -> h.setLayerOnce(arg)
            ActionVerb.LAYER_LOCK -> h.lockLayer(arg)
            ActionVerb.SWITCH_LAYOUT -> h.switchLayout(arg)
            else -> Unit
        }
    }

    private fun reachableFrom(start: String, origin: String): Map<String, List<Int>> {
        val seen = LinkedHashMap<String, List<Int>>()
        seen[origin] = emptyList()
        var frontier = listOf<List<Int>>(emptyList())
        repeat(depth) {
            val next = ArrayList<List<Int>>()
            for (p in frontier) {
                val h = replay(start, p)
                for (i in navActions(h).indices) {
                    val np = p + i
                    val s = replay(start, np).state()
                    if (!seen.containsKey(s)) {
                        seen[s] = np
                        next += np
                    }
                }
            }
            frontier = next
        }
        return seen
    }

    private fun canReturnTo(start: String, prefix: List<Int>, origin: String): Boolean {
        var frontier = listOf(prefix)
        val visited = HashSet<String>()
        repeat(depth) {
            val next = ArrayList<List<Int>>()
            for (p in frontier) {
                val h = replay(start, p)
                for (i in navActions(h).indices) {
                    val np = p + i
                    val s = replay(start, np).state()
                    if (s == origin) return true
                    if (visited.add(s + "@" + np.size)) next += np
                }
            }
            frontier = next
        }
        return false
    }

    private fun describe(start: String, path: List<Int>): String {
        val steps = ArrayList<String>()
        for (n in path.indices) {
            val h = replay(start, path.take(n))
            steps += navActions(h)[path[n]].raw
        }
        return steps.joinToString(" → ")
    }
}
