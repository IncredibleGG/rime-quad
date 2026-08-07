# 隨附資料目錄（shared_data_dir）

本目錄的內容會在首次啟動時被解到 `filesDir/rime/shared/`，
並以 `rs_setup.shared_data_dir` 傳給 `rs_init()`。

目前只有這個佔位檔。真正的隨附 schema／詞庫、以及
`core/themes/`、`core/layouts/` 產出的 YAML，都要放進這裡。

放入新檔案後，記得把 `RimeRuntime.ASSET_REVISION` 加一，
否則已安裝的裝置不會重新解壓。
