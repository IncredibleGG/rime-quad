package org.rimequad.ime.keyboard

import android.util.Log
import org.rimequad.ime.theme.Appearance
import org.rimequad.ime.theme.KeyboardLayout
import org.rimequad.ime.theme.LoadResult
import org.rimequad.ime.theme.Theme

/**
 * [LayoutHost] 對「佈局／主題從哪來」的唯一需求。
 *
 * 抽出這層介面**不是**為了多態 —— 正式路徑上永遠只有 [ConfigRepository] 一個
 * 實作。是為了讓狀態機能在 JVM 單元測試裡跑：ConfigRepository 建構時就要
 * `Context.getAssets()`，綁著它等於把 layer/layout 的切換規則鎖在儀器測試裡，
 * 而「切出去回不來」這種缺陷正是純邏輯測試最該擋下的一類。
 */
interface LayoutRepository {
    fun layoutIds(): List<String>
    fun loadLayout(id: String): LoadResult<KeyboardLayout>
    fun loadTheme(id: String): LoadResult<Theme>
    fun builtinFallbackTheme(): Theme
}

/**
 * 「現在該畫哪一份佈局、哪一層、哪一個主題」的狀態機。
 *
 * 刻意與 Compose 無關、與 InputMethodService 無關：它只吃 id 與方案 id，
 * 吐出已解析好的 [KeyboardLayout] / [Theme]。四端的這段邏輯是同一套規則
 * （規範 §9.1.1、§9.5 的 layer/switch_layout、§8.2 的深淺切換），
 * 之後 iOS 端可以照抄這個狀態機的行為。
 */
class LayoutHost(private val repo: LayoutRepository) {

    private companion object {
        const val TAG = "LayoutHost"
        const val DEFAULT_LAYOUT = "qwerty"
        const val DEFAULT_LIGHT_THEME = "default-light"
        const val DEFAULT_DARK_THEME = "default-dark"
    }

    private val layoutCache = HashMap<String, KeyboardLayout>()
    private val themeCache = HashMap<String, Theme>()

    var layout: KeyboardLayout? = null
        private set

    var layerId: String = ""
        private set

    var theme: Theme? = null
        private set

    /** 最近一次載入產生的問題（ERROR 或 WARNING），供 UI 顯示。null = 沒問題。 */
    var problem: String? = null
        private set

    /** `switch_layout:@previous` 用。 */
    private var previousLayoutId: String? = null

    /**
     * 方案市集提供的 `recommended_layout` 查詢（索引欄位，見
     * docs/schema-store.md §1 與 [org.rimequad.ime.store.InstalledRegistry]）。
     *
     * 為什麼需要它：新導入的方案不可能出現在**隨附**佈局的 `for_schema` 裡
     * —— 那些 yaml 是跟著 APK 出貨的，不會知道未來會裝什麼。少了這條線，
     * 使用者啟用注音類方案卻看到 QWERTY 鍵面，鍵盤上根本沒有ㄅㄆㄇ，
     * 一個字也打不出來。
     *
     * 優先序刻意排在 `for_schema` 精確命中**之後**：使用者自己放在
     * user_data_dir 的佈局檔是本機的、明確的意圖，不該被遠端索引蓋過。
     */
    var recommendedLayoutResolver: ((schemaId: String) -> String?)? = null

    /** `layer_once:<id>` 送出一個鍵之後要回到的層；null 代表沒有待回彈的層。 */
    private var layerOnceReturn: String? = null

    /** 使用者明確選定的主題 id；null = 跟隨系統（§8.2 執行期規則第 2 條）。 */
    private var pinnedThemeId: String? = null

    /* ─────────────────────── 佈局 ─────────────────────── */

    /** 首次啟動：載入主要英數佈局。 */
    fun ensureLoaded() {
        if (layout == null) switchLayout(DEFAULT_LAYOUT)
    }

    fun switchLayout(id: String): Boolean {
        val resolved = resolveLayoutRef(id)
        if (resolved == layout?.id) {
            // 已經在目標佈局上。仍要把層歸位到 default_layer：使用者按的是
            // 「回到英數／回到原鍵盤」，停在符號層上不算回到。
            layout?.let { layerId = it.defaultLayer }
            layerOnceReturn = null
            return true
        }
        val next = load(resolved) ?: return false
        previousLayoutId = layout?.id
        layout = next
        layerId = next.defaultLayer
        layerOnceReturn = null
        Log.i(TAG, "佈局 → ${next.id}（層 $layerId）")
        return true
    }

    /**
     * §9.5 的 `@primary` / `@previous`。
     *
     * `@previous` 的用途永遠是**離開目前這份佈局**（符號層的 ABC 鍵、
     * 九宮格的返回鍵）。因此「指不回去」的兩種情況 —— 沒有上一份、或上一份
     * 記成了自己 —— 都必須退到 primary，**不得**解析成當前佈局：那會讓
     * 這顆鍵變成 no-op，使用者按了沒反應，就是「進得去出不來」。
     */
    private fun resolveLayoutRef(id: String): String = when (id) {
        "@primary" -> primaryLayoutId()
        "@previous" -> previousLayoutId?.takeIf { it != layout?.id } ?: primaryLayoutId()
        else -> id
    }

    /**
     * §9.1.1：方案改變時自動換佈局。
     *
     * 規範只寫了「命中就換、沒命中就沿用當前佈局」。照字面實作會有一個洞：
     * 從注音切回拼音時，`bopomofo-dachen` 的 `for_schema` 不含 `luna_pinyin`，
     * 而 `qwerty` 是 `"*"`（不算命中），於是使用者會停在注音鍵盤上打拼音。
     * 這裡多一條補救：**當前佈局若不適用於新方案，退回 primary**。
     * 這是規範的缺口，已記在回報中。
     */
    fun applySchema(schemaId: String) {
        if (schemaId.isEmpty()) return
        val exact = repo.layoutIds()
            .asSequence()
            .mapNotNull { load(it) }
            .firstOrNull { !it.forSchema.contains("*") && it.forSchema.contains(schemaId) }
        if (exact != null) {
            // 已經在對的佈局上時仍要把層歸位。使用者明確重選一次方案，語義是
            // 「把鍵盤給我調回這個方案該有的樣子」，停在英數層上不算調回。
            if (exact.id != layout?.id) switchLayout(exact.id) else resetToDefaultLayer()
            return
        }
        // 索引宣告的 recommended_layout。只有在該佈局真的載得起來時才採用，
        // 否則索引寫錯一個 id 就會讓鍵盤消失。
        recommendedLayoutResolver?.invoke(schemaId)?.let { wanted ->
            if (wanted == layout?.id) return
            if (load(wanted) != null) {
                Log.i(TAG, "方案 $schemaId 依索引的 recommended_layout 切到 $wanted")
                switchLayout(wanted)
                return
            }
            Log.w(TAG, "索引建議的佈局 $wanted 載不起來，改用一般規則")
        }
        val current = layout
        if (current != null && current.matchesSchema(schemaId)) return
        switchLayout("@primary")
    }

    private fun primaryLayoutId(): String {
        repo.layoutIds().forEach { id -> if (load(id)?.primary == true) return id }
        return DEFAULT_LAYOUT
    }

    private fun load(id: String): KeyboardLayout? {
        layoutCache[id]?.let { return it }
        val r = repo.loadLayout(id)
        val problems = ConfigRepository.describe(r.diagnostics)
        if (problems.isNotEmpty()) {
            Log.w(TAG, "佈局 $id 的診斷: ${problems.joinToString(" | ")}")
        }
        val v = r.value
        if (v == null) {
            // §6.1：致命錯誤時退回上一個成功的佈局，不得顯示空白鍵盤。
            problem = "佈局 $id 載入失敗: ${r.errors.firstOrNull()?.message ?: "未知原因"}"
            return null
        }
        if (problems.isNotEmpty()) problem = "佈局 $id: ${problems.first()}"
        layoutCache[id] = v
        return v
    }

    /* ─────────────────────── 層 ─────────────────────── */

    fun setLayer(id: String) {
        if (layout?.layer(id) == null) return
        layerId = id
        layerOnceReturn = null
    }

    /** 回到當前佈局的 `default_layer`。方案重選、佈局「已經在目標上」時的歸位。 */
    fun resetToDefaultLayer() {
        val l = layout ?: return
        layerId = l.defaultLayer
        layerOnceReturn = null
    }

    /** `layer_once`：切過去，送出一個鍵之後自動回來。 */
    fun setLayerOnce(id: String) {
        if (layout?.layer(id) == null) return
        val from = layerId
        layerId = id
        layerOnceReturn = from
    }

    /** `layer_lock`：切過去並停留（渲染為 active 由佈局的 `active: true` 負責）。 */
    fun lockLayer(id: String) = setLayer(id)

    /** 每送出一個鍵之後呼叫；負責 `layer_once` 的回彈。 */
    fun afterKeySent() {
        val back = layerOnceReturn ?: return
        layerOnceReturn = null
        if (layout?.layer(back) != null) layerId = back
    }

    /* ─────────────────────── 主題 ─────────────────────── */

    /**
     * §8.2 的執行期外觀選擇：
     *   1. 使用者明確指定 → 直接用，不做深淺切換
     *   2. 跟隨系統 → 取當前主題的 appearance；不符且 counterpart 可載入 → 換過去
     *   3. counterpart 載入失敗 → 沿用當前主題（**不是**致命錯誤）
     */
    fun applyAppearance(systemDark: Boolean) {
        val pinned = pinnedThemeId
        if (pinned != null) {
            theme = loadTheme(pinned) ?: theme ?: repo.builtinFallbackTheme()
            return
        }
        val want = if (systemDark) Appearance.DARK else Appearance.LIGHT
        val current = theme ?: loadTheme(if (systemDark) DEFAULT_DARK_THEME else DEFAULT_LIGHT_THEME)
        if (current == null) {
            theme = repo.builtinFallbackTheme()
            return
        }
        if (current.appearance == want) {
            theme = current
            return
        }
        val counterpart = current.counterpart?.let { loadTheme(it) }
        theme = counterpart ?: current
        if (counterpart == null && current.counterpart != null) {
            problem = "主題 ${current.counterpart} 載入失敗，沿用 ${current.id}"
        }
    }

    fun pinTheme(id: String?) {
        pinnedThemeId = id
    }

    private fun loadTheme(id: String): Theme? {
        themeCache[id]?.let { return it }
        val r = repo.loadTheme(id)
        val problems = ConfigRepository.describe(r.diagnostics)
        if (problems.isNotEmpty()) Log.w(TAG, "主題 $id 的診斷: ${problems.joinToString(" | ")}")
        val v = r.value ?: run {
            problem = "主題 $id 載入失敗: ${r.errors.firstOrNull()?.message ?: "未知原因"}"
            return null
        }
        if (problems.isNotEmpty()) problem = "主題 $id: ${problems.first()}"
        themeCache[id] = v
        return v
    }
}
