package org.rimequad.ime.prefs

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.rimequad.ime.theme.HapticStrength

/**
 * 偏好的序列化不變式。
 *
 * [PrefsStore] 的寫入是「clear() 之後只寫 [UserPrefs.toMap] 有的條目」，
 * 所以「未設定 = 儲存層沒有這個 key」這件事，等價於「toMap 不得為 null
 * 欄位產生條目」。那是可以純函式測的，本檔就測它 —— 不需要 Android
 * 執行環境，也就不需要 Robolectric。
 */
class UserPrefsTest {

    @Test
    fun pristinePrefsSerializeToAnEmptyMap() {
        assertTrue("全未設定時不得寫入任何 key", UserPrefs().toMap().isEmpty())
        assertTrue(UserPrefs().isPristine)
    }

    @Test
    fun onlyTouchedFieldsAppearInTheMap() {
        val m = UserPrefs(keyboardHeightScale = 1.2f, hints = HintVisibility.HIDDEN).toMap()
        assertEquals(setOf(UserPrefs.K_HEIGHT_SCALE, UserPrefs.K_HINTS), m.keys)
        assertEquals(1.2f, m[UserPrefs.K_HEIGHT_SCALE])
        assertEquals("HIDDEN", m[UserPrefs.K_HINTS])
    }

    @Test
    fun falseIsAValueNotAnAbsence() {
        // 「使用者主動關掉震動」與「使用者沒碰過震動」必須是兩件事，
        // 否則主題把 haptic 預設成 true 時使用者永遠關不掉。
        val m = UserPrefs(hapticEnabled = false).toMap()
        assertEquals(setOf(UserPrefs.K_HAPTIC), m.keys)
        assertEquals(false, m[UserPrefs.K_HAPTIC])
        assertEquals(false, UserPrefs.fromMap(m).hapticEnabled)
        assertNull(UserPrefs.fromMap(emptyMap()).hapticEnabled)
    }

    @Test
    fun zeroIsAValueNotAnAbsence() {
        val m = UserPrefs(candidateCount = 0, soundVolume = 0f).toMap()
        assertEquals(0, UserPrefs.fromMap(m).candidateCount)
        assertEquals(0f, UserPrefs.fromMap(m).soundVolume)
    }

    @Test
    fun roundTripsEveryField() {
        val full = UserPrefs(
            keyboardHeightScale = 1.15f,
            soundEnabled = true,
            soundVolume = 0.55f,
            hapticEnabled = false,
            hapticStrength = HapticStrength.HEAVY,
            longPressMs = 320,
            repeatDelayMs = 280,
            repeatIntervalMs = 45,
            hints = HintVisibility.SHOWN,
            themeId = "sakura-dark",
            appearanceMode = AppearanceMode.DARK,
            candidateSizeScale = 1.25f,
            candidateCount = 7,
            simplification = true,
            asciiPunct = false,
            spaceBehavior = SpaceBehavior.ALWAYS_SPACE,
            autoCheckUpdate = false,
        )
        assertFalse(full.isPristine)
        assertEquals(full, UserPrefs.fromMap(full.toMap()))
        // 每一個欄位都必須真的有進映射，否則就是 toMap 漏了一個。
        assertEquals(17, full.toMap().size)
    }

    @Test
    fun autoUpdateCheckIsUnsetByDefaultAndFalseIsARealValue() {
        // 「未設定」在行為上等於開（消費端寫 `autoCheckUpdate ?: true`），
        // 但不可以把 true 抄進偏好 —— 那會讓日後改預設值時，從沒動過這一項
        // 的使用者被釘在舊預設上。
        assertNull(UserPrefs().autoCheckUpdate)
        assertTrue(UserPrefs().toMap()[UserPrefs.K_AUTO_CHECK_UPDATE] == null)

        val off = UserPrefs(autoCheckUpdate = false)
        assertEquals(setOf(UserPrefs.K_AUTO_CHECK_UPDATE), off.toMap().keys)
        assertEquals(false, UserPrefs.fromMap(off.toMap()).autoCheckUpdate)
    }

    @Test
    fun unknownEnumNamesDegradeToUnsetRatherThanCrash() {
        val decoded = UserPrefs.fromMap(
            mapOf(
                UserPrefs.K_HAPTIC_STRENGTH to "ULTRA",
                UserPrefs.K_APPEARANCE to "SEPIA",
                UserPrefs.K_HINTS to 42,          // 型別也錯
                UserPrefs.K_HEIGHT_SCALE to 1.1f, // 這個是好的，不該被連累
            )
        )
        assertNull(decoded.hapticStrength)
        assertNull(decoded.appearanceMode)
        assertNull(decoded.hints)
        assertEquals(1.1f, decoded.keyboardHeightScale)
    }

    @Test
    fun removingOneFieldRemovesExactlyThatKey() {
        val before = UserPrefs(keyboardHeightScale = 1.2f, candidateCount = 5)
        val after = before.copy(keyboardHeightScale = null)
        assertEquals(setOf(UserPrefs.K_CANDIDATE_COUNT), after.toMap().keys)
        assertNull(UserPrefs.fromMap(after.toMap()).keyboardHeightScale)
        assertEquals(5, UserPrefs.fromMap(after.toMap()).candidateCount)
    }
}
