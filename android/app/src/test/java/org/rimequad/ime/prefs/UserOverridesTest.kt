package org.rimequad.ime.prefs

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import org.rimequad.ime.theme.HapticStrength
import org.rimequad.ime.theme.HintPosition
import org.rimequad.ime.theme.MapDocumentSource
import org.rimequad.ime.theme.Platform
import org.rimequad.ime.theme.RepoFixtures
import org.rimequad.ime.theme.Theme
import org.rimequad.ime.theme.ThemeLoader

/**
 * 三層覆寫最上層的單元測試。
 *
 * ── 這組測試在守什麼 ────────────────────────────────────────────────────
 * 唯一真正重要的不變式是：**「回復預設」必須回到主題檔當下的值，而不是
 * 回到一個寫死在程式裡的數字。** 存副本的實作在「設定 → 回復」的來回中
 * 看起來完全正確，只有在主題檔改了值之後才會露出馬腳。
 *
 * 所以本檔的斷言一律拿**解析出來的主題物件**當期望值，從不寫字面數字；
 * 另外有一組測試刻意換一份不同數值的主題文件，證明覆寫層會跟著動
 * （[resetGoesBackToUpdatedThemeNotToASnapshot]）。
 */
class UserOverridesTest {

    private fun realTheme(id: String = "default-light"): Theme {
        val r = ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID, locale = "zh-Hant-TW")
        assertTrue("主題 $id 應該載得起來: ${RepoFixtures.describe(r.diagnostics)}", r.value != null)
        return r.value!!
    }

    /**
     * §8.8.0 的高度是**算出來的**，所以測「高度有沒有變」只能真的算一次。
     * 參數取一支 411dp 寬的手機 + 10 欄 4 列的 QWERTY。
     */
    private fun resolvedKeyHeight(t: Theme): Float = t.keyboard.geometry.resolve(
        widthDp = 411f,
        availHeightDp = 780f,
        landscape = false,
        units = 10f,
        rowsWeight = 4f,
        rowCount = 4,
        padding = t.keyboard.padding,
        keySpacing = t.keyboard.keySpacing,
        rowSpacing = t.keyboard.rowSpacing,
    ).keyHeight

    private fun resolvedKeyboardHeight(t: Theme): Float = t.keyboard.geometry.resolve(
        widthDp = 411f,
        availHeightDp = 780f,
        landscape = false,
        units = 10f,
        rowsWeight = 4f,
        rowCount = 4,
        padding = t.keyboard.padding,
        keySpacing = t.keyboard.keySpacing,
        rowSpacing = t.keyboard.rowSpacing,
    ).keyboardHeight

    /* ───────────────── 未設定 → 主題原值 ───────────────── */

    @Test
    fun pristinePrefsReturnTheExactSameThemeObject() {
        val theme = realTheme()
        // 不只是「值相等」：完全沒設定過就不該產生新物件，否則 Compose 每次
        // 重組都拿到一個 equals 但不 identical 的主題，整塊鍵盤會重畫。
        assertSame(theme, applyUserOverrides(theme, UserPrefs()))
    }

    @Test
    fun unsetHeightKeepsThemeGeometry() {
        val theme = realTheme()
        // 只設了 hint，幾何必須原封不動。
        val out = applyUserOverrides(theme, UserPrefs(hints = HintVisibility.HIDDEN))
        assertEquals(theme.keyboard.geometry, out.keyboard.geometry)
    }

    @Test
    fun unsetFeedbackKeepsThemeValues() {
        val theme = realTheme()
        val out = applyUserOverrides(theme, UserPrefs(candidateCount = 5))
        assertEquals(theme.feedback, out.feedback)
    }

    /* ───────────────── 設定 → 覆寫值 ───────────────── */

    @Test
    fun heightScaleMultipliesAspectAndBothClamps() {
        val theme = realTheme()
        val g = theme.keyboard.geometry
        val out = applyUserOverrides(theme, UserPrefs(keyboardHeightScale = 1.25f))
        val og = out.keyboard.geometry
        assertEquals(g.aspect * 1.25f, og.aspect, 1e-5f)
        assertEquals(g.keyHeightMin * 1.25f, og.keyHeightMin, 1e-5f)
        assertEquals(g.keyHeightMax * 1.25f, og.keyHeightMax, 1e-5f)
        // 三個一起乘的目的：解出來的鍵高必須嚴格是 1.25 倍，而不是被夾制吃掉。
        assertEquals(resolvedKeyHeight(theme) * 1.25f, resolvedKeyHeight(out), 1e-3f)
        assertTrue(resolvedKeyboardHeight(out) > resolvedKeyboardHeight(theme))
    }

    @Test
    fun heightScaleCanShrink() {
        val theme = realTheme()
        val out = applyUserOverrides(theme, UserPrefs(keyboardHeightScale = 0.7f))
        assertEquals(resolvedKeyHeight(theme) * 0.7f, resolvedKeyHeight(out), 1e-3f)
        assertTrue(resolvedKeyboardHeight(out) < resolvedKeyboardHeight(theme))
    }

    @Test
    fun shrinkingDoesNotTightenTheScreenRatioSafetyNet() {
        // 安全網收緊毫無意義，只會讓列數多的佈局（注音大千 5 列）被反推壓扁。
        val theme = realTheme()
        val out = applyUserOverrides(theme, UserPrefs(keyboardHeightScale = 0.7f))
        assertEquals(
            theme.keyboard.geometry.maxScreenRatioPortrait,
            out.keyboard.geometry.maxScreenRatioPortrait,
            1e-6f,
        )
    }

    @Test
    fun heightScaleIsClampedToUsableRange() {
        val theme = realTheme()
        val wild = applyUserOverrides(theme, UserPrefs(keyboardHeightScale = 99f))
        val atMax = applyUserOverrides(theme, UserPrefs(keyboardHeightScale = HEIGHT_SCALE_MAX))
        assertEquals(atMax.keyboard.geometry, wild.keyboard.geometry)
        // 乘完仍須落在 §8.8 表列的合法值域內。
        assertTrue(wild.keyboard.geometry.aspect in 0.6f..2.5f)
        assertTrue(wild.keyboard.geometry.keyHeightMax in 20f..200f)
        assertTrue(wild.keyboard.geometry.maxScreenRatioPortrait in 0.2f..0.8f)
        assertTrue(wild.keyboard.geometry.maxScreenRatioLandscape in 0.2f..0.9f)
    }

    @Test
    fun hiddenHintsSetEveryKeyStyleToNone() {
        val theme = realTheme()
        assertTrue(
            "前提：主題本來至少有一個 style 會顯示 hint",
            theme.keyboard.keyStyles.values.any { it.hintPosition != HintPosition.NONE },
        )
        val out = applyUserOverrides(theme, UserPrefs(hints = HintVisibility.HIDDEN))
        assertTrue(out.keyboard.keyStyles.values.all { it.hintPosition == HintPosition.NONE })
    }

    @Test
    fun shownHintsOnlyRescuesStylesThatWereNone() {
        val theme = realTheme()
        val out = applyUserOverrides(theme, UserPrefs(hints = HintVisibility.SHOWN))
        assertTrue(out.keyboard.keyStyles.values.none { it.hintPosition == HintPosition.NONE })
        // 主題作者已經指定過位置的 style 不該被搬家。
        theme.keyboard.keyStyles.forEach { (name, st) ->
            if (st.hintPosition != HintPosition.NONE) {
                assertEquals(
                    "style $name 的 hint 位置不該被改動",
                    st.hintPosition,
                    out.keyboard.keyStyles.getValue(name).hintPosition,
                )
            }
        }
    }

    @Test
    fun candidateSizeScaleScalesTextLabelCommentAndBarHeight() {
        val theme = realTheme()
        val st = theme.candidates.bar.style
        val out = applyUserOverrides(theme, UserPrefs(candidateSizeScale = 1.3f))
        val ost = out.candidates.bar.style
        assertEquals(st.text.size * 1.3f, ost.text.size, 1e-4f)
        assertEquals(st.label.size * 1.3f, ost.label.size, 1e-4f)
        assertEquals(st.comment.size * 1.3f, ost.comment.size, 1e-4f)
        assertEquals(
            (theme.candidates.bar.height * 1.3f).coerceAtMost(96f),
            out.candidates.bar.height,
            1e-4f,
        )
    }

    @Test
    fun candidateCountMapsToMaxVisible() {
        val theme = realTheme()
        assertEquals(6, applyUserOverrides(theme, UserPrefs(candidateCount = 6)).candidates.bar.maxVisible)
    }

    @Test
    fun feedbackOverridesEachFieldIndependently() {
        val theme = realTheme()
        val out = applyUserOverrides(
            theme,
            UserPrefs(soundEnabled = true, hapticStrength = HapticStrength.HEAVY),
        )
        assertEquals(true, out.feedback.sound)
        assertEquals(HapticStrength.HEAVY, out.feedback.hapticStrength)
        // 沒設過的兩項必須維持主題值。
        assertEquals(theme.feedback.haptic, out.feedback.haptic)
        assertEquals(theme.feedback.soundVolume, out.feedback.soundVolume, 1e-6f)
    }

    /* ───────────────── 回復預設 → 回到主題檔的值 ───────────────── */

    @Test
    fun resetRestoresThemeValues() {
        val theme = realTheme()
        val touched = UserPrefs(
            keyboardHeightScale = 1.4f,
            hints = HintVisibility.HIDDEN,
            candidateSizeScale = 1.5f,
            candidateCount = 3,
            soundEnabled = true,
            hapticEnabled = false,
            hapticStrength = HapticStrength.HEAVY,
        )
        val overridden = applyUserOverrides(theme, touched)
        assertNotEquals(theme.keyboard.geometry, overridden.keyboard.geometry)

        // 「回復預設」在儲存層就是把 key 全部刪掉 → 回到 UserPrefs()。
        val restored = applyUserOverrides(theme, UserPrefs())
        assertEquals(theme.keyboard.geometry, restored.keyboard.geometry)
        assertEquals(theme.feedback, restored.feedback)
        assertEquals(theme.candidates.bar.height, restored.candidates.bar.height, 1e-6f)
        assertEquals(theme.candidates.bar.maxVisible, restored.candidates.bar.maxVisible)
        assertEquals(
            theme.keyboard.keyStyles.mapValues { it.value.hintPosition },
            restored.keyboard.keyStyles.mapValues { it.value.hintPosition },
        )
    }

    /**
     * 這一題才是重點：主題檔改了值之後，「回復預設」必須回到**新的**值。
     *
     * 用兩份數值不同的主題文件模擬「主題更新了」。若偏好層存的是一份預設值
     * 的副本，這個測試就會失敗 —— 它會回到舊主題的 1.20 而不是新主題的 1.60。
     */
    @Test
    fun resetGoesBackToUpdatedThemeNotToASnapshot() {
        val v1 = miniTheme(aspect = 1.20f, min = 40f, max = 56f, sound = false)
        val v2 = miniTheme(aspect = 1.60f, min = 48f, max = 70f, sound = true)

        val prefs = UserPrefs(keyboardHeightScale = 1.2f, soundEnabled = false)

        // 使用者設過值：兩版各以自己的主題值為基準做覆寫。
        assertEquals(1.20f * 1.2f, applyUserOverrides(v1, prefs).keyboard.geometry.aspect, 1e-5f)
        assertEquals(1.60f * 1.2f, applyUserOverrides(v2, prefs).keyboard.geometry.aspect, 1e-5f)

        // 回復預設之後，各自回到「自己那份主題檔」的值。
        val reset = UserPrefs()
        assertEquals(1.20f, applyUserOverrides(v1, reset).keyboard.geometry.aspect, 1e-6f)
        assertEquals(1.60f, applyUserOverrides(v2, reset).keyboard.geometry.aspect, 1e-6f)
        assertEquals(40f, applyUserOverrides(v1, reset).keyboard.geometry.keyHeightMin, 1e-6f)
        assertEquals(48f, applyUserOverrides(v2, reset).keyboard.geometry.keyHeightMin, 1e-6f)
        // 布林同理：v2 把 sound 改成 true，回復預設就該是 true。
        assertEquals(false, applyUserOverrides(v1, reset).feedback.sound)
        assertEquals(true, applyUserOverrides(v2, reset).feedback.sound)
    }

    /** 覆寫層不得改動與該項無關的欄位。 */
    @Test
    fun overridesDoNotDisturbUnrelatedFields() {
        val theme = realTheme()
        val out = applyUserOverrides(
            theme,
            UserPrefs(keyboardHeightScale = 1.3f, hints = HintVisibility.HIDDEN),
        )
        assertEquals(theme.id, out.id)
        assertEquals(theme.palette, out.palette)
        assertEquals(theme.ancestry, out.ancestry)
        assertEquals(theme.appearance, out.appearance)
        assertEquals(theme.keyboard.background, out.keyboard.background)
        assertEquals(theme.keyboard.padding, out.keyboard.padding)
        assertEquals(theme.keyboard.rowSpacing, out.keyboard.rowSpacing, 1e-6f)
        assertEquals(theme.motion, out.motion)
    }

    /* ───────────────── 行為參數（規範缺口）───────────────── */

    @Test
    fun keyBehaviorFallsBackToImplementationConstantsWhenUnset() {
        val theme = realTheme()
        val b = KeyBehavior.of(theme.feedback, UserPrefs())
        // 主題格式沒有這三個欄位，所以「未設定」只能退回實作常數。
        assertEquals(KeyBehavior.DEFAULT_LONG_PRESS_MS, b.longPressMs)
        assertEquals(KeyBehavior.DEFAULT_REPEAT_DELAY_MS, b.repeatDelayMs)
        assertEquals(KeyBehavior.DEFAULT_REPEAT_INTERVAL_MS, b.repeatIntervalMs)
        // 有欄位的四項則來自主題。
        assertEquals(theme.feedback.haptic, b.haptic)
        assertEquals(theme.feedback.hapticStrength, b.hapticStrength)
        assertEquals(theme.feedback.sound, b.sound)
    }

    @Test
    fun keyBehaviorTakesOverriddenFeedbackNotRawPrefs() {
        val theme = realTheme()
        val prefs = UserPrefs(soundEnabled = true, soundVolume = 0.9f, repeatIntervalMs = 25)
        val b = KeyBehavior.of(applyUserOverrides(theme, prefs).feedback, prefs)
        assertEquals(true, b.sound)
        assertEquals(0.9f, b.soundVolume, 1e-6f)
        assertEquals(25, b.repeatIntervalMs)
    }

    @Test
    fun keyBehaviorClampsAbsurdValues() {
        val theme = realTheme()
        val b = KeyBehavior.of(theme.feedback, UserPrefs(repeatIntervalMs = 1, longPressMs = 99999))
        assertTrue("重複間隔不得小到把宿主淹掉", b.repeatIntervalMs >= 20)
        assertTrue("長按門檻不得大到按不出來", b.longPressMs <= 1200)
    }

    /* ───────────────── 夾具 ───────────────── */

    /**
     * 最小可載入主題。刻意不用 core/themes 的檔案 —— 這幾個測試要的是
     * 「同一份程式碼面對兩份不同的主題檔」，用 repo 裡的檔案沒辦法在測試裡
     * 表達「主題更新了」這件事。
     */
    private fun miniTheme(aspect: Float, min: Float, max: Float, sound: Boolean): Theme {
        val text = """
            format: rime-theme/1
            id: mini
            appearance: light
            keyboard:
              key_aspect: $aspect
              key_height:
                min: $min
                max: $max
            feedback:
              sound: $sound
        """.trimIndent()
        val r = ThemeLoader.load("mini", MapDocumentSource(mapOf("mini" to text)), Platform.ANDROID)
        assertTrue("夾具主題應載得起來: ${RepoFixtures.describe(r.diagnostics)}", r.value != null)
        return r.value!!
    }
}
