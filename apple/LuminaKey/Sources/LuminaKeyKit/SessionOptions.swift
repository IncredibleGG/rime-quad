//
//  SessionOptions.swift — 設定 → librime 的 session 選項
//
//  每次建立 session(切到別的 app、部署完成之後重建)都要重新套一次,
//  因為選項是掛在 session 上的,不是持久化的。
//
//  ⚠ 優先順序在 docs/settings-model.md §4,四端共用。
//

import Foundation

public enum SessionOptions {

    /// 要設哪些選項。**只回傳需要設的**:沒有出現在字典裡的選項一律不碰,
    /// 讓方案自己的預設值生效。
    ///
    /// 「不碰」與「設成 false」是兩件事:許多方案的 `ascii_punct` 預設是
    /// false,但也有方案根本沒有這個開關。無條件設 false 會讓
    /// 「跟著方案」這個選項變成「一律中文標點」,而使用者選的是前者。
    public static func resolve(settings: LuminaKeySettings,
                               inputModeScript: ScriptVariant) -> [String: Bool] {
        var out: [String: Bool] = [:]

        // ── 簡繁 ──────────────────────────────────────────
        // 1. 使用者在「文字」頁明確選了「一律繁體/簡體」→ 他說了算。
        // 2. 否則跟著輸入來源(這是預設)。
        // 3. 輸入來源認不出來 → 不碰。
        switch settings.variant {
        case .traditional: out["simplification"] = false
        case .simplified: out["simplification"] = true
        case .followInputMode:
            if settings.followInputMode,
               let v = InputModeBinding.simplificationOption(for: inputModeScript) {
                out["simplification"] = v
            }
        }

        switch settings.punctuation {
        case .followSchema: break
        case .full: out["ascii_punct"] = false
        case .half: out["ascii_punct"] = true
        }

        switch settings.shape {
        case .followSchema: break
        case .halfShape: out["full_shape"] = false
        case .fullShape: out["full_shape"] = true
        }

        return out
    }
}
