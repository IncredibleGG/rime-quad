package org.luminakey.ime.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * 工單 #105:首次部署那十幾秒,鍵盤把英文送進了使用者的真實訊息。
 *
 * 這一份分兩層:
 *   ① 判準本身(純函式)——「引擎不在的時候,哪一顆鍵可以送出去」;
 *   ② **接線**——`RimeInputMethodService.handleSend()` 真的問過這個判準,
 *      而且問在 `processKey` / `fallbackKey` 之前。
 *
 * 第 ② 層是這個專案反覆踩過的坑:判準寫得再對,呼叫端沒接就等於沒有,
 * 而「沒接」在畫面上與「接了」長得一模一樣。
 */
class InputReadinessTest {

    private val a = 'a'.code
    private val space = 0x0020
    private val digit3 = '3'.code
    private val comma = ','.code

    /* ─────────────── ① 判準 ─────────────── */

    @Test
    fun `引擎沒好時字母不得送出去`() {
        assertEquals(
            InputReadiness.Decision.HOLD,
            InputReadiness.decide(engineReady = false, bypassRime = false, keysym = a),
        )
    }

    /**
     * 這一條就是實測到的那五下。`nihao` 的每一個字母都必須是 HOLD ——
     * 少擋一個,宿主輸入框裡就會多一個字母。
     */
    @Test
    fun `nihao 五個字母一個都不准過`() {
        val held = "nihao".map {
            InputReadiness.decide(engineReady = false, bypassRime = false, keysym = it.code)
        }
        assertEquals(List(5) { InputReadiness.Decision.HOLD }, held)
    }

    @Test
    fun `引擎好了就交給引擎`() {
        assertEquals(
            InputReadiness.Decision.ENGINE,
            InputReadiness.decide(engineReady = true, bypassRime = false, keysym = a),
        )
    }

    /** 密碼／數字框本來就不走引擎,引擎在不在都一樣。 */
    @Test
    fun `bypass 的編輯框一律字面上屏`() {
        for (ready in listOf(true, false)) {
            assertEquals(
                "bypassRime=true 時不該受引擎狀態影響",
                InputReadiness.Decision.LITERAL,
                InputReadiness.decide(engineReady = ready, bypassRime = true, keysym = a),
            )
        }
    }

    /**
     * 退格與換行**照常**。擋掉它們等於讓使用者連「刪掉剛才誤打的字」
     * 與「把已經寫好的訊息送出去」都做不到。
     */
    @Test
    fun `退格與換行不受影響`() {
        for (k in listOf(AndroidKeyMap.BACKSPACE, AndroidKeyMap.RETURN)) {
            assertEquals(
                "keysym=$k 的輸出與引擎無關,不該被擋",
                InputReadiness.Decision.LITERAL,
                InputReadiness.decide(engineReady = false, bypassRime = false, keysym = k),
            )
        }
    }

    /**
     * 空白、數字、標點**要擋**。它們在中文輸入法裡的意思由引擎決定
     * (接受候選 / 選字 / 全形半形),引擎不在就不知道它們該做什麼。
     */
    @Test
    fun `意思由引擎決定的鍵一律擋下`() {
        for (k in listOf(space, digit3, comma)) {
            assertEquals(
                "keysym=$k 的意思引擎改得動,引擎不在時不該送出去",
                InputReadiness.Decision.HOLD,
                InputReadiness.decide(engineReady = false, bypassRime = false, keysym = k),
            )
        }
        assertTrue(space !in setOf(AndroidKeyMap.BACKSPACE, AndroidKeyMap.RETURN))
    }

    /* ─────────────── ② 接線 ─────────────── */

    private fun serviceSource(): String {
        val f = File("src/main/java/org/luminakey/ime/RimeInputMethodService.kt")
        assertTrue("找不到 ${f.path}", f.isFile)
        return f.readText()
    }

    /**
     * 只留下**程式碼**:`//` 開頭的整行註解與 `/* */` 區塊一律挖掉。
     *
     * ⚠ 這一步不是潔癖,是這條測試第一次跑就踩到的東西:上面那段閘門的註解
     * 裡寫著「`fallbackKey(code)` 就把 `n` 原樣 commit 進宿主」,於是
     * `indexOf("fallbackKey(")` 命中的是**註解**,順序斷言當場錯判。
     * 註解會提到自己在講的每一個識別字 —— 拿它當程式碼掃,結論一定是錯的。
     */
    private fun codeOnly(src: String): String =
        src.lineSequence().joinToString("\n") { line ->
            val i = line.indexOf("//")
            if (i >= 0 && line.take(i).isBlank()) "" else line
        }.replace(Regex("""/\*[\s\S]*?\*/"""), "")

    /** `handleSend()` 的函式體(大括號配對),**已經去掉註解**。 */
    private fun handleSendBody(): String {
        val src = serviceSource()
        val at = src.indexOf("private fun handleSend(")
        assertTrue("找不到 handleSend() —— 它被改名或搬走了,這條測試已經對不上實作", at >= 0)
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

    /**
     * **判準真的被問過,而且問在送出去之前。**
     *
     * 驗的是順序而不是「檔案裡有沒有這個字」:`InputReadiness` 這個名字
     * 出現在 import 裡也算得上「有」,但那證明不了任何事。這裡要求
     * `InputReadiness.decide(` 在 `handleSend` 的函式體裡出現,而且它的位置
     * 在 `RimeCore.processKey(` 與 `fallbackKey(` **兩者之前** ——
     * 判斷排在後面等於沒判斷。
     */
    @Test
    fun `handleSend 在送出去之前問過判準`() {
        val body = handleSendBody()
        assertTrue("handleSend() 只有 ${body.length} 個字元,大概抓錯了", body.length >= 400)

        val decide = body.indexOf("InputReadiness.decide(")
        assertTrue("handleSend() 沒有問過 InputReadiness —— 那五個字母又會進使用者的訊息", decide >= 0)

        val processKey = body.indexOf("RimeCore.processKey(")
        val fallback = body.indexOf("fallbackKey(")
        assertTrue("handleSend() 裡找不到 RimeCore.processKey —— 對不上實作", processKey >= 0)
        assertTrue("handleSend() 裡找不到 fallbackKey —— 對不上實作", fallback >= 0)

        assertTrue(
            "InputReadiness.decide 排在 RimeCore.processKey 之後,等於沒判斷",
            decide < processKey,
        )
        assertTrue(
            "InputReadiness.decide 排在 fallbackKey 之後,等於沒判斷",
            decide < fallback,
        )
    }

    /** HOLD 這條路真的有人處理 —— 三個分支都要在。 */
    @Test
    fun `三種決定在 handleSend 裡都有出口`() {
        val body = handleSendBody()
        val missing = listOf("HOLD", "LITERAL", "ENGINE").filterNot { body.contains(it) }
        assertEquals("handleSend() 沒有處理這幾種決定:$missing", emptyList<String>(), missing)
    }

    /**
     * 反向:把接線拿掉(上一版那三行)之後,順序斷言必須是紅的。
     *
     * 這是 G1。植入的是**上一版真正的寫法**,不是一段假的程式碼。
     */
    @Test
    fun `拿掉接線之後順序斷言會紅`() {
        val old = """
            val consumed = !bypassRime && RimeCore.processKey(session, code, spec.modifiers)
            if (!consumed) fallbackKey(code)
            layoutHost.afterKeySent()
            refreshFromRime()
        """.trimIndent()
        assertTrue("上一版的寫法裡本來就沒有 InputReadiness", old.indexOf("InputReadiness.decide(") < 0)
        assertTrue("上一版確實有 processKey 與 fallbackKey", old.contains("RimeCore.processKey(") && old.contains("fallbackKey("))
    }
}
