package org.rimequad.ime.keyboard

import org.rimequad.ime.theme.ActionVerb
import org.rimequad.ime.theme.KeyboardLayout
import org.rimequad.ime.theme.LayoutKey
import org.rimequad.ime.theme.LayoutLayer
import org.rimequad.ime.theme.SendSpec

/**
 * 使用者自訂鍵位 —— 引擎層。
 *
 * ── v1 只動「位置」，絕不動「送出什麼」 ──────────────────────────────────
 *
 * 這個區別是本檔存在的全部理由，必須寫在最前面：
 *
 * · **交換位置**：標著 `a` 的鍵搬到 `s` 原本的格子。它仍然送 `a`。安全。
 * · **交換送出內容**：`a` 鍵改成送 `s`。**會壞方案。**
 *
 * 為什麼後者會壞：九宮格那八顆鍵送的 `A/D/G/J/M/P/T/W` 不是隨手挑的字母，
 * 而是 `t9_pinyin` 方案 `speller/alphabet: 'ADGJMPTW'` 的契約（見
 * `core/layouts/t9-pinyin.yaml` 的檔頭）。把「ABC」鍵改成送 `D`，librime 會
 * 老老實實當成 def 去查 prism，使用者打出來的每一個字都是錯的，而且畫面上
 * 完全看不出原因 —— 鍵面還是寫著 ABC。注音大千、倉頡同理：鍵面文字與送出的
 * keysym 是兩件事（規範 §9.4），使用者看得到的只有前者。
 *
 * 因此 [applyKeyRemap] 在套用之後會**逐層比對鍵的完整簽章多重集**
 * （[keySignature]）：套用前後必須是同一批鍵，只有先後順序不同。任何一個
 * `send` 被動過，這個比對就會失敗，整份自訂被拒絕。這不是防呆，是防止我們
 * 自己日後在這裡加一個「順手」的功能。
 *
 * ── 為什麼用 id 定位而不是索引 ──────────────────────────────────────────
 * `core/layouts` 下的 yaml 隨 APK 出貨，也會被主題／佈局市集更新。上游在某一列
 * 中間插一顆鍵，所有索引就位移一格 —— 使用者的「第 2 顆與第 3 顆對調」會
 * 悄悄變成另外兩顆鍵對調，而他從沒改過設定。id 是佈局作者宣告的穩定名字，
 * 上游改版時它要嘛還在、要嘛不在（[applyKeyRemap] 會明確拒絕並說明），
 * 不會靜靜地指到別顆鍵上。
 *
 * ── 為什麼是純函式 ──────────────────────────────────────────────────────
 * 不碰 Android、不碰 Compose、不碰檔案：唯一的輸入是一份已解析的
 * [KeyboardLayout] 與一份 [LayoutRemap]，唯一的輸出是新的 [KeyboardLayout]
 * 或一串拒絕原因。「使用者能不能把自己鎖死」這個問題因此可以在 JVM 單元測試
 * 裡回答（見 KeyRemapTest），不必開模擬器。
 */

/** 一則自訂鍵位操作。全部以鍵的 `id` 定位（見檔頭）。 */
sealed class RemapOp {

    /** 這則操作作用在哪一層。層 id 見佈局檔的 `layers[].id`。 */
    abstract val layerId: String

    /** 給 UI 與診斷訊息用的一行說明。 */
    abstract fun describe(): String

    /**
     * 兩顆鍵互換所在的格子。**寬度不同的鍵交換會破壞「每列寬度總和守恆」**，
     * 由 [applyKeyRemap] 擋下（規範 §9：`Σwidth == units`）。
     */
    data class Swap(
        override val layerId: String,
        val first: String,
        val second: String,
    ) : RemapOp() {
        override fun describe(): String = "$layerId：「$first」⇄「$second」"
    }

    /**
     * 把 [keyId] 移到 [beforeKeyId] 現在所在的格子（[beforeKeyId] 與其後的鍵
     * 往後遞補）。跨列移動幾乎必然破壞寬度守恆，同樣由 [applyKeyRemap] 擋下 ——
     * 這是刻意的：與其發明一套「自動補寬度」的規則（四端各補各的），不如
     * 明確拒絕。
     */
    data class Move(
        override val layerId: String,
        val keyId: String,
        val beforeKeyId: String,
    ) : RemapOp() {
        override fun describe(): String = "$layerId：「$keyId」移到「$beforeKeyId」的位置"
    }
}

/**
 * 一份佈局的使用者自訂。一個 layout id 對應一份，依序套用 [ops]。
 *
 * 存的是**操作**而不是「改完之後的佈局快照」。理由與 [org.rimequad.ime.prefs.UserPrefs]
 * 開頭那一段是同一個：存快照等於把當下的基礎佈局複製一份進使用者資料，
 * 上游更新（多一顆鍵、換一個圖示、修一個 keysym）之後使用者永遠停在舊版上，
 * 而且他無從得知為什麼別人的鍵盤好了他的沒有。存操作則是「相對於當前基礎
 * 佈局的意圖」，基礎佈局更新之後意圖仍然說得通 —— 說不通時（鍵被上游刪了）
 * 會明確拒絕，而不是默默套到別顆鍵上。
 */
data class LayoutRemap(
    val layoutId: String,
    val ops: List<RemapOp>,
) {
    val isEmpty: Boolean get() = ops.isEmpty()

    fun plus(op: RemapOp): LayoutRemap = copy(ops = ops + op)

    companion object {
        fun empty(layoutId: String) = LayoutRemap(layoutId, emptyList())
    }
}

/**
 * 套用結果。[layout] 為 null 代表整份自訂被拒絕，[problems] 是給使用者看的
 * 拒絕原因（每一條都要說明「為什麼不行」，不能只說「無效」）。
 *
 * **沒有「部分套用」。** 一半生效的自訂是最糟的結果：使用者看到鍵盤變了、
 * 但變得跟他要的不一樣，而且沒有任何訊息。要嘛整份成立，要嘛整份退回原樣。
 */
class RemapOutcome(val layout: KeyboardLayout?, val problems: List<String>) {
    val isSuccess: Boolean get() = layout != null
}

/** 每列寬度總和與 `units` 的容許誤差。與 [org.rimequad.ime.theme.LayoutParser] 一致。 */
private const val WIDTH_EPSILON = 0.01f

private val NAV_VERBS = setOf(
    ActionVerb.LAYER, ActionVerb.LAYER_ONCE, ActionVerb.LAYER_LOCK, ActionVerb.SWITCH_LAYOUT,
)

/**
 * 必要鍵的類別。判定依**行為**而非 id —— 佈局作者可以把退格鍵叫做任何名字，
 * 但它送的一定是 `BackSpace`。用 id 判定會在第一份第三方佈局上失效。
 */
enum class EssentialKey(val zh: String) {
    BACKSPACE("退格"),
    SPACE("空白"),
    ENTER("換行"),
    LAYER_SWITCH("層切換"),
}

/**
 * 把使用者自訂套到一份已解析的佈局上。**純函式。**
 *
 * 套用順序是「基礎佈局 YAML → inherits → key_patches → 本函式」：本函式吃的
 * 已經是規範 §7.4 全部走完的結果，所以它看得到的就是使用者眼睛看得到的那份
 * 鍵盤。刻意**不**寫回 `core/layouts/` —— 那些檔案隨 APK 散佈、會被升級覆蓋，
 * 寫回去等於下一次更新就把使用者的設定洗掉。
 */
fun applyKeyRemap(layout: KeyboardLayout, remap: LayoutRemap): RemapOutcome {
    if (remap.layoutId != layout.id) {
        return RemapOutcome(
            null,
            listOf("這份自訂鍵位是給佈局「${remap.layoutId}」的，不能套到「${layout.id}」上"),
        )
    }
    if (remap.isEmpty) return RemapOutcome(layout, emptyList())

    val problems = ArrayList<String>()
    var out = layout
    for (op in remap.ops) {
        out = applyOne(out, op, problems) ?: return RemapOutcome(null, problems)
    }

    problems += invariantProblems(layout, out)
    return if (problems.isEmpty()) RemapOutcome(out, emptyList()) else RemapOutcome(null, problems)
}

/* ══════════════════════════ 單一操作 ══════════════════════════ */

private data class Slot(val row: Int, val index: Int)

private fun applyOne(
    layout: KeyboardLayout,
    op: RemapOp,
    problems: MutableList<String>,
): KeyboardLayout? {
    val layerIndex = layout.layers.indexOfFirst { it.id == op.layerId }
    if (layerIndex < 0) {
        problems += "佈局「${layout.id}」沒有「${op.layerId}」這一層 —— " +
            "上游佈局可能改版了，請重置這份自訂"
        return null
    }
    val layer = layout.layers[layerIndex]
    val next = when (op) {
        is RemapOp.Swap -> swap(layout.id, layer, op, problems)
        is RemapOp.Move -> move(layout.id, layer, op, problems)
    } ?: return null

    val layers = layout.layers.toMutableList()
    layers[layerIndex] = next
    return layout.copy(layers = layers)
}

private fun swap(
    layoutId: String,
    layer: LayoutLayer,
    op: RemapOp.Swap,
    problems: MutableList<String>,
): LayoutLayer? {
    if (op.first == op.second) {
        problems += "「${op.first}」不能和自己交換"
        return null
    }
    val a = locate(layoutId, layer, op.first, problems) ?: return null
    val b = locate(layoutId, layer, op.second, problems) ?: return null
    val grid = layer.rows.map { it.keys.toMutableList() }
    val ka = grid[a.row][a.index]
    val kb = grid[b.row][b.index]
    grid[a.row][a.index] = kb
    grid[b.row][b.index] = ka
    return rebuild(layer, grid)
}

private fun move(
    layoutId: String,
    layer: LayoutLayer,
    op: RemapOp.Move,
    problems: MutableList<String>,
): LayoutLayer? {
    if (op.keyId == op.beforeKeyId) {
        problems += "「${op.keyId}」不能移到自己的位置"
        return null
    }
    val from = locate(layoutId, layer, op.keyId, problems) ?: return null
    locate(layoutId, layer, op.beforeKeyId, problems) ?: return null

    val grid = layer.rows.map { it.keys.toMutableList() }
    val moved = grid[from.row].removeAt(from.index)
    // 目標索引必須在移除之後重算：同一列內往後搬時，移除會讓目標往前挪一格。
    val destRow = grid.indexOfFirst { row -> row.any { it.id == op.beforeKeyId } }
    val destIndex = if (destRow < 0) -1 else grid[destRow].indexOfFirst { it.id == op.beforeKeyId }
    if (destRow < 0 || destIndex < 0) {
        problems += "$layoutId/${layer.id}：找不到「${op.beforeKeyId}」的位置"
        return null
    }
    grid[destRow].add(destIndex, moved)
    if (grid.any { it.isEmpty() }) {
        problems += "$layoutId/${layer.id}：移動之後有一列變成空的，鍵盤會畫不出來"
        return null
    }
    return rebuild(layer, grid)
}

private fun rebuild(layer: LayoutLayer, grid: List<List<LayoutKey>>): LayoutLayer =
    layer.copy(rows = layer.rows.mapIndexed { i, r -> r.copy(keys = grid[i].toList()) })

/**
 * 以 id 找出唯一的一顆鍵。
 *
 * 找不到、或找到不只一顆，都是**拒絕**而不是猜。同一層裡出現兩顆同 id 的鍵是
 * 佈局作者的疏漏，這時「用 id 定位」這個前提本身不成立，繼續下去就會變成
 * 「有時候換對、有時候換錯」。
 */
private fun locate(
    layoutId: String,
    layer: LayoutLayer,
    keyId: String,
    problems: MutableList<String>,
): Slot? {
    val hits = ArrayList<Slot>(2)
    layer.rows.forEachIndexed { r, row ->
        row.keys.forEachIndexed { i, k -> if (k.id == keyId) hits += Slot(r, i) }
    }
    return when (hits.size) {
        0 -> {
            problems += "$layoutId/${layer.id} 裡沒有 id 為「$keyId」的鍵 —— " +
                "上游佈局可能改版了，請重置這份自訂"
            null
        }

        1 -> hits[0]

        else -> {
            problems += "$layoutId/${layer.id} 裡有 ${hits.size} 顆 id 都是「$keyId」的鍵，" +
                "無法用 id 定位；這是佈局檔的缺陷"
            null
        }
    }
}

/* ══════════════════════════ 不變式 ══════════════════════════ */

/**
 * 套用後必須仍然成立的三條結構不變式。
 *
 * 「逃生不變式」（每個到得了的層都回得了起點）**不在這裡** —— 它需要整份
 * 搜尋路徑上的佈局才走得完，見 [LayoutEscape] 與 [LayoutRemapValidator]。
 * 分開是刻意的：本函式是純的、只看一份佈局；那一條要有 repository。
 */
internal fun invariantProblems(base: KeyboardLayout, out: KeyboardLayout): List<String> {
    val problems = ArrayList<String>()
    problems += widthProblems(out)
    problems += essentialProblems(base, out)
    problems += payloadProblems(base, out)
    return problems
}

/**
 * 規範 §9：`Σwidth == units`。
 *
 * 這是最容易被使用者觸發的一條：把 `a`（寬 1.0）跟 `shift`（寬 1.5）對調，
 * 兩列的總和立刻一個變 10.5 一個變 9.5。不擋的話鍵盤畫出來會歪掉一整列，
 * 而且是**每一次**開鍵盤都歪。
 */
private fun widthProblems(out: KeyboardLayout): List<String> {
    val problems = ArrayList<String>()
    for (layer in out.layers) {
        for ((i, row) in layer.rows.withIndex()) {
            val sum = row.widthSum
            if (kotlin.math.abs(sum - layer.units) > WIDTH_EPSILON) {
                problems += "${out.id}/${layer.id} 第 ${i + 1} 列的寬度總和變成 " +
                    "${fmt(sum)}，應為 ${fmt(layer.units)} —— " +
                    "交換寬度不同的鍵會讓這一列排不下（或排不滿）"
            }
        }
    }
    return problems
}

/**
 * 退格、空白、換行、層切換不可消失。
 *
 * v1 只有交換與移動，理論上刪不掉鍵，所以這一條現在**永遠通過**。留著是因為
 * 它守的是「使用者不能把自己鎖死」這條產品承諾，而不是某一個實作細節：
 * 日後只要有人加一個「隱藏這顆鍵」的操作，這一條會在第一次跑測試時就擋下來。
 */
private fun essentialProblems(base: KeyboardLayout, out: KeyboardLayout): List<String> {
    val problems = ArrayList<String>()
    for (layer in base.layers) {
        val after = out.layer(layer.id)
        if (after == null) {
            problems += "${out.id}：「${layer.id}」這一層不見了"
            continue
        }
        val before = essentialCounts(layer)
        val now = essentialCounts(after)
        for ((kind, n) in before) {
            val m = now[kind] ?: 0
            if (m < n) {
                problems += "${out.id}/${layer.id} 的${kind.zh}鍵從 $n 顆變成 $m 顆 —— " +
                    "必要鍵不可移除，否則使用者會被鎖在這一層裡"
            }
        }
    }
    return problems
}

private fun essentialCounts(layer: LayoutLayer): Map<EssentialKey, Int> {
    val counts = HashMap<EssentialKey, Int>()
    for (row in layer.rows) {
        for (k in row.keys) {
            val kind = essentialOf(k) ?: continue
            counts[kind] = (counts[kind] ?: 0) + 1
        }
    }
    return counts
}

internal fun essentialOf(k: LayoutKey): EssentialKey? {
    val tap = k.tap
    if (tap != null && NAV_VERBS.contains(tap.verb)) return EssentialKey.LAYER_SWITCH
    val send = k.send as? SendSpec.Keysym ?: return null
    return when (send.name) {
        "BackSpace" -> EssentialKey.BACKSPACE
        "space" -> EssentialKey.SPACE
        "Return", "KP_Enter" -> EssentialKey.ENTER
        else -> null
    }
}

/**
 * **v1 的硬限制：送出內容一個位元都不准動。**
 *
 * 作法是逐層比對「鍵的完整簽章多重集」。套用前後若是同一批鍵、只有順序不同，
 * 排序後的簽章清單必然逐項相等。任何 `send` / `tap` / 標籤 / 寬度被改動，
 * 這裡就會不相等。
 *
 * 為什麼連 label 與 width 都納入比對，而不是只比 `send`：因為只比 `send` 的話，
 * 一個「順手」把標籤也跟著換過去的實作（讓 a 鍵看起來像 s）會通過檢查，
 * 而那正是使用者眼中「送出內容被改了」的樣子 —— 鍵面寫 s、送出 a，比純粹
 * 改送出內容更難察覺。整批比對讓這件事不可能悄悄發生。
 */
private fun payloadProblems(base: KeyboardLayout, out: KeyboardLayout): List<String> {
    val problems = ArrayList<String>()
    for (layer in base.layers) {
        val after = out.layer(layer.id) ?: continue     // 已由 essentialProblems 報過
        val before = layer.rows.flatMap { it.keys }.map(::keySignature).sorted()
        val now = after.rows.flatMap { it.keys }.map(::keySignature).sorted()
        if (before != now) {
            problems += "${out.id}/${layer.id}：套用後的鍵不是原本那一批 —— " +
                "自訂鍵位只能改變位置，不得改變任何一顆鍵送出什麼"
            // 找出第一個差異，讓訊息可行動而不只是「不對」。
            val gone = before.filterNot { now.contains(it) }.firstOrNull()
            if (gone != null) problems += "  少了：${describeSignature(gone)}"
            val added = now.filterNot { before.contains(it) }.firstOrNull()
            if (added != null) problems += "  多了：${describeSignature(added)}"
        }
    }
    return problems
}

/**
 * 一顆鍵除了「在哪個格子」以外的全部內容。
 *
 * 用 `toString()` 序列化 [org.rimequad.ime.theme.Popup] 與 swipe：那些型別都是
 * data class，`toString` 是結構性的，改欄位會自動反映到簽章裡。若日後有人把
 * 它們改成非 data class，這裡會靜靜失效 —— KeyRemapTest 有一條測試專門釘住
 * 「動了 send 就必須被擋下」，那條會先紅。
 */
internal fun keySignature(k: LayoutKey): String = listOf(
    k.id ?: "",
    k.label,
    k.hint,
    k.icon ?: "",
    k.labelFrom.name,
    k.width.toString(),
    k.style,
    k.spacer.toString(),
    k.active.toString(),
    k.repeat.toString(),
    sendSignature(k.send),
    k.tap?.raw ?: "",
    k.doubleTap?.raw ?: "",
    k.longPress?.raw ?: "",
    k.popup?.toString() ?: "",
    k.swipe.toString(),
).joinToString(SIG_SEP)

/** 佈局檔的任何欄位都不會含 U+0001，所以簽章拆得回來。 */
private const val SIG_SEP = "\u0001"

private fun sendSignature(s: SendSpec?): String = when (s) {
    null -> "-"
    is SendSpec.Keysym -> "keysym:${s.name}/${s.code}/${s.modifiers}"
    is SendSpec.Text -> "text:${s.text}"
}

/** 把簽章還原成人看得懂的一行（`id`「label」→ send）。 */
private fun describeSignature(sig: String): String {
    val parts = sig.split(SIG_SEP)
    val id = parts.getOrNull(0).orEmpty()
    val label = parts.getOrNull(1).orEmpty()
    val send = parts.getOrNull(10).orEmpty()
    return "$id「$label」→ $send"
}

private fun fmt(v: Float): String =
    if (v == v.toInt().toFloat()) v.toInt().toString() else String.format("%.2f", v)
