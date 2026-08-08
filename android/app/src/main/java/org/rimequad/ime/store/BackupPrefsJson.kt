package org.rimequad.ime.store

import org.rimequad.ime.prefs.UserPrefs

/**
 * 使用者偏好在備份裡的載體。
 *
 * 走的是 [UserPrefs.toMap] / [UserPrefs.fromMap] 那一對純函式，而不是
 * DataStore 的檔案格式 —— DataStore 存的是 protobuf，換一版函式庫就換一個
 * 樣子，拿它當跨端格式等於把備份綁死在一個 Android 專屬的實作上。
 *
 * ⚠ **「未設定」必須原樣保留。** [UserPrefs] 的第一原則是「null = 使用者
 * 沒設定過」，而不是「使用者選了預設值」。所以這裡只搬**出現在映射裡的
 * key**，缺席的一律不補 —— 補了就等於把匯出當下的預設值凍進備份，
 * 使用者換手機之後會被永遠釘在舊預設上（見 UserPrefs 檔頭）。
 */
object BackupPrefsJson {

    const val FORMAT_VERSION = 1

    /**
     * **刻意不進備份**的偏好。
     *
     * · `network_enabled` —— 這是安全預設，不是喜好。UserPrefs 檔頭寫得很清楚：
     *   「從沒表態過的人一律當作不連網」。如果備份可以把它設成 true，那麼
     *   「打開一個檔案」就變成一個**可以替使用者開啟連網的動作**，而他
     *   可能是在一台全新的、還沒決定要不要信任這個網路的手機上做這件事。
     *   新機器上要連網，請他自己再按一次那個開關。
     * · `offline_notice_seen` —— 上面那個開關的第一次說明。開關回到預設，
     *   說明就該再出現一次；兩件事要一起重來（同 UserPrefs 對「全部回復
     *   預設」的說明）。
     * · `onboarding_done` —— 它記的是「這台裝置上曾經走完引導」，是**這台
     *   裝置**與系統輸入法設定的狀態，不是偏好。搬過去只會讓新手機跳過
     *   他還沒做的那幾步。
     */
    val NOT_BACKED_UP: Set<String> = setOf(
        UserPrefs.K_NETWORK_ENABLED,
        UserPrefs.K_OFFLINE_NOTICE_SEEN,
        UserPrefs.K_ONBOARDING_DONE,
    )

    fun encode(values: Map<String, Any>): String {
        val kept = values.filterKeys { it !in NOT_BACKED_UP }
        val sb = StringBuilder()
        sb.append("{\n")
        sb.append("  \"format_version\": ").append(FORMAT_VERSION).append(",\n")
        sb.append("  \"values\": {\n")
        val keys = kept.keys.sorted()   // 固定順序，備份才是可重現的
        keys.forEachIndexed { i, k ->
            sb.append("    ").append(q(k)).append(": ").append(literal(kept.getValue(k)))
            sb.append(if (i == keys.lastIndex) "\n" else ",\n")
        }
        sb.append("  }\n")
        sb.append("}\n")
        return sb.toString()
    }

    /**
     * 解析。壞掉就回空映射 —— 一份讀不懂的偏好不該讓整個匯入失敗，
     * 使用者頂多是設定沒跟過來，詞庫還在。
     *
     * [NOT_BACKED_UP] 的 key **在讀取端再擋一次**：舊版（或別人手改過）的
     * 備份裡可能有它們，而擋在寫入端一次是不夠的 —— 讀取端才是真正會
     * 改變這台機器狀態的地方。
     */
    fun decode(text: String): Map<String, Any?> {
        val root = MiniJson.parseOrNull(text) ?: return emptyMap()
        val values = root["values"] as? Json.Obj ?: return emptyMap()
        val out = LinkedHashMap<String, Any?>()
        for ((k, v) in values.entries) {
            if (k in NOT_BACKED_UP) continue
            when (v) {
                is Json.Bool -> out[k] = v.value
                is Json.Str -> out[k] = v.value
                is Json.Num ->
                    // 整數與小數必須分得開：UserPrefs.fromMap 兩種都吃 Number，
                    // 但 raw 帶小數點時走 Double 才不會把 1.15 磨成 1。
                    out[k] = if (v.raw.any { it == '.' || it == 'e' || it == 'E' }) {
                        v.asDouble
                    } else {
                        v.asLong
                    }

                else -> Unit   // null / 陣列 / 物件：偏好裡沒有這幾種，忽略
            }
        }
        return out
    }

    private fun literal(v: Any): String = when (v) {
        is Boolean -> v.toString()
        is Int, is Long -> v.toString()
        is Float, is Double -> v.toString()
        is String -> q(v)
        else -> q(v.toString())
    }

    private fun q(s: String): String {
        val sb = StringBuilder("\"")
        for (c in s) {
            when {
                c == '"' -> sb.append("\\\"")
                c == '\\' -> sb.append("\\\\")
                c == '\n' -> sb.append("\\n")
                c == '\r' -> sb.append("\\r")
                c == '\t' -> sb.append("\\t")
                c.code < 0x20 -> sb.append(String.format("\\u%04x", c.code))
                else -> sb.append(c)
            }
        }
        return sb.append('"').toString()
    }
}
