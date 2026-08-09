package org.luminakey.ime.store

import android.content.Context

/**
 * 引擎層要對使用者說的一句話：**資源 id + 參數**，不是已經拼好的字串。
 *
 * ── 為什麼不直接回傳 String ────────────────────────────────────────────
 * 因為引擎層（[SchemaStore]、[ArchiveGuard]、[SchemaPreflight]、[DeployGate]）
 * 沒有 `Context`，於是「順手寫一句中文字面值」是阻力最小的路 —— 而這個專案
 * 的**預設語系是英文**（`res/values/` 是英文），那些字會原樣上畫面。
 * 實際發生過的版本：一個法國使用者裝了一個壞掉的方案，看到的是
 * 「缺少相依檔案，已停止」。
 *
 * 帶著 id 走到有 `Context` 的那一層（[StoreController] / [BackupController]）
 * 再 `getString()`，語系就自然跟著系統走，而且 `StringCatalogTest` 那一族
 * 守門看得到它們（那些守門讀的是 `res/values…/strings….xml`，不是 Kotlin）。
 *
 * ── 為什麼參數是 `List<Any>` ──────────────────────────────────────────
 * `getString(id, *args)` 的格式化要區分 `%1$s` 與 `%1$d`，所以數字要以數字型別
 * 傳進來，不能先 `toString()`。
 *
 * ⚠ **不要加一個「直接吃字串」的建構子。** 那等於把上面整段話廢掉：
 * 一旦有一條路可以塞進已經拼好的中文，它就會被用，而守門看不到。
 * 需要顯示引擎的原始故障訊息時，它是**參數**（見 `store_err_download` 的
 * `%2$s`），不是訊息本身。
 */
data class UiMessage(val id: Int, val args: List<Any> = emptyList()) {

    fun format(context: Context): String =
        if (args.isEmpty()) context.getString(id)
        else context.getString(id, *args.toTypedArray())

    companion object {
        fun of(id: Int, vararg args: Any): UiMessage = UiMessage(id, args.toList())
    }
}
