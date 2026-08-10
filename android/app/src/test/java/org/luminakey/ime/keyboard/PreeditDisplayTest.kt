package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.RepoFixtures

/**
 * 宿主 app 的輸入框裡該看到什麼。
 *
 * 使用者的原話：「但是你顯示 PGM 就很奇怪？」——`PGM` 是我們的內部編碼，
 * 而它一路送進了他正在打字的那個 app。
 *
 * ⚠ **這裡刻意不寫「檔案裡有沒有這個字」那種檢查。** 每一條驗的都是
 * 「給定**真的會被載入的那份佈局**，算出來的字串是什麼」，而且每一條都
 * 附一段「它該在什麼時候紅」。
 */
class PreeditDisplayTest {

    /* ═══════════════ 1. 代碼不准進到宿主輸入框 ═══════════════ */

    /**
     * 使用者截圖上那一串。打「你好」= M G G A M，speller 依 delimiter
     * 斷成 `MG GAM`。
     *
     * 這條紅的時候代表：宿主輸入框又在印九宮格的代表字母了。
     */
    @Test
    fun `九宮格的按鍵代碼進不到宿主輸入框`() {
        val codes = t9GroupCodes()
        assertTrue("送 M 的那顆鍵鍵面是 mno —— M 是一整組的代號", codes.contains('M'))

        // 一段都還沒定：只說「我在組字」，不說謊。
        assertEquals("⋯", host("MG GAM", codes))
        // 使用者點了消歧欄的 ni → 引擎收到 `niGAM`，preedit 成了 `ni GAM`。
        assertEquals("ni⋯", host("ni GAM", codes))
        // 選了字之後前半段變成漢字。漢字不是按鍵代碼，一律留著。
        assertEquals("你⋯", host("你GAM", codes))
        // 兩個音節都定了，剩最後一鍵。
        assertEquals("ni hao⋯", host("ni hao M", codes))
        // 全部定完就沒有代碼了 —— 這時候原封不動，連 ⋯ 都不加。
        assertEquals("ni hao", host("ni hao", codes))
        // 使用者截圖上的另一串。
        assertEquals("⋯", host("PGM", codes))
    }

    /**
     * 已確定的音節與未確定的尾巴**分得開** —— 不是只有畫面上分得開，
     * 資料結構上就分得開。
     *
     * 這是這一版交付的**機制**：t9-model.md §1.5 的 S1-2 要把 `pending`
     * 換成引擎自己的讀音（`GAM` → `hao`），換的時候動的是呈現，
     * 不是這條界線。界線沒了，那個改動就得整個重做。
     */
    @Test
    fun `已確定的音節與未確定的代碼是兩個欄位`() {
        val split = PreeditParts.of("ni GAM", t9GroupCodes())
        assertEquals("ni", split.settled)
        assertEquals("GAM", split.pending)
        assertEquals("ni GAM", split.raw)
        assertTrue(split.hasPending)

        val done = PreeditParts.of("ni hao", t9GroupCodes())
        assertEquals("ni hao", done.settled)
        assertEquals("", done.pending)
        assertFalse(done.hasPending)
    }

    /**
     * 掃**每一份**宣告給 `t9_pinyin` 的佈局。代碼字串不是手寫的，是拿
     * [T9Syllables.t9Encode]（改寫輸入串用的同一張對照表）現算的 ——
     * 對照表與這條規則因此不可能各走各的。
     *
     * 新增一份九宮格佈局而它的鍵面用了大寫代號，這裡會紅。
     */
    @Test
    fun `每一份九宮格佈局都送不出連續兩個以上的代碼`() {
        val repo = FixtureRepo()
        val t9Layouts = RepoFixtures.layoutIds
            .mapNotNull { repo.loadLayout(it).value }
            .filter { it.forSchema.contains(T9_SCHEMA) }
        assertTrue(
            "掃不到任何宣告 for_schema: $T9_SCHEMA 的佈局 —— 判準壞了,這條在空轉",
            t9Layouts.size >= 3,
        )
        val nihao = T9Syllables.t9Encode("ni")!! + T9Syllables.t9Encode("hao")!!
        assertEquals("MGGAM", nihao)

        var checked = 0
        for (layout in t9Layouts) {
            val layer = layout.layers.first()
            assertEquals("${layout.id} 的第一層不是九宮格那一層", "t9", layer.id)
            val codes = InlinePreedit.groupCodeChars(layer)
            // 引擎在組字途中會給出的各種形狀：全模糊、定了一半、定了兩段。
            for (preedit in listOf(nihao, "MG GAM", "ni $nihao", "ni GAM", "ni hao M", "PGM")) {
                val shown = HostPreedit.forHost(preedit, codes)
                assertNotNull("$preedit 在 ${layout.id} 上組字卻沒有組字區", shown)
                val run = codeRun(shown!!.text, codes)
                assertNull(
                    "${layout.id}/${layer.id}：preedit「$preedit」送進宿主輸入框變成" +
                        "「${shown.text}」,裡面還有連續代碼「$run」—— " +
                        "使用者按的是 mno/ghi,他的 app 裡卻冒出 $run",
                    run,
                )
            }
            checked++
        }
        assertEquals("掃到的份數對不上 —— 掃到 0 份一樣是全綠", t9Layouts.size, checked)
    }

    /* ═══════════════ 2. 別的方案一個字元都不准動 ═══════════════ */

    /**
     * 反過來的那一半，而且是這個檔案最重要的一條。
     *
     * 使用者說的是「九宮格顯示 PGM 很奇怪」，**不是**「輸入框不要顯示組字」。
     * 全拼打 `nihao` 時 preedit 就是 `nihao`、注音顯示注音符號 —— 那些都是
     * 他剛剛按過的東西，有意義。把整個功能拿掉一樣能讓上面幾條變綠，
     * 這一條就是攔那個做法的。
     */
    @Test
    fun `全拼與注音的組字串原封不動送進宿主輸入框`() {
        val repo = FixtureRepo()

        val qwerty = repo.loadLayout("qwerty").value!!
        val qwertyCodes = InlinePreedit.groupCodeChars(qwerty.layers.first())
        assertTrue("全鍵盤一鍵一字母,沒有任何「一組的代號」", qwertyCodes.isEmpty())
        for (p in listOf("nihao", "ni hao", "n", "zhuang", "ni'hao")) {
            assertEquals("全拼的 $p 被動過了", p, host(p, qwertyCodes))
        }

        val bopomofo = repo.loadLayout("bopomofo-dachen").value!!
        val bpmfCodes = InlinePreedit.groupCodeChars(bopomofo.layers.first())
        for (p in listOf("ㄋㄧˇㄏㄠˇ", "你ㄏㄠˇ", "ㄅ")) {
            assertEquals("注音的 $p 被動過了", p, host(p, bpmfCodes))
        }
    }

    /**
     * 佈局還沒載進來時**不准藏東西**。
     *
     * 「沒有資訊」與「確定沒有意義」是兩回事。混為一談的話，鍵盤剛彈出來的
     * 那幾十毫秒（佈局還在載）宿主輸入框裡的組字會整個閃掉。
     */
    @Test
    fun `不知道鍵面是什麼的時候一律照送`() {
        assertEquals("MG GAM", host("MG GAM", emptySet()))
        assertEquals(emptySet<Char>(), InlinePreedit.groupCodeChars(null))
    }

    /* ═══════════════ 3. 組字區的硬性質 ═══════════════ */

    /**
     * **preedit 非空 → 送進去的文字一定非空。**
     *
     * 空字串會讓組字區當場消失，等於偷偷改成了「宿主輸入框全程留空」那個
     * 做法（t9-model.md §1.5 的 (A)）。那個做法本身不是錯的，但它會拿掉
     * 無障礙服務與宿主自動完成賴以判斷「使用者正在輸入」的訊號 ——
     * 要改就要有意識地改，不可以是某一條分支不小心回了空字串。
     *
     * 另一半：preedit 是空的才回 null，呼叫端據此收掉組字區。
     */
    @Test
    fun `組字中就一定佔著組字區`() {
        val codes = t9GroupCodes()
        for (p in listOf("M", "MG", "MG GAM", "PGM", "ni GAM", "ni hao", "'", " ", "MG'GAM")) {
            val shown = HostPreedit.forHost(p, codes)
            assertNotNull("preedit「$p」在組字中,組字區卻不見了", shown)
            assertTrue("preedit「$p」送出了空字串 —— 組字區會當場消失", shown!!.text.isNotEmpty())
        }
        assertNull("沒有組字就不該有組字區", HostPreedit.forHost("", codes))
        assertNull(HostPreedit.forHost("", emptySet()))
    }

    /**
     * **游標不隨長度變動。**
     *
     * `setComposingText()` 的第二個參數不是字元索引：`> 0` 是相對於文字
     * **結尾**，`1` 永遠是「文字的最後面」。這裡轉換過後長度會變
     * （`MG GAM` 6 字 → `⋯` 1 字），而正是那種時候最容易有人「順手」
     * 把它改成 `preedit.length` —— 那會把游標送到組字區外面，
     * 宿主 app 的反應各家不同，有的直接當掉。
     *
     * 這條紅的時候代表有人讓游標跟著長度走了。
     */
    @Test
    fun `游標位置與文字長度無關`() {
        val codes = t9GroupCodes()
        val cases = listOf("M", "MG GAM", "ni GAM", "ni hao", "PGM", "ㄋㄧˇㄏㄠˇ")
        val cursors = cases.mapNotNull { HostPreedit.forHost(it, codes)?.cursor }.toSet()
        assertEquals("長度不同的組字串拿到了不同的游標位置", setOf(HostPreedit.AFTER_TEXT), cursors)
        assertEquals("AFTER_TEXT 不再是「文字的最後面」", 1, HostPreedit.AFTER_TEXT)

        // 文字真的變短了 —— 這條在的話上面那條才有意義。
        val shrunk = HostPreedit.forHost("MG GAM", codes)!!
        assertTrue("轉換之後長度沒變,游標那條測試在空轉", shrunk.text.length < "MG GAM".length)
    }

    /* ═══════════════ 4. 兩個顯示端走同一條切分 ═══════════════ */

    /**
     * 候選列左端那一格與宿主輸入框**不可以各切各的**。
     *
     * 上一輪只修了候選列那一格，宿主輸入框那一份原封不動 —— 於是同一串
     * 按鍵在鍵盤上是 `ni⋯`、在使用者的 app 裡是 `ni GAM`。這條就是釘住
     * 「兩邊同一個切分」的。
     *
     * 兩邊唯一的差別是**沒有東西可印的時候**：候選列那一格整格收掉
     * （null，讓候選往左靠），宿主輸入框留一個記號（組字區不能空）。
     */
    @Test
    fun `候選列與宿主輸入框切的是同一刀`() {
        val codes = t9GroupCodes()
        for (p in listOf("ni GAM", "你GAM", "ni hao M", "ni hao", "nihao", "ni'GAM")) {
            val bar = InlinePreedit.forDisplay(p, codes)
            val hostText = host(p, codes)
            assertEquals("「$p」在候選列上是「$bar」,在宿主輸入框裡卻是「$hostText」", bar, hostText)
        }
        // 全是代碼：候選列收掉整格，宿主輸入框留記號。
        assertNull(InlinePreedit.forDisplay("MG GAM", codes))
        assertEquals(HostPreedit.PENDING_MARK, host("MG GAM", codes))
    }

    /** 砍完只剩分隔符時要收乾淨，不留一個孤零零的 `'` 或空白。 */
    @Test
    fun `砍掉代碼之後不留懸空的分隔符`() {
        val codes = t9GroupCodes()
        assertEquals("⋯", host("MG'GAM", codes))
        assertEquals("ni⋯", host("ni'GAM", codes))
        assertEquals("ni⋯", host("ni  GAM  ", codes))
        assertEquals("ni hao⋯", host("ni hao  M", codes))
    }

    /* ═══════════════ 工具 ═══════════════ */

    private fun host(preedit: String, codes: Set<Char>): String? =
        HostPreedit.forHost(preedit, codes)?.text

    /** 文字裡第一段連續兩個以上的代碼；沒有就回 null。守門腳本用的是同一條判準。 */
    private fun codeRun(text: String, codes: Set<Char>): String? {
        var run = 0
        for (i in text.indices) {
            if (text[i] in codes) {
                run++
                if (run >= 2 && (i + 1 == text.length || text[i + 1] !in codes)) {
                    return text.substring(i - run + 1, i + 1)
                }
            } else {
                run = 0
            }
        }
        return null
    }

    private fun t9GroupCodes(): Set<Char> {
        val layout = FixtureRepo().loadLayout(T9_LAYOUT).value!!
        val layer = layout.layer("t9")
        assertNotNull("$T9_LAYOUT 沒有 t9 層", layer)
        return InlinePreedit.groupCodeChars(layer)
    }

    private companion object {
        const val T9_LAYOUT = "cn-t9-pinyin"
        const val T9_SCHEMA = "t9_pinyin"
    }
}
