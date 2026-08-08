//
//  StoreEngine.swift — 下載、安裝、啟用、移除的整條流程
//
//  這一層把 NetworkGate / ArchiveGuard / SchemaPreflight / RimeConfigPatch /
//  InstalledRegistry 串起來,**但自己不碰 librime**:部署經由 `Deployer`
//  這個介面。理由有二 ——
//
//    · 設定介面是**另一個行程**,它沒有 librime session,部署得請輸入法本體做
//      (見 IPC.swift)。
//    · 換掉 `Deployer` 就能在 CI 上完整跑一遍「下載→驗章→解壓→預檢→啟用→
//      失敗→回滾」,不需要真的建一次詞庫。
//
//  ⚠ **回滾的範圍是刻意的。** 安裝到一半失敗時,已經解壓成功的套件
//  **留在磁碟上不刪** —— 它們沒有被加進 schema_list,librime 不會碰它們,
//  留著的成本是零,而刪掉的成本是使用者要重新下載一次。
//

import Foundation

// MARK: - 部署

public enum DeployOutcome: Equatable, Sendable {
    case success(elapsedMs: Int)
    case failure(elapsedMs: Int, lastError: String)
    case timeout(elapsedMs: Int)
    case notStarted(reason: String)
}

public protocol Deployer {
    /// **同步阻塞**直到真的完成。`onTick` 給經過的毫秒數 ——
    /// librime 不提供百分比,唯一誠實的進度就是時間。
    func deployAndWait(onTick: @escaping (Int) -> Void) -> DeployOutcome
}

// MARK: - 進度與結果

public enum StoreProgress: Equatable, Sendable {
    case downloading(name: String, ordinal: Int, total: Int, read: Int64, bytes: Int64)
    case verifying(name: String)
    case extracting(name: String)
    case preflight
    case deploying(elapsedMs: Int)
    case rollingBack(reason: String)

    /// 0…1,負數 = 沒有百分比可言。**不要**為了畫面好看而假造一個。
    public var fraction: Double {
        if case .downloading(_, _, _, let read, let bytes) = self, bytes > 0 {
            return min(1, max(0, Double(read) / Double(bytes)))
        }
        return -1
    }
}

public struct StoreOutcome: Equatable, Sendable {
    public let ok: Bool
    public let message: T
    public let details: [String]
    /// 動過 schema_list 而且把它放回去了。
    public let rolledBack: Bool
    /// false = **回滾本身也失敗了**,使用者必須自己處理。
    public let recoverable: Bool

    public init(ok: Bool, message: T, details: [String] = [],
                rolledBack: Bool = false, recoverable: Bool = true) {
        self.ok = ok
        self.message = message
        self.details = details
        self.rolledBack = rolledBack
        self.recoverable = recoverable
    }
}

// MARK: - 引擎

public final class StoreEngine {

    public static let maxPackageBytes: Int64 = 128 * 1024 * 1024
    public static let maxSingleYamlBytes: Int64 = 8 * 1024 * 1024

    let userDir: URL
    let sharedDir: URL
    let workDir: URL
    public let registry: InstalledRegistry
    let deployer: Deployer

    public init(userDir: URL, sharedDir: URL, workDir: URL, deployer: Deployer) {
        self.userDir = userDir
        self.sharedDir = sharedDir
        self.workDir = workDir
        self.registry = InstalledRegistry(userDir: userDir)
        self.deployer = deployer
    }

    var searchDirs: [URL] { [userDir, sharedDir] }

    public var enabledSchemas: [String] { RimeConfigPatch.readSchemaList(userDir: userDir) }

    // MARK: - 安裝

    /// 下載並解壓 `plan` 裡的每一個套件。**不動 schema_list、不部署。**
    public func install(indexURL: String, index: SchemaIndex, plan: DependencyResolver.Plan,
                        progress: @escaping (StoreProgress) -> Void) -> StoreOutcome {
        var installedNow: [String] = []
        try? FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)

        for (i, pkg) in plan.toDownload.enumerated() {
            progress(.downloading(name: pkg.name, ordinal: i + 1, total: plan.count,
                                  read: 0, bytes: pkg.size))
            let url = NetworkGate.resolveURL(indexURL: indexURL, baseURL: index.baseURL,
                                             file: pkg.file)
            let tmp = workDir.appendingPathComponent("\(pkg.id).zip.part")
            let got: NetworkGate.Downloaded
            switch NetworkGate.download(url, to: tmp, maxBytes: StoreEngine.maxPackageBytes,
                                        purpose: .storePackage, label: pkg.name,
                                        onProgress: { read, total in
                                            progress(.downloading(name: pkg.name, ordinal: i + 1,
                                                                  total: plan.count,
                                                                  read: read,
                                                                  bytes: total > 0 ? total : pkg.size))
                                        }) {
            case .ok(let d): got = d
            case .err(let m, let blocked):
                try? FileManager.default.removeItem(at: tmp)
                return StoreOutcome(
                    ok: false,
                    message: blocked
                        ? T("連網開關是關閉的,已停止下載", "联网开关是关闭的,已停止下载",
                            "Networking is off — the download did not start")
                        : T("下載「\(pkg.name)」失敗:\(m)", "下载「\(pkg.name)」失败:\(m)",
                            "Downloading \(pkg.name) failed: \(m)"),
                    details: keptNote(installedNow))
            }

            progress(.verifying(name: pkg.name))
            // ⚠ sha256 不符 → **立刻刪檔並返回**。壓縮檔連 ArchiveGuard 都不會看到。
            guard got.sha256.caseInsensitiveCompare(pkg.sha256) == .orderedSame else {
                try? FileManager.default.removeItem(at: tmp)
                return StoreOutcome(
                    ok: false,
                    message: T("「\(pkg.name)」的內容與清單宣告的不符,已丟棄",
                               "「\(pkg.name)」的内容与清单声明的不符,已丢弃",
                               "\(pkg.name) does not match the checksum in the list and was discarded"),
                    details: ["索引宣告: \(pkg.sha256)", "實際下載: \(got.sha256)", "URL: \(url)"]
                        + keptNote(installedNow))
            }

            progress(.extracting(name: pkg.name))
            switch ArchiveGuard.extract(tmp, to: userDir, stagingParent: workDir) {
            case .rejected(let report):
                try? FileManager.default.removeItem(at: tmp)
                return StoreOutcome(
                    ok: false,
                    message: T("「\(pkg.name)」的內容沒有通過安全檢查,已整包拒絕",
                               "「\(pkg.name)」的内容没有通过安全检查,已整包拒绝",
                               "\(pkg.name) failed the safety checks and was rejected"),
                    details: report.rejections.map { $0.message.hant } + keptNote(installedNow))
            case .failed(let m):
                try? FileManager.default.removeItem(at: tmp)
                return StoreOutcome(
                    ok: false,
                    message: T("解壓「\(pkg.name)」失敗:\(m)", "解压「\(pkg.name)」失败:\(m)",
                               "Extracting \(pkg.name) failed: \(m)"),
                    details: keptNote(installedNow))
            case .ok(let files, _):
                try? FileManager.default.removeItem(at: tmp)
                registry.put(InstalledPackage(id: pkg.id, name: pkg.name, sha256: pkg.sha256,
                                              source: "store", requires: pkg.requires,
                                              files: files, schemas: pkg.schemas))
                installedNow.append(pkg.id)
            }
        }
        return StoreOutcome(ok: true,
                            message: T("已安裝 \(installedNow.count) 個套件",
                                       "已安装 \(installedNow.count) 个套件",
                                       "Installed \(installedNow.count) package(s)"))
    }

    private func keptNote(_ ids: [String]) -> [String] {
        ids.isEmpty ? []
            : ["已經成功安裝的套件保留在電腦上(未啟用):" + ids.joined(separator: "、")]
    }

    // MARK: - 啟用 / 停用

    /// 改 schema_list 並重新部署。失敗就把整份 `default.custom.yaml` 放回去。
    public func setEnabled(_ ids: [String], enabled: Bool,
                           progress: @escaping (StoreProgress) -> Void) -> StoreOutcome {
        guard !ids.isEmpty else {
            return StoreOutcome(ok: true, message: T("沒有變更", "没有变更", "Nothing changed"))
        }

        if enabled {
            progress(.preflight)
            // **動 schema_list 之前**先檢查。過不了的話什麼都沒改,不必回滾。
            let missing = SchemaPreflight.checkAll(schemaIds: ids, searchDirs: searchDirs)
            if !missing.isEmpty {
                return StoreOutcome(
                    ok: false,
                    message: T("缺少相依檔案,已停止(沒有動到已啟用的清單)",
                               "缺少依赖文件,已停止(没有动到已启用的清单)",
                               "Missing dependencies — stopped without changing the enabled list"),
                    details: missing.map { $0.message.hant })
            }
        }

        let snapshot = RimeConfigPatch.snapshot(userDir: userDir)
        let current = enabledSchemas
        let next: [String] = enabled
            ? current + ids.filter { !current.contains($0) }
            : current.filter { !ids.contains($0) }
        guard next != current else {
            return StoreOutcome(ok: true, message: T("沒有變更", "没有变更", "Nothing changed"))
        }

        let text = RimeConfigPatch.writeSchemaList(into: snapshot.text ?? "", ids: next)
        do { try RimeConfigPatch.write(text, userDir: userDir) } catch {
            return StoreOutcome(ok: false,
                                message: T("寫入設定檔失敗", "写入配置文件失败",
                                           "Could not write the configuration file"),
                                details: ["\(error)"])
        }

        // 使用者自己加的詞要對新啟用的方案生效,掛載點也在這裡補上。
        if enabled {
            for id in ids { _ = UserPhrases.mount(schemaId: id, userDir: userDir) }
        }

        return deployOrRollback(snapshot: snapshot, progress: progress, successMessage:
            enabled
                ? T("已啟用 \(ids.count) 個方案", "已启用 \(ids.count) 个方案",
                    "Enabled \(ids.count) schema(s)")
                : T("已停用 \(ids.count) 個方案", "已停用 \(ids.count) 个方案",
                    "Disabled \(ids.count) schema(s)"))
    }

    /// 重新排序。順序就是切換順序,第一個是預設方案。
    public func reorder(_ ids: [String], progress: @escaping (StoreProgress) -> Void) -> StoreOutcome {
        let snapshot = RimeConfigPatch.snapshot(userDir: userDir)
        let text = RimeConfigPatch.writeSchemaList(into: snapshot.text ?? "", ids: ids)
        do { try RimeConfigPatch.write(text, userDir: userDir) } catch {
            return StoreOutcome(ok: false, message: T("寫入設定檔失敗", "写入配置文件失败",
                                                      "Could not write the configuration file"))
        }
        return deployOrRollback(snapshot: snapshot, progress: progress,
                                successMessage: T("順序已更新", "顺序已更新", "Order updated"))
    }

    /// 只改每頁候選數。
    public func setPageSize(_ size: Int?, progress: @escaping (StoreProgress) -> Void) -> StoreOutcome {
        let snapshot = RimeConfigPatch.snapshot(userDir: userDir)
        let text = RimeConfigPatch.writePageSize(into: snapshot.text ?? "", size: size)
        do { try RimeConfigPatch.write(text, userDir: userDir) } catch {
            return StoreOutcome(ok: false, message: T("寫入設定檔失敗", "写入配置文件失败",
                                                      "Could not write the configuration file"))
        }
        return deployOrRollback(snapshot: snapshot, progress: progress,
                                successMessage: T("候選字數已更新", "候选字数已更新",
                                                  "Candidates per page updated"))
    }

    /// 純粹重新部署,不改任何設定。
    ///
    /// ⚠ 「重新部署」這顆按鈕**不可以**直接呼叫 `rs_deploy()`:它是非同步的,
    /// 直接呼叫會立刻返回,畫面上什麼都不會發生,而背景其實跑了十幾秒。
    /// 使用者只能猜它成功了沒 —— 那正是 Android 端真機回報過的那個 bug。
    public func redeploy(progress: @escaping (StoreProgress) -> Void) -> StoreOutcome {
        finish(deployer.deployAndWait { ms in progress(.deploying(elapsedMs: ms)) },
               snapshot: nil, progress: progress,
               successMessage: nil)
    }

    private func deployOrRollback(snapshot: RimeConfigPatch.Snapshot,
                                  progress: @escaping (StoreProgress) -> Void,
                                  successMessage: T) -> StoreOutcome {
        let outcome = deployer.deployAndWait { ms in progress(.deploying(elapsedMs: ms)) }
        return finish(outcome, snapshot: snapshot, progress: progress,
                      successMessage: successMessage)
    }

    private func finish(_ outcome: DeployOutcome, snapshot: RimeConfigPatch.Snapshot?,
                        progress: @escaping (StoreProgress) -> Void,
                        successMessage: T?) -> StoreOutcome {
        switch outcome {
        case .success(let ms):
            return StoreOutcome(ok: true,
                                message: successMessage
                                    ?? T("重新整理完成,花了 \(seconds(ms)) 秒",
                                         "重新整理完成,花了 \(seconds(ms)) 秒",
                                         "Finished in \(seconds(ms))s"))
        case .failure(let ms, let err):
            return rollback(snapshot, progress: progress, ms: ms, reason:
                err.isEmpty
                    // librime 部署失敗時不提供原因(coordination.md §4),
                    // 所以這裡只能說「失敗了」,不能假裝知道為什麼。
                    ? "librime 沒有提供失敗原因"
                    : err)
        case .timeout(let ms):
            return rollback(snapshot, progress: progress, ms: ms, reason: "超過時間上限")
        case .notStarted(let reason):
            return StoreOutcome(ok: false,
                                message: T("沒有辦法開始重新整理", "没有办法开始重新整理",
                                           "Could not start rebuilding"),
                                details: [reason])
        }
    }

    private func rollback(_ snapshot: RimeConfigPatch.Snapshot?,
                          progress: @escaping (StoreProgress) -> Void,
                          ms: Int, reason: String) -> StoreOutcome {
        guard let snapshot else {
            return StoreOutcome(ok: false,
                                message: T("重新整理失敗", "重新整理失败", "Rebuilding failed"),
                                details: [reason])
        }
        progress(.rollingBack(reason: reason))
        let restored = RimeConfigPatch.restore(snapshot, userDir: userDir)
        let second = deployer.deployAndWait { _ in }
        let recovered: Bool
        if case .success = second { recovered = true } else { recovered = false }
        return StoreOutcome(
            ok: false,
            message: recovered
                ? T("失敗了,已經回到原本的設定", "失败了,已经回到原本的设置",
                    "It failed — your previous settings have been restored")
                : T("失敗了,而且回復原本設定時也失敗。請重新啟動輸入法。",
                    "失败了,而且恢复原本设置时也失败。请重新启动输入法。",
                    "It failed, and restoring your previous settings also failed. Please restart the input method."),
            details: [reason] + (restored ? [] : ["設定檔還原失敗"]),
            rolledBack: true,
            recoverable: recovered)
    }

    private func seconds(_ ms: Int) -> String {
        let tenths = (ms + 50) / 100
        return "\(tenths / 10).\(tenths % 10)"
    }

    // MARK: - 移除

    public func uninstall(_ packageId: String,
                          progress: @escaping (StoreProgress) -> Void) -> StoreOutcome {
        guard let pkg = registry.get(packageId) else {
            return StoreOutcome(ok: false, message: T("「\(packageId)」不在已安裝清單裡",
                                                      "「\(packageId)」不在已安装清单里",
                                                      "\(packageId) is not installed"))
        }
        let deps = registry.dependents(of: packageId)
        guard deps.isEmpty else {
            return StoreOutcome(
                ok: false,
                message: T("「\(pkg.name)」被其他已安裝的套件依賴,不能移除",
                           "「\(pkg.name)」被其他已安装的套件依赖,不能移除",
                           "\(pkg.name) is required by other installed packages"),
                details: deps.map { "\($0.name)(\($0.id))需要它" })
        }
        let toDisable = pkg.schemaIds.filter { enabledSchemas.contains($0) }
        if !toDisable.isEmpty {
            let r = setEnabled(toDisable, enabled: false, progress: progress)
            guard r.ok else { return r }
        }
        var deleted = 0
        let root = userDir.resolvingSymlinksInPath()
        for name in pkg.files {
            let f = userDir.appendingPathComponent(name)
            // 帳本本身也在磁碟上,可能被竄改。刪之前再驗一次範圍。
            guard ArchiveGuard.isInside(f, root: root) else { continue }
            var isDir: ObjCBool = false
            guard FileManager.default.fileExists(atPath: f.path, isDirectory: &isDir),
                  !isDir.boolValue else { continue }
            if (try? FileManager.default.removeItem(at: f)) != nil { deleted += 1 }
        }
        registry.remove(packageId)
        return StoreOutcome(ok: true,
                            message: T("已移除「\(pkg.name)」(刪除 \(deleted) 個檔案)",
                                       "已移除「\(pkg.name)」(删除 \(deleted) 个文件)",
                                       "Removed \(pkg.name) (\(deleted) files deleted)"))
    }
}
