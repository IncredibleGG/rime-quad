package org.luminakey.ime.keyboard

import org.luminakey.ime.core.RimeCandidate
import org.luminakey.ime.theme.ActionVerb
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.LayoutKey

/**
 * 九宮格的**拼音消歧欄** —— 純邏輯那一半。
 *
 * ── 這是什麼、為什麼非有不可 ────────────────────────────────────────────
 * 九宮格一顆鍵代表三到四個字母。打「你好」是 `M G ＋ G A M` 六下，而 `MG`
 * 這兩下同時可能是 mi、ni、o…… **不消歧就只能靠猜**。三星與語燕都在左側
 * 直欄把「這串按鍵可能是哪些音節」列出來給你點（見
 * `docs/competitive-review.md` §3.3 與 P1-1），這是九宮格能不能用的關鍵，
 * 不是裝飾。
 *
 * ── 音節從哪裡來：librime 的 spelling hints ──────────────────────────────
 * `t9_pinyin.schema.yaml` 的 `translator/spelling_hints: 5` 讓 librime 把候選
 * 的**原始拼寫**放進候選的 `comment`。實測（`tools/rime_console.cc`，
 * x86_64 主機端）：
 *
 *     按鍵 MG    → 你 # ni／米 # mi／迷 # mi／擬 # ni／尼 # ni…
 *     按鍵 MGGAM → 你好 # ni hao／米高 # mi gao／你敢 # ni gan…
 *
 * 所以「這串按鍵可能是哪些音節」= 候選 comment 的**第一個音節**去重。
 * 音節之間的分隔字元是方案 `speller/delimiter` 的第一個字（本方案是空白，
 * librime `ScriptTranslator::Spell()` 用 `delimiters_.at(0)` 串接），
 * 為了不綁死在單一方案上，這裡把空白與 `'` 都當成分隔字元。
 *
 * ⚠ **順序就是 librime 的排序**，不是我們發明的。第一個音節之所以排在前面，
 * 是因為引擎認為那個讀音最可能 —— 前端沒有比引擎更好的資訊，不要自己排。
 *
 * ── 為什麼「選了」是篩選，而不是真的把讀音餵回引擎 ──────────────────────
 * 引擎那一側**做不到**：`t9_pinyin` 的 `speller/alphabet` 只有 `ADGJMPTW`，
 * 「ni」這三個字母根本送不進 `rs_process_key`；而 rime_shell 也沒有
 * 「限定這一段的讀音」這種 API。真正的收斂需要 librime 的
 * `RimeSetInput` 等級的能力加上一份能接受原始拼音的方案 —— 兩者都跨越了
 * 本支線的檔案範圍，已寫進 `docs/coordination.md` §5 回報。
 *
 * 所以這一版做的是**候選篩選**：點了「ni」，候選列只留讀音是 ni 的那些。
 * 使用者拿到的效果一樣（看到的候選收斂了），少的是「引擎重新排序」那一層。
 *
 * ── ⚠ 篩選的鐵律：高亮的那一個永遠不准被篩掉 ────────────────────────────
 * 空白鍵送出去之後，librime 上屏的是**引擎目前高亮的候選**（`selector` 的
 * 行為），而 rime_shell 的 `rs_highlight_candidate` 在 Android 這一端還沒有
 * JNI 綁定（見 [VerbSupport]），前端**無法**把高亮移到篩選後的第一個。
 *
 * 於是「把不合讀音的都藏起來」會做出這個專案抓過六次的那類缺陷的鏡像：
 * 使用者點了 mi、看到四個 mi 的候選、按下空白，上屏的卻是他從頭到尾沒看過的
 * 「你」。畫面完全正常、測試全綠、使用者拿到錯字。
 *
 * 所以 [visibleIndices] 一律把 [highlighted] 留在畫面上。多出來的那一個看起來
 * 突兀，但它正是空白鍵會送出的東西 —— **看得到才摸得到**。
 * 等 `rs_highlight_candidate` 接上 JNI，這條規則就該換成「點了讀音就把高亮移
 * 過去」，那才是真正對的解法。
 */
object T9Syllables {

    /**
     * 消歧欄佔用哪幾格。
     *
     * ── ⚠ 這份宣告**本該住在佈局 YAML 裡** ─────────────────────────────
     * 正確的形態是在 `core/layouts` 底下那幾份 yaml 的 layer 上加一個
     * `syllable_slots: [pu_comma, pu_period, pu_question]`，由解析器讀進
     * [org.luminakey.ime.theme.LayoutLayer]。那是**格式擴充**，而佈局格式
     * (`docs/theme-format.md` §9) 與它的解析器 (`ime/theme/`) 都不屬於本支線，
     * 照 `docs/coordination.md` §2 只能回報、不能自己加。已寫進 §5。
     *
     * 在那之前先放在這裡。**這不是寫死的鍵盤**：它引用的是佈局檔裡既有的
     * `key.id`，而 `T9SyllablesTest` 會驗每一個 id 真的存在於它宣告的那一層 ——
     * 有人把 `pu_comma` 改名，測試會紅，不會靜靜地少一欄。
     *
     * ── 為什麼是三格，不是四格 ─────────────────────────────────────────
     * 左欄有四格（，。？ 與 `!@#`），這裡只吃前三格，**底列那一格不動**。
     *
     * 理由不是美觀，是安全：`!@#` 是 `switch_layout:cn-symbols`，一顆導覽鍵。
     * [LayoutEscape] 把導覽鍵當成一張圖來走，用來擋「進得去出不來」——
     * 但它走的是**佈局檔的靜態內容**，看不見執行期的替換。在組字中把一顆導覽
     * 鍵換掉，等於在那張圖上開一個測不到的洞。這條規矩與鍵盤內面板
     * 「底列全程露在外面」（見 [PanelRoute]）是同一條。
     */
    /**
     * ⚠ 這份白名單已經**搬進佈局 YAML** 了：layer 上的 `syllable_slots:`
     * （見 [org.luminakey.ime.theme.LayoutLayer.syllableSlots]）。
     *
     * 留著這段註解是因為它記著搬家的理由：原本是「兩個寫死的佈局 id + 寫死的
     * layer id `"t9"` + 寫死的三個 key id」，於是新增一份九宮格佈局就得改
     * Kotlin，而漏改的樣子是「消歧欄整欄不見，畫面只是照常顯示標點」——
     * 沒有任何東西會叫。
     */


    /**
     * 少於兩格就沒辦法翻頁（翻頁鍵自己要佔一格），那樣會有讀音**看不到也摸不到**。
     * [SLOTS] 的每一筆都必須至少兩格，由測試守著。
     */
    const val MIN_SLOTS = 2

    /** 這一格要畫什麼。 */
    sealed interface Cell {
        /** 一個候選讀音。點下去 = 只留這個讀音的候選。 */
        data class Reading(val syllable: String) : Cell

        /** 「還有更多讀音」。點下去 = 捲到下一批。 */
        data object More : Cell

        /**
         * 這一格空著（讀音比格位少）。
         *
         * ⚠ **不是「維持原本的標點」。** 一開始就是那樣做的，實機截圖立刻看出
         * 問題：讀音只有 ni / mi 兩個時，第三格還印著「？」，一整欄變成
         * 「ni / mi / ？」—— 使用者沒有理由知道第三個不是第三個讀音。
         * 一欄只能有一種意思。
         *
         * 也不是「畫一顆沒有字的鍵」：那是一顆按得到、念得出「按鈕」、
         * 按下去沒反應的鍵。空格就讓它是空的。
         */
        data object Empty : Cell
    }

    /** 目前這一層有沒有消歧欄；沒有就回空清單，呼叫端照常畫標點。 */
    fun slotKeys(layout: KeyboardLayout?, layerId: String): List<String> =
        layout?.layers?.firstOrNull { it.id == layerId }?.syllableSlots ?: emptyList()

    /**
     * 一則 comment 的**第一個音節**；認不出來就回 null。
     *
     * 認不出來時**回 null 而不是硬切一段出來**：comment 不是只有拼寫提示會用，
     * `simplifier` 的字形提示、方案自訂的 `comment_format` 都可能寫別的東西。
     * 把「〔简〕」當成音節印在鍵面上，比不印還糟。
     */
    fun readingOf(comment: String): String? {
        val head = comment.trim().takeWhile { it != ' ' && it != '\'' }
        if (head.isEmpty() || head.length > MAX_SYLLABLE) return null
        if (!head.all { it in 'a'..'z' }) return null
        return head
    }

    /** 候選讀音去重，**保持 librime 給的順序**。 */
    fun readingsOf(candidates: List<RimeCandidate>): List<String> {
        val out = LinkedHashSet<String>()
        for (c in candidates) readingOf(c.comment)?.let { out.add(it) }
        return out.toList()
    }

    /**
     * 使用者選過的讀音是否還算數。
     *
     * 換頁、再按一鍵之後讀音集合會變；釘住一個**已經不在集合裡**的讀音，
     * [visibleIndices] 會篩出一片空白。這裡先擋掉，讓「篩完必定非空」
     * 是一條結構上成立的性質，而不是靠呼叫端小心。
     */
    fun resolvePin(readings: List<String>, requested: String?): String? =
        if (requested != null && readings.contains(requested)) requested else null

    /**
     * 這一輪填進格位的內容。**長度恆等於 [slots]** —— 消歧欄是全有或全無，
     * 不會有一半讀音一半標點的混合欄（見 [Cell.Empty]）。
     * 讀音多於格位時，最後一格讓給「⋯」。
     *
     * @param offset 目前捲到第幾個讀音（見 [nextOffset]）。
     */
    fun cells(readings: List<String>, slots: Int, offset: Int): List<Cell> {
        if (slots <= 0 || readings.isEmpty()) return emptyList()
        val out = ArrayList<Cell>(slots)
        if (readings.size <= slots || slots < MIN_SLOTS) {
            readings.take(slots).forEach { out += Cell.Reading(it) }
        } else {
            val window = slots - 1
            val start = ((offset % readings.size) + readings.size) % readings.size
            for (i in 0 until window) out += Cell.Reading(readings[(start + i) % readings.size])
            out += Cell.More
        }
        while (out.size < slots) out += Cell.Empty
        return out
    }

    /**
     * 按下「⋯」之後捲到哪裡。
     *
     * 一次前進一整個視窗（`slots - 1`），所以相鄰兩批是**接續**的、不重疊 ——
     * 連續按下去會把整圈走完再回到起點。「每一個讀音都摸得到」這件事因此是
     * 算出來的性質，不是靠人數格子，[T9SyllablesTest] 直接驗它。
     */
    fun nextOffset(readings: List<String>, slots: Int, offset: Int): Int {
        if (readings.size <= slots || slots < MIN_SLOTS) return 0
        val n = readings.size
        return (((offset + slots - 1) % n) + n) % n
    }

    /**
     * 候選列該顯示哪幾個（回傳的是**引擎的頁內索引**，不是畫面位置）。
     *
     * 回傳索引而不是候選本身，是因為選字走 `rs_select_candidate(index_on_page)`：
     * 畫面位置一旦與引擎索引脫鉤，使用者點第二個卻選到第五個 —— 而畫面
     * 完全正常。索引留在資料裡，兩者就不可能對不上。
     *
     * [highlighted] 一律保留，理由見本物件檔頭那段鐵律。
     */
    fun visibleIndices(
        candidates: List<RimeCandidate>,
        pin: String?,
        highlighted: Int,
    ): List<Int> {
        if (pin == null) return candidates.indices.toList()
        val out = ArrayList<Int>(candidates.size)
        for (i in candidates.indices) {
            if (i == highlighted || readingOf(candidates[i].comment) == pin) out += i
        }
        return out
    }

    /**
     * 把一格替換成消歧欄的鍵面。
     *
     * 沿用原鍵的 `id` / `width` / `style`，只換鍵面文字與行為 —— 幾何一格都不
     * 能動，否則整列會重排，使用者會看到一個在組字途中自己跳動的鍵盤。
     *
     * `tap` 給的是 [ActionVerb.NOOP]：**消歧欄不是 §9.5 的動作動詞**，它沒有
     * 也不該有 YAML 表示法（真要有，那是 `syllable_slots` 落地之後的事）。
     * 這裡只借 `tap != null` 讓 [KeyView] 認得它可以點、並且**補上無障礙的
     * ACTION_CLICK** —— 少了那個，做出來的又是一顆念得出名字、按下去沒反應的
     * 鍵。實際要做什麼由呼叫端包一層 onEvent 決定（見 KeyGrid）。
     */
    fun slotKey(original: LayoutKey, cell: Cell, pinned: Boolean): LayoutKey =
        original.copy(
            label = when (cell) {
                is Cell.Reading -> cell.syllable
                Cell.More -> MORE_LABEL
                Cell.Empty -> ""
            },
            hint = "",
            icon = null,
            labelFrom = LabelSource.NONE,
            // 空格走 §9.6 的 spacer：**不是**一顆沒有字的鍵。spacer 佔一樣的寬度
            // （整列不會重排），但它不是按鈕、沒有語意節點、TalkBack 不會停在
            // 上面念「按鈕」。
            spacer = cell == Cell.Empty,
            active = pinned,
            repeat = false,
            send = null,
            tap = if (cell == Cell.Empty) null else KeyAction(ActionVerb.NOOP, emptyList(), ""),
            doubleTap = null,
            longPress = null,
            popup = null,
            swipe = emptyMap(),
        )

    /** 鍵面上的「還有更多」。與 `faceOf` 一樣是鍵面字，不是介面文案，故不進 strings。 */
    const val MORE_LABEL = "⋯"

    /** 「zhuang」是最長的漢語拼音音節（6）。留一點餘裕，但不留到能吞下一整句。 */
    private const val MAX_SYLLABLE = 8

    /* ═════════════════ 逐個音節選下去（輸入串改寫）═════════════════
     *
     * ── 為什麼上面那一套「篩選」不夠 ──────────────────────────────────
     * 本檔原本的做法是**候選篩選**：點了 ni 就只留讀音是 ni 的候選。當時的理由
     * 寫在檔頭 —— 引擎側做不到，因為 `t9_pinyin` 的 alphabet 只有 `ADGJMPTW`，
     * 「ni」這三個字母送不進去。
     *
     * **那個前提已經不成立了。** 方案改成雙編碼之後（`core/data/schemas/`，
     * alphabet 同時含小寫拼音與大寫 T9 碼），精確拼音與模糊碼可以共存在同一串
     * 輸入裡：`niGAM` 解得出「你好」—— 第一個音節精確、後面仍然模糊。
     * 加上 `rs_set_input()` 進了 ABI 3，真正的收斂終於做得到：
     *
     *     使用者點 ni  →  把輸入串從 `MGGAM` 改寫成 `niGAM`
     *                  →  引擎重新切分，候選收斂成 ni 開頭的
     *                  →  左欄換成**第二個音節**的候選讀音（hao / gao / gan…）
     *
     * 這才是使用者要的「選了一個之後讓我選下一個」。篩選只是讓畫面看起來收斂，
     * 引擎完全不知道使用者做過選擇 —— 空白鍵送出去的仍是原本那個高亮候選。
     */

    /**
     * 拼音字母 → 九宮格的代表字母。
     *
     * 這是方案 `speller/algebra` 那條 `xlit` 的**反向**，兩邊必須一致：
     * 那條 xlit 把 `ABCDEFGHIJKLMNOPQRSTUVWXYZ` 折成
     * `AAADDDGGGJJJMMMPPPPTTTWWWW`。改方案而沒改這裡，改寫出來的字串會落在
     * alphabet 之外，`rs_set_input()` 直接回 false（不會靜靜地錯）。
     */
    private val T9_OF: Map<Char, Char> = HashMap<Char, Char>().apply {
        fun group(letters: String, rep: Char) = letters.forEach { put(it, rep) }
        group("abc", 'A'); group("def", 'D'); group("ghi", 'G'); group("jkl", 'J')
        group("mno", 'M'); group("pqrs", 'P'); group("tuv", 'T'); group("wxyz", 'W')
    }

    /** 一個音節的九宮格按鍵序列；含非拼音字母就回 null。 */
    fun t9Encode(syllable: String): String? {
        if (syllable.isEmpty()) return null
        val sb = StringBuilder(syllable.length)
        for (c in syllable) sb.append(T9_OF[c] ?: return null)
        return sb.toString()
    }

    /** 一則 comment 的全部音節。分隔字元同 [readingOf]：空白與 `'`。 */
    fun syllablesOf(comment: String): List<String> =
        comment.trim().split(' ', '\'').filter { it.isNotEmpty() }

    /**
     * 候選的**第 [index] 個**音節有哪些可能，去重、保持 librime 給的順序。
     *
     * [index] = 已確定的音節數。0 就是原本的 [readingsOf]。
     * 音節數不足的候選（例如已經確定兩個音節時，那些只涵蓋一個音節的候選）
     * 直接跳過 —— 它們對「下一個音節是什麼」沒有意見。
     */
    fun readingsAt(candidates: List<RimeCandidate>, index: Int): List<String> {
        if (index < 0) return emptyList()
        val out = LinkedHashSet<String>()
        for (c in candidates) {
            val s = syllablesOf(c.comment).getOrNull(index) ?: continue
            if (s.length in 1..MAX_SYLLABLE && s.all { it in 'a'..'z' }) out.add(s)
        }
        return out.toList()
    }

    /**
     * 這個音節會吃掉模糊尾巴的前幾個按鍵。認不出來就回 null。
     *
     * ⚠ **不是「音節有幾個字母就吃幾鍵」。** 方案有
     * `abbrev/^([ADGJMPTW]).+$/$1/`（超級簡拼），所以「ni」也可能只按了一鍵 `M`。
     * 兩種都要認：先試完整長度，再試簡拼的一鍵。都對不上就回 null ——
     * **猜一個數字出來會把使用者後面打的東西吃掉**，那比不動更糟。
     */
    fun consumedCodes(fuzzyTail: String, syllable: String): Int? {
        val full = t9Encode(syllable) ?: return null
        if (fuzzyTail.startsWith(full)) return full.length
        if (fuzzyTail.isNotEmpty() && fuzzyTail[0] == full[0]) return 1
        return null
    }

    /**
     * 點了 [syllable] 之後，引擎的輸入串該變成什麼。不合法就回 null。
     *
     * [confirmed] 是**已經確定**的音節（小寫精確拼音），它們必須真的是 [input]
     * 的前綴 —— 否則代表引擎那邊已經變過（使用者按了刪除、或重新組字），
     * 這時回 null 讓呼叫端什麼都不做，而不是把一段不存在的前綴接回去。
     */
    fun rewriteInput(input: String, confirmed: List<String>, syllable: String): String? {
        val prefix = confirmed.joinToString("")
        if (!input.startsWith(prefix)) return null
        val tail = input.substring(prefix.length)
        val n = consumedCodes(tail, syllable) ?: return null
        return prefix + syllable + tail.substring(n)
    }

    /**
     * 引擎的輸入串變了之後，[confirmed] 還有幾個算數。
     *
     * 使用者按刪除鍵會把尾巴砍掉，砍過頭就會吃到已確定的那一段；重新開始組字
     * 更是整串換掉。留著過期的前綴，消歧欄會從第 3 個音節開始問，而畫面上
     * 根本沒有前兩個 —— 又是一個「內部狀態對、畫面對不上」。
     */
    fun syncConfirmed(input: String, confirmed: List<String>): List<String> {
        val out = ArrayList<String>(confirmed.size)
        val acc = StringBuilder()
        for (s in confirmed) {
            acc.append(s)
            if (!input.startsWith(acc)) break
            out += s
        }
        return out
    }
}
