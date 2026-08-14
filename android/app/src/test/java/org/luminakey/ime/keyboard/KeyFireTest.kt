package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Test
import org.luminakey.ime.theme.ActionVerb
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.LayoutKey
import org.luminakey.ime.theme.SendSpec

/**
 * ⛔ **工單 #99 的第二半:判準是對的,而送到判準面前的那份答案過期了。**
 *
 * [SelectionDigitKeysTest] 守的是判準本身（「現在有沒有在組字」→ 選字／
 * 送數字／什麼都不做）。這一份守的是**那個答案有沒有及時送到**。
 *
 * 2026-08-14 emulator-5558 / lumina_test2 實測，`cn-t9-pinyin-numrow` ＋
 * `t9_pinyin`,同一顆 `n3`:
 *
 * ```
 * 閒置按 n3       → 輸入框「3」            （對的:沒在組字,就打一個 3）
 * 打 M G G A M    → 候選列畫出 1..9        （畫面上 n3 已經是「選第 3 個」）
 * 再按 n3         → 輸入框「33⋯」          ⛔ 組字被毀
 * ```
 *
 * 探針把凍住的那一刻印了出來 —— `captured` 是手勢協程手上那一份,
 * `fresh` 是這一幀真正的答案:
 *
 * ```
 * 07:38:01.585 launch key=n3 captured=SendDigit
 * 07:38:09.907 compose key=n3 act=Select(indexOnPage=2) isComposing=true cands=7
 * 07:38:20.956 fire   key=n3 captured=SendDigit fresh=Select(indexOnPage=2)
 * ```
 *
 * 為什麼會凍:`Modifier.pointerInput(key)` 的協程在這顆鍵**第一次被碰到**
 * 時才啟動,而它的 key 只有 `key` —— 組字狀態變了不會讓它重啟,於是它
 * 捕捉到的區域變數從此不動。**一顆數字鍵只要在沒組字時被按過一次,
 * 之後它就一直答「打數字」。**
 *
 * ⚠ **這一份摸不到 `KeyView` 本身**(它是 `@Composable`,而本模組的單元
 * 測試是純 JVM、沒有 compose-ui-test)。它守的是 [keyFire] 這個接縫:
 * 「手上抓著的是一個問,不是一份抄本」。`KeyView` 真的走這條路那一半,
 * 由 `scripts/verify_selection_digit.sh` 的第 3b 步在真機上守
 * ——那一步的前提斷言(閒置那一下**必須真的送出數字**)在修好之前是紅的。
 */
class KeyFireTest {

    private fun key(
        id: String,
        label: String,
        keysym: String? = null,
        tap: KeyAction? = null,
    ) = LayoutKey(
        id = id, label = label, hint = "", icon = null,
        labelFrom = LabelSource.NONE, width = 1f, style = "default",
        spacer = false, active = false, repeat = false,
        send = keysym?.let { SendSpec.Keysym(it, 0, 0) },
        tap = tap, doubleTap = null, longPress = null,
        popup = null, swipe = emptyMap(),
    )

    private val n3 = key("n3", "3", "3")
    private val send3 = KeyboardEvent.Send(n3.send!!)

    /**
     * ⛔ **這一條就是缺陷本身。**
     *
     * 手勢協程只建立一次 —— 所以底下的 `fire` 也只建立一次,之後每一下
     * 都走同一個閉包。第二下必須看到「使用者已經在組字了」。
     */
    @Test
    fun `閒置按過一次的數字鍵,組字之後再按要選字而不是把數字送進引擎`() {
        var now: SelectionDigitKeys.Act? = SelectionDigitKeys.Act.SendDigit
        val out = mutableListOf<KeyboardEvent>()
        // 手勢協程在這顆鍵**第一次被碰到**時啟動,並且抓著這一份不放。
        val fire = keyFire(n3, { now }, out::add)

        fire()                                          // 閒置:打一個 3
        now = SelectionDigitKeys.Act.Select(2)          // 使用者組了字,候選列畫出序號
        fire()                                          // 同一顆鍵再按一次

        assertEquals(
            "第二下送出去的必須是「選頁內第 3 個」。送 Send(3) = 數字進引擎、" +
                "被 recognizer 收走、組字變成 `33⋯` —— 那正是工單 #99。",
            listOf(send3, KeyboardEvent.Candidate(2)),
            out.toList(),
        )
    }

    /** 反過來一樣要成立:組字中按過之後,收掉組字再按必須打得出數字。 */
    @Test
    fun `組字中按過一次的數字鍵,收掉組字再按要打得出數字`() {
        var now: SelectionDigitKeys.Act? = SelectionDigitKeys.Act.Select(2)
        val out = mutableListOf<KeyboardEvent>()
        val fire = keyFire(n3, { now }, out::add)

        fire()
        now = SelectionDigitKeys.Act.SendDigit
        fire()

        assertEquals(listOf(KeyboardEvent.Candidate(2), send3), out.toList())
    }

    /**
     * ⛔ `Ignore` 是「什麼都不做」,而且**不得**退回 §9.6 的 send ——
     * 退回去就是把數字送進引擎,也就是這個缺陷的另一個入口。
     */
    @Test
    fun `Ignore 什麼都不做,不得退回去送數字`() {
        var now: SelectionDigitKeys.Act? = SelectionDigitKeys.Act.SendDigit
        val out = mutableListOf<KeyboardEvent>()
        val fire = keyFire(n3, { now }, out::add)

        fire()
        now = SelectionDigitKeys.Act.Ignore
        fire()

        assertEquals(listOf(send3), out.toList())
    }

    /** 不是數字鍵(問到 null)就走 §9.6 原路:tap 勝過 send。 */
    @Test
    fun `不是數字鍵就走 §9·6 原路`() {
        val out = mutableListOf<KeyboardEvent>()
        keyFire(n3, { null }, out::add).invoke()

        val clear = KeyAction(ActionVerb.CLEAR, emptyList(), "clear")
        val both = key("k", "x", keysym = "x", tap = clear)
        keyFire(both, { null }, out::add).invoke()

        val noop = key("z", "z")
        keyFire(noop, { null }, out::add).invoke()

        assertEquals(listOf(send3, KeyboardEvent.Act(clear)), out.toList())
    }
}
