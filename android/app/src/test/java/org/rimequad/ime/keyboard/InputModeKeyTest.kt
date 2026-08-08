package org.rimequad.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import org.junit.Test
import org.rimequad.ime.core.RimeStatus
import org.rimequad.ime.theme.ActionVerb
import org.rimequad.ime.theme.Actions
import org.rimequad.ime.theme.Diagnostics
import org.rimequad.ime.theme.LabelSource
import org.rimequad.ime.theme.LayoutKind
import org.rimequad.ime.theme.RepoFixtures

/**
 * 「中／En」這顆鍵：**標籤說得清楚**，而且**按下去真的有用**。
 *
 * ── 使用者回報 ──────────────────────────────────────────────────────────
 * 「這裡的『中』要修改成『中/en』，這樣別人才知道你現在的語言是啥。然後點擊了
 * 以後變成 26 鍵才對，但是他沒變……無論是 26 還是 9 鍵，這樣才對。」
 *
 * 兩個缺陷，第二個比較根本：
 *
 * 1. 只寫一個「中」的切換鍵有兩種讀法 —— 「現在是中文」與「按了會變中文」，
 *    而它們指向相反的操作。使用者沒有辦法從鍵面判斷自己在哪一態。
 * 2. 那顆鍵原本是 `toggle:ascii_mode`，**只切引擎、不動佈局**。在 QWERTY 上
 *    剛好沒問題（本來就是 26 鍵），在九宮格上等於按了沒用：進了英文模式，
 *    眼前還是 abc/def/ghi 八顆鍵，26 個字母一個都打不出來。
 *
 * 修法是把「切中英」做成一個**語義完整**的動作 `input_mode:toggle`：
 * 切模式，並且切到本佈局宣告的 `alpha_layer`；沒宣告的佈局只切模式。
 */
class InputModeKeyTest {

    private fun host() = LayoutHost(FixtureRepo())

    /* ── 動作 ───────────────────────────────────────────────────────── */

    @Test
    fun theActionParsesAndCarriesNoArgument() {
        val diag = Diagnostics()
        val a = Actions.parse("input_mode:toggle", "t", diag, null)
        assertNotNull(a)
        assertEquals(ActionVerb.INPUT_MODE_TOGGLE, a!!.verb)
        assertTrue("目標是佈局的 alpha_layer，不是寫在鍵上", a.args.isEmpty())
        assertEquals("input_mode:toggle", a.raw)
    }

    @Test
    fun anUnknownInputModeActionIsRejectedRatherThanSilentlyIgnored() {
        val diag = Diagnostics()
        assertNull(Actions.parse("input_mode:sideways", "t", diag, null))
        assertTrue(diag.hasErrors)
    }

    /** 舊動詞仍然合法：只切模式，不動佈局。兩者語義不同，不是別名。 */
    @Test
    fun theOldModeOnlyActionStillMeansWhatItMeant() {
        val a = Actions.parse("toggle:ascii_mode", "t", Diagnostics(), null)
        assertEquals(ActionVerb.TOGGLE_OPTION, a!!.verb)
        assertEquals("ascii_mode", a.arg)
    }

    /* ── 九宮格：按下去真的變 26 鍵 ─────────────────────────────────── */

    @Test
    fun theGridKeyboardSwitchesToItsAlphabetLayerAndBack() {
        val h = host()
        h.ensureLoaded()
        h.switchLayout("cn-t9-pinyin")
        assertEquals("cn-t9-pinyin/t9", "${h.layout?.id}/${h.layerId}")

        h.setInputMode(true)
        assertEquals("進英文就要看得到 26 個字母", "cn-t9-pinyin/en", "${h.layout?.id}/${h.layerId}")

        h.setInputMode(false)
        assertEquals("回中文就要回九宮格", "cn-t9-pinyin/t9", "${h.layout?.id}/${h.layerId}")
    }

    /**
     * 回程必須**每一次**都在。先前的缺陷是「從九宮格切英語就再也切不回來」，
     * 那類問題往往在第二輪才現形（第一次好好的，第二次卡住）。
     */
    @Test
    fun theRoundTripSurvivesRepetition() {
        val h = host()
        h.ensureLoaded()
        h.switchLayout("cn-t9-pinyin")
        repeat(5) {
            assertEquals(true, h.toggleInputMode())
            assertEquals("en", h.layerId)
            assertEquals(false, h.toggleInputMode())
            assertEquals("t9", h.layerId)
        }
    }

    /** 從數字層按「中／En」也要落在字母層，而不是留在數字層打英文。 */
    @Test
    fun itWorksFromAnyLayerNotJustTheDefaultOne() {
        val h = host()
        h.ensureLoaded()
        h.switchLayout("cn-t9-pinyin")
        h.setLayer("num")
        assertEquals("num", h.layerId)
        h.setInputMode(true)
        assertEquals("en", h.layerId)
        h.setInputMode(false)
        assertEquals("t9", h.layerId)
    }

    /* ── QWERTY：只切模式，佈局不動 ─────────────────────────────────── */

    @Test
    fun anAlphabeticKeyboardOnlyChangesTheModeAndStaysPut() {
        val h = host()
        h.ensureLoaded()
        h.switchLayout("qwerty")
        val before = h.layerId
        h.setInputMode(true)
        assertEquals("qwerty 本來就是 26 鍵，沒有要換的層", before, h.layerId)
        assertTrue(h.asciiMode)
        h.setInputMode(false)
        assertEquals(before, h.layerId)
    }

    @Test
    fun switchingKeyboardResetsTheMode() {
        val h = host()
        h.ensureLoaded()
        h.switchLayout("cn-t9-pinyin")
        h.setInputMode(true)
        assertEquals("en", h.layerId)
        h.switchLayout("qwerty")
        assertFalse("換鍵盤＝重新開始，不該繼承上一份的中英模式", h.asciiMode)
        h.switchLayout("cn-t9-pinyin")
        assertEquals("回到九宮格要停在九宮格層，使用者沒按過任何鍵", "t9", h.layerId)
    }

    /* ── 鍵面 ───────────────────────────────────────────────────────── */

    @Test
    fun theFaceShowsBothStatesAtOnce() {
        val cn = RimeStatus(isAsciiMode = false)
        val en = RimeStatus(isAsciiMode = true)
        // 純文字形態兩態相同 —— 差別在強調哪一段，不在寫哪幾個字。
        assertEquals("中/En", faceOf(LabelSource.INPUT_MODE_PAIR, null, "", cn))
        assertEquals("中/En", faceOf(LabelSource.INPUT_MODE_PAIR, null, "", en))
    }

    @Test
    fun theCurrentStateIsTheEmphasisedSegment() {
        val on = 0xFF000000.toInt()
        val off = 0xFF999999.toInt()

        val cn = inputModeFace(asciiMode = false, activeColor = on, idleColor = off)
        assertEquals("中/En", cn.text)
        assertEquals("打中文時亮的是「中」", Color(on), colorAt(cn, cn.text.indexOf('中')))
        assertEquals(Color(off), colorAt(cn, cn.text.indexOf('E')))

        val en = inputModeFace(asciiMode = true, activeColor = on, idleColor = off)
        assertEquals("打英文時亮的是「En」", Color(on), colorAt(en, en.text.indexOf('E')))
        assertEquals(Color(off), colorAt(en, en.text.indexOf('中')))
    }

    /**
     * 整顆鍵**不**染 active 色。鍵面已經說了現在是哪一態，再把整顆鍵變成
     * accent 色只是重複同一個訊息，而且看起來像「這顆鍵被鎖住了」。
     */
    @Test
    fun thePairKeyDoesNotAlsoLightUpTheWholeKey() {
        assertFalse(isActiveFace(false, LabelSource.INPUT_MODE_PAIR, RimeStatus(isAsciiMode = true)))
        assertTrue(isActiveFace(true, LabelSource.INPUT_MODE_PAIR, RimeStatus(isAsciiMode = true)))
    }

    /* ── 隨附佈局的不變式 ───────────────────────────────────────────── */

    /**
     * 半套的鍵是最糟的結果：鍵面寫「中/En」卻只切模式，或反過來。
     * 兩者必須成對出現，而且是掃**每一份**佈局，不是白名單。
     */
    @Test
    fun everyShippedModeKeyUsesBothHalvesOfTheNewVocabulary() {
        val repo = FixtureRepo()
        var seen = 0
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value ?: continue
            for (layer in layout.layers) {
                for (key in layer.rows.flatMap { it.keys }) {
                    val pairFace = key.labelFrom == LabelSource.INPUT_MODE_PAIR
                    val pairAction = key.tap?.verb == ActionVerb.INPUT_MODE_TOGGLE
                    if (!pairFace && !pairAction) continue
                    seen++
                    assertTrue(
                        "$id/${layer.id}/${key.id}: 鍵面與動作必須成對",
                        pairFace && pairAction,
                    )
                }
            }
        }
        assertTrue("隨附佈局裡至少要有一批這種鍵", seen >= 20)
    }

    /**
     * 非字母鍵盤（九宮格、筆畫、注音大千）**必須**宣告 alpha_layer。
     *
     * 這條是使用者那句話的直譯：「無論是 26 還是 9 鍵，這樣才對」。
     * 沒有 alpha_layer 的九宮格，那顆鍵按下去就是按了沒用 —— 而「按了沒用」
     * 不會有任何錯誤訊息，只有使用者在真機上發現。
     */
    @Test
    fun everyNonAlphabeticKeyboardDeclaresWhereItsLettersAre() {
        val repo = FixtureRepo()
        val needsLetters = setOf(LayoutKind.GRID, LayoutKind.PHONETIC)
        var checked = 0
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value ?: continue
            if (layout.kind !in needsLetters) continue
            checked++
            val alpha = layout.alphaLayer
            assertNotNull("$id 是 ${layout.kind} 鍵盤，必須宣告 alpha_layer", alpha)
            assertNotNull("$id: alpha_layer「$alpha」不存在", layout.layer(alpha!!))
            assertTrue(
                "$id: 字母層至少要有 26 個字母鍵",
                layout.layer(alpha)!!.rows.flatMap { it.keys }.count { it.label.length == 1 } >= 26,
            )
        }
        assertTrue("夾具裡必須真的有這種鍵盤", checked >= 3)
    }

    /** 宣告了 alpha_layer 的佈局，回程一定落在自己的 default_layer 上。 */
    @Test
    fun theReturnTripIsStructuralForEveryLayoutThatDeclaresAnAlphabetLayer() {
        val repo = FixtureRepo()
        for (id in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(id).value ?: continue
            val alpha = layout.alphaLayer ?: continue
            val h = host()
            h.ensureLoaded()
            h.switchLayout(id)
            h.setInputMode(true)
            assertEquals("$id 進不了字母層", alpha, h.layerId)
            h.setInputMode(false)
            assertEquals("$id 回不去", layout.defaultLayer, h.layerId)
        }
    }

    private fun colorAt(s: AnnotatedString, index: Int): Color =
        s.spanStyles.first { index >= it.start && index < it.end }.item.color
}
