package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.RepoFixtures
import java.io.File

/**
 * 「佈局裡寫著、但使用者一次都按不到」的那一整類：`swipe:`。
 *
 * ── 這一檔為什麼存在 ────────────────────────────────────────────────────
 * 覆核抓到的：`ActionVerb.CURSOR_LEFT/RIGHT/HOME/END` 在**整個 app 裡沒有任何
 * 使用者觸達得到的路徑**。所有佈局的 `cursor:*` 都掛在 `swipe:` 底下，而
 * `KeyboardView` 那顆鍵只有 `detectTapGestures` —— 沒有任何拖曳／滑動偵測。
 * 於是有一段程式碼讀起來像功能、測起來像功能，而使用者按不到。
 *
 * 這一檔把那件事**寫下來並守住**，做三件事：
 *
 *   1. 釘住前提：Android 端現在確實沒有 swipe 分派。哪天有人接上去（那是
 *      滑動分派那條線的工作），這一支會紅 —— 提醒他把佈局檔頭那段
 *      「按不到」的註解、規範 §9.6 的對照表、以及這一檔一起更新。
 *   2. 每一條 `swipe:` 都必須落在 §9.6 那張「另有他途」對照表的四類之一。
 *      規範明訂**佈局作者 MUST NOT 把功能設計成只有 swipe 能觸達**；
 *      多出來的第五類就是一條沒有人按得到的死路。
 *   3. 每一份含 `swipe:` 的佈局檔頭都要寫著它按不到。沒有那一行的話，
 *      下一個人讀到 `swipe: { left: { tap: "cursor:left" } }` 會以為它會動。
 *
 * ── 它**不**做什麼（誠實說明）────────────────────────────────────────────
 * 它不會讓 swipe 動起來。這一輪刻意沒有實作滑動分派：`KeyboardView` 那條
 * 手勢路徑是全 app 最常走的一條，而 tap／長按／自動重複／彈出盤／無障礙
 * 五件事共用它；把四向拖曳擠進去要動的是那一整段，不是加一個分支。
 * 那件事排在滑動分派那條線上做，不是在一個修正批次裡順手做。
 */
class LayoutSwipeReachabilityTest {

    /* ──────────── 1. 前提：現在真的沒有人分派 swipe ──────────── */

    /**
     * `KeyboardView` 只有點擊手勢。這一條**不是**在守「不要實作 swipe」——
     * 它守的是「這一檔其餘兩條的前提還成立」。
     */
    @Test
    fun `Android 端現在沒有任何 swipe 分派`() {
        val view = source("src/main/java/org/luminakey/ime/keyboard/KeyboardView.kt")
        val hits = mutableListOf<String>()
        if (view.contains("key.swipe")) hits += "KeyboardView.kt 讀了 key.swipe"
        for (needle in DRAG_APIS) {
            if (view.contains(needle)) hits += "KeyboardView.kt 用了 $needle"
        }
        assertTrue(
            "看起來滑動分派已經接上了。那是好事 —— 但這一檔與它引用的三處說明" +
                "現在都變成假話，請一起更新：\n" +
                "  · core/layouts/*.yaml 檔頭那段「$MARK」\n" +
                "  · docs/theme-format.md §9.6 的「另有他途」對照表\n" +
                "  · 本檔（改成驗每一條 swipe 真的送得出它的動作）\n" +
                "命中：" + hits.joinToString("、"),
            hits.isEmpty(),
        )
    }

    /* ──────────── 2. 每一條 swipe 都另有他途 ──────────── */

    /**
     * §9.6：「佈局作者 MUST NOT 把任何功能設計成只有 swipe 能觸達。」
     *
     * 判準刻意是**分類**而不是「有沒有替代鍵」：`cursor:left` 的等效路徑是
     * 「直接點宿主的輸入框」，那不在佈局裡，任何掃佈局的規則都看不到它。
     * 所以這裡逐類對照規範那張表，而多出來的類別一律當成死路。
     */
    @Test
    fun `每一條 swipe 都落在 §9-6 的四類捷徑之一`() {
        val repo = FixtureRepo()
        val unknown = mutableListOf<String>()
        val counted = mutableMapOf<String, Int>()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value ?: error("佈局 $id 載不起來,先修那個")
            for (site in swipeSites(layout, id)) {
                val kind = classify(site)
                if (kind == null) unknown += "$site"
                else counted[kind] = (counted[kind] ?: 0) + 1
            }
        }
        assertTrue(
            "這幾條 swipe 不在 §9.6 的對照表裡 —— 也就是沒有寫下它的等效路徑。\n" +
                "而 swipe 現在沒有人分派，所以它們是使用者**按不到**的功能：\n  " +
                unknown.joinToString("\n  ") +
                "\n要嘛給它一條別的路並補進 §9.6 的表，要嘛把它從佈局裡拿掉。",
            unknown.isEmpty(),
        )
        // 四類都必須真的有樣本。少了一類 = 對照表有一行是空頭支票，
        // 或者這支測試的分類器沒有真的走到那條分支。
        assertEquals(
            "§9.6 的四類捷徑必須每一類都在佈局裡找得到樣本",
            KINDS.toSortedSet(),
            counted.keys.toSortedSet(),
        )
    }

    /** 分類器本身要擋得住沒見過的東西，否則第 2 條永遠是綠的。 */
    @Test
    fun `分類器不認得的動作一律當成死路`() {
        val bogus = SwipeSite("qwerty", "lower", "q", "UP", "emoji", popup = false, longPress = false)
        assertTrue("沒見過的動作被默默放行了", classify(bogus) == null)
        // 正控：真的認得的那幾條不能也回 null，否則上面那條是空的。
        assertTrue(
            classify(bogus.copy(action = "cursor:left", key = "space")) != null,
        )
    }

    /* ──────────── 3. 佈局檔頭要說實話 ──────────── */

    @Test
    fun `有 swipe 的佈局都在檔頭寫著它按不到`() {
        val missing = mutableListOf<String>()
        val marked = mutableListOf<String>()
        for (id in RepoFixtures.layoutIds) {
            val text = File(RepoFixtures.coreDir, "layouts/$id.yaml").readText(Charsets.UTF_8)
            val hasSwipe = text.contains("swipe:")
            val hasMark = text.contains(MARK)
            if (hasSwipe && !hasMark) missing += id
            if (hasMark) marked += id
            if (!hasSwipe && hasMark) missing += "$id（沒有 swipe 卻標了「$MARK」）"
        }
        assertTrue(
            "這幾份佈局有 swipe 卻沒有說它按不到：${missing.joinToString("、")}",
            missing.isEmpty(),
        )
        assertTrue("一份都沒標到 —— 這條大概量錯了地方", marked.isNotEmpty())
    }

    /* ────────────────────────────── 夾具 ────────────────────────────── */

    internal data class SwipeSite(
        val layout: String,
        val layer: String,
        val key: String,
        val dir: String,
        val action: String,
        val popup: Boolean,
        val longPress: Boolean,
    ) {
        override fun toString(): String = "$layout/$layer/$key swipe:$dir → $action"
    }

    private fun swipeSites(layout: KeyboardLayout, id: String): List<SwipeSite> {
        val out = mutableListOf<SwipeSite>()
        for (layer in layout.layers) {
            for (row in layer.rows) {
                for (key in row.keys) {
                    for ((dir, sub) in key.swipe) {
                        val action = sub.tap?.raw
                            ?: sub.send?.let { "send" }
                            ?: "<空>"
                        out += SwipeSite(
                            layout = id,
                            layer = layer.id,
                            key = key.id ?: key.label,
                            dir = dir.name,
                            action = action,
                            popup = key.popup?.keys?.isNotEmpty() == true,
                            longPress = key.longPress != null,
                        )
                    }
                }
            }
        }
        return out
    }

    /**
     * `docs/theme-format.md` §9.6 那張表，一行一類。回 `null` = 表上沒有。
     *
     * 每一類的第二欄（等效路徑）寫在這裡而不是只寫在規範裡，是因為規範改了
     * 而程式沒改時，這裡會是那個「兩邊對不上」的地方 —— 有一個地方對不上，
     * 好過兩邊都很順而使用者按不到。
     */
    private fun classify(s: SwipeSite): String? = when {
        // 字母鍵上滑出數字 → 長按彈出盤、或 numeric-symbol 佈局的數字面
        s.dir == "UP" && s.action == "send" -> KIND_DIGIT
        // 空白鍵左右滑移動游標 → 直接點宿主的輸入框（宿主提供，不在佈局裡）
        s.action == "cursor:left" || s.action == "cursor:right" -> KIND_CURSOR
        // 退格左滑清除 → 按住退格自動重複，直到清空
        s.action == "clear" && s.dir == "LEFT" -> KIND_CLEAR
        // 注音空白鍵上下滑翻頁 → 候選列的翻頁指示器 / 展開鈕
        s.action == "candidate:next_page" || s.action == "candidate:prev_page" -> KIND_PAGE
        else -> null
    }

    private fun source(rel: String): String {
        val f = File(rel)
        return f.takeIf { it.isFile }?.readText(Charsets.UTF_8)
            ?: error("找不到 ${f.path} —— 單元測試的工作目錄應該是 android/app")
    }

    private companion object {
        /** 與 `core/layouts` 底下每一份佈局檔頭那一行必須一字不差。 */
        const val MARK = "⚠ swipe 現在按不到"

        const val KIND_DIGIT = "字母鍵上滑出數字"
        const val KIND_CURSOR = "空白鍵左右滑移動游標"
        const val KIND_CLEAR = "退格左滑清除"
        const val KIND_PAGE = "空白鍵上下滑翻頁"
        val KINDS = listOf(KIND_DIGIT, KIND_CURSOR, KIND_CLEAR, KIND_PAGE)

        /** Compose 裡做得出四向滑動的入口。有任何一個出現就代表前提變了。 */
        val DRAG_APIS = listOf(
            "detectDragGestures",
            "detectHorizontalDragGestures",
            "detectVerticalDragGestures",
            "awaitDragOrCancellation",
            "SwipeDirection",
        )
    }
}
