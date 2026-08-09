package org.luminakey.ime.keyboard

import android.util.Log
import org.luminakey.ime.theme.SendSpec
import org.luminakey.ime.theme.Appearance
import org.luminakey.ime.theme.DiagnosticText
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.LoadResult
import org.luminakey.ime.theme.Theme

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
     * docs/schema-store.md §1 與 [org.luminakey.ime.store.InstalledRegistry]）。
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

    /**
     * §9.1.1 的 SHOULD：使用者**明確指定**過的佈局（schema id → layout id）。
     *
     * > 使用者若曾為當前 schema 明確指定過佈局，實作 **應** 記住該選擇並跳過
     * > 第 1 步。自動切換是便利機制，不該覆蓋使用者的明示意圖。
     *
     * 具體的事故長這樣：使用者在鍵盤類型選單裡替朙月拼音挑了九宮格，然後切去
     * 注音打了幾個字，再切回朙月拼音 —— 若沒有這張表，`for_schema` 的精確命中
     * 會把他丟回全鍵盤，而他剛剛才親手選過九宮格。自動規則在這種時候應該閉嘴。
     *
     * 只記「明確選擇」，不記自動切換的結果：後者會讓這張表變成一份無人請求的
     * 快取，使用者永遠停在他第一次碰巧看到的那份佈局上。
     */
    private val pinnedLayouts = LinkedHashMap<String, String>()

    /** 使用者明確選定的主題 id；null = 跟隨系統（§8.2 執行期規則第 2 條）。 */
    private var pinnedThemeId: String? = null

    /* ─────────────────────── 佈局 ─────────────────────── */

    /** 首次啟動：載入主要英數佈局。 */
    fun ensureLoaded() {
        if (layout == null) switchLayout(DEFAULT_LAYOUT)
    }

    /**
     * 丟掉佈局快取並重載當前佈局。
     *
     * 使用者在設定裡改過自訂鍵位（`RemappingLayoutRepository` 的來源變了）之後，
     * 快取裡那幾份是舊的。這裡不動「現在在哪一份、哪一層」——那是使用者的位置，
     * 重載不該把他踢回預設層。
     */
    fun invalidateLayouts() {
        layoutCache.clear()
        val current = layout?.id ?: return
        val keepLayer = layerId
        val fresh = load(current) ?: return
        layout = fresh
        // 舊層在新佈局裡還在就留著；被使用者改掉了才退回 default_layer。
        layerId = if (fresh.layer(keepLayer) != null) keepLayer else fresh.defaultLayer
        layerOnceReturn = null
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
        // 換佈局＝換鍵盤，中英模式跟著回到預設。留著上一份佈局的模式，
        // 會讓新佈局一上來就停在它自己的字母層上，而使用者沒按過任何鍵。
        asciiMode = false
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
     * v1 初稿只寫了「命中就換、沒命中就沿用當前佈局」。照字面實作會有一個洞：
     * 從注音切回拼音時，`bopomofo-dachen` 不適用於 `luna_pinyin`，
     * 而 `qwerty` 的 `auto_for_schema` 是空的（不算命中），
     * 於是使用者會停在注音鍵盤上打拼音。所以第 3 步是**退回 primary**。
     * 規範 §9.1.1 已經補上這一步（第 2、3 步）。
     *
     * 最前面還有一步：使用者明確指定過的佈局勝過整套自動規則（§9.1.1 的
     * SHOULD，見 [pinnedLayouts]）。
     */
    fun applySchema(schemaId: String) {
        if (schemaId.isEmpty()) return
        // 第 0 步：使用者的明示意圖。放在精確命中之前，正是規範要的
        //「記住該選擇並跳過第 1 步」。
        pinnedLayouts[schemaId]?.let { pinned ->
            if (load(pinned) != null) {
                if (pinned != layout?.id) switchLayout(pinned) else resetToDefaultLayer()
                Log.i(TAG, "方案 $schemaId 沿用使用者指定的佈局 $pinned")
                return
            }
            // 佈局被刪掉或改壞了。留著這筆會讓使用者每次切回這個方案都撞一次
            // 載入失敗，不如丟掉、退回自動規則。
            Log.w(TAG, "使用者為 $schemaId 指定的佈局 $pinned 載不起來，改走自動規則")
            pinnedLayouts.remove(schemaId)
        }
        // §9.1.1 第 1 步走的是 `auto_for_schema`，不是 `for_schema`。兩者拆開之後
        // 「可以用這份佈局」與「預設就用這份」不再互相綁架：`cn-t9-pinyin-numrow`
        // 可以老實宣告自己只給 t9_pinyin 用，同時把自動命中讓給 `cn-t9-pinyin`。
        val exact = repo.layoutIds()
            .asSequence()
            .mapNotNull { load(it) }
            .firstOrNull { it.autoMatchesSchema(schemaId) }
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

    /* ────────────────── 使用者明確指定的佈局（§9.1.1 SHOULD）────────────── */

    /**
     * 記下「使用者為這個方案挑了這份佈局」。
     *
     * `layoutId` 傳 `null` = 忘掉這筆，回到自動規則。這條路必須留著：
     * 一個記得住但忘不掉的偏好就是另一種「進得去出不來」。
     */
    fun pinLayout(schemaId: String, layoutId: String?) {
        if (schemaId.isEmpty()) return
        if (layoutId == null) pinnedLayouts.remove(schemaId) else pinnedLayouts[schemaId] = layoutId
    }

    fun pinnedLayoutFor(schemaId: String): String? = pinnedLayouts[schemaId]

    /** 供偏好層持久化用的快照。 */
    fun pinnedLayouts(): Map<String, String> = LinkedHashMap(pinnedLayouts)

    /** 由偏好層在啟動時（與偏好變更時）灌回來。 */
    fun setPinnedLayouts(pins: Map<String, String>) {
        pinnedLayouts.clear()
        pinnedLayouts.putAll(pins)
    }

    /**
     * 目前搜尋路徑上所有佈局的摘要，供鍵盤類型選單使用（見 [KeyboardTypes]）。
     *
     * 載不起來的佈局直接不列：選單裡放一個點下去會壞掉的項目比沒有更糟。
     */
    fun layoutBriefs(locale: String): List<LayoutBrief> =
        repo.layoutIds().mapNotNull { id ->
            load(id)?.let {
                LayoutBrief(
                    id = it.id,
                    name = it.name.get(locale),
                    kind = it.kind,
                    forSchema = it.forSchema,
                    autoForSchema = it.autoForSchema,
                    letters = lettersOf(it),
                    deprecated = it.deprecated,
                    primary = it.primary,
                )
            }
        }

    /**
     * 這份佈局的鍵實際送出哪些字母（小寫化）。
     *
     * 只看 `send: keysym`（走引擎的那一種）。`send: text` 繞過引擎直接上屏，
     * 它打得出什麼與方案的 alphabet 無關，算進來會把判斷弄糊。
     */
    private fun lettersOf(layout: KeyboardLayout): Set<Char> {
        // ⚠ **只看預設層。** 九宮格佈局自己也有一個英數層(a-z),把它算進來會讓
        // 聯集變成整個字母表,於是 QWERTY 又通過了 —— 實測就是這樣漏掉的:
        // 選單照樣在 t9_pinyin 底下提供 QWERTY。使用者打字時待的是預設層,
        // 那一層送什麼才是「這個方案吃得下什麼」的證據。
        val out = HashSet<Char>()
        val layers = layout.layers.filter { it.id == layout.defaultLayer }
            .ifEmpty { layout.layers.take(1) }
        for (layer in layers) {
            for (row in layer.rows) {
                for (key in row.keys) {
                    val send = key.send as? SendSpec.Keysym ?: continue
                    val name = send.name
                    if (name.length == 1 && name[0].isLetter()) out.add(name[0].lowercaseChar())
                }
            }
        }
        return out
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
            problem = "佈局 $id 載入失敗: ${r.errors.firstOrNull()?.let(DiagnosticText::render) ?: "未知原因"}"
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

    /* ─────────────────── 中英模式與字母層 ─────────────────── */

    /**
     * 中英模式。真正的權威是 librime 的 `ascii_mode` 開關 —— 這裡存一份，
     * 是因為**佈局層**也要跟著動，而 LayoutHost 才知道本佈局有沒有字母層。
     *
     * 寫入端：IME 收到 `input_mode:toggle` 之後回寫；以及方案／佈局切換時歸零。
     */
    var asciiMode: Boolean = false
        private set

    /**
     * 「切中英」的完整語義（見 [ActionVerb.INPUT_MODE_TOGGLE]）。
     *
     * 宣告了 `alpha_layer` 的佈局（九宮格、筆畫、注音大千）：
     * 進英文 → 跳到字母層；回中文 → 回 `default_layer`。
     * 沒宣告的（qwerty、intl-*）：佈局原地不動，只有模式變。
     *
     * ⚠ 回程是**結構性**存在的：字母層是本佈局的一層，而回中文永遠落在
     * `default_layer` 上，不需要那一層自己記得放一顆回程鍵。這正是先前
     * 「從九宮格切英語就再也切不回來」的成因（那時走的是
     * `switch_layout:@primary`，跳到 qwerty 就沒有回頭路了）。
     */
    fun setInputMode(ascii: Boolean) {
        asciiMode = ascii
        val l = layout ?: return
        val alpha = l.alphaLayer ?: return
        val want = if (ascii) alpha else l.defaultLayer
        if (l.layer(want) == null) return
        layerId = want
        layerOnceReturn = null
    }

    /** 只給不知道引擎現值的呼叫端用（佈局驗證的走訪）。回傳切換後的模式。 */
    fun toggleInputMode(): Boolean {
        setInputMode(!asciiMode)
        return asciiMode
    }

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
            problem = "主題 $id 載入失敗: ${r.errors.firstOrNull()?.let(DiagnosticText::render) ?: "未知原因"}"
            return null
        }
        if (problems.isNotEmpty()) problem = "主題 $id: ${problems.first()}"
        themeCache[id] = v
        return v
    }
}
