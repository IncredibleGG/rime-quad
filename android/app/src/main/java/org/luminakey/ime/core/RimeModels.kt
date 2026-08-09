package org.luminakey.ime.core

/**
 * rime_shell.h 各 struct 的 Kotlin 對應。
 *
 * 這些全部是**值複製**。原生端 `rs_snapshot_acquire()` 回傳的指標只在
 * 下一次 acquire / release 之前有效，因此 JNI 在同一次呼叫內就把資料
 * 搬進這些 data class，Kotlin 這側永遠不持有原生指標。
 */

/** 對應 `rs_composition`。offset 以 UTF-8 位元組計，與原生端一致。 */
data class RimeComposition(
    val preedit: String = "",
    val selStart: Int = 0,
    val selEnd: Int = 0,
    val caret: Int = 0,
)

/** 對應 `rs_candidate`。 */
data class RimeCandidate(
    val text: String,
    val comment: String = "",
    val label: String = "",
)

/** 對應 `rs_menu`。 */
data class RimeMenu(
    val candidates: List<RimeCandidate> = emptyList(),
    val pageNo: Int = 0,
    /** 本頁高亮索引；無候選時為 -1。 */
    val highlighted: Int = -1,
    val isLastPage: Boolean = true,
)

/** 對應 `rs_status`。 */
data class RimeStatus(
    val schemaId: String = "",
    val schemaName: String = "",
    val isComposing: Boolean = false,
    val isAsciiMode: Boolean = false,
    val isFullShape: Boolean = false,
    val isSimplified: Boolean = false,
    val isAsciiPunct: Boolean = false,
    val isDisabled: Boolean = false,
)

/** 對應 `rs_snapshot`。[commitText] 為 null 代表本次沒有要上屏的文字。 */
data class RimeSnapshot(
    val composition: RimeComposition = RimeComposition(),
    val menu: RimeMenu = RimeMenu(),
    val status: RimeStatus = RimeStatus(),
    val commitText: String? = null,
) {
    companion object {
        /**
         * 解碼 JNI 的扁平表示。編碼格式與 `jni_bridge.cc` 的 `nativeSnapshot`
         * 註解一一對應，兩邊改動必須同步。
         *
         * ints 長度 14；strs 長度 4 + 3 * count。
         */
        internal fun decode(ints: IntArray, strs: Array<String?>): RimeSnapshot {
            require(ints.size >= INT_COUNT) { "快照 int 陣列長度不足: ${ints.size}" }
            require(strs.size >= STRING_HEADER) { "快照 string 陣列長度不足: ${strs.size}" }

            val count = ints[3].coerceAtLeast(0)
            val candidates = ArrayList<RimeCandidate>(count)
            for (i in 0 until count) {
                val base = STRING_HEADER + i * 3
                if (base + 2 >= strs.size) break
                candidates.add(
                    RimeCandidate(
                        text = strs[base].orEmpty(),
                        comment = strs[base + 1].orEmpty(),
                        label = strs[base + 2].orEmpty(),
                    )
                )
            }

            return RimeSnapshot(
                composition = RimeComposition(
                    preedit = strs[0].orEmpty(),
                    selStart = ints[0],
                    selEnd = ints[1],
                    caret = ints[2],
                ),
                menu = RimeMenu(
                    candidates = candidates,
                    pageNo = ints[4],
                    highlighted = ints[5],
                    isLastPage = ints[6] != 0,
                ),
                status = RimeStatus(
                    schemaId = strs[2].orEmpty(),
                    schemaName = strs[3].orEmpty(),
                    isComposing = ints[7] != 0,
                    isAsciiMode = ints[8] != 0,
                    isFullShape = ints[9] != 0,
                    isSimplified = ints[10] != 0,
                    isAsciiPunct = ints[11] != 0,
                    isDisabled = ints[12] != 0,
                ),
                commitText = if (ints[13] != 0) strs[1] else null,
            )
        }

        private const val INT_COUNT = 14
        private const val STRING_HEADER = 4
    }
}

/** 對應 `rs_deploy_status`。 */
enum class RimeDeployStatus {
    IDLE, RUNNING, SUCCESS, FAILURE;

    companion object {
        fun fromNative(value: Int): RimeDeployStatus =
            entries.getOrElse(value) { IDLE }
    }
}

/** 對應 `rs_schema_list` 的一筆。 */
data class RimeSchema(val id: String, val name: String)
