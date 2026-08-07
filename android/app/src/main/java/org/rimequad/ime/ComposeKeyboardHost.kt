package org.rimequad.ime

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
 * 少掛任何一個，ComposeView 在 attach 時就會直接丟例外。
 *
 * 注意：[LifecycleRegistry] 進入 DESTROYED 後不可復用，所以每次
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

    fun createView(content: @Composable () -> Unit): View {
        savedStateController.performRestore(null)
        lifecycleRegistry.currentState = Lifecycle.State.CREATED

        return ComposeView(context).apply {
            setViewTreeLifecycleOwner(this@ComposeKeyboardHost)
            setViewTreeViewModelStoreOwner(this@ComposeKeyboardHost)
            setViewTreeSavedStateRegistryOwner(this@ComposeKeyboardHost)
            setViewCompositionStrategy(
                ViewCompositionStrategy.DisposeOnViewTreeLifecycleDestroyed
            )
            setContent(content)
        }
    }

    fun onStartInput() {
        if (lifecycleRegistry.currentState != Lifecycle.State.DESTROYED) {
            lifecycleRegistry.currentState = Lifecycle.State.RESUMED
        }
    }

    fun onFinishInput() {
        if (lifecycleRegistry.currentState.isAtLeast(Lifecycle.State.CREATED)) {
            lifecycleRegistry.currentState = Lifecycle.State.CREATED
        }
    }

    fun onDestroy() {
        if (lifecycleRegistry.currentState != Lifecycle.State.DESTROYED) {
            lifecycleRegistry.currentState = Lifecycle.State.DESTROYED
        }
        store.clear()
    }
}
