package org.luminakey.ime

import android.content.Context
import android.view.View
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.ViewCompositionStrategy
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import androidx.lifecycle.ViewModelStore
import androidx.lifecycle.ViewModelStoreOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.lifecycle.setViewTreeViewModelStoreOwner
import androidx.savedstate.SavedStateRegistry
import androidx.savedstate.SavedStateRegistryController
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

/**
 * 在 [android.inputmethodservice.InputMethodService] 裡跑 Compose 的必要膠水。
 *
 * Compose 需要 view tree 上掛著 LifecycleOwner / ViewModelStoreOwner /
 * SavedStateRegistryOwner，而 Service 天生沒有這些（Activity 才有）。
 *
 * ⚠ 只掛在自己的 ComposeView 上是**不夠的**，這點踩過坑：
 *
 *   InputMethodService.setInputView() 會把我們的 view 塞進 IME 自己的
 *   視窗階層（android:id/parentPanel 那個 LinearLayout）底下。Compose 建立
 *   window recomposer 時，是從**視窗的 decor view** 往下找 ViewTreeLifecycleOwner，
 *   不是從 ComposeView 自己往上找。少掛 decor view 就會在 attach 當下丟：
 *
 *     java.lang.IllegalStateException: ViewTreeLifecycleOwner not found from
 *     android.widget.LinearLayout{... android:id/parentPanel}
 *
 *   所以 decor view 與 ComposeView 兩邊都要掛。
 *
 * 另注意：[LifecycleRegistry] 進入 DESTROYED 後不可復用，所以每次
 * `onCreateInputView()` 都要 new 一個 host，舊的呼叫 [onDestroy]。
 */
class ComposeKeyboardHost(private val context: Context) :
    LifecycleOwner, ViewModelStoreOwner, SavedStateRegistryOwner {

    private val lifecycleRegistry = LifecycleRegistry(this)
    private val store = ViewModelStore()
    private val savedStateController = SavedStateRegistryController.create(this)

    override val lifecycle: Lifecycle get() = lifecycleRegistry
    override val viewModelStore: ViewModelStore get() = store
    override val savedStateRegistry: SavedStateRegistry
        get() = savedStateController.savedStateRegistry

    /**
     * @param decorView IME 視窗的 decor view（`InputMethodService.getWindow()?.window?.decorView`）。
     *                  必須傳，理由見類別註解。
     */
    fun createView(decorView: View?, content: @Composable () -> Unit): View {
        savedStateController.performRestore(null)
        // Compose 的 recomposer 要等 lifecycle 至少到 CREATED 才會跑；
        // 這裡直接給到 RESUMED，onStartInputView 之後維持不變。
        lifecycleRegistry.currentState = Lifecycle.State.RESUMED

        decorView?.let { attachOwners(it) }

        return ComposeView(context).apply {
            attachOwners(this)
            setViewCompositionStrategy(
                ViewCompositionStrategy.DisposeOnViewTreeLifecycleDestroyed
            )
            setContent(content)
        }
    }

    private fun attachOwners(view: View) {
        view.setViewTreeLifecycleOwner(this)
        view.setViewTreeViewModelStoreOwner(this)
        view.setViewTreeSavedStateRegistryOwner(this)
    }

    fun onStartInput() {
        if (lifecycleRegistry.currentState != Lifecycle.State.DESTROYED) {
            lifecycleRegistry.currentState = Lifecycle.State.RESUMED
        }
    }

    fun onFinishInput() {
        // 刻意不降到 CREATED：IME 視窗只是隱藏，Compose 的 composition
        // 還要留著，下次 showWindow 才不用整棵重建。
    }

    fun onDestroy() {
        if (lifecycleRegistry.currentState != Lifecycle.State.DESTROYED) {
            lifecycleRegistry.currentState = Lifecycle.State.DESTROYED
        }
        store.clear()
    }
}
