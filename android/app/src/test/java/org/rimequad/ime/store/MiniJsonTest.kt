package org.rimequad.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class MiniJsonTest {

    @Test
    fun `解析基本型別`() {
        val v = MiniJson.parse(
            """{"s":"文字","n":123,"neg":-4.5e2,"t":true,"f":false,"z":null,"a":[1,"2",[3]]}"""
        )
        assertEquals("文字", v.str("s"))
        assertEquals(123L, v.long("n"))
        assertEquals(-450.0, (v["neg"] as Json.Num).asDouble!!, 0.0001)
        assertEquals(true, v.bool("t"))
        assertEquals(false, v.bool("f"))
        assertTrue(v["z"] is Json.Null)
        assertEquals(3, v.arr("a").size)
    }

    @Test
    fun `大整數不經 Double 而失真`() {
        // 索引的 size 欄位以位元組計，超過 2^53 雖然不切實際，但摘要／時間戳
        // 之類的欄位不該因為讀取器的內部表示而改值。
        val v = MiniJson.parse("""{"size": 9007199254740993}""")
        assertEquals(9007199254740993L, v.long("size"))
    }

    @Test
    fun `接受註解 —— 規範 §1 的範例本身就是 jsonc`() {
        val v = MiniJson.parse(
            """
            {
              // 行註解
              "format_version": 1, /* 區塊
                                      註解 */
              "packages": []
            }
            """.trimIndent()
        )
        assertEquals(1L, v.long("format_version"))
    }

    @Test
    fun `跳脫序列`() {
        val v = MiniJson.parse("""{"k":"a\"b\\c\nd你"}""")
        assertEquals("a\"b\\c\nd你", v.str("k"))
    }

    @Test(expected = JsonSyntaxException::class)
    fun `尾隨逗號視為錯誤`() {
        MiniJson.parse("""{"a":1,}""")
    }

    @Test(expected = JsonSyntaxException::class)
    fun `單引號字串視為錯誤`() {
        MiniJson.parse("""{'a':1}""")
    }

    @Test(expected = JsonSyntaxException::class)
    fun `尾端有多餘內容視為錯誤`() {
        MiniJson.parse("""{"a":1} garbage""")
    }

    @Test
    fun `取值輔助在型別不符時回 null 而不是丟例外`() {
        val v = MiniJson.parse("""{"a":1}""")
        assertNull(v.str("a"))
        assertNull(v.str("missing"))
        assertEquals(emptyList<Json>(), v.arr("missing"))
        assertNull(MiniJson.parseOrNull("{"))
    }
}
