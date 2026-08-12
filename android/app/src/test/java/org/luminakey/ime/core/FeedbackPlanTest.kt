package org.luminakey.ime.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.HapticStrength

/**
 * 「使用者選了哪一階」→「送出什麼」。
 *
 * ── 這一份測試在守什麼 ──────────────────────────────────────────────────
 * 手感是這個專案裡**唯一一件自動化驗不到結果**的事:聽起來對不對、震起來
 * 對不對,只有真機上的人分得出。正因為如此,「有沒有送出不同的東西」這一半
 * 必須被釘得特別死 —— 那是機器驗得到的全部。
 *
 * 舊實作在這一層有一個真的缺陷,而且沒有任何測試會紅:三個檔位對到
 * `CLOCK_TICK` / `KEYBOARD_TAP` / `LONG_PRESS`,而那是三支**不相干的波形**。
 * 在 API 35 的 AOSP 上實測 `dumpsys vibrator_manager`:
 *
 *     弱 → TEXTURE_TICK  101 ms
 *     中 → CLICK         101 ms
 *     強 → HEAVY_CLICK    30 ms   ← 比「中」短三倍
 *
 * 使用者叫它「大小」,而那個階梯上根本沒有大小。所以下面有一條
 * [`四階是同一支波形的四種大小_而不是四支不相干的波形`] 專門盯著它:
 * 振幅**與**時長都必須隨檔位單調遞增。把舊的三個常數翻譯回來就會紅。
 */
class FeedbackPlanTest {

    private val allRoles = KeyRole.entries
    private val allTimbres = SoundTimbre.entries
    private val realStrengths =
        listOf(HapticStrength.LIGHT, HapticStrength.MEDIUM, HapticStrength.HEAVY)

    /* ══════════════════════════ 按鍵音 ══════════════════════════ */

    @Test
    fun `關掉按鍵音就是一聲都不出_不管音色與角色`() {
        for (timbre in allTimbres) {
            for (role in allRoles) {
                assertEquals(
                    "音色=$timbre 角色=$role",
                    FeedbackPlan.Sound.Silent,
                    FeedbackPlan.sound(enabled = false, volume = 1f, timbre = timbre, role = role),
                )
            }
        }
    }

    @Test
    fun `音量拉到零等同於關掉`() {
        for (timbre in allTimbres) {
            assertEquals(
                FeedbackPlan.Sound.Silent,
                FeedbackPlan.sound(enabled = true, volume = 0f, timbre = timbre, role = KeyRole.STANDARD),
            )
        }
    }

    @Test
    fun `系統音色走系統音效_其餘三種走自帶素材`() {
        for (role in allRoles) {
            val system = FeedbackPlan.sound(true, 0.5f, SoundTimbre.SYSTEM, role)
            assertTrue("$role 應該走系統音效,得到 $system", system is FeedbackPlan.Sound.System)
            assertEquals(role, (system as FeedbackPlan.Sound.System).role)

            for (timbre in FeedbackPlan.SAMPLE_TIMBRES) {
                val s = FeedbackPlan.sound(true, 0.5f, timbre, role)
                assertTrue("$timbre/$role 應該走素材,得到 $s", s is FeedbackPlan.Sound.Sample)
            }
        }
    }

    /**
     * 這一條是「聽得出差別」在自動化這一側的全部:**十二種組合送出十二個
     * 不同的東西**。
     *
     * 少了它,「選了水滴其實還是敲擊」這種缺陷完全沉默 —— 兩邊都會播、
     * 都不會崩潰、畫面上一模一樣。
     */
    @Test
    fun `每一種音色與角色的組合都送出不一樣的素材`() {
        val plans = mutableListOf<FeedbackPlan.Sound>()
        val names = mutableListOf<String>()
        for (timbre in FeedbackPlan.SAMPLE_TIMBRES) {
            for (role in allRoles) {
                val s = FeedbackPlan.sound(true, 0.5f, timbre, role) as FeedbackPlan.Sound.Sample
                plans += s
                names += s.asset
            }
        }
        assertEquals("應該有 3 種音色 × 4 種角色 = 12 份素材", 12, plans.size)
        assertEquals("素材名重複了:$names", names.size, names.toSet().size)
        assertEquals("計畫本身重複了", plans.size, plans.toSet().size)
    }

    /**
     * 系統音效的四個角色也必須是四個不同的東西。
     *
     * 改動前這裡**一律是** `FX_KEYPRESS_STANDARD` —— 連系統免費給的區分都
     * 沒有用上。把角色抹平就會紅。
     */
    @Test
    fun `系統音效的四個角色互不相同`() {
        val roles = allRoles.map {
            (FeedbackPlan.sound(true, 1f, SoundTimbre.SYSTEM, it) as FeedbackPlan.Sound.System).role
        }
        assertEquals(4, roles.toSet().size)
    }

    @Test
    fun `素材名與產生腳本的檔名規則一致`() {
        assertEquals("key_soft_standard", FeedbackPlan.assetName(SoundTimbre.SOFT, KeyRole.STANDARD))
        assertEquals("key_drop_return", FeedbackPlan.assetName(SoundTimbre.DROP, KeyRole.RETURN))
        assertEquals(
            "key_mechanical_delete",
            FeedbackPlan.assetName(SoundTimbre.MECHANICAL, KeyRole.DELETE),
        )
        // scripts/gen_key_sounds.py 產生的就是這 12 個名字。命名規則一改,
        // res/raw 裡的檔案就對不上 —— 而那時 SoundPool 只會安靜地不出聲。
        val expected = setOf(
            "key_soft_standard", "key_soft_space", "key_soft_delete", "key_soft_return",
            "key_mechanical_standard", "key_mechanical_space",
            "key_mechanical_delete", "key_mechanical_return",
            "key_drop_standard", "key_drop_space", "key_drop_delete", "key_drop_return",
        )
        val actual = FeedbackPlan.SAMPLE_TIMBRES.flatMap { t ->
            allRoles.map { r -> FeedbackPlan.assetName(t, r) }
        }.toSet()
        assertEquals(expected, actual)
    }

    @Test
    fun `音量超出範圍會被夾回零到一`() {
        val loud = FeedbackPlan.sound(true, 9f, SoundTimbre.SOFT, KeyRole.STANDARD)
        assertEquals(1f, (loud as FeedbackPlan.Sound.Sample).volume, 0f)
        // 負音量與 0 一樣是「不出聲」,不是「反相播放」。
        assertEquals(
            FeedbackPlan.Sound.Silent,
            FeedbackPlan.sound(true, -1f, SoundTimbre.SOFT, KeyRole.STANDARD),
        )
    }

    /* ══════════════════════════ 震動 ══════════════════════════ */

    @Test
    fun `關掉震動或強度為無就是不震`() {
        for (amp in listOf(true, false)) {
            assertEquals(
                FeedbackPlan.Haptic.Silent,
                FeedbackPlan.haptic(enabled = false, strength = HapticStrength.HEAVY, amplitudeControl = amp),
            )
            assertEquals(
                FeedbackPlan.Haptic.Silent,
                FeedbackPlan.haptic(enabled = true, strength = HapticStrength.NONE, amplitudeControl = amp),
            )
        }
    }

    /**
     * ⚠ 這是這一份測試的核心。
     *
     * 使用者說的是「震動**大小**可調」。大小的意思是同一支波形的三種強弱,
     * 不是三支感覺不同的波形。所以振幅必須遞增,**而且時長不可以反過來**。
     *
     * 舊實作在這裡是紅的:它的「強」(`LONG_PRESS` → HEAVY_CLICK)實測 30 ms,
     * 而「中」(`KEYBOARD_TAP` → CLICK)是 101 ms。
     */
    @Test
    fun `四階是同一支波形的四種大小_而不是四支不相干的波形`() {
        val shots = realStrengths.map {
            FeedbackPlan.haptic(true, it, amplitudeControl = true) as FeedbackPlan.Haptic.OneShot
        }
        for (i in 1 until shots.size) {
            assertTrue(
                "振幅必須隨檔位遞增:${shots.map { it.amplitude }}",
                shots[i].amplitude > shots[i - 1].amplitude,
            )
            assertTrue(
                "時長不可以反過來 —— 「強」比「中」短就是舊實作的那個缺陷:" +
                    "${shots.map { it.durationMs }}",
                shots[i].durationMs >= shots[i - 1].durationMs,
            )
        }
    }

    @Test
    fun `振幅落在馬達認得的範圍內`() {
        for (s in realStrengths) {
            val shot = FeedbackPlan.haptic(true, s, true) as FeedbackPlan.Haptic.OneShot
            assertTrue("$s 的振幅 ${shot.amplitude} 超出 1..255", shot.amplitude in 1..255)
        }
    }

    /**
     * 時長要短。
     *
     * 量到的:`dumpsys vibrator_manager` 在快打時出現大量
     * `cancelled_superseded` —— 100 ms 的波形還沒播完就被下一顆鍵砍掉。
     * 一顆按鍵只需要「有東西碰了我一下」。
     */
    @Test
    fun `每一階都短到不會被下一顆鍵砍掉`() {
        for (s in realStrengths) {
            val shot = FeedbackPlan.haptic(true, s, true) as FeedbackPlan.Haptic.OneShot
            assertTrue("$s 的時長 ${shot.durationMs} ms 太長了", shot.durationMs in 5..30)
        }
    }

    @Test
    fun `三個檔位送出三個互不相同的計畫`() {
        val plans = realStrengths.map { FeedbackPlan.haptic(true, it, true) }
        assertEquals("三階之間有兩階送出一模一樣的東西:$plans", 3, plans.toSet().size)
    }

    /**
     * 馬達分不出強弱時退回舊的常數 —— 但**仍然是三個不同的常數**,
     * 而且設定頁要把這件事講出來(見 `feel_haptic_flat`)。
     */
    @Test
    fun `馬達沒有振幅控制時退回常數而不是靜音`() {
        val kinds = realStrengths.map {
            val p = FeedbackPlan.haptic(true, it, amplitudeControl = false)
            assertTrue("$it 應該退回常數,得到 $p", p is FeedbackPlan.Haptic.Constant)
            (p as FeedbackPlan.Haptic.Constant).kind
        }
        assertEquals("退回路徑也不該把三階壓成一階", 3, kinds.toSet().size)
    }

    @Test
    fun `有沒有振幅控制走的是兩條不同的路`() {
        for (s in realStrengths) {
            assertNotEquals(
                FeedbackPlan.haptic(true, s, true),
                FeedbackPlan.haptic(true, s, false),
            )
        }
    }

    /* ══════════════════════════ 角色判定 ══════════════════════════ */

    @Test
    fun `空白刪除換行各自認得出來`() {
        assertEquals(KeyRole.SPACE, FeedbackPlan.roleOf("space"))
        assertEquals(KeyRole.SPACE, FeedbackPlan.roleOf("KP_Space"))
        assertEquals(KeyRole.DELETE, FeedbackPlan.roleOf("BackSpace"))
        assertEquals(KeyRole.DELETE, FeedbackPlan.roleOf("Delete"))
        assertEquals(KeyRole.RETURN, FeedbackPlan.roleOf("Return"))
        assertEquals(KeyRole.RETURN, FeedbackPlan.roleOf("KP_Enter"))
    }

    @Test
    fun `大小寫不影響角色判定`() {
        // 佈局檔是人寫的:qwerty.yaml 寫 `BackSpace`,別的檔案可能寫 `backspace`。
        assertEquals(KeyRole.DELETE, FeedbackPlan.roleOf("BACKSPACE"))
        assertEquals(KeyRole.SPACE, FeedbackPlan.roleOf("SPACE"))
    }

    @Test
    fun `認不得 keysym 時退而看 id`() {
        assertEquals(KeyRole.SPACE, FeedbackPlan.roleOf(null, "space"))
        assertEquals(KeyRole.DELETE, FeedbackPlan.roleOf(null, "backspace"))
        assertEquals(KeyRole.RETURN, FeedbackPlan.roleOf(null, "enter"))
    }

    @Test
    fun `兩個都認不得就是一般鍵_不是靜音`() {
        assertEquals(KeyRole.STANDARD, FeedbackPlan.roleOf("a"))
        assertEquals(KeyRole.STANDARD, FeedbackPlan.roleOf(null, null))
        assertEquals(KeyRole.STANDARD, FeedbackPlan.roleOf("Shift_L", "shift"))
    }

    /**
     * keysym 勝過 id。
     *
     * 佈局的 `id` 是給主題與換鍵用的名字,人可以隨便取;keysym 是真的送給
     * 引擎的東西。兩者衝突時要相信後者。
     */
    @Test
    fun `keysym 勝過 id`() {
        assertEquals(KeyRole.SPACE, FeedbackPlan.roleOf("space", "my_big_key"))
        assertEquals(KeyRole.STANDARD, FeedbackPlan.roleOf("a", "space"))
    }
}
