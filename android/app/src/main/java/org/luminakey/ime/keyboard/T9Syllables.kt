package org.luminakey.ime.keyboard

import org.luminakey.ime.core.RimeCandidate
import org.luminakey.ime.theme.ActionVerb
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.LayoutKey
import org.luminakey.ime.theme.Popup

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
         * 這一格**不歸消歧欄管**（讀音比格位少），維持佈局檔原本的鍵。
         *
         * ── ⚠ 這裡翻過一次案,兩個方向都上過實機 ───────────────────────────
         * 第一版就是「維持原本的標點」。當時的實機截圖看到「ni / mi / ？」,
         * 判斷是「一欄只能有一種意思 —— 使用者沒有理由知道第三個不是第三個
         * 讀音」,於是改成空白。
         *
         * **那個改法更糟,而且是使用者拿著實機截圖回報的(task #78)。**
         * `build/look/android/04-t9-composing-ime.png`:打了 PGM 之後左欄變成
         * 「qin / pin / □ / !@#」—— 第三格的「？」整顆消失,留下一個空的灰色
         * 方塊。使用者看到的不是「這一格沒有讀音」,是**鍵盤破了一個洞**,
         * 而且那顆本來按得到的標點鍵在組字途中被無聲拿走了。
         *
         * 兩害相權很清楚:留著標點,最壞是使用者多看一眼 —— 而「？」長得就
         * 不像拼音,分辨成本接近零;挖成空洞,則是把一顆既有的鍵拿掉而沒有
         * 任何東西說它去哪了。那正是這個專案抓過七次的「看得到但摸不到」
         * 的鏡像:**本來摸得到,現在連看都看不到。**
         *
         * 所以這一格是**原封不動**:鍵面、長按盤、`send`、無障礙朗讀名全部走
         * 佈局檔自己那一份。[slotKey] 直接回傳原鍵,KeyGrid 也把它當成
         * 「這一格沒有 cell」處理 —— 消歧欄完全不碰它。
         */
        data object Original : Cell
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
     * 這一輪填進格位的內容。**長度恆等於 [slots]**（[slots] ≤ 0 才是空清單）,
     * 呼叫端才能一格對一格地 zip 上去 —— 少一格就是少一個 key id 被接管,
     * 而那個 id 會靜靜地照佈局畫,沒有任何東西會叫。
     *
     * 讀音填不滿時,剩下的是 [Cell.Original]（**原本那顆鍵**,不是空洞);
     * 讀音多於格位時,最後一格讓給「⋯」。
     *
     * ⚠ **一個讀音都沒有**時每一格也是 [Cell.Original],不是空清單。
     * 這裡一度回 `emptyList()`,與上面那句「長度恆等於 slots」互相矛盾 ——
     * 文件與碼不符時,對的是文件那一邊:呼叫端真的在 `zip`,而 zip 遇到
     * 短清單不會叫,只會少接管幾個 key id。
     *
     * @param offset 目前捲到第幾個讀音（見 [nextOffset]）。
     */
    fun cells(readings: List<String>, slots: Int, offset: Int): List<Cell> {
        if (slots <= 0) return emptyList()
        val out = ArrayList<Cell>(slots)
        if (readings.size <= slots || slots < MIN_SLOTS) {
            readings.take(slots).forEach { out += Cell.Reading(it) }
        } else {
            val window = slots - 1
            val start = ((offset % readings.size) + readings.size) % readings.size
            for (i in 0 until window) out += Cell.Reading(readings[(start + i) % readings.size])
            out += Cell.More
        }
        // 沒用到的格位**還原成原本的鍵**,不是留空(task #78)。
        while (out.size < slots) out += Cell.Original
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
    fun slotKey(original: LayoutKey, cell: Cell, pinned: Boolean): LayoutKey {
        // 不歸消歧欄管的格位**原封不動退回去**。少了這一行,沒用到的格位會被
        // 換成一顆沒有鍵面的 spacer —— 使用者看到的是鍵盤上一個灰色的洞,
        // 而原本的標點鍵在組字途中消失了(task #78)。
        if (cell == Cell.Original) return original
        return original.copy(
            label = when (cell) {
                is Cell.Reading -> cell.syllable
                Cell.More -> MORE_LABEL
                Cell.Original -> original.label   // 上面已經 return,這裡到不了
            },
            hint = "",
            icon = null,
            labelFrom = LabelSource.NONE,
            // 被接管的格位一定畫得出鍵面（讀音或「⋯」),不會是 spacer。
            spacer = false,
            active = pinned,
            repeat = false,
            send = null,
            tap = KeyAction(ActionVerb.NOOP, emptyList(), ""),
            doubleTap = null,
            longPress = null,
            popup = null,
            swipe = emptyMap(),
        )
    }

    /**
     * 一格在畫面上的**四個決定**,一次算完。
     *
     * ── 為什麼要有這個型別 ─────────────────────────────────────────────
     * 一格被消歧欄接管與否,同時決定四件事:**鍵面**、**點下去做什麼**、
     * **長按盤開不開**、**朗讀名念什麼**。四件必須同進同出。
     *
     * 這四件原本散在 KeyGrid 的四個三元運算式裡,各自判斷一次
     * `cell == null` —— 四個判斷點意味著它們**可以分岔**,而分岔的樣子在
     * 螢幕上看不出來:鍵面完全正確、按下去什麼都不做、長按盤也開不出來的
     * 標點鍵（task #78 的孿生兄弟）。
     *
     * 收成一個純函式之後,四件事只剩**一個**判斷點,而那個判斷點是單元測試
     * 摸得到的 —— `@Composable` 那一層在這個模組裡沒有任何東西摸得到。
     */
    data class SlotRender(
        /** 要畫的鍵。沒被接管時**就是原鍵本人**（同一個實例）,不是複製品。 */
        val key: LayoutKey,
        /**
         * 點下去要交給消歧欄的 cell;`null` = 走原鍵自己的 `onEvent`。
         *
         * ⚠ 沒被接管的格位這裡**一定要是 null**。給了 [Cell.Original],
         * 呼叫端會把點擊導進 `onSlot`,而 `onSlot` 對 [Cell.Original] 是
         * `Unit` —— 那顆標點鍵就變成按下去什麼都不會發生。
         */
        val tapCell: Cell?,
        /** 長按盤;`null` = 沒有盤。恆等於 [key] 自己那一份。 */
        val popup: Popup?,
        /** 朗讀名要換成哪一格的說法;`null` = 念原鍵自己的名字。 */
        val speaks: Cell?,
        /** 這一格是不是使用者釘住的那個讀音(鍵面要看得出來)。 */
        val pinned: Boolean,
    ) {
        /** 消歧欄有沒有接管這一格。 */
        val takenOver: Boolean get() = tapCell != null
    }

    /**
     * 這一格該畫什麼、按下去該怎麼反應。**純函式;呼叫端只照著做,不再自己判斷。**
     *
     * @param original 佈局檔裡的那顆鍵。
     * @param declared 消歧欄替這個 `key.id` 宣告了什麼;`null` = 它不是格位。
     * @param pinnedSyllable 使用者釘住的讀音,沒釘就 null。
     */
    fun renderSlot(original: LayoutKey, declared: Cell?, pinnedSyllable: String?): SlotRender {
        // 三條「不歸消歧欄管」的路徑,結果**必須一模一樣**:
        //   · spacer:它本來就沒有鍵面、沒有行為;
        //   · 這顆鍵根本不在格位清單裡;
        //   · 是格位,但這一輪讀音不夠用（[Cell.Original],見 task #78）。
        // 第三條就是那個沒有守門的缺口:少折這一次,鍵面照樣對(slotKey 會把
        // Cell.Original 原封退回),但點擊與長按盤被交給了消歧欄。
        if (original.spacer || declared == null || declared == Cell.Original) {
            return SlotRender(
                key = original,
                tapCell = null,
                popup = original.popup,
                speaks = null,
                pinned = false,
            )
        }
        val pinned = declared is Cell.Reading && declared.syllable == pinnedSyllable
        val face = slotKey(original, declared, pinned)
        return SlotRender(
            key = face,
            tapCell = declared,
            popup = face.popup,
            speaks = declared,
            pinned = pinned,
        )
    }

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

    /* ════════ 改寫成功了沒:這其實是兩個問題,原本混成了一個 ════════
     *
     * 這一段原本是 `rewriteAccepted()`:改寫送進引擎之後,回頭問候選 ——
     * 「comment 有沒有以那幾個音節開頭」。理由聽起來很硬:引擎真的聽懂了的話,
     * `spelling_hints` 給的 comment 必然是 `ni hao`。
     *
     * **但 comment 只在「拼寫與輸入不同」的時候才有。** 實測(rime_console,
     * x86_64 模擬器,雙編碼方案):
     *
     *     PGM → 1. 品 # pin   2. 親 # qin   3. 秦 # qin …   ← 有 comment
     *     pin → 1. 品         2. 拼         3. 浜 …         ← 沒有 comment
     *
     * 也就是說 **改寫得越徹底,那個判準越確定它失敗**。單音節的 `PGM → pin`
     * 改寫完之後輸入已經是精確拼音,librime 沒有提示可給,於是「引擎完全聽懂
     * 了」被讀成「引擎不認得」,輸入串被還原 —— 使用者點下 `pin`,畫面一動也
     * 不動,候選也沒變。那正是真機回報的那一條。
     *
     * ⚠ 但那個保護擋的是**真的**東西,不能拿掉:裝置上的方案可能還是舊的
     *   單編碼版(`alphabet: 'ADGJMPTW'`,`scripts/collect_data.sh` 沒跑過時
     *   就會這樣)。那時 `rs_set_input("niGAM")` 照樣回 true,引擎把 `ni` 當成
     *   一段翻不出東西的原文、只替 `GAM` 出候選,使用者點第一個就上屏
     *   **「ni好」**。畫面上沒有任何東西說出事了。
     *
     * 所以修法是**把一個問題拆成兩個**,各問各的:
     *
     *   ① 「這個方案有沒有辦法接受精確拼音?」
     *      —— 這是**方案的性質**,與使用者這一下點了什麼無關,
     *         所以是**啟動時問一次**的事。見 [PROBE_KEYS] / [probeAccepted]。
     *   ② 「引擎有沒有把我寫進去的那一串原封不動收下?」
     *      —— 這是**輸入串自己**的事,問 `rs_get_input()` 就好,
     *         不必去猜候選長什麼樣。見 [rewriteOutcome]。
     *
     * ⚠ **為什麼 ② 不會在「成功」的時候誤判。** `RimeSetInput()` 的實作只有
     *   `ctx->set_input(input)`,而 `Context::set_input()` 就一行
     *   `input_ = value`(librime `src/rime/context.cc:280`);`rs_get_input()`
     *   回的是同一個 `input_`。成功時兩者**逐位元組相等**,中間沒有任何正規化
     *   會介入。而 ①、② 都不看「改寫完之後還有沒有 comment」——
     *   那個會隨著改寫變徹底而消失的東西,已經不在判準裡了。
     */

    /**
     * 啟動探針要送進引擎的按鍵。
     *
     * 形狀是**一個精確拼音音節 + 一個仍然模糊的 T9 碼**,因為要問到兩件事:
     * 小寫拼音進不進得了 `speller/alphabet`,以及 `ni` 切不切得出一個音節。
     * 尾巴刻意留一個模糊碼 —— 這樣 `spelling_hints` 才一定給得出 comment,
     * 不會踩到上面那個「全部精確就沒有提示」的坑。
     *
     * 為什麼是 `ni` 與 `G`:`G` 是 [T9_OF] 裡 `ghi` 的代表字母,而 `ni` 的 T9
     * 編碼是 `MG` —— 兩者都落在本檔改寫得出來的字元集裡。哪天方案換了一套
     * T9 字母而沒有同步 [T9_OF],探針會失敗、消歧欄整條不出現,**而不是**讓
     * 改寫在使用者手上產生一串垃圾。
     */
    const val PROBE_SYLLABLE = "ni"
    const val PROBE_KEYS = "niG"

    /**
     * 探針的判讀。這個方案能做音節改寫 ⇔ 下面兩件事同時成立:
     *
     * · **每一個探針按鍵都被引擎消費了。** 小寫不在 alphabet 裡時,`n` 與 `i`
     *   會被 speller 直接放行。
     * · **有候選的 comment 以 [PROBE_SYLLABLE] 開頭。** 字元集收下了字母不代表
     *   `ni` 切得出一個音節;這一條問的是切分,不是字元集。
     *
     * ⚠ 這裡**不能**改用 `rs_set_input()` 去問。`rs_set_input()` 繞過 speller,
     *   舊方案上餵 `niGAM` 一樣回 true(見
     *   [org.luminakey.ime.core.RimeCore.setInput] 的註解)—— 它問不出
     *   alphabet 這一題,而那正是這裡要問的那一題。`rs_process_key()` 才問得到。
     *
     * 兩種方案上都實測過(`rime_console`,emulator-5554):
     *
     *     雙編碼 niG → n/i/G 全部消費,preedit="ni G",候選 你好 # ni hao …
     *     單編碼 niG → n、i **未消費**,preedit="G",候選 和 # he／好 # hao …
     */
    fun probeAccepted(allKeysConsumed: Boolean, candidates: List<RimeCandidate>): Boolean {
        if (!allKeysConsumed) return false
        return candidates.any { syllablesOf(it.comment).firstOrNull() == PROBE_SYLLABLE }
    }

    /**
     * 一次改寫的結果。**三種失敗要有三個名字** —— 全印成同一句話的話,
     * 下一個人得把整條路徑重查一次才知道是哪一種(這個專案在 Windows 那一端
     * 正在還這筆帳)。
     */
    enum class Rewrite {
        /** 引擎收下了,留著。 */
        OK,

        /** 方案沒通過啟動探針:它接不了精確拼音,改寫只會產生一串垃圾。 */
        SCHEMA_CANNOT,

        /** `rs_set_input()` 自己回 false。 */
        ENGINE_REFUSED,

        /** 回了 true,但引擎手上的輸入串不是我們寫進去的那一串。 */
        ENGINE_DROPPED,

        /** 收下了,卻一個候選都翻不出來 —— 使用者會看到一條空的候選列。 */
        EMPTY_RESULT,
    }

    /**
     * 這一次改寫算不算成立。純判準,沒有副作用 —— 呼叫端照結果決定要不要還原。
     *
     * ⚠ [candidateCount] 這一條**不會**在成功時誤判:消歧欄上那個讀音本來就是
     *   從某個候選的 comment 第 [confirmed]`.size` 個音節取出來的,所以改寫之後
     *   那個候選必定還在。翻不出任何候選只可能是改寫寫壞了。
     */
    fun rewriteOutcome(
        schemaCanRewrite: Boolean,
        setInputReturned: Boolean,
        inputAfterRewrite: String,
        wantedInput: String,
        candidateCount: Int,
    ): Rewrite = when {
        !schemaCanRewrite -> Rewrite.SCHEMA_CANNOT
        !setInputReturned -> Rewrite.ENGINE_REFUSED
        inputAfterRewrite != wantedInput -> Rewrite.ENGINE_DROPPED
        candidateCount <= 0 -> Rewrite.EMPTY_RESULT
        else -> Rewrite.OK
    }
}
