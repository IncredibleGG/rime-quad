#!/usr/bin/env bash
#
# build_schema_store.sh — 方案市集的伺服器側流水線
#
#   列舉上游 → clone → 解析相依 → 打包 zip → **在模擬器上真的部署一次** → 產索引 → 上傳 R2
#
# 索引格式與品質閘門的規範在 docs/schema-store.md。本腳本是那份規範的產生端；
# 其中最重要的一條是 §1 的「verified.deployed 為 false 或缺漏的套件不得進索引」——
# 也就是每個進索引的套件，都必須真的被 librime 部署成功過一次。
#
# 為什麼要在模擬器上跑：相依關係只靠讀 yaml 是驗不完的。我們自己就中過一次
# （rime-bopomofo 的詞典是 terra_pinyin 不是 luna_pinyin，另外還要 stroke 做筆畫反查）。
# 驗證時 user_data_dir 只放「這個套件 + 它宣告的相依」，shared_data_dir 只有 opencc，
# 所以少宣告任何一個相依，這裡一定會炸。
#
# 用法:
#   ./scripts/build_schema_store.sh                 # 跑完整流程（不上傳）
#   ./scripts/build_schema_store.sh --upload        # 跑完整流程並上傳 R2
#   ./scripts/build_schema_store.sh --phase verify  # 只跑某一階段
#   ./scripts/build_schema_store.sh --phase verify --force   # 忽略已有結果重驗
#   ./scripts/build_schema_store.sh --jobs 2        # 模擬器上的平行度
#
# 階段（可單獨執行，中間產物都留在 build/schema-store/_work，可續跑）:
#   fetch    列舉 rime 組織的 rime-* 與 rppi 官方索引收錄的第三方庫，淺層 clone
#   analyze  解析相依、授權、分類、建議佈局 → _work/packages.json
#   pack     攤平成 zip（規範 §2）→ build/schema-store/*.zip
#   verify   在模擬器上逐一部署（品質閘門）→ _work/verify/<id>.json
#   index    產生 index.json，未通過驗證者一律排除
#   test     撞號偵測與語言標記的測試（拿真實的 34 個套件跑）
#   upload   上傳 R2 並用 curl 驗證對外網址真的取得到
#
# 實作說明：各階段的 Python 放在 scripts/schema_store/ 底下。這裡不用單一巨型
# bash 檔，是因為相依解析與索引產生有相當份量的邏輯，塞進 heredoc 會失去
# 語法檢查與可讀性。
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="$ROOT/scripts/schema_store"
DIST="$ROOT/build/schema-store"          # 產物：zip 與 index.json
WORK="$DIST/_work"                       # 中間產物：clone、staging、驗證結果
SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
ADB="$SDK/platform-tools/adb"
# shellcheck source=lib/device.sh
. "$ROOT/scripts/lib/device.sh"

# R2 發布位置（見 /home/lc/R2-ACCESS.md）。我們只碰 rime/schemas/ 這條路徑。
R2_REMOTE="r2:tgapk/rime/schemas"
BASE_URL="https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/schemas/"

PHASE=all; JOBS=2; FORCE=""; UPLOAD=0; ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --phase)  PHASE="$2"; shift 2 ;;
    --jobs)   JOBS="$2"; shift 2 ;;
    --only)   ONLY="$2"; shift 2 ;;
    --force)  FORCE="--force"; shift ;;
    --upload) UPLOAD=1; shift ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "未知參數: $1" >&2; exit 2 ;;
  esac
done

die()  { echo "錯誤: $*" >&2; exit 1; }
step() { echo; echo "=== $* ==="; }

mkdir -p "$WORK" "$DIST"
command -v python3 >/dev/null || die "需要 python3"
python3 -c 'import yaml' 2>/dev/null || die "需要 PyYAML（pip install pyyaml）"
command -v zip >/dev/null || die "需要 zip"

run_phase() { [ "$PHASE" = all ] || [ "$PHASE" = "$1" ]; }

# ─────────────────────────────────────────────────────────────── fetch ────
if run_phase fetch; then
  step "1. 列舉上游"
  python3 "$LIB/gen_repos.py" "$WORK"

  step "2. clone（淺層，已存在就跳過）"
  mkdir -p "$WORK/upstream"
  python3 - "$WORK" <<'PY' | while read -r slug branch; do
import json, sys
for r in json.load(open(sys.argv[1] + "/repos.json")):
    print(r["repo"], r.get("branch") or "")
PY
    dir="$WORK/upstream/${slug//\//__}"
    if [ -d "$dir/.git" ]; then continue; fi
    args=(--depth 1)
    [ -n "$branch" ] && args+=(--branch "$branch")
    echo "  clone $slug"
    timeout 1200 git clone "${args[@]}" "https://github.com/$slug.git" "$dir" \
        >/dev/null 2>&1 || { echo "  [失敗] $slug"; rm -rf "$dir"; }
  done
fi

# ───────────────────────────────────────────────────────────── analyze ────
if run_phase analyze; then
  step "3. 解析相依 / 授權 / 分類"
  # APK 內建的 opencc 設定清單：套件引用到這些以外的就記下來（規範 §2 說 .ocd2 不重複打包）
  OPENCC_BUILTIN="$(ls "$ROOT/core/data/shared/opencc"/*.json 2>/dev/null \
                    | xargs -n1 basename 2>/dev/null | paste -sd, - || true)"
  python3 "$LIB/analyze.py" "$WORK" "$OPENCC_BUILTIN"
fi

# ──────────────────────────────────────────────────────────────── pack ────
if run_phase pack; then
  step "4. 打包（規範 §2：扁平、含 LICENSE 與 UPSTREAM.txt、無穿越路徑）"
  python3 "$LIB/pack.py" "$WORK" "$DIST"
fi

# ────────────────────────────────────────────────────────────── verify ────
if run_phase verify; then
  step "5. 品質閘門：在模擬器上逐一部署"
  [ -f "$ROOT/third_party/build/rime_console.x86_64" ] || \
    die "缺少 rime_console 執行檔，先跑 scripts/run_console_test.sh"
  [ -d "$ROOT/core/data/shared/opencc" ] || \
    die "缺少 core/data/shared/opencc，先跑 scripts/collect_data.sh"
  "$ROOT/scripts/emu.sh" status >/dev/null 2>&1 || "$ROOT/scripts/emu.sh" start
  # 這台機器上可能同時有多個模擬器（其他 agent 在用）。預設綁 emulator-5554，
  # 需要時以 RIME_EMU_SERIAL 覆寫；只有它不在時才退而挑第一個可用的。
  # ⛔ 這一段從前明著優先綁 emulator-5554,而那台是**別條線**的機器。
  #   第三個變數名 RIME_EMU_SERIAL 也一併收斂到 RIME_SERIAL:同一件事三個名字,
  #   帶對了一個而另外兩個不看,是「帶了也沒用」的另一種形狀。
  SERIAL="$(rs_pick_serial "$ADB")" || SERIAL=""
  [ -n "$SERIAL" ] || die "找不到模擬器"
  echo "  裝置 $SERIAL，平行度 $JOBS"
  python3 "$LIB/verify.py" --work "$WORK" --adb "$ADB" --serial "$SERIAL" \
      --binary "$ROOT/third_party/build/rime_console.x86_64" \
      --opencc "$ROOT/core/data/shared/opencc" \
      --jobs "$JOBS" ${FORCE:+$FORCE} ${ONLY:+--only "$ONLY"}
fi

# ─────────────────────────────────────────────────────────────── index ────
if run_phase index; then
  step "6. 產生 index.json（未通過驗證者一律排除）"
  python3 "$LIB/mkindex.py" "$WORK" "$DIST" "$BASE_URL"

  # 隨 APK 出貨的語言對照表。索引是下載來的、可能比 app 舊，而內建的四個方案
  # 根本不在索引的 packages 裡 —— 鍵盤類型選單在還沒下載過索引時也要分得了組。
  step "6b. 產生 core/schema-languages.json（隨 APK 出貨的語言對照）"
  # --max-unknown 0：語言標記判不出來的方案一個都不許有。目前確實是 0；
  # 哪天上游加了新東西讓它變成 1，這裡就會擋下來，而不是讓一個 und 悄悄
  # 混進選單的「其他」分組裡（使用者會以為清單裡沒有他要的東西）。
  python3 "$LIB/languages.py" "$WORK" \
      --asset "$ROOT/core/schema-languages.json" \
      --report "$WORK/languages-report.json" \
      --max-unknown 0

  step "6c. 重新產生測試語料（data/corpus.json）"
  # 語料是「真實的 34 個套件」脫水之後的版本，測試靠它才跑得動。
  # 每次產索引都重抽一次，它就不會腐爛成「以前某個人跑過一次」。
  python3 "$LIB/snapshot.py" "$WORK"
fi

# ──────────────────────────────────────────────────────────────── test ────
if run_phase test; then
  step "6d. 撞號與語言標記測試（真實語料 + 反向測試）"
  # --require-live：現場資料在的時候，「語料與現場相符」那兩條**必須真的跑**。
  # 沒有這個旗標，它們會 skip，而 skip 在總結裡長得跟通過很像。
  if [ -f "$WORK/packages.json" ]; then
    RIME_STORE_WORK="$WORK" python3 "$LIB/test_store.py" --require-live
  else
    python3 "$LIB/test_store.py"
  fi
fi

# ────────────────────────────────────────────────────────────── upload ────
if [ "$UPLOAD" -eq 1 ] || [ "$PHASE" = upload ]; then
  step "7. 上傳 R2"
  command -v rclone >/dev/null || die "需要 rclone"
  [ -f "$DIST/index.json" ] || die "沒有 index.json，先跑 index 階段"
  # 先傳 zip 再傳 index.json：index 指到的檔案必須先在。
  for f in "$DIST"/*.zip; do
    n="$(basename "$f")"
    echo "  → $n"
    rclone copyto "$f" "$R2_REMOTE/$n" --s3-no-check-bucket
  done
  # languages.json 是語言標記的逐條依據（誰標的、依據什麼）。先傳它再傳索引：
  # 索引裡的 language_source 只有一個字，要複查得靠這一份。
  # ⚠ 寫成 if 而不是 `[ -f x ] && cmd`：後者在檔案不存在時整條回傳 1，
  #    配上 set -e 會讓腳本在這裡無聲結束。
  if [ -f "$DIST/languages.json" ]; then
    echo "  → languages.json"
    rclone copyto "$DIST/languages.json" "$R2_REMOTE/languages.json" --s3-no-check-bucket
  fi
  rclone copyto "$DIST/index.json" "$R2_REMOTE/index.json" --s3-no-check-bucket

  step "8. 用 curl 驗證對外網址（不信 rclone 說成功）"
  # 失敗計數寫檔而不是用變數：下面的 check 跑在 pipeline 的子 shell 裡，
  # 變數加了也帶不回來（曾經因此差點把 404 當成通過）。
  FAILS="$WORK/upload_fails"; : > "$FAILS"
  check() {
    local url="$1" want="$2"
    local hdr code len
    hdr="$(curl -sS -m 60 -I "$url" || true)"
    code="$(printf '%s' "$hdr" | awk 'NR==1{print $2}')"
    len="$(printf '%s' "$hdr" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-length"{print $2}')"
    if [ "$code" = 200 ] && [ -n "$len" ] && [ "$len" = "$want" ]; then
      echo "  ✓ $code len=$len  $(basename "$url")"
    else
      echo "  ✗ code=$code len=${len:-?} 應為 $want  $url"
      echo "$url" >> "$FAILS"
    fi
  }
  # BASE_URL 結尾已經有斜線，這裡不能再加一個（多一個 "/" R2 會回 404）
  check "${BASE_URL}index.json" "$(stat -c%s "$DIST/index.json")"
  if [ -f "$DIST/languages.json" ]; then
    check "${BASE_URL}languages.json" "$(stat -c%s "$DIST/languages.json")"
  fi
  python3 - "$DIST/index.json" <<'PY' | while read -r f n; do
import json, sys
d = json.load(open(sys.argv[1]))
for p in d["packages"]:
    print(p["file"], p["size"])
PY
    check "$BASE_URL$f" "$n"
  done
  nfail="$(wc -l < "$FAILS")"
  [ "$nfail" -eq 0 ] || die "有 $nfail 個對外網址驗不過（清單見 $FAILS）"
  echo "  以上全部 HTTP 200 且 Content-Length 與本地檔案一致"
fi

echo
echo "=== 完成 ==="
[ -f "$DIST/index.json" ] && python3 - "$DIST/index.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
cats = {}
for p in d["packages"]:
    cats.setdefault(p["category"], []).append(p["id"])
print(f"index.json: {len(d['packages'])} 個套件")
for c in d["categories"]:
    ids = cats.get(c["id"], [])
    print(f"  {c['name']}（{c['id']}）{len(ids)}: {' '.join(ids)}")
print("base_url:", d["base_url"])
PY
