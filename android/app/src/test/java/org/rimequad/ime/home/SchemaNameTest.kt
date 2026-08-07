package org.rimequad.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * 部署完成前的方案名旁路。
 *
 * 存在的理由見 [schemasFromFiles]：引導頁要在首次部署那十幾秒裡問使用者
 * 「你要哪一種鍵盤」，而那段時間 `rs_schema_list()` 什麼都答不出來。
 *
 * 這幾條守的是「寧可不列，也不要印代號」——抽不到名字就回 null，
 * 呼叫端會把整項丟掉，介面上絕不會冒出 `luna_pinyin_tw`。
 */
class SchemaNameTest {

    @Test
    fun readsTheNameUnderTheSchemaBlock() {
        val yaml = """
            # Rime schema
            __include: luna_pinyin.schema:/

            schema:
              schema_id: luna_pinyin_tw
              name: 朙月拼音·臺灣正體
              version: "0.31"

            translator:
              name: 不是這個
        """.trimIndent()
        assertEquals("朙月拼音·臺灣正體", schemaNameOf(yaml))
    }

    @Test
    fun onlyTheSchemaBlockCounts() {
        // `translator` 底下也有 name，但那不是方案名。抓錯區塊會讓選單上出現
        // 一堆叫「translator」的鍵盤。
        val yaml = """
            translator:
              name: 錯的
            engine:
              name: 也是錯的
        """.trimIndent()
        assertNull(schemaNameOf(yaml))
    }

    @Test
    fun stripsQuotesAndComments() {
        assertEquals("九宮格拼音", schemaNameOf("schema:\n  name: \"九宮格拼音\"   # 註解\n"))
        assertEquals("五筆", schemaNameOf("schema:\n  name: '五筆'\n"))
    }

    @Test
    fun missingNameIsNull() {
        assertNull(schemaNameOf("schema:\n  schema_id: x\n"))
        assertNull(schemaNameOf(""))
        assertNull(schemaNameOf("name: 頂層的不算\n"))
    }
}
