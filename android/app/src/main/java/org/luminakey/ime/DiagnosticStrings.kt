package org.luminakey.ime

import android.content.Context
import org.luminakey.ime.theme.DiagnosticCode
import org.luminakey.ime.theme.DiagnosticTerm
import org.luminakey.ime.theme.DiagnosticText

/**
 * 診斷樣板的 Android 接線：`DiagnosticCode` → `res/values.../strings_diag.xml`。
 *
 * ── 為什麼是一個 exhaustive `when` 而不是 `getIdentifier()` ────────────
 * `when` 沒有 `else`，所以**新增一個 code 而忘記加樣板時是編譯錯誤**，
 * 不是執行期才發現的空白。`getIdentifier()` 反過來：漏了一條要等到那份壞
 * 主題出現在真使用者手上才看得到，而且開啟資源縮減之後它會整批失效。
 *
 * 「`when` 裡指到別的 code 的資源」這種手滑，由
 * [DiagnosticStringsTest.每一個_code_都對到同名的資源] 用反射比對
 * `R.string` 的欄位名擋下 —— 資源名是 aapt 從 xml 的 `name` 生出來的，
 * 所以欄位名對得上就等於指對了。
 *
 * ⚠ 新增字串一律進 `strings_diag.xml`，不要動 `strings.xml`：`values/` 底下
 * 所有 xml 會被合併，分檔不影響行為，但省掉多條支線同時改同一個檔案的衝突。
 */
class DiagnosticStrings(context: Context) : DiagnosticText.TemplateSource {

    private val app = context.applicationContext

    override fun format(code: DiagnosticCode, name: String, args: List<String>): String? {
        val id = ID_BY_NAME[name] ?: return null
        // `type_mismatch` 的兩個參數是 DiagnosticTerm 的代號，不是給人看的字。
        // 不在這裡換掉，中文使用者會看到「這裡應該是 mapping」。
        val ready =
            if (code == DiagnosticCode.TYPE_MISMATCH) args.map { localizedTerm(it) } else args
        return runCatching {
            @Suppress("SpreadOperator")
            app.getString(id, *ready.toTypedArray())
        }.getOrNull()
    }

    /** 認不得的代號就原樣顯示 —— 少一個字總比整則診斷消失好（§6.5）。 */
    private fun localizedTerm(id: String): String {
        val term = DiagnosticTerm.values().firstOrNull { it.id == id } ?: return id
        return runCatching { app.getString(TERM_IDS.getValue(term)) }.getOrNull() ?: id
    }

    companion object {

        /** 安裝到純邏輯層。App 與輸入法各自建 ConfigRepository 時都會走到。 */
        fun install(context: Context) {
            DiagnosticText.install(DiagnosticStrings(context))
        }

        /** [DiagnosticTerm] 的字面值。同樣是沒有 `else` 的 `when`。 */
        internal val TERM_IDS: Map<DiagnosticTerm, Int> =
            DiagnosticTerm.values().associateWith { term ->
                when (term) {
                    DiagnosticTerm.MAPPING -> R.string.diag_kind_mapping
                    DiagnosticTerm.SEQUENCE -> R.string.diag_kind_sequence
                    DiagnosticTerm.SCALAR -> R.string.diag_kind_scalar
                    DiagnosticTerm.NOTHING -> R.string.diag_kind_null
                    DiagnosticTerm.STRING_LIST -> R.string.diag_kind_string_list
                    DiagnosticTerm.LOCALIZED_STRING -> R.string.diag_kind_localized_string
                }
            }

        /**
         * `resourceName` → `R.string.*`。由 [resIdFor] 展開，所以真正的
         * 完整性保證在那個 exhaustive `when` 上。
         */
        private val ID_BY_NAME: Map<String, Int> by lazy {
            val out = LinkedHashMap<String, Int>()
            for (code in DiagnosticCode.values()) {
                for (n in code.arity) out[code.resourceName(n)] = resIdFor(code, n)
            }
            out
        }

        /**
         * 沒有 `else` 分支 —— 這是本檔案存在的理由。
         *
         * @param argCount 只有 `unknown_field` 用得到（規範允許 `[field]` 與
         *                 `[field, suggestion]` 兩種寫法，各一份樣板）。
         */
        internal fun resIdFor(code: DiagnosticCode, argCount: Int): Int = when (code) {
            DiagnosticCode.FATAL_YAML_SYNTAX -> R.string.diag_fatal_yaml_syntax
            DiagnosticCode.FATAL_ROOT_NOT_MAPPING -> R.string.diag_fatal_root_not_mapping
            DiagnosticCode.FATAL_FORMAT_MISSING -> R.string.diag_fatal_format_missing
            DiagnosticCode.FATAL_FORMAT_MALFORMED -> R.string.diag_fatal_format_malformed
            DiagnosticCode.FATAL_FORMAT_KIND_MISMATCH -> R.string.diag_fatal_format_kind_mismatch
            DiagnosticCode.FATAL_FORMAT_MAJOR_UNSUPPORTED ->
                R.string.diag_fatal_format_major_unsupported
            DiagnosticCode.FATAL_ID_MISSING -> R.string.diag_fatal_id_missing
            DiagnosticCode.FATAL_ID_INVALID -> R.string.diag_fatal_id_invalid
            DiagnosticCode.FATAL_ID_MISMATCH -> R.string.diag_fatal_id_mismatch
            DiagnosticCode.FATAL_DOCUMENT_NOT_FOUND -> R.string.diag_fatal_document_not_found
            DiagnosticCode.FATAL_PARENT_NOT_FOUND -> R.string.diag_fatal_parent_not_found
            DiagnosticCode.FATAL_INHERITS_CYCLE -> R.string.diag_fatal_inherits_cycle
            DiagnosticCode.FATAL_INHERITS_TOO_DEEP -> R.string.diag_fatal_inherits_too_deep
            DiagnosticCode.FATAL_MIN_CLIENT -> R.string.diag_fatal_min_client
            DiagnosticCode.FATAL_LAYERS_MISSING -> R.string.diag_fatal_layers_missing
            DiagnosticCode.FATAL_DEFAULT_LAYER_UNKNOWN -> R.string.diag_fatal_default_layer_unknown
            DiagnosticCode.FATAL_ALPHA_LAYER_UNKNOWN -> R.string.diag_fatal_alpha_layer_unknown
            DiagnosticCode.FATAL_LAYER_EMPTY -> R.string.diag_fatal_layer_empty

            DiagnosticCode.UNKNOWN_FIELD ->
                if (argCount >= 2) R.string.diag_unknown_field_2 else R.string.diag_unknown_field
            DiagnosticCode.DUPLICATE_KEY -> R.string.diag_duplicate_key
            DiagnosticCode.TYPE_MISMATCH -> R.string.diag_type_mismatch
            DiagnosticCode.BAD_BOOL -> R.string.diag_bad_bool
            DiagnosticCode.BAD_NUMBER -> R.string.diag_bad_number
            DiagnosticCode.OUT_OF_RANGE -> R.string.diag_out_of_range
            DiagnosticCode.BAD_ENUM -> R.string.diag_bad_enum
            DiagnosticCode.BAD_COLOR -> R.string.diag_bad_color
            DiagnosticCode.PALETTE_NOT_SCALAR -> R.string.diag_palette_not_scalar
            DiagnosticCode.PALETTE_BAD_COLOR -> R.string.diag_palette_bad_color
            DiagnosticCode.PALETTE_UNRESOLVED_REF -> R.string.diag_palette_unresolved_ref
            DiagnosticCode.PALETTE_SELF_REFERENCE -> R.string.diag_palette_self_reference
            DiagnosticCode.PALETTE_CYCLE_OR_TOO_DEEP -> R.string.diag_palette_cycle_or_too_deep
            DiagnosticCode.ENTRY_DROPPED -> R.string.diag_entry_dropped
            DiagnosticCode.ASSET_INCOMPLETE -> R.string.diag_asset_incomplete
            DiagnosticCode.ASSET_PATH_ESCAPE -> R.string.diag_asset_path_escape
            DiagnosticCode.UNKNOWN_SCRIPT_TAG -> R.string.diag_unknown_script_tag
            DiagnosticCode.UNKNOWN_ICON -> R.string.diag_unknown_icon
            DiagnosticCode.UNKNOWN_ACTION -> R.string.diag_unknown_action
            DiagnosticCode.BAD_ACTION_ARGUMENT -> R.string.diag_bad_action_argument
            DiagnosticCode.TOOLBAR_ITEM_NO_TAP -> R.string.diag_toolbar_item_no_tap
            DiagnosticCode.STATUS_ITEM_NO_SOURCE -> R.string.diag_status_item_no_source
            DiagnosticCode.NESTED_PLATFORM_OVERRIDES -> R.string.diag_nested_platform_overrides

            DiagnosticCode.REQUIRED_ITEM_RESTORED -> R.string.diag_required_item_restored
            DiagnosticCode.DEPRECATED_FIELD -> R.string.diag_deprecated_field
            DiagnosticCode.FEATURE_UNSUPPORTED -> R.string.diag_feature_unsupported
            DiagnosticCode.LEGACY_BLOCK_IGNORED -> R.string.diag_legacy_block_ignored

            DiagnosticCode.UNKNOWN_KEYSYM -> R.string.diag_unknown_keysym
            DiagnosticCode.UNKNOWN_MODIFIER -> R.string.diag_unknown_modifier
            DiagnosticCode.UNKNOWN_SWIPE_DIRECTION -> R.string.diag_unknown_swipe_direction
            DiagnosticCode.MUTUALLY_EXCLUSIVE -> R.string.diag_mutually_exclusive
            DiagnosticCode.SEND_INCOMPLETE -> R.string.diag_send_incomplete
            DiagnosticCode.ROW_WIDTH_MISMATCH -> R.string.diag_row_width_mismatch
            DiagnosticCode.KEY_PATCH_NO_TARGET -> R.string.diag_key_patch_no_target
            DiagnosticCode.ACTION_TARGET_MISSING -> R.string.diag_action_target_missing
            DiagnosticCode.AUTO_FOR_SCHEMA_WILDCARD -> R.string.diag_auto_for_schema_wildcard
            DiagnosticCode.SYLLABLES_SLOT_UNKNOWN -> R.string.diag_syllables_slot_unknown
            DiagnosticCode.USER_REMAP_UNAPPLICABLE -> R.string.diag_user_remap_unapplicable
        }
    }
}
