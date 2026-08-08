package org.rimequad.ime.theme

/**
 * 把一則 [Diagnostic] 變成使用者看得懂的一句話。
 *
 * ── 為什麼這一層是純 Kotlin ──────────────────────────────────────────
 * `theme/` 這個套件一行 `android.*` 都沒有，iOS 端要照抄的就是它。所以
 * 「哪一個 code 對到哪一段字」由這裡定義（[DiagnosticCode.resourceName]），
 * 「那段字長什麼樣」由平台的資源檔提供（Android 是
 * `res/values.../strings_diag.xml`，接線在
 * [org.rimequad.ime.DiagnosticStrings]）。
 *
 * ── 回退是刻意的，但不是藉口 ─────────────────────────────────────────
 * 沒有安裝樣板來源時（JVM 單元測試、或 app 還沒建好 ConfigRepository），
 * 這裡回落到 [Diagnostic.developerMessage]，讓診斷**永遠不會消失** ——
 * 這也是規範 §6.5 對未知 code 的要求：不得丟棄，退化為顯示字面值。
 *
 * 但「翻譯漏了一條，於是使用者看到一行英文開發訊息」正是本專案再三提防的
 * 那種沒人會發現的缺陷，所以**缺翻譯不靠回退兜住，靠測試變紅**：
 * [org.rimequad.ime.DiagnosticStringsTest] 逐 code、逐語系檢查
 * `strings_diag.xml`，少一條就失敗。
 */
object DiagnosticText {

    /**
     * 平台的樣板來源。回傳 null 代表**沒有**這一條樣板 —— 呼叫端會回退，
     * 但那是不該發生的狀態。
     */
    fun interface TemplateSource {
        /**
         * @param code 產生這則診斷的碼；有些 code 的 args 本身是要翻譯的代號
         *             （[DiagnosticTerm]），平台端要先把它們換成當地語言的字面值
         * @param name [DiagnosticCode.resourceName] 推導出來的資源名
         * @param args 位置參數，順序由 §6.5.1 的碼表逐碼固定
         */
        fun format(code: DiagnosticCode, name: String, args: List<String>): String?
    }

    @Volatile
    private var source: TemplateSource? = null

    /** 由持有 Context 的那一層安裝（[org.rimequad.ime.keyboard.ConfigRepository]）。 */
    fun install(s: TemplateSource) {
        source = s
    }

    /** 測試用：拆掉樣板來源，回到開發者回退。 */
    fun uninstall() {
        source = null
    }

    /** 使用者看得到的那一句。 */
    fun render(d: Diagnostic): String {
        val s = source ?: return d.developerMessage
        return s.format(d.code, d.code.resourceName(d.args.size), d.args) ?: d.developerMessage
    }

    /** 一整份清單。INFO 不上畫面（§6.4：棄用欄位、被忽略的平台專屬欄位）。 */
    fun renderAll(diagnostics: List<Diagnostic>): List<String> =
        diagnostics.filter { it.severity != Severity.INFO }.map { render(it) }
}
