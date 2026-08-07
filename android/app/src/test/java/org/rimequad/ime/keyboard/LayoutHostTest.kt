package org.rimequad.ime.keyboard

import org.junit.Assert.assertEquals
import org.rimequad.ime.theme.KeyboardLayout
import org.rimequad.ime.theme.LayoutLoader
import org.rimequad.ime.theme.LoadResult
import org.rimequad.ime.theme.Platform
import org.rimequad.ime.theme.RepoFixtures
import org.rimequad.ime.theme.Theme
import org.rimequad.ime.theme.ThemeLoader
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

    @Test
    fun ninePadEnglishRoundTripStaysInsideTheSameLayout() {
        val h = booted()
        h.applySchema("t9_pinyin")
        assertEquals("t9-pinyin/t9", h.state())

        // 「ABC」鍵
        val abc = tapOf(h, "to_alpha")
        assertEquals("layer", abc.substringBefore(':'))
        h.setLayer(abc.substringAfter(':'))
        assertEquals("t9-pinyin/en", h.state())

        // 英數層左下角的回程鍵
        val back = tapOf(h, "to_t9")
        h.setLayer(back.substringAfter(':'))
        assertEquals("t9-pinyin/t9", h.state())
    }

    /** 九宮格 → 符號 → ABC。修正前這條會把人丟在 qwerty 上出不來。 */
    @Test
    fun ninePadSymbolsReturnToTheNinePadNotToQwerty() {
        val h = booted()
        h.applySchema("t9_pinyin")
        h.switchLayout("numeric-symbol")
        val abc = tapOf(h, "to_alpha")
        assertEquals("switch_layout:@previous", abc)
        h.switchLayout(abc.substringAfter(':'))
        assertEquals("t9-pinyin/t9", h.state())
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
        h.setLayer("en")
        assertEquals("t9-pinyin/en", h.state())

        h.applySchema("t9_pinyin")
        assertEquals("t9-pinyin/t9", h.state())
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
