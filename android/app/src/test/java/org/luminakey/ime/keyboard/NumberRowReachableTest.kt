package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.core.RimeSchema
import org.luminakey.ime.theme.LayoutLoader
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures

/**
 * 「常駐數字列」那份鍵盤，**預設使用者找得到，而且不會被硬塞給他**。
 *
 * ── 缺陷長什麼樣 ────────────────────────────────────────────────────────
 * `cn-qwerty-numrow.yaml` 是一份做完的佈局（5 列、常駐數字列、依 S24U 實機
 * 重建）。它的 `for_schema` 原本只寫 `["luna_pinyin"]`，而
 * [KeyboardTypes.matches] 是**逐字比對**，隨附的預設方案是 `luna_pinyin_tw`
 * （`core/data/user/default.custom.yaml` 的 schema_list 第一項）。
 *
 * 於是它在鍵盤類型選單裡**對預設使用者不存在**：要先切到「朙月拼音（原版）」
 * 才看得到 —— 那是一個他沒有理由做、也不會想到要做的動作。
 * 功能做完了，只差一行讓人找得到它。
 *
 * ── 加上資格之後冒出來的第二個問題 ──────────────────────────────────────
 * 加進 `for_schema` 之後它變成 declared，而舊的排序是「declared 整批排在
 * generic 前面」，於是選單的**第一項**變成它 —— 而引導頁每個方案只留一張卡
 * （`starterKeyboards`），第一屏預選的那張卡就指著一個使用者其實沒在用的
 * 鍵盤。所以這裡兩件事都要測：**找得到**，而且**第一項仍然是現況**。
 */
class NumberRowReachableTest {

    private val defaultSchema = RimeSchema("luna_pinyin_tw", "朙月拼音·臺灣正體")

    /**
     * 直接讀 repo 的隨附佈局做成 [LayoutBrief]。
     *
     * ⚠ `letters` 刻意留空：那一欄是「泛用佈局配不配得上這個方案」的判準
     * （見 [LayoutBrief.letters]），空集合＝不做那層過濾，而這支測試要問的是
     * **排序與資格**，不是相容性。相容性由 KeyboardTypesTest 那邊負責。
     */
    private fun briefs(): List<LayoutBrief> =
        RepoFixtures.layoutIds.map { id ->
            val l = LayoutLoader.load(id, RepoFixtures.layouts, Platform.ANDROID).value
                ?: error("佈局 $id 載不起來")
            LayoutBrief(
                id = l.id,
                name = l.name.get("zh-Hant"),
                kind = l.kind,
                forSchema = l.forSchema,
                autoForSchema = l.autoForSchema,
                deprecated = l.deprecated,
                primary = l.primary,
            )
        }

    private fun typesForDefaultSchema(): List<KeyboardType> {
        val groups = KeyboardTypes.build(listOf(defaultSchema), briefs())
        return groups.flatMap { it.types }
    }

    @Test
    fun theNumberRowKeyboardIsOfferedForTheDefaultSchema() {
        val ids = typesForDefaultSchema().map { it.layoutId }
        assertTrue(
            "預設方案 ${defaultSchema.id} 的鍵盤類型選單裡沒有 cn-qwerty-numrow —— " +
                "那份佈局對預設使用者等於不存在。實際清單：$ids",
            ids.contains("cn-qwerty-numrow"),
        )
    }

    /**
     * **選單的第一項必須是「什麼都不做會拿到的那一個」。**
     *
     * 引導頁只留每個方案一張卡，而且那張卡是預選的 —— 它與現況不一致的話，
     * 使用者第一次打開 app 看到的就是一個他沒在用的鍵盤名字。
     */
    @Test
    fun theFirstEntryIsWhatTheUserActuallyGets() {
        val first = typesForDefaultSchema().firstOrNull()
        assertEquals(
            "第一項不是 qwerty —— 預設使用者不做任何事時拿到的是 qwerty" +
                "（cn-qwerty-numrow 的 auto_for_schema 沒有 ${defaultSchema.id}）",
            "qwerty",
            first?.layoutId,
        )
    }

    /**
     * 反向測試：把 `auto_for_schema` 也加上去（= 自動命中）的話，
     * 第一項才該換人。這一條證明上面那條測的是排序規則，不是巧合。
     */
    @Test
    fun anAutoHitLayoutDoesTakeTheFirstSlot() {
        val patched = briefs().map {
            if (it.id == "cn-qwerty-numrow") it.copy(autoForSchema = listOf(defaultSchema.id))
            else it
        }
        val first = KeyboardTypes.build(listOf(defaultSchema), patched)
            .flatMap { it.types }.firstOrNull()
        assertEquals(
            "自動命中的佈局沒有排到第一項 —— 那條排序規則沒有在做事",
            "cn-qwerty-numrow",
            first?.layoutId,
        )
    }

    /**
     * 自動切換**沒有**因為這次改動而變 —— 使用者不點它就什麼都不會發生。
     */
    @Test
    fun theDefaultSchemaStillDoesNotAutoSwitchToIt() {
        val brief = briefs().first { it.id == "cn-qwerty-numrow" }
        assertTrue(
            "cn-qwerty-numrow 現在自動命中 ${defaultSchema.id} —— 那是行為變更，" +
                "不是「讓人找得到」",
            !brief.autoForSchema.contains(defaultSchema.id),
        )
        assertTrue(
            "它仍然要保留對 luna_pinyin 的自動命中（那是改動前就有的行為）",
            brief.autoForSchema.contains("luna_pinyin"),
        )
    }
}
