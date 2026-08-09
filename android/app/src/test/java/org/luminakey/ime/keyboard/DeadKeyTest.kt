package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.ActionVerb
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.LayoutKey
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures
import org.luminakey.ime.theme.ThemeLoader
import java.io.File

/**
 * 「畫面完全正常、自動化全過、但按下去什麼都沒發生」的那一類鍵。
 *
 * ── 為什麼需要一條專門的防線 ────────────────────────────────────────────
 * 這個專案已經抓到五顆:重輸鍵呼叫的是結束組字而不是清空、中英鍵切了模式
 * 卻不換佈局、按下後顏色回不來、工具列的表情鍵什麼都不做、intl-gboard 逗號
 * 的標點彈出盤被一個指向表情面板的 `long_press` 整個遮住。
 *
 * 它們沒有一顆是被既有測試抓到的,原因也一致:**每一項單獨看都是對的**。
 * YAML 合法、解析器沒有診斷、鍵畫得出來、按下去有回饋色與震動。缺的是一個
 * 會去問「這顆鍵宣稱做的事,這一端做得到嗎」的東西。
 *
 * 這一檔就是那個東西。它不驗打不打得出字 —— 那是
 * [org.luminakey.ime.keyboard.LayoutEscapeTest] 與 `verify_rime_compose.sh` 的事。
 *
 * ── 它抓不到什麼(誠實說明) ─────────────────────────────────────────────
 * 它驗的是**宣告**:動詞在不在 [VerbSupport.UNIMPLEMENTED] 裡、分派表有沒有
 * 那一支。它沒辦法驗「那支分派寫得對不對」—— `CLEAR` 呼叫成 `finishComposingText`
 * 那種缺陷,型別與字面都合法,只有真的按下去才看得出來。那一層必須是
 * `scripts/verify_rime_compose.sh` 走真正的按鍵路徑,以及人在真機上按一次。
 */
class DeadKeyTest {

    /* ───────────────── 1. 內容:沒有一顆鍵指向本端做不到的事 ───────────────── */

    /**
     * `core/layouts` 的**每一份**、每一層、每一列、每一顆鍵,連同彈出盤與滑動
     * 的子鍵 —— 任何一條走得到的動作都不可以是本端還沒實作的動詞。
     *
     * 佈局故意不在執行期過濾:鍵有寬度,少一顆整列會重排。所以擋在這裡,
     * 由佈局作者決定那個位置該放什麼(intl-ios 的做法是把 1.00 單位還給空白鍵)。
     */
    @Test
    fun `佈局裡沒有一顆鍵指向尚未實作的動詞`() {
        val repo = FixtureRepo()
        val dead = mutableListOf<String>()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value
                ?: error("佈局 $id 載不起來,先修那個")
            for ((where, action) in actionsOf(layout)) {
                if (!VerbSupport.isImplemented(action.verb)) {
                    dead += "$id $where → '${action.raw}'(${action.verb})"
                }
            }
        }
        assertTrue(
            "這幾顆鍵按下去不會有任何反應 —— 動詞在 VerbSupport.UNIMPLEMENTED 裡:\n  " +
                dead.joinToString("\n  ") +
                "\n把那個位置改成別的鍵,或是把動詞實作出來再從 UNIMPLEMENTED 移除。",
            dead.isEmpty(),
        )
    }

    /**
     * 工具列可以在執行期過濾(LazyRow,少一項不影響其他項的幾何),所以主題**允許**
     * 留著本端做不到的項目 —— §8.6.6.1 的規範性預設工具列就含 `emoji`,刻意不刪,
     * 桌面端做出表情面板時不必反過來改主題。
     *
     * 但過濾完不能把使用者鎖死:§8.6.6.1 的必備項(方案選單、設定)是他修好其他
     * 一切問題的入口,任何一份主題濾完都必須還留著它們。
     *
     * ⚠ 這條**不會**因為某份主題的 YAML 裡沒寫 globe 而紅 —— ThemeParser 的
     * `REQUIRED_TOOLBAR_ITEMS` 本來就會補回來,那是設計不是缺陷。它擋的是補回來
     * 之後**又被本端的不支援清單濾掉**:只要有人把 SCHEMA_PICKER 誤放進
     * [VerbSupport.UNIMPLEMENTED],12 份主題會同時失去換鍵盤的唯一入口,
     * 而畫面上只是少一顆圖示。已用植入驗證過這條會紅。
     */
    @Test
    fun `每一份主題的工具列濾掉做不到的項目之後仍留著必備項`() {
        for (id in allThemeIds()) {
            val theme = ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID, locale = LOCALE)
                .value ?: error("主題 $id 載不起來,先修那個")
            val kept = theme.candidates.bar.toolbar.items
                .filter { VerbSupport.isImplemented(it.tap.verb) }
                .map { it.tap.verb }
                .toSet()
            assertTrue(
                "$id 的工具列濾完之後沒有方案選單,使用者換不了鍵盤",
                ActionVerb.SCHEMA_PICKER in kept,
            )
            assertTrue(
                "$id 的工具列濾完之後沒有設定,使用者到不了任何開關",
                ActionVerb.SETTINGS in kept,
            )
        }
    }

    /* ───────────────── 2. 遮蔽:兩個手勢搶同一個入口 ───────────────── */

    /**
     * `long_press` 與 `popup` 不可以同時存在。
     *
     * §9.6 的長按解析是 `long_press` 勝過 `popup`,兩者都寫等於那個彈出盤
     * **永遠叫不出來** —— 而它在 YAML 裡看起來完全正常,解析器也不會有話說。
     * intl-gboard 的逗號就是這樣把自己的「、？！…」遮了四份佈局那麼久。
     *
     * 解析器層已經保證 `repeat` 與 `long_press` 不同時存在(§6.3),這裡補上另一半。
     */
    @Test
    fun `沒有一顆鍵同時宣告 long_press 與 popup`() {
        val repo = FixtureRepo()
        val shadowed = mutableListOf<String>()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value ?: error("佈局 $id 載不起來")
            for (layer in layout.layers) {
                for (row in layer.rows) {
                    for (key in row.keys) {
                        if (key.longPress != null && key.popup != null) {
                            shadowed += "$id/${layer.id}/${key.id ?: key.label}"
                        }
                    }
                }
            }
        }
        assertTrue(
            "這幾顆鍵的彈出盤永遠叫不出來(§9.6:long_press 勝過 popup):\n  " +
                shadowed.joinToString("\n  "),
            shadowed.isEmpty(),
        )
    }

    /**
     * 每一顆看得見的鍵都必須至少做一件事。
     *
     * `spacer` 是明確宣告的空位,不算;除此之外一顆畫得出來的鍵如果 tap、send、
     * double_tap、long_press、popup、swipe 全部都沒有,那它就是一顆裝飾品 ——
     * 而使用者沒辦法知道它是裝飾品,他只會按了又按。
     */
    @Test
    fun `每一顆非 spacer 的鍵都至少宣告了一種行為`() {
        val repo = FixtureRepo()
        val inert = mutableListOf<String>()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value ?: error("佈局 $id 載不起來")
            for (layer in layout.layers) {
                for (row in layer.rows) {
                    for (key in row.keys) {
                        if (key.spacer) continue
                        val does = key.hasTapBehavior ||
                            key.doubleTap != null ||
                            key.longPress != null ||
                            key.popup != null ||
                            key.swipe.isNotEmpty()
                        if (!does) inert += "$id/${layer.id}/${key.id ?: key.label}"
                    }
                }
            }
        }
        assertTrue(
            "這幾顆鍵畫得出來但什麼都不做,又沒宣告 spacer:\n  " + inert.joinToString("\n  "),
            inert.isEmpty(),
        )
    }

    /* ───────────────── 3. 分派:宣稱實作的動詞真的有一支分派 ───────────────── */

    /**
     * [VerbSupport.UNIMPLEMENTED] 以外的每一個動詞,都必須在
     * `RimeInputMethodService.handleAction` 的 `when` 裡有自己的分支。
     *
     * ── 為什麼是掃原始碼 ────────────────────────────────────────────────
     * `handleAction` 是 InputMethodService 的私有方法,JVM 單元測試呼叫不到它
     * (要 Robolectric 或連真機)。而它正是「動詞 → 真的做了什麼」的唯一交會點。
     * 掃檔案不優雅,但這一檔本來就是在守一類**只在整合處才看得出來**的缺陷,
     * 而且本專案已有先例([org.luminakey.ime.StringCatalogTest] 直接讀 res)。
     *
     * 這條抓的是新增一個動詞、解析器認得、YAML 寫得出來,卻忘了接分派的情況 ——
     * 那顆鍵會安靜地什麼都不做,和表情鍵一模一樣。
     */
    @Test
    fun `每一個已實作的動詞在分派表裡都有一支`() {
        val body = handleActionBody()
        val missing = ActionVerb.values()
            .filter { it !in VerbSupport.UNIMPLEMENTED }
            .filterNot { Regex("""ActionVerb\.${it.name}\b""").containsMatchIn(body) }
        assertEquals(
            "這幾個動詞解析得出來、卻在 handleAction 裡沒有分支 —— " +
                "寫得出這個動作的佈局會得到一顆按了沒反應的鍵",
            emptyList<ActionVerb>(),
            missing,
        )
    }

    /**
     * 「有一支分支」還不夠 —— 那一支不可以只是 `Unit`。
     *
     * ── 這條是實測補上的,不是想出來的 ──────────────────────────────────
     * 上面那條只問「`when` 裡出不出現這個動詞的名字」。把 `EMOJI` 從
     * [VerbSupport.UNIMPLEMENTED] 拿掉之後實測:
     *
     *   · 工具列的過濾條件消失 → 12 份主題裡的表情鍵**全部長回來**
     *     (default-* 明寫、其餘 8 份繼承),模擬器上看得到那顆 ☺;
     *   · 分派落到 `ActionVerb.EMOJI -> Unit`,按下去什麼都不會發生;
     *   · **367 項單元測試全過。**
     *
     * 也就是說,這套機制原本可以被一行刪除安靜地繞過,而症狀正是它當初要防的
     * 那一個。`when` 需要窮盡,所以未實作的動詞必須留一支空分支 —— 那就讓
     * 「空分支」與「未實作清單」互相釘住:**只要不在清單裡,就不准是空的。**
     *
     * `NOOP` 是唯一的例外,它的語義本來就是什麼都不做(§9.5:未知 verb 會被
     * 降級成 noop)。
     */
    @Test
    fun `已實作的動詞沒有一個落在空的分派分支上`() {
        val body = handleActionBody()
        val silent = ActionVerb.values()
            .filter { it !in VerbSupport.UNIMPLEMENTED }
            .filter { it != ActionVerb.NOOP }
            .filter { v ->
                Regex(
                    """ActionVerb\.${v.name}\b[^\n]*->\s*Unit\s*$""",
                    RegexOption.MULTILINE,
                ).containsMatchIn(body)
            }
        assertEquals(
            "這幾個動詞宣稱已實作,但 handleAction 的分支是空的 `-> Unit` —— " +
                "鍵畫得出來、按下去有回饋色,就是什麼都不會發生。\n" +
                "要嘛把它實作出來,要嘛把它加回 VerbSupport.UNIMPLEMENTED " +
                "(工具列會據此不渲染它)。",
            emptyList<ActionVerb>(),
            silent,
        )
    }

    /**
     * 反過來:還沒實作的動詞必須在進 `when` **之前**就被擋掉。
     *
     * 少了這道早退,`UNIMPLEMENTED` 就只是一份沒有人讀的清單,分派照樣會落進
     * 一個安靜的分支 —— 也就回到最初的問題。
     */
    @Test
    fun `分派在進 when 之前先問過 VerbSupport`() {
        val src = serviceSource()
        val head = src.substringAfter("private fun handleAction(action: KeyAction) {")
            .substringBefore("when (action.verb) {")
        assertTrue(
            "handleAction 沒有在 when 之前檢查 VerbSupport.isImplemented,\n" +
                "未實作的動詞會安靜地落進 when 的某個分支",
            head.contains("VerbSupport.isImplemented"),
        )
    }

    /* ────────────────────────────── 夾具 ────────────────────────────── */

    /** 一顆鍵身上所有走得到的動作,連同「它在哪裡」以便錯誤訊息指得出位置。 */
    private fun actionsOf(layout: KeyboardLayout): List<Pair<String, KeyAction>> {
        val out = mutableListOf<Pair<String, KeyAction>>()
        for (layer in layout.layers) {
            for (row in layer.rows) {
                for (key in row.keys) {
                    val at = "${layer.id}/${key.id ?: key.label}"
                    fun add(what: String, a: KeyAction?) {
                        if (a != null) out += "$at $what" to a
                    }
                    add("tap", key.tap)
                    add("double_tap", key.doubleTap)
                    add("long_press", key.longPress)
                    key.popup?.keys?.forEachIndexed { i, sub -> add("popup[$i]", sub.tap) }
                    key.swipe.forEach { (dir, sub) -> add("swipe:$dir", sub.tap) }
                }
            }
        }
        return out
    }

    /**
     * `core/themes` 底下**每一份**,不是白名單。
     *
     * [RepoFixtures.themeIds] 一度只有四個 id,於是十二份主題有八份從來沒有被
     * 任何測試載入過 —— 與佈局清單踩過的是同一個坑。
     */
    private fun allThemeIds(): List<String> =
        File(RepoFixtures.coreDir, "themes").listFiles().orEmpty()
            .filter { it.isFile && it.name.endsWith(".yaml") }
            .map { it.name.removeSuffix(".yaml") }
            .sorted()

    /**
     * `handleAction` 的 `when` 本體。
     *
     * 長度下限不是防禦性程式設計,是這一檔的命脈:方法改名或搬家時
     * `substringAfter` 會安靜地回傳整份檔案或空字串,兩條分派測試就會
     * **在該紅的時候變綠**。寧可讓它直接紅在這裡。
     */
    private fun handleActionBody(): String {
        val src = serviceSource()
        val body = src.substringAfter("private fun handleAction(action: KeyAction) {")
            .substringBefore("\n    private fun sendHostKey(")
        assertTrue(
            "找不到 handleAction 的內容(長度 ${body.length})—— " +
                "方法改名或搬家了就要改這裡,不能讓這兩條測試靜靜地不跑",
            body.length in 501..20000 && body.contains("when (action.verb) {"),
        )
        return body
    }

    private fun serviceSource(): String {
        val f = File(
            RepoFixtures.coreDir.parentFile,
            "android/app/src/main/java/org/luminakey/ime/RimeInputMethodService.kt",
        )
        assertTrue("找不到 ${f.path} —— 檔案搬家了就要改這裡,不能讓這條測試靜靜地不跑", f.isFile)
        return f.readText(Charsets.UTF_8)
    }

    private companion object {
        const val LOCALE = "zh-Hant-TW"
    }
}
