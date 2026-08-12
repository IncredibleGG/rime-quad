package org.luminakey.ime.prefs

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.core.SoundTimbre
import org.luminakey.ime.theme.HapticStrength

/**
 * 手感這四項的**檔位換算**,以及兩個入口不會漂移這件事。
 *
 * ── 「兩個入口」是什麼 ──────────────────────────────────────────────────
 * 音量與震動在 App 設定頁(`FeelPage`)與鍵盤內快捷面板(`FeelStripContent`)
 * 各有一份分段控制。它們讀寫的是**同一組** [PrefLevels] 函式 —— 這一份測試
 * 守的就是那個「同一組」:`indexOf*` 與 `with*` 必須互為反函式,否則兩個
 * 入口會慢慢長成兩套「中」,而畫面上完全看不出來。
 */
class FeelPrefsTest {

    private val theme = Triple(true, 0.65f, HapticStrength.MEDIUM) // 假的主題底值

    /* ────────────────────────── 音色 ────────────────────────── */

    @Test
    fun `沒設定過音色就是系統音色`() {
        assertEquals(0, PrefLevels.indexOfTimbre(UserPrefs()))
        assertNull(UserPrefs().soundTimbre)
    }

    @Test
    fun `四個檔位對到四種不同的音色`() {
        val picked = (0..3).map {
            PrefLevels.withTimbre(UserPrefs(), it, soundLevel = 2).soundTimbre
        }
        assertEquals("四格應該對到四種音色:$picked", 4, picked.toSet().size)
        assertEquals(SoundTimbre.SYSTEM, picked[0])
    }

    @Test
    fun `選了哪一格_回頭問就是哪一格`() {
        for (i in 0..3) {
            val p = PrefLevels.withTimbre(UserPrefs(), i, soundLevel = 2)
            assertEquals("第 $i 格存進去又讀出來變成別的", i, PrefLevels.indexOfTimbre(p))
        }
    }

    /**
     * ⚠ 「按鍵音＝關」時點音色會**順便把音量打開到「小」**。
     *
     * 這是刻意的:另外兩種做法(畫成灰的／隱藏)一個違反「做不到的功能不要
     * 畫出來」、一個會讓版面跳,而「點下去什麼都沒發生」是這個專案抓過好幾次
     * 的那一類缺陷。
     */
    @Test
    fun `按鍵音是關的時候選音色會順便把音量打開`() {
        val p = PrefLevels.withTimbre(UserPrefs(), 2, soundLevel = 0)
        assertEquals(SoundTimbre.MECHANICAL, p.soundTimbre)
        assertEquals(true, p.soundEnabled)
        assertEquals(
            "應該開到最小的那一檔,不是最大聲",
            1,
            PrefLevels.indexOfSound(p, baseEnabled = false, baseVolume = 0f),
        )
    }

    @Test
    fun `按鍵音本來就開著的時候選音色不會動到音量`() {
        val loud = PrefLevels.withSound(UserPrefs(), 3)
        val p = PrefLevels.withTimbre(loud, 3, soundLevel = 3)
        assertEquals(SoundTimbre.DROP, p.soundTimbre)
        assertEquals(loud.soundVolume, p.soundVolume)
        assertEquals(3, PrefLevels.indexOfSound(p, theme.first, theme.second))
    }

    @Test
    fun `超出範圍的格子被夾回來而不是丟例外`() {
        assertEquals(SoundTimbre.SYSTEM, PrefLevels.withTimbre(UserPrefs(), -5, 2).soundTimbre)
        assertEquals(SoundTimbre.DROP, PrefLevels.withTimbre(UserPrefs(), 99, 2).soundTimbre)
    }

    /* ────────────────────── 兩個入口不會漂移 ────────────────────── */

    @Test
    fun `音量四格存進去讀出來都對得回原來那一格`() {
        for (i in 0..3) {
            val p = PrefLevels.withSound(UserPrefs(), i)
            assertEquals(i, PrefLevels.indexOfSound(p, theme.first, theme.second))
        }
    }

    @Test
    fun `震動四格存進去讀出來都對得回原來那一格`() {
        for (i in 0..3) {
            val p = PrefLevels.withHaptic(UserPrefs(), i)
            assertEquals(i, PrefLevels.indexOfHaptic(p, theme.first, theme.third))
        }
    }

    /* ────────────────────── 儲存層 ────────────────────── */

    @Test
    fun `音色存得進去也讀得回來`() {
        val p = UserPrefs(soundTimbre = SoundTimbre.DROP)
        assertEquals(SoundTimbre.DROP, UserPrefs.fromMap(p.toMap()).soundTimbre)
        assertEquals("drop 應該以列舉名存", "DROP", p.toMap()[UserPrefs.K_SOUND_TIMBRE])
    }

    @Test
    fun `沒設定過就不寫進儲存層`() {
        // UserPrefs 的第一原則:不把預設值抄一份進偏好。
        assertFalse(UserPrefs.K_SOUND_TIMBRE in UserPrefs().toMap())
        assertTrue(UserPrefs().isPristine)
    }

    @Test
    fun `認不得的音色名視同沒設定過`() {
        val p = UserPrefs.fromMap(mapOf(UserPrefs.K_SOUND_TIMBRE to "GONG"))
        assertNull("壞掉的值不該把整份偏好拖垮", p.soundTimbre)
    }

    /* ────────────────────── 檔位標籤的數量 ────────────────────── */

    @Test
    fun `音色的檔位數與其他幾組一樣是四`() {
        // StringCatalogTest 會拿這個數字去比對三份 strings.xml 的 levels_timbre。
        assertEquals(4, PrefLevels.TIMBRE_LABELS.size)
    }
}
