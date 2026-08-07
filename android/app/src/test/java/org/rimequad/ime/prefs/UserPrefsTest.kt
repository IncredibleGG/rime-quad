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
            layoutPins = "luna_pinyin=cn-t9-pinyin-numrow",
            candidateSizeScale = 1.25f,
            candidateCount = 7,
            simplification = true,
            asciiPunct = false,
            spaceBehavior = SpaceBehavior.ALWAYS_SPACE,
            networkEnabled = true,
            offlineNoticeSeen = true,
            autoCheckUpdate = false,
        )
        assertFalse(full.isPristine)
        assertEquals(full, UserPrefs.fromMap(full.toMap()))
        // 每一個欄位都必須真的有進映射，否則就是 toMap 漏了一個。
        assertEquals(20, full.toMap().size)
    }

    /* ── 佈局指定（§9.1.1 的 SHOULD 之持久化）────────────────────────── */

    @Test
    fun layoutPinsRoundTripThroughTheEncodedString() {
        val pins = linkedMapOf(
            "luna_pinyin" to "cn-t9-pinyin-numrow",
            "bopomofo_tw" to "bopomofo-dachen",
        )
        val encoded = UserPrefs.encodeLayoutPins(pins)!!
        assertEquals(pins, UserPrefs.decodeLayoutPins(encoded))
        // 順序要保住：選單的高亮與「上次挑的」都靠它。
        assertEquals(listOf("luna_pinyin", "bopomofo_tw"), UserPrefs.decodeLayoutPins(encoded).keys.toList())
    }

    @Test
    fun anEmptyPinMapIsAbsenceNotAnEmptyString() {
        // 空字串會在儲存層留下一個 key，違反本檔的第一不變式。
        assertNull(UserPrefs.encodeLayoutPins(emptyMap()))
        assertTrue(UserPrefs(layoutPins = null).toMap().isEmpty())
        assertEquals(emptyMap<String, String>(), UserPrefs.decodeLayoutPins(null))
        assertEquals(emptyMap<String, String>(), UserPrefs.decodeLayoutPins(""))
    }

    @Test
    fun oneCorruptPinDoesNotTakeTheOthersDown() {
        val decoded = UserPrefs.decodeLayoutPins("luna_pinyin=qwerty;garbage;=orphan;trailing=")
        assertEquals(mapOf("luna_pinyin" to "qwerty"), decoded)
    }

    @Test
    fun idsCarryingTheSeparatorsAreDroppedRatherThanCorruptingTheRest() {
        // 解回來會錯位的字串比少記一筆偏好糟糕得多。
        val encoded = UserPrefs.encodeLayoutPins(
            linkedMapOf("a=b" to "qwerty", "ok" to "t9;pinyin", "good" to "qwerty")
        )
        assertEquals("good=qwerty", encoded)
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

    /* ── 連網開關 ────────────────────────────────────────────────── */

    @Test
    fun `連網開關未設定時就是關，而且不得把 false 抄進偏好`() {
        // 與 autoCheckUpdate 相反：那一項的「未設定」等同開，這一項的
        // 「未設定」等同**關**。消費端一律寫 `networkEnabled == true`，
        // 絕不可寫 `networkEnabled ?: true` —— 那會讓從沒表態過的使用者
        // 被預設成連網，整個「離線為預設」的定位就沒了。
        assertNull(UserPrefs().networkEnabled)
        assertTrue(UserPrefs().toMap()[UserPrefs.K_NETWORK_ENABLED] == null)
        assertFalse(UserPrefs().networkEnabled == true)

        val on = UserPrefs(networkEnabled = true)
        assertEquals(setOf(UserPrefs.K_NETWORK_ENABLED), on.toMap().keys)
        assertEquals(true, UserPrefs.fromMap(on.toMap()).networkEnabled)

        // 「他明確關過」與「他從沒開過」在支援上是兩件事，值得分得出來。
        val off = UserPrefs(networkEnabled = false)
        assertEquals(false, UserPrefs.fromMap(off.toMap()).networkEnabled)
    }

    @Test
    fun `全部回復預設會把連網開關關回去`() {
        // resetAll 寫的是 UserPrefs()，也就是「一個 key 都沒有」。
        // 這裡守的是：那個狀態解出來一定是「不連網」。
        val afterReset = UserPrefs.fromMap(UserPrefs().toMap())
        assertFalse(afterReset.networkEnabled == true)
        assertNull(afterReset.offlineNoticeSeen)
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
