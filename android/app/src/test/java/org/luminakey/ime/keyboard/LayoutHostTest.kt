package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.LayoutLoader
import org.luminakey.ime.theme.LoadResult
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures
import org.luminakey.ime.theme.Theme
import org.luminakey.ime.theme.ThemeLoader
import org.junit.Test

/**
 * [LayoutHost] 的層／佈局狀態機。
 *
 * 這批測試存在的理由是一個真機缺陷：使用者在九宮格按下「ABC」切到英數之後，
 * **再也回不到九宮格**。當時的組合是「t9-pinyin 的 ABC 鍵 → `@primary` →
 * qwerty」，而 qwerty 上沒有任何一顆鍵能回九宮格。狀態機本身沒有壞，壞的是
 * 「出去的路存在、回來的路不存在」——這正是純邏輯測試該擋下、而組字層級的
 * 驗證腳本永遠看不見的一類缺陷。
 *
 * 夾具直接讀 repo 的 `core/layouts`（見 [RepoFixtures]），因此這裡守的是
 * **真的會裝進 APK 的那幾份 yaml**，不是測試專用的複製品。
 */
class LayoutHostTest {

    private fun host() = LayoutHost(FixtureRepo())

    /** 開機狀態：qwerty。使用者的起點永遠是這裡。 */
    private fun booted(): LayoutHost = host().apply { ensureLoaded() }

    private fun LayoutHost.state(): String = "${layout?.id}/$layerId"

    /* ── @previous ────────────────────────────────────────────────────── */

    @Test
    fun previousReturnsToTheLayoutYouCameFrom() {
        val h = booted()
        h.switchLayout("t9-pinyin")
        h.switchLayout("numeric-symbol")
        assertEquals("numeric-symbol", h.layout?.id)

        h.switchLayout("@previous")
        assertEquals("從符號佈局回去的必須是九宮格，不是 primary", "t9-pinyin", h.layout?.id)
    }

    @Test
    fun previousFallsBackToPrimaryWhenThereIsNoHistory() {
        val h = host()
        h.switchLayout("numeric-symbol")   // 沒有前一份佈局
        h.switchLayout("@previous")
        assertEquals("qwerty", h.layout?.id)
    }

    /**
     * `@previous` 永遠是「離開這裡」。萬一記錄指回自己，必須退到 primary
     * 而不是變成 no-op —— no-op 就是使用者按了沒反應，就是出不去。
     */
    @Test
    fun previousNeverResolvesToTheCurrentLayout() {
        val h = booted()
        h.switchLayout("numeric-symbol")
        h.switchLayout("@previous")        // → qwerty，此時 previous = numeric-symbol
        h.switchLayout("numeric-symbol")   // previous 變回 qwerty
        assertEquals("numeric-symbol", h.layout?.id)
        h.switchLayout("@previous")
        assertEquals("qwerty", h.layout?.id)
    }

    /** 已經在目標佈局上時，層要歸位到 default_layer；停在符號層不算「回去」。 */
    @Test
    fun switchingToTheLayoutYouAreAlreadyOnResetsTheLayer() {
        val h = booted()
        h.switchLayout("numeric-symbol")
        h.setLayer("symbol")
        assertEquals("numeric-symbol/symbol", h.state())

        h.switchLayout("numeric-symbol")
        assertEquals("numeric-symbol/numeric", h.state())
    }

    /* ── 層 ───────────────────────────────────────────────────────────── */

    @Test
    fun layerOnceBouncesBackAfterOneKeyAndLayerLockDoesNot() {
        val h = booted()
        h.setLayerOnce("upper")
        assertEquals("qwerty/upper", h.state())
        h.afterKeySent()
        assertEquals("qwerty/lower", h.state())

        h.lockLayer("upper")
        h.afterKeySent()
        assertEquals("qwerty/upper", h.state())
    }

    @Test
    fun unknownLayerIdIsIgnoredRatherThanStranding() {
        val h = booted()
        h.setLayer("no-such-layer")
        assertEquals("qwerty/lower", h.state())
    }

    /** 換佈局要把待回彈的 layer_once 一起清掉，否則回彈會指向別份佈局的層。 */
    @Test
    fun switchingLayoutClearsAPendingLayerOnce() {
        val h = booted()
        h.setLayerOnce("upper")
        h.switchLayout("numeric-symbol")
        h.afterKeySent()
        assertEquals("numeric-symbol/numeric", h.state())
    }

    /* ── 九宮格 ⇄ 英數（真機回報的那條路）───────────────────────────── */

    /**
     * **九宮格的預設是 5 欄的 `cn-t9-pinyin`，不是 4 欄的 `t9-pinyin`。**
     *
     * 4 欄那份把缺掉的第五欄寬度攤給了四顆鍵，鍵的長寬比被拉到 1.95:1
     * （三星 1.53、語燕 1.36）。5 欄那份補上左標點欄與右功能欄之後
     * 長寬比自動落回 1.53。兩者的 speller 契約相同，可以互換。
     *
     * 這條同時守住 §9.1.1 的欄位拆分：`t9-pinyin` 的 `for_schema` 仍含
     * `t9_pinyin`（選單裡選得到），但 `auto_for_schema` 是空的，所以搶不到
     * 自動命中。少了那個拆分，兩份都宣告 `t9_pinyin`，誰贏取決於檔名排序。
     */
    @Test
    fun theNinePadDefaultIsTheFiveColumnLayout() {
        val h = booted()
        h.applySchema("t9_pinyin")
        assertEquals("cn-t9-pinyin/t9", h.state())
        assertEquals(
            "九宮格主層必須是 5 欄（左標點欄 + 3x3 字母 + 右功能欄）",
            5,
            h.layout!!.layer("t9")!!.rows.first().keys.size,
        )
    }

    @Test
    fun ninePadNumberLayerRoundTripStaysInsideTheSameLayout() {
        val h = booted()
        h.applySchema("t9_pinyin")
        assertEquals("cn-t9-pinyin/t9", h.state())

        // 「123」鍵
        val num = tapOf(h, "to_num")
        assertEquals("layer", num.substringBefore(':'))
        h.setLayer(num.substringAfter(':'))
        assertEquals("cn-t9-pinyin/num", h.state())

        // 數字層的「返回」鍵
        val back = tapOf(h, "back")
        h.setLayer(back.substringAfter(':'))
        assertEquals("cn-t9-pinyin/t9", h.state())
    }

    /** 舊的 4 欄版仍然選得到、也仍然出得來（使用者可能 pin 過它）。 */
    @Test
    fun theRetiredFourColumnNinePadStillRoundTrips() {
        val h = booted()
        h.switchLayout("t9-pinyin")
        assertEquals("t9-pinyin/t9", h.state())
        val abc = tapOf(h, "to_alpha")
        h.setLayer(abc.substringAfter(':'))
        assertEquals("t9-pinyin/en", h.state())
        h.setLayer(tapOf(h, "to_t9").substringAfter(':'))
        assertEquals("t9-pinyin/t9", h.state())
    }

    /** 九宮格 → 符號 → 返回。修正前這條會把人丟在 qwerty 上出不來。 */
    @Test
    fun ninePadSymbolsReturnToTheNinePadNotToQwerty() {
        val h = booted()
        h.applySchema("t9_pinyin")
        h.switchLayout("cn-symbols")
        val back = tapOf(h, "bar_back")
        assertEquals("switch_layout:@previous", back)
        h.switchLayout(back.substringAfter(':'))
        assertEquals("cn-t9-pinyin/t9", h.state())
    }

    /** 注音 → ?123 → ABC 同樣不得掉進 qwerty。 */
    @Test
    fun bopomofoSymbolsReturnToBopomofo() {
        val h = booted()
        h.applySchema("bopomofo_tw")
        assertEquals("bopomofo-dachen", h.layout?.id)
        h.switchLayout("numeric-symbol")
        h.switchLayout("@previous")
        assertEquals("bopomofo-dachen/bopomofo", h.state())
    }

    /** qwerty → ?123 → ABC 仍然要回 qwerty（@previous 沒有壞掉舊路徑）。 */
    @Test
    fun qwertySymbolsStillReturnToQwerty() {
        val h = booted()
        h.switchLayout("numeric-symbol")
        h.switchLayout("@previous")
        assertEquals("qwerty/lower", h.state())
    }

    /* ── 方案 → 佈局 ─────────────────────────────────────────────────── */

    @Test
    fun applySchemaPicksTheLayoutBoundToTheSchema() {
        val h = booted()
        h.applySchema("bopomofo_tw")
        assertEquals("bopomofo-dachen", h.layout?.id)
        h.applySchema("luna_pinyin_tw")   // 沒有專屬佈局 → 退回 primary
        assertEquals("qwerty", h.layout?.id)
    }

    /**
     * 重選同一個方案是使用者的救援動作：「把鍵盤調回這個方案該有的樣子」。
     * 即使佈局已經對了，層也要歸位，否則卡在英數層的人重選方案毫無效果。
     */
    @Test
    fun reapplyingTheSameSchemaResetsTheLayer() {
        val h = booted()
        h.applySchema("t9_pinyin")
        h.setLayer("num")
        assertEquals("cn-t9-pinyin/num", h.state())

        h.applySchema("t9_pinyin")
        assertEquals("cn-t9-pinyin/t9", h.state())
    }

    /* ── 夾具 ─────────────────────────────────────────────────────────── */

    private fun tapOf(h: LayoutHost, keyId: String): String {
        val layer = h.layout!!.layer(h.layerId)!!
        for (row in layer.rows) for (k in row.keys) {
            if (k.id == keyId) return k.tap?.raw ?: error("key '$keyId' has no tap action")
        }
        throw AssertionError("no key '$keyId' in ${h.state()}")
    }
}

/** 讀 repo 真檔的 [LayoutRepository]。不碰 Android Context。 */
internal class FixtureRepo : LayoutRepository {
    override fun layoutIds(): List<String> = RepoFixtures.layoutIds

    override fun loadLayout(id: String): LoadResult<KeyboardLayout> =
        LayoutLoader.load(id, RepoFixtures.layouts, Platform.ANDROID, locale = LOCALE)

    override fun loadTheme(id: String): LoadResult<Theme> =
        ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID, locale = LOCALE)

    override fun builtinFallbackTheme(): Theme =
        loadTheme("default-light").value ?: error("夾具主題載不起來")

    private companion object {
        const val LOCALE = "zh-Hant-TW"
    }
}

/**
 * §9.1.1 的自動命中：**兩份佈局同時命中同一個方案時，決勝必須是確定的。**
 *
 * ⛔ 從前這裡是 `repo.layoutIds().…firstOrNull { autoMatchesSchema }`，
 * 而那份清單的順序來自 `File.listFiles()` —— 檔案系統不保證順序。
 * 症狀是 `scripts/verify_selection_digit.sh` 一次紅一次綠，紅的那一次
 * 裝置上載進去的根本不是它要驗的那一份佈局，而畫面完全正常。
 *
 * 這一組測試把「同一組佈局、不同的清單順序」釘成同一個答案，
 * 並釘住「使用者目錄勝過隨附」。
 */
class LayoutAutoHitTieTest {

    /** 兩份九宮格都宣告自動命中 `t9_pinyin`；`ids` 決定清單順序。 */
    private class TieRepo(
        private val ids: List<String>,
        private val userDir: Set<String> = emptySet(),
    ) : LayoutRepository {
        override fun layoutIds(): List<String> = ids

        override fun layoutIsUserProvided(id: String): Boolean = id in userDir

        override fun loadLayout(id: String): LoadResult<KeyboardLayout> {
            val r = LayoutLoader.load(id, RepoFixtures.layouts, Platform.ANDROID, locale = LOCALE)
            val v = r.value ?: return r
            // 兩份都自動命中 —— 那正是要測的那個平手。
            return if (v.id.startsWith("cn-t9-pinyin")) {
                LoadResult(v.copy(autoForSchema = listOf("t9_pinyin")), r.diagnostics)
            } else {
                r
            }
        }

        override fun loadTheme(id: String): LoadResult<Theme> =
            ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID, locale = LOCALE)

        override fun builtinFallbackTheme(): Theme =
            loadTheme("default-light").value ?: error("夾具主題載不起來")
    }

    private val both = listOf("cn-t9-pinyin", "cn-t9-pinyin-numrow", "qwerty")

    @Test
    fun theWinnerIsTheSameWhicheverOrderTheFilesystemGivesThem() {
        val forward = LayoutHost(TieRepo(both)).apply { applySchema("t9_pinyin") }.layout?.id
        val backward = LayoutHost(TieRepo(both.reversed())).apply { applySchema("t9_pinyin") }
            .layout?.id
        assertEquals("清單順序不得決定答案", forward, backward)
        assertEquals("同級用 id 字典序", "cn-t9-pinyin", forward)
    }

    @Test
    fun theUserDirCopyWins() {
        val h = LayoutHost(TieRepo(both, userDir = setOf("cn-t9-pinyin-numrow")))
        h.applySchema("t9_pinyin")
        assertEquals("cn-t9-pinyin-numrow", h.layout?.id)
    }

    private companion object {
        const val LOCALE = "zh-Hant-TW"
    }
}
