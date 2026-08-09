package org.luminakey.ime.keyboard

import android.util.Log
import org.luminakey.ime.theme.Diagnostic
import org.luminakey.ime.theme.DiagnosticCode
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.LoadResult
import org.luminakey.ime.theme.Theme

/**
 * 「使用者自訂鍵位」接進正式路徑的接縫。
 *
 * ── 為什麼是 repository 裝飾器，而不是塞進 LayoutHost ────────────────────
 * [LayoutHost] 是「現在該畫哪一份佈局、哪一層」的狀態機，四端要照抄它的行為。
 * 自訂鍵位不改變任何一條切換規則，只改變「同一個 id 載進來的那份佈局長什麼
 * 樣」——那正好是 [LayoutRepository] 的職責。裝在這一層有兩個好處：
 *
 *  1. 狀態機一行都不用改，iOS 端照抄時不必連自訂功能一起抄。
 *  2. **驗證與正式路徑走的是同一個裝飾器。** [LayoutRemapValidator] 檢查
 *     逃生不變式時，餵給 [LayoutEscape] 的是同一個 `RemappingLayoutRepository`
 *     包出來的 repo，所以「驗證時通過、實際用起來壞掉」在結構上不可能發生。
 */
class RemappingLayoutRepository(
    private val base: LayoutRepository,
    /**
     * `true` = 自訂套不上時**回報失敗**（驗證用）；
     * `false` = 自訂套不上時**退回基礎佈局**並附一則 WARNING（正式路徑用）。
     *
     * 正式路徑必須退回而不是失敗：上游佈局改版把某顆鍵刪掉之後，使用者的舊
     * 自訂會套不上，這時該給他一個能用的鍵盤加一則說明，而不是一片空白。
     */
    private val strict: Boolean = false,
    private val remapFor: (layoutId: String) -> LayoutRemap?,
) : LayoutRepository {

    override fun layoutIds(): List<String> = base.layoutIds()

    override fun loadTheme(id: String): LoadResult<Theme> = base.loadTheme(id)

    override fun builtinFallbackTheme(): Theme = base.builtinFallbackTheme()

    override fun loadLayout(id: String): LoadResult<KeyboardLayout> {
        val loaded = base.loadLayout(id)
        val value = loaded.value ?: return loaded
        val remap = remapFor(id)?.takeIf { !it.isEmpty } ?: return loaded

        val outcome = applyKeyRemap(value, remap)
        if (outcome.layout != null) return LoadResult(outcome.layout, loaded.diagnostics)

        // ⚠ 嚴重度**不再**由 strict 決定。規範 §6.5 規定 severity 是 code 上的
        // 函式：同一件事在驗證路徑記成 ERROR、在正式路徑記成 WARNING，正是
        // 「四端報一樣多則」失守的那個形狀，只是這裡失守在同一端的兩條路徑上。
        // strict 要表達的是「這份佈局算不算載得起來」，那是 value 的事
        // （下一行的 `if (strict) null else value`），不是診斷等級的事。
        //
        // path 帶序號：多個自訂各是一則診斷，共用一個 path 會讓它們的
        // (severity, code, path) 撞在一起。
        val extra = outcome.problems.mapIndexed { i, problem ->
            Diagnostic(
                DiagnosticCode.USER_REMAP_UNAPPLICABLE,
                listOf(problem),
                "user_key_remap[$i]",
            )
        }
        return LoadResult(if (strict) null else value, loaded.diagnostics + extra)
    }
}

/**
 * 只解析一次的裝飾器。
 *
 * [LayoutEscape] 的走訪是指數級的重放，每一步都會 `loadLayout`。少了這一層，
 * 一次驗證會把同一份 yaml 解析上百次 —— 實測讓「按下交換」到「看到結果」
 * 從幾十毫秒變成好幾秒，使用者會以為 app 當掉了。
 *
 * 刻意**不**做成永久快取：每次驗證新建一個，用完即丟，不會有「改了設定但
 * 快取還是舊的」這種問題。
 */
class CachingLayoutRepository(private val base: LayoutRepository) : LayoutRepository {

    private val layouts = HashMap<String, LoadResult<KeyboardLayout>>()
    private val themes = HashMap<String, LoadResult<Theme>>()
    private val ids: List<String> by lazy { base.layoutIds() }

    override fun layoutIds(): List<String> = ids

    override fun loadLayout(id: String): LoadResult<KeyboardLayout> =
        layouts.getOrPut(id) { base.loadLayout(id) }

    override fun loadTheme(id: String): LoadResult<Theme> =
        themes.getOrPut(id) { base.loadTheme(id) }

    override fun builtinFallbackTheme(): Theme = base.builtinFallbackTheme()
}

/**
 * 存檔前的完整驗證。**任何一條不過就拒絕，並說明原因。**
 *
 * 涵蓋的不變式：
 *
 *  1. 操作解析得開（鍵存在、唯一、不是自己換自己）  —— [applyKeyRemap]
 *  2. 每列寬度總和守恆（規範 §9 的 `Σwidth == units`）—— [applyKeyRemap]
 *  3. 必要鍵（退格／空白／換行／層切換）不減少        —— [applyKeyRemap]
 *  4. 送出內容一個位元都沒動                          —— [applyKeyRemap]
 *  5. **逃生不變式**：每個到得了的層都回得了起點      —— [LayoutEscape]
 *  6. `layer:` / `switch_layout:` 指向的目標存在      —— [LayoutEscape]
 *
 * 第 5、6 兩條走的是 LayoutEscapeTest 用的同一份程式碼，不是另寫一份。
 */
object LayoutRemapValidator {

    private const val TAG = "LayoutRemapValidator"

    fun validate(base: LayoutRepository, remap: LayoutRemap): List<String> {
        val cached = CachingLayoutRepository(base)
        val loaded = cached.loadLayout(remap.layoutId).value
            ?: return listOf(
                "佈局「${remap.layoutId}」本身就載不起來，沒有可以自訂的基礎"
            )

        val outcome = applyKeyRemap(loaded, remap)
        val applied = outcome.layout ?: return outcome.problems

        val remapped = RemappingLayoutRepository(cached, strict = true) { id ->
            if (id == remap.layoutId) remap else null
        }

        // ── 為什麼是「差集」而不是「套用後有沒有問題」───────────────────────
        //
        // 第一版是後者，然後在模擬器上立刻踩到：使用者在 qwerty 交換 a 與 s，
        // 卻被一串「cn-stroke 走 switch_layout:@primary 之後回不來」擋下。那些
        // 死路是**隨附佈局本來就有的**（見交付報告：cn-stroke / cn-symbols /
        // cn-t9-pinyin* 都有，而 LayoutEscapeTest 的夾具只列了四份佈局，從來
        // 沒走到它們）。
        //
        // 拿別人的缺陷去擋使用者的自訂有兩個問題：他看不懂那段訊息跟他按的
        // 兩顆鍵有什麼關係，而且**他也修不了** —— 那是隨 APK 出貨的 yaml。
        // 結果就是一個永遠按不下去的按鈕。
        //
        // 正確的判準是「你的自訂有沒有讓情況變糟」：同一條走訪跑兩次，
        // 一次不套自訂、一次套，只回報新增的那些。既守住了不變式
        // （自訂不得製造任何新的死路），又不把別人的 bug 變成他的懲罰。
        val before = escapeProblems(cached) { LayoutHost(cached) } +
            LayoutEscape.checkLayerReferences(loaded, cached.layoutIds())
        val after = escapeProblems(cached) { LayoutHost(remapped) } +
            LayoutEscape.checkLayerReferences(applied, cached.layoutIds())

        val preexisting = before.toSet()
        if (preexisting.isNotEmpty()) {
            // 不擋使用者，但**不可以靜靜吞掉** —— 這些是隨附佈局的真缺陷。
            Log.w(TAG, "隨附佈局本來就存在的逃生缺陷 ${preexisting.size} 則：" +
                preexisting.joinToString(" | "))
        }
        return after.filterNot { preexisting.contains(it) }.distinct()
    }

    /**
     * 從**每一份**佈局出發走一次，而不是只走被改的那一份：`switch_layout`
     * 是跨佈局的，別人切進來之後出不去一樣是把使用者鎖死。
     */
    private fun escapeProblems(
        repo: LayoutRepository,
        newHost: () -> LayoutHost,
    ): List<String> = repo.layoutIds().flatMap { LayoutEscape.check(it, newHost) }
}
