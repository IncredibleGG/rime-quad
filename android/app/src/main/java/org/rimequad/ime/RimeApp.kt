package org.rimequad.ime

import android.app.Application
import org.rimequad.ime.net.NetworkAudit

/**
 * 行程進入點。
 *
 * 存在的唯一理由是**時機**：連網開關與連網紀錄必須在任何一個 Activity、
 * 任何一個 IME 服務之前就接好，否則會出現「行程剛起來的那幾毫秒政策還沒
 * 裝上」的空窗。這裡是 Android 保證最早跑到的地方。
 *
 * 就算這個類別哪天被誤刪、或 manifest 上的 `android:name` 掉了，
 * [org.rimequad.ime.net.NetworkGate.policy] 的初值是「拒絕」，
 * 行為會退化成**完全離線**而不是完全開放。這一點是刻意的：
 * 接線出錯的後果只能是更保守，不能是更開放。
 *
 * 這裡刻意什麼都不做 —— 不初始化 librime、不解壓 assets。那些是
 * [org.rimequad.ime.core.RimeRuntime] 的事，而且它們慢，壓在
 * `Application.onCreate` 上會拖慢每一次冷啟動（包含只是被叫起來打字的那次）。
 */
class RimeApp : Application() {
    override fun onCreate() {
        super.onCreate()
        NetworkAudit.install(this)
    }
}
