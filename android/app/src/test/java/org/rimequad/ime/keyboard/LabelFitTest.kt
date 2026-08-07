package org.rimequad.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * §9.6「鍵面文字放不下時」的縮放規則。
 *
 * > 渲染器 **應** 等比縮小字級以求完整顯示，下限為 `label_size × 0.5`；
 * > 到下限仍放不下才截斷。
 *
 * 改動前的實作是 `face.length > 2 → labelSize × 0.55` —— 依**字數**縮放。
 * 於是「分詞」（2 字）用滿級數而「abc」（3 字）只有 55%，同一列視覺跳動；
 * 「中／英」這種一個字的鍵也一併被規則波及。這裡測的是新規則的算術部分：
 * 縮放的依據是**量到的寬度與可用寬度之比**，與字數無關。
 */
class LabelFitTest {

    @Test
    fun textThatFitsIsNeverScaled() {
        assertEquals(1f, shrinkRatio(measuredPx = 40f, availablePx = 100f))
        assertEquals("剛好放得下也算放得下", 1f, shrinkRatio(100f, 100f))
    }

    @Test
    fun textThatFitsIsNeverEnlarged() {
        // 規範只說「縮小」。把短標籤放大會讓同一列的鍵面大小不一。
        assertEquals(1f, shrinkRatio(10f, 500f))
    }

    @Test
    fun overflowShrinksInProportionToHowMuchItOverflows() {
        assertEquals(0.8f, shrinkRatio(125f, 100f), 1e-4f)
        assertEquals(0.625f, shrinkRatio(160f, 100f), 1e-4f)
    }

    @Test
    fun theShrinkNeverGoesBelowHalf() {
        // 「注音·臺灣正體」塞進一顆窄鍵時會撞到下限；規範要求到此為止，
        // 再放不下就讓它截斷，而不是縮到讀不出來。
        assertEquals(MIN_LABEL_SHRINK, shrinkRatio(1000f, 100f))
        assertTrue(shrinkRatio(1e6f, 1f) >= MIN_LABEL_SHRINK)
    }

    @Test
    fun degenerateMeasurementsAreTreatedAsFitting() {
        // 量測還沒回來（0 寬）時寧可用滿級數，也不要先畫一次縮小的再跳回去。
        assertEquals(1f, shrinkRatio(0f, 100f))
        assertEquals(1f, shrinkRatio(100f, 0f))
        assertEquals(1f, shrinkRatio(-1f, -1f))
    }

    /**
     * 缺陷本身：字數不再是縮放的依據。
     *
     * 同樣寬度的鍵、同樣量到 90px 的兩個字串，不論一個是 2 字一個是 3 字，
     * 都必須得到同一個結果。
     */
    @Test
    fun characterCountIsNoLongerTheCriterion() {
        val fenci = shrinkRatio(measuredPx = 90f, availablePx = 100f)   // 「分詞」
        val abc = shrinkRatio(measuredPx = 90f, availablePx = 100f)     // 「abc」
        assertEquals(fenci, abc)
        assertEquals(1f, fenci)
    }
}
