package org.luminakey.ime.store

/**
 * 一次市集／部署作業結束之後，畫面上該出現什麼。
 *
 * ── 為什麼是一個純函式，而不是 finish() 裡的一個 if ──────────────────────
 * 因為那個 if 曾經**默默吃掉**預檢的警告。`finish(ok, message, details)` 的
 * 成功分支只有 `showToast(message)`，`details` 在整條成功路徑上一次都沒被讀；
 * 而三個呼叫端全部是 `ok = true`。上一個 commit 特地把 `emptyList()` 換成
 * `r.details`，訊息裡寫著「否則就是一個使用者永遠不會知道自己少了什麼的功能」
 * —— 傳了，但沒有人接。
 *
 * 實際後果：SchemaPreflight 自陳市集 98 個方案裡有 20 個會產生 WARNING
 * （倉頡／五筆／粵拼／鄭碼那些反查詞典不在的），使用者只看到綠色的
 * 「已啟用並部署完成」，幾天後發現筆畫反查打不出東西，而且毫無線索。
 *
 * 抽成純函式之後，「成功但有話要說」變成一條**可以斷言**的事實，
 * 而不是一段沒有人看得到的分支。對照組：[BackupController] 兩條路都保留 notes，
 * `BackupSection` 真的把它畫出來。
 */
internal sealed class FinishUi {

    /** 成功、而且沒有別的話要說 —— 短暫的 snackbar，不擋路。 */
    data class Toast(val message: String) : FinishUi()

    /** 有話要說（失敗，或成功但帶著警告）—— 停在對話框上，由使用者自己關掉。 */
    data class Dialog(
        val ok: Boolean,
        val message: String,
        val details: List<String>,
    ) : FinishUi()
}

/**
 * ⚠ **成功 + 有 details 一定走對話框。**
 *
 * 「成功不要彈對話框」本身是對的規則（使用者按「重新整理字詞」的目的是把它
 * 弄好，不是讀一份報告，真機回報的原話是「部署完就要退出界面對不」），
 * 但那條規則的前提是**沒有別的話要說**。警告不是報告，是「你少了一塊東西」；
 * 一個幾秒後自己消失的 snackbar 對這種話等於沒說。
 *
 * 空白的 detail 不算話：它只會在對話框上留一個空的項目符號。
 */
internal fun finishUi(ok: Boolean, message: String, details: List<String>): FinishUi {
    val notes = details.filter { it.isNotBlank() }
    return if (ok && notes.isEmpty()) {
        FinishUi.Toast(message)
    } else {
        FinishUi.Dialog(ok, message, notes)
    }
}
