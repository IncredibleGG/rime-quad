package org.luminakey.ime.keyboard

import android.content.Context
import org.luminakey.ime.R

/*
 * 鍵盤類型清單的**顯示名**在地化。
 *
 * ── 為什麼名字不直接存 strings.xml ──────────────────────────────────────
 * [KeyboardTypes] 是純函式:它把「已啟用的方案」與「可用的佈局」攤平、分組、
 * 排序,而那些邏輯要能在 JVM 單元測試裡直接斷言(KeyboardTypesTest)。一旦它
 * 需要 Context,那些測試就得扛 Robolectric。所以分組鍵與「自動」那張卡在
 * 純函式那邊是**固定的字面值當代號**,翻譯在這一層做。
 *
 * ── 為什麼這一份要搬出 home/ ────────────────────────────────────────────
 * 同一份清單有兩個消費端:App 的鍵盤設定頁,與鍵盤上 `schema:picker` 開出來的
 * 面板。翻譯原本只做在 App 那一邊,於是**鍵盤上那份分組標題印的是寫死的
 * 繁體**:一個英文系統的使用者在自己的鍵盤上看到「中文（臺灣正體）」,
 * 而 App 裡同一份清單寫的是「Chinese (Taiwan)」—— 同一個東西兩種講法。
 *
 * 所以搬到兩邊都到得了的地方,只留一份。這與 [org.luminakey.ime.prefs.PrefLabels]
 * 的理由完全一樣:數字留在純函式,字留在資源,接起來的那一層只有一個。
 */

/** 分組標題的代號 → 當地語言。認不得的代號原樣回傳（方案自己的名字就是這種）。 */
internal fun localizedGroupTitle(context: Context, raw: String): String = when (raw) {
    KeyboardTypes.ZH_TW -> context.getString(R.string.lang_group_zh_tw)
    KeyboardTypes.ZH_HK -> context.getString(R.string.lang_group_zh_hk)
    KeyboardTypes.YUE -> context.getString(R.string.lang_group_yue)
    KeyboardTypes.ZH -> context.getString(R.string.lang_group_zh)
    KeyboardTypes.OTHER -> context.getString(R.string.lang_group_other)
    else -> raw
}

/**
 * 「這個方案一份佈局都配不上」時 [KeyboardTypes] 會放一張寫著
 * [KeyboardType.AUTO_LABEL] 的卡。那個常數同樣是代號,不是文案。
 */
internal fun KeyboardType.localized(context: Context): KeyboardType =
    if (layoutName == KeyboardType.AUTO_LABEL) {
        copy(layoutName = context.getString(R.string.keyboard_auto_label))
    } else {
        this
    }
