# 隨附資料目錄（shared_data_dir / user_data_dir）

APK 內的 `assets/rime/` 有兩個子目錄，兩者**都不在本目錄底下**，
而是由 `app/build.gradle.kts` 的 `syncRimeData` 任務在建置時，
從版控外的 `core/data/` 同步進 `build/generated/rimeAssets/`：

| APK 內路徑            | 來源                    | 對應 `rs_setup` 欄位 |
|-----------------------|-------------------------|----------------------|
| `assets/rime/shared/` | `core/data/shared/`     | `shared_data_dir`    |
| `assets/rime/user/`   | `core/data/user/`       | `user_data_dir`      |

`core/data/` 由 `scripts/collect_data.sh` 產生，13MB，刻意不進版控。
**新 clone 的機器要先跑那支腳本**，否則 APK 內不會有任何 schema，
librime 部署會失敗（建置時 Gradle 會先印警告）。

執行期行為見 `RimeRuntime`：

- `shared` 每次 `ASSET_REVISION` 變動就整個重解。
- `user` 只在檔案不存在時「種下」初始內容，**不覆蓋**使用者改過的
  `default.custom.yaml`。
- 改動 assets 內容後記得把 `RimeRuntime.ASSET_REVISION` 加一，
  否則已安裝的裝置不會重新解壓。
