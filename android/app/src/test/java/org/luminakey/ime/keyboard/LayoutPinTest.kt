package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import org.luminakey.ime.theme.RepoFixtures

/**
 * §9.1.1 的 SHOULD：
 *
 * > 使用者若曾為當前 schema **明確指定**過佈局，實作 **應** 記住該選擇並跳過
 * > 第 1 步。自動切換是便利機制，不該覆蓋使用者的明示意圖。
 *
 * 沒有這一段時的事故：使用者在鍵盤類型選單裡替注音挑了 QWERTY（他習慣用
 * 拼音鍵位打注音），切去別的方案再切回來，`for_schema` 的精確命中就把他丟回
 * 大千注音 —— 而他上一個動作正是親手挑掉它。
 */
class LayoutPinTest {

    private fun booted(): LayoutHost = LayoutHost(FixtureRepo()).apply { ensureLoaded() }

    @Test
    fun anExplicitChoiceBeatsTheForSchemaExactMatch() {
        val h = booted()
        h.applySchema("bopomofo_tw")
        assertEquals("自動規則的結果", "bopomofo-dachen", h.layout?.id)

        // 使用者在選單裡挑了 QWERTY 給注音用。
        h.pinLayout("bopomofo_tw", "qwerty")
        h.applySchema("bopomofo_tw")
        assertEquals("qwerty", h.layout?.id)
    }

    @Test
    fun theChoiceSurvivesGoingAwayAndComingBack() {
        val h = booted()
        h.pinLayout("t9_pinyin", "qwerty")
        h.applySchema("t9_pinyin")
        assertEquals("qwerty", h.layout?.id)

        h.applySchema("bopomofo_tw")
        assertEquals("bopomofo-dachen", h.layout?.id)

        h.applySchema("t9_pinyin")
        assertEquals("回來之後仍然是使用者挑的那份", "qwerty", h.layout?.id)
    }

    @Test
    fun aChoiceOnlyAppliesToTheSchemaItWasMadeFor() {
        val h = booted()
        h.pinLayout("t9_pinyin", "qwerty")
        h.applySchema("bopomofo_tw")
        assertEquals("bopomofo-dachen", h.layout?.id)
    }

    /** 記得住也要忘得掉：一個解除不了的偏好是另一種「進得去出不來」。 */
    @Test
    fun clearingTheChoiceHandsControlBackToTheAutomaticRule() {
        val h = booted()
        h.pinLayout("bopomofo_tw", "qwerty")
        h.applySchema("bopomofo_tw")
        assertEquals("qwerty", h.layout?.id)

        h.pinLayout("bopomofo_tw", null)
        assertNull(h.pinnedLayoutFor("bopomofo_tw"))
        h.applySchema("bopomofo_tw")
        assertEquals("bopomofo-dachen", h.layout?.id)
    }

    /**
     * 指定的佈局被刪掉（使用者從 user_data_dir 移走、或方案套件被解除安裝）
     * 之後，不得每次切到這個方案都撞一次載入失敗然後留在原地。
     */
    @Test
    fun aChoicePointingAtAMissingLayoutFallsBackAndForgetsItself() {
        val h = booted()
        h.pinLayout("bopomofo_tw", "no-such-layout")
        h.applySchema("bopomofo_tw")
        assertEquals("bopomofo-dachen", h.layout?.id)
        assertNull("壞掉的指定要自己清掉", h.pinnedLayoutFor("bopomofo_tw"))
    }

    /** 選單重選同一種鍵盤仍要把層歸位 —— 那是使用者的救援動作。 */
    @Test
    fun reselectingTheSameChoiceStillResetsTheLayer() {
        val h = booted()
        h.pinLayout("t9_pinyin", "t9-pinyin")
        h.applySchema("t9_pinyin")
        h.setLayer("en")
        assertEquals("t9-pinyin/en", "${h.layout?.id}/${h.layerId}")

        h.applySchema("t9_pinyin")
        assertEquals("t9-pinyin/t9", "${h.layout?.id}/${h.layerId}")
    }

    @Test
    fun pinsCanBeRestoredWholesaleFromStorage() {
        val h = booted()
        h.setPinnedLayouts(mapOf("bopomofo_tw" to "qwerty", "t9_pinyin" to "numeric-symbol"))
        assertEquals("qwerty", h.pinnedLayoutFor("bopomofo_tw"))
        assertEquals(2, h.pinnedLayouts().size)

        h.setPinnedLayouts(emptyMap())
        assertNull(h.pinnedLayoutFor("bopomofo_tw"))
    }

    /* ── 選單的資料來源 ───────────────────────────────────────────────── */

    @Test
    fun layoutBriefsCarryWhatTheMenuNeeds() {
        val briefs = booted().layoutBriefs("zh-Hant-TW")
        assertEquals(RepoFixtures.layoutIds.toSet(), briefs.map { it.id }.toSet())
        val bopomofo = briefs.first { it.id == "bopomofo-dachen" }
        assertEquals("注音 大千", bopomofo.name)
        assertEquals(true, bopomofo.declares("bopomofo_tw"))
        assertEquals(false, bopomofo.wildcard)
        assertEquals(true, briefs.first { it.id == "numeric-symbol" }.isAccessory)
    }

}
