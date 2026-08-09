package org.luminakey.ime.prefs

import android.content.Context
import android.util.Log
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.MutablePreferences
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.runBlocking
import java.io.IOException

private val Context.userPrefsDataStore: DataStore<Preferences> by
    preferencesDataStore(name = "rime_user_prefs")

/**
 * 使用者偏好的儲存層。DataStore(Preferences) 背書。
 *
 * ── 「未設定」怎麼存 ────────────────────────────────────────────────────
 * 存 **key 不存在**。[write] 的實作是先 `clear()` 再把 [UserPrefs.toMap]
 * 產生的條目寫回去 —— 映射裡沒有的欄位，檔案裡就沒有那個 key。
 * 「回復預設」因此是真的把資料刪掉，而不是寫入一份當下主題值的副本。
 *
 * ── 為什麼需要一個同步快取 ──────────────────────────────────────────────
 * DataStore 只有非同步 API，但 `InputMethodService.onCreate()` 必須在
 * 第一次畫鍵盤之前就知道高度倍率是多少 —— 慢一拍的話使用者會看到鍵盤
 * 先用主題高度畫一次再跳成他設定的高度。所以這裡保留一份 [current]，
 * 首次存取以 `runBlocking` 取一次（檔案只有幾百 bytes），之後由 [flow]
 * 的收集者持續更新。
 *
 * ── 執行緒 ──────────────────────────────────────────────────────────────
 * [current] 是 `@Volatile` 的不可變值物件，任何執行緒讀都安全。
 * 寫入是 suspend 的，呼叫端自行決定在哪個 scope 執行。
 * 本類別**不呼叫任何 `rs_*`**，因此與 rime_shell.h 的單執行緒約定無關。
 */
class PrefsStore private constructor(private val store: DataStore<Preferences>) {

    @Volatile
    private var cached: UserPrefs? = null

    /**
     * 偏好的變更串流。每次寫入都會發出一個新值 —— IME 就是靠它做到
     * 「設定變更立即生效，不需要重啟輸入法」。
     */
    val flow: Flow<UserPrefs> = store.data
        .catch { e ->
            // 檔案損毀不該讓輸入法打不開字。退回全空（= 全部走主題值）。
            if (e is IOException) {
                Log.w(TAG, "讀取偏好失敗，本次視為未設定", e)
                emit(androidx.datastore.preferences.core.emptyPreferences())
            } else {
                throw e
            }
        }
        .map { decode(it) }
        .onEach { cached = it }

    /** 最近一次已知的偏好；首次呼叫會阻塞讀一次檔案。 */
    val current: UserPrefs
        get() = cached ?: runBlocking {
            val v = runCatching { decode(store.data.first()) }.getOrDefault(UserPrefs())
            cached = v
            v
        }

    /** 以既有值為基礎做局部修改。傳 `null` 給某欄位即為「回復該項預設」。 */
    suspend fun update(transform: (UserPrefs) -> UserPrefs) {
        val next = transform(current)
        write(next)
    }

    /** 全部回復預設 —— 真的把所有 key 刪掉。 */
    suspend fun resetAll() = write(UserPrefs())

    private suspend fun write(next: UserPrefs) {
        store.edit { p ->
            // clear() 之後只寫 toMap() 有的條目：這一行就是「未設定 = 沒有 key」
            // 的兌現處，也是 UserPrefsTest 在守的不變式。
            p.clear()
            encode(p, next)
        }
        cached = next
    }

    /* ─────────────────────── 編解碼 ─────────────────────── */

    private fun encode(p: MutablePreferences, v: UserPrefs) {
        v.toMap().forEach { (key, value) ->
            when (value) {
                is Float -> p[floatPreferencesKey(key)] = value
                is Int -> p[intPreferencesKey(key)] = value
                is Boolean -> p[booleanPreferencesKey(key)] = value
                is String -> p[stringPreferencesKey(key)] = value
                else -> Log.w(TAG, "無法序列化的偏好型別: $key=${value::class.java}")
            }
        }
    }

    private fun decode(p: Preferences): UserPrefs =
        UserPrefs.fromMap(p.asMap().entries.associate { (k, v) -> k.name to v })

    companion object {
        private const val TAG = "PrefsStore"

        @Volatile
        private var instance: PrefsStore? = null

        /**
         * 同一個行程內只能有一個 DataStore 實例指向同一個檔案（DataStore 自己
         * 會丟 `IllegalStateException`）。IME 服務與設定 Activity 在同一個
         * 行程，所以這裡必須是單例。
         */
        fun get(context: Context): PrefsStore = instance ?: synchronized(this) {
            instance ?: PrefsStore(context.applicationContext.userPrefsDataStore).also {
                instance = it
            }
        }
    }
}
