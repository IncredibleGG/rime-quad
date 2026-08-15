package org.luminakey.ime.core

import android.view.inputmethod.EditorInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * 覆核抓到的第四句假話,以及它底下那顆**會送出訊息**的鍵。
 *
 * ── 量到的東西 ──────────────────────────────────────────────────────────
 * 全新安裝、部署中,依序點 逗號 / Enter / 空白 / `?123`:
 *
 *     HOST-after-specials=[\n]                        ← Enter 真的寫進了宿主
 *     引擎尚未就緒（phase=DEPLOYING），按鍵 44 不送出   ← 逗號 擋掉
 *     引擎尚未就緒（phase=DEPLOYING），按鍵 32 不送出   ← 空白 擋掉
 *
 * 而遮罩上寫的是「**Keys are off** for about … seconds」(秒數來自
 * [DeployEstimate]),底下的鍵盤同時
 * 已經換成數字符號層。三件事在「keys are off」底下照常發生:換行編輯宿主、
 * 退格刪宿主文字、`?123` 換層。
 *
 * ── 這一份守兩件事 ──────────────────────────────────────────────────────
 *   ① **文案只宣告它真的擋得住的那一類**,而且同一個階段在兩種編輯框上
 *      說的是兩句不同的話 —— 因為換行在那兩種框裡做的**不是同一件事**。
 *   ② **會送出的那一顆換行,在引擎沒好的時候擋下來,而且擋了看得見。**
 *
 * ⚠ 第 ② 條刻意**不動** [InputReadiness.decide] 的判準:換行的輸出與引擎
 *   無關,那條判準是對的。這裡加的是另一條軸 —— 「這一下收不收得回來」。
 *   多行框裡的換行照送,只有掛了 editor action 的框(enter-to-send、搜尋的
 *   「前往」)才擋。判準與代價寫在 [InputReadiness.holdsEnter] 的檔頭。
 */
class EnterHoldWiringTest {

    /* ─────────────── ① 判準:這顆換行會送出嗎 ─────────────── */

    /** 多行框:`TextView` 自己會補 `IME_FLAG_NO_ENTER_ACTION` → 換一行。 */
    @Test
    fun `多行框的換行只是換一行`() {
        assertFalse(
            HostEditorPolicy.enterCommitsToHost(
                EditorInfo.IME_FLAG_NO_ENTER_ACTION or EditorInfo.IME_ACTION_SEND
            )
        )
        assertFalse(HostEditorPolicy.enterCommitsToHost(EditorInfo.IME_ACTION_NONE))
    }

    /** enter-to-send 的聊天框、搜尋框的「前往」—— 按下去東西就交出去了。 */
    @Test
    fun `掛了 editor action 的框按換行就是送出`() {
        for (action in listOf(
            EditorInfo.IME_ACTION_SEND,
            EditorInfo.IME_ACTION_GO,
            EditorInfo.IME_ACTION_SEARCH,
            EditorInfo.IME_ACTION_DONE,
        )) {
            assertTrue(
                "imeOptions=$action 的框,換行是把東西交出去",
                HostEditorPolicy.enterCommitsToHost(action),
            )
        }
    }

    /**
     * 判準抄的是 AOSP `sendDefaultEditorAction(fromEnterKey = true)` 的條件,
     * 而 `fallbackKey()` 送換行的第一步正是呼叫它。兩者一旦分家,畫面上寫的
     * 與實際發生的就會對不上 —— 那正是這一輪要修的東西。
     *
     * `IME_ACTION_UNSPECIFIED`(0)在 AOSP 那一支裡**會**走 performEditorAction
     * (它只排除 `IME_ACTION_NONE`),所以這裡也必須回 true。
     */
    @Test
    fun `未指定的 action 與 AOSP 同一邊`() {
        assertTrue(HostEditorPolicy.enterCommitsToHost(EditorInfo.IME_ACTION_UNSPECIFIED))
    }

    /* ─────────────── ② 判準:這一下擋不擋 ─────────────── */

    @Test
    fun `引擎沒好而換行會送出時要擋`() {
        assertTrue(
            InputReadiness.holdsEnter(
                engineReady = false,
                bypassRime = false,
                keysym = AndroidKeyMap.RETURN,
                enterCommitsToHost = true,
            )
        )
    }

    /** 多行框的換行照送 —— 擋掉它等於連換一行都做不到。 */
    @Test
    fun `換行只是換一行的時候不擋`() {
        assertFalse(
            InputReadiness.holdsEnter(
                engineReady = false,
                bypassRime = false,
                keysym = AndroidKeyMap.RETURN,
                enterCommitsToHost = false,
            )
        )
    }

    /** 引擎好了就與這條規則無關;密碼框那條路從一開始就不走引擎。 */
    @Test
    fun `引擎好了或密碼框都不擋`() {
        assertFalse(
            InputReadiness.holdsEnter(true, bypassRime = false, keysym = AndroidKeyMap.RETURN, enterCommitsToHost = true)
        )
        assertFalse(
            InputReadiness.holdsEnter(false, bypassRime = true, keysym = AndroidKeyMap.RETURN, enterCommitsToHost = true)
        )
    }

    /**
     * **退格一個字都不准動。** 這條規則只碰換行。
     *
     * 擋掉退格會讓使用者連「把剛才誤打的字刪掉」都做不到,而那件事在
     * 引擎沒好的時候特別重要 —— 他手上很可能正有一段打錯的字。
     */
    @Test
    fun `退格與其他鍵一律不受這條規則影響`() {
        for (k in listOf(AndroidKeyMap.BACKSPACE, 'a'.code, 0x0020, ','.code)) {
            assertFalse(
                "keysym=$k 不該被這條規則碰到",
                InputReadiness.holdsEnter(false, false, k, enterCommitsToHost = true),
            )
        }
    }

    /** [InputReadiness.decide] 的判準**沒有被改動** —— 換行仍然是 LITERAL。 */
    @Test
    fun `decide 的判準沒有被改掉`() {
        for (k in listOf(AndroidKeyMap.BACKSPACE, AndroidKeyMap.RETURN)) {
            assertEquals(
                "decide() 的判準是「引擎改不改得動這顆鍵的意思」,這一輪不改它",
                InputReadiness.Decision.LITERAL,
                InputReadiness.decide(engineReady = false, bypassRime = false, keysym = k),
            )
        }
    }

    /* ─────────────── ③ 接線 ─────────────── */

    /**
     * 服務層真的問過,而且問在 `fallbackKey(` **之前**。
     *
     * 驗的是位置而不是「檔案裡有沒有這個字」:`holdsEnter` 出現在 import 或
     * 註解裡也算「有」,而那證明不了任何事(這個專案抓過四次的形狀)。
     */
    @Test
    fun `handleSend 在送出換行之前問過`() {
        val body = handleSendBody()
        val ask = body.indexOf("InputReadiness.holdsEnter(")
        assertTrue("handleSend() 沒有問過 holdsEnter —— 那一下又會把半句話送出去", ask >= 0)
        val fallback = body.indexOf("fallbackKey(")
        assertTrue("handleSend() 裡找不到 fallbackKey —— 這條測試已經對不上實作", fallback >= 0)
        assertTrue("holdsEnter 排在 fallbackKey 之後,等於沒判斷", ask < fallback)
        assertTrue(
            "沒有把宿主那個事實餵進去,那就只是換個地方寫死",
            body.contains("enterCommitsToHost"),
        )
    }

    /**
     * 那個事實真的來自宿主的 `EditorInfo`,而不是某個猜出來的預設值。
     *
     * 驗的是**資料流**而不是「檔案裡有沒有這個字」:先在 `applyEditorPolicy()`
     * 的**函式體內**找到 `HostEditorPolicy.enterCommitsToHost(info?.imeOptions…)`
     * 算出來的那個名字,再要求欄位真的被指派成它。中間換一個變數名、
     * 或改成寫死一個布林,兩步都接不起來。
     */
    @Test
    fun `enterCommitsToHost 由編輯框政策更新`() {
        val body = funcBody("private fun applyEditorPolicy(")
        assertTrue("找不到 applyEditorPolicy() —— 這條測試已經對不上實作", body != null)
        // 先找欄位是被**哪一個名字**指派的,再回頭看那個名字是怎麼算出來的。
        // 反過來寫(先找呼叫再找欄位)會被「算了但沒寫進去」騙過。
        val write = Regex("""^\s*enterCommitsToHost = (\w+)\s*$""", RegexOption.MULTILINE)
            .find(body!!)
        assertTrue(
            "applyEditorPolicy() 從頭到尾沒有指派 enterCommitsToHost —— " +
                "換了編輯框之後判斷會停在上一個框",
            write != null,
        )
        val name = write!!.groupValues[1]
        assertTrue(
            "「$name」不是從宿主的 EditorInfo 算出來的 —— 那就只是換個地方寫死一個布林",
            Regex("""val\s+$name\s*=[\s\S]{0,400}?HostEditorPolicy\.enterCommitsToHost\(info""")
                .containsMatchIn(body),
        )
    }

    /**
     * **擋了一定要看得見。**「按了沒反應」是這個專案抓過六次的那種缺陷,
     * 而這一版自己新增了一條會擋鍵的路。
     */
    @Test
    fun `擋下來的那一下在遮罩上看得見`() {
        val body = handleSendBody()
        assertTrue(
            "擋下換行卻什麼都沒說 —— 使用者得到的是「按了沒反應」",
            body.contains("flashHeldKey(") && body.contains("R.string.ime_notice_enter_held"),
        )
        val veil = File(mainRoot, "keyboard/KeyboardView.kt").readText()
        assertTrue(
            "遮罩沒有畫 heldKeyNotice,那句話寫進了狀態卻沒有出口",
            Regex("""message\s*=\s*state\.fatalMessage\s*\n?\s*\?:\s*state\.heldKeyNotice""")
                .containsMatchIn(codeOnly(veil)),
        )
    }

    /**
     * 遮罩那句話**分兩種**,而且分岔的依據就是同一個事實。
     *
     * 一句話蓋兩種行為必有一種是假的 —— 這正是這一輪要修的東西。
     */
    @Test
    fun `遮罩那句話跟著編輯框走`() {
        val body = funcBody("private fun busyNoticeFor(")
        assertTrue("找不到 busyNoticeFor() —— 這條測試已經對不上實作", body != null)
        val b = body!!
        for (id in listOf(
            "ime_notice_preparing",
            "ime_notice_preparing_enter_held",
            "ime_notice_deploying",
            "ime_notice_deploying_enter_held",
        )) {
            assertTrue("busyNoticeFor() 沒有用到 R.string.$id", b.contains("R.string.$id"))
        }
        assertTrue(
            "busyNoticeFor() 沒有看 enterCommitsToHost —— 兩句話永遠只會出現一句",
            b.contains("enterCommitsToHost"),
        )
    }

    /**
     * 上一版那句話**已經不在**了。
     *
     * 這是這條缺陷的本體:「Keys are off」宣告了三件它做不到的事
     * (換行編輯宿主、退格刪字、`?123` 換層)。留著它,上面那些綠燈
     * 一條都不算數。
     */
    @Test
    fun `上一版那句假話不在字串表裡`() {
        val en = File("src/main/res/values/strings.xml").readText()
        assertTrue("讀不到 values/strings.xml", en.length > 1000)
        assertFalse(
            "「Keys are off」還在 —— 那句話宣告了三件它做不到的事",
            en.contains("Keys are off"),
        )
        // 而它真的有講到那兩顆放行的鍵,否則使用者按下退格看到字少一個,
        // 又會覺得畫面在騙他。
        assertTrue("英文文案沒有交代退格", en.contains("Backspace"))
        assertTrue("英文文案沒有交代換行", en.contains("Enter"))
    }

    /* ─────────────── ④ 反向測試(G1)─────────────── */

    /**
     * 把這一版的接線逐條拆掉,每一條都必須是紅的。
     * 植入用真的原始碼,而且先斷言它真的變了。
     */
    @Test
    fun `拆掉接線之後每一條都要紅`() {
        val svc = File(mainRoot, "RimeInputMethodService.kt").readText()
        val kv = File(mainRoot, "keyboard/KeyboardView.kt").readText()

        val gone = svc.replaceFirst(
            "if (InputReadiness.holdsEnter(",
            "if (false && InputReadiness.holdsEnter(",
        )
        assertNotEquals("(a) 沒有植入成功", svc, gone)
        // 這一種植入騙得過「有沒有這個字」的檢查,所以順序斷言不夠 ——
        // 誠實記下:本測試對 `false &&` 這種寫法**沒有牙齒**,它擋的是
        // 「整段被刪掉」與「排在 fallbackKey 之後」。
        assertTrue(gone.contains("InputReadiness.holdsEnter("))

        val deleted = svc.replace(
            Regex("""if \(InputReadiness\.holdsEnter\([\s\S]*?\n {24}\}\n"""),
            "",
        )
        assertNotEquals("(b) 沒有植入成功 —— 錨點對不上,這一條反向測試等於沒做", svc, deleted)
        assertFalse(
            "(b) 整段刪掉之後仍然找得到 holdsEnter —— 判準抓錯位置了",
            handleSendBodyOf(deleted).contains("InputReadiness.holdsEnter("),
        )

        val muted = kv.replaceFirst("?: state.heldKeyNotice", "")
        assertNotEquals("(c) 沒有植入成功", kv, muted)
        assertFalse(
            "(c) 遮罩不畫 heldKeyNotice 之後判準仍然綠 —— 那條綠燈不代表任何事",
            Regex("""message\s*=\s*state\.fatalMessage\s*\n?\s*\?:\s*state\.heldKeyNotice""")
                .containsMatchIn(codeOnly(muted)),
        )
    }

    /* ─────────────── 掃描器本體 ─────────────── */

    private fun serviceCode(): String {
        val f = File(mainRoot, "RimeInputMethodService.kt")
        assertTrue("找不到 ${f.path}", f.isFile)
        return codeOnly(f.readText())
    }

    private fun handleSendBody(): String = handleSendBodyOf(
        File(mainRoot, "RimeInputMethodService.kt").readText()
    )

    /** `handleSend()` 的函式體(大括號配對),**已經去掉註解**。 */
    private fun handleSendBodyOf(src: String): String {
        val at = src.indexOf("private fun handleSend(")
        assertTrue("找不到 handleSend() —— 它被改名或搬走了", at >= 0)
        val open = src.indexOf('{', at)
        var depth = 0
        for (i in open until src.length) {
            when (src[i]) {
                '{' -> depth++
                '}' -> {
                    depth--
                    if (depth == 0) return codeOnly(src.substring(open + 1, i))
                }
            }
        }
        throw AssertionError("handleSend() 的大括號沒有配對成功")
    }

    /** 從 [anchor] 起的函式體(大括號配對),去註解。找不到回 null。 */
    private fun funcBody(anchor: String): String? {
        val src = File(mainRoot, "RimeInputMethodService.kt").readText()
        val at = src.indexOf(anchor)
        if (at < 0) return null
        // 表達式函式體(`= when (…) { … }`)也要接得住:從錨點之後第一個 `{` 起算。
        val open = src.indexOf('{', at)
        if (open < 0) return null
        var depth = 0
        for (i in open until src.length) {
            when (src[i]) {
                '{' -> depth++
                '}' -> {
                    depth--
                    if (depth == 0) return codeOnly(src.substring(open + 1, i))
                }
            }
        }
        return null
    }

    /**
     * 只留下程式碼。註解會提到它在講的每一個識別字 —— 拿它當程式碼掃,
     * 結論一定是錯的(這個專案在 [InputReadinessTest] 第一次跑就踩到)。
     */
    private fun codeOnly(src: String): String =
        src.replace(Regex("""/\*[\s\S]*?\*/"""), "")
            .lineSequence()
            .joinToString("\n") { line ->
                val i = line.indexOf("//")
                if (i >= 0 && line.take(i).isBlank()) "" else line
            }

    companion object {
        /** 單元測試的工作目錄是模組目錄(`android/app`)。 */
        private val mainRoot = File("src/main/java/org/luminakey/ime")
    }
}
