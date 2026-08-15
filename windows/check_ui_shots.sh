#!/usr/bin/env bash
#
# windows/check_ui_shots.sh — 設定視窗的截圖**真的拍到畫面了嗎**
#
# ⚠ **一張全黑的圖比沒有圖更糟。** 它在 artifact 上看起來像有交付,
#   而沒有人會去打開它 —— 這條線今晚已經被「寫下來但沒有接上」花掉三輪,
#   而「拍了但拍到的是黑的」是同一個形狀的第四種。所以這一支存在的
#   唯一理由是:**拍不到要紅**,而且要說出是哪一張、為什麼。
#
#   用法:
#     windows/check_ui_shots.sh <目錄>      # 判準(CI 用的就是這一行)
#     windows/check_ui_shots.sh --self-check # 反向測試:每一種壞法都要真的紅
#
#   要求(十張,五頁 × 深淺兩份):
#     1. `settings-p<0..4>-<light|dark>.bmp` 十個檔名一個都不能少
#     2. 每一張大小 > 0,而且是真的 BMP(magic 'BM',尺寸合理)
#     3. 每一張**不是整片同一個顏色**(PrintWindow 對自繪內容全黑的
#        已知風險,見 setup/setup_main.cc 的 CaptureWindow 檔頭)
#     4. 同一頁的深色與淺色**必須不一樣** —— 一樣代表「深色沒切成功」,
#        而那在 artifact 上與「深色沒做壞」分不出來
#
# ⚠ 為什麼是一支獨立的腳本,不寫進 verify_installer.sh:
#   那支的結束碼要回答「安裝/解除安裝對不對」。把「畫面拍到了嗎」混進
#   同一個結束碼,紅起來就分不出是哪一件事壞了 —— 而分不出來的紅
#   會被當成雜訊、然後被關掉。這裡各自佔一個結束碼、在 GitHub 上各自
#   佔一列。
#
# ⚠ 為什麼要 python:第 3 條要逐像素看。BMP 每一列會補到 4 的倍數,
#   而補到幾與寬度有關 —— 用 od/awk 分組會在某些寬度下錯位,而錯位的
#   症狀是「全黑的圖被判成有內容」(假綠)。找不到 python 就**紅**,
#   不是跳過:跳過等於第 3 條不存在,而沒有人會發現。
set -euo pipefail

red()  { printf '\033[1;31m  !! %s\033[0m\n' "$*" >&2; }
ok()   { printf '  ✓ %s\n' "$*"; }
log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

PY=""
for c in python3 python; do
  if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done

MODES="light dark"
PAGES="0 1 2 3 4"

# 逐像素判斷:回傳 "uniform" / "varied" / "bad:<原因>"。
bmp_verdict() {
  "${PY}" - "$1" <<'PYEOF'
import struct, sys

path = sys.argv[1]
try:
    data = open(path, 'rb').read()
except OSError as e:
    print('bad:讀不到(%s)' % e); raise SystemExit(0)

if len(data) < 54:
    print('bad:只有 %d 位元組,連 BMP 檔頭都不夠' % len(data)); raise SystemExit(0)
if data[:2] != b'BM':
    print('bad:開頭不是 BM(不是 BMP)'); raise SystemExit(0)

offbits = struct.unpack_from('<I', data, 10)[0]
w, h = struct.unpack_from('<ii', data, 18)
bpp = struct.unpack_from('<H', data, 28)[0]
if bpp != 24:
    print('bad:%d bpp,這支只認得 24-bit' % bpp); raise SystemExit(0)
if w <= 0 or h == 0:
    print('bad:尺寸是 %dx%d' % (w, h)); raise SystemExit(0)
# 設定視窗最小 660x460 DIP。比這小很多的話不是視窗,是別的東西。
if w < 200 or abs(h) < 200:
    print('bad:%dx%d —— 太小,不像是設定視窗' % (w, abs(h))); raise SystemExit(0)

rows = abs(h)
stride = ((w * 3) + 3) & ~3
need = offbits + stride * rows
if len(data) < need:
    print('bad:像素資料不完整(要 %d 位元組,只有 %d)' % (need, len(data)))
    raise SystemExit(0)

first = data[offbits:offbits + 3]
for y in range(rows):
    base = offbits + y * stride
    row = data[base:base + w * 3]
    # 一整列都是同一個像素嗎 —— 不是就可以停了。
    if row != first * w:
        print('varied'); raise SystemExit(0)
print('uniform')
PYEOF
}

check_dir() {
  local dir="$1"
  local bad=0
  log "檢查截圖:${dir}"

  if [ -z "${PY}" ]; then
    red "找不到 python3 / python —— 「整張同一個顏色」那一條驗不了。
     這一條**不能跳過**:跳過等於它不存在,而全黑的圖看起來像有交付。"
    return 1
  fi
  if [ ! -d "${dir}" ]; then
    red "目錄不存在:${dir}"
    return 1
  fi

  local mode page f n=0
  for mode in ${MODES}; do
    for page in ${PAGES}; do
      f="${dir}/settings-p${page}-${mode}.bmp"
      n=$((n + 1))
      if [ ! -f "${f}" ]; then
        red "少了 $(basename "${f}") —— 那一頁根本沒拍到
     (視窗沒開出來?換頁的訊息沒送到?見同一個目錄裡的 settings-shot-${mode}.log)"
        bad=1
        continue
      fi
      if [ ! -s "${f}" ]; then
        red "$(basename "${f}") 大小是 0 —— 檔案建了但一個位元組都沒寫進去"
        bad=1
        continue
      fi
      local v
      v="$(bmp_verdict "${f}")"
      case "${v}" in
        varied)
          ok "$(basename "${f}") $(wc -c < "${f}" | tr -d ' ') 位元組,有內容" ;;
        uniform)
          red "$(basename "${f}") **整張同一個顏色**。
     PrintWindow 對自繪內容可能整片全黑(setup_main.cc 的已知風險),
     而一張全黑的圖在 artifact 上看起來像有交付 —— 所以它是紅的,不是證據。"
          bad=1 ;;
        *)
          red "$(basename "${f}") ${v#bad:}"
          bad=1 ;;
      esac
    done
  done

  # ── 深色真的切過去了嗎 ────────────────────────────────────────
  #
  # ⚠ 這一條是「不要靜靜只出淺色」的守門。深色走的是產品自己的
  #   `appearance.appearance = dark`,而它有沒有生效這裡看得出來:
  #   同一頁的兩張如果**位元組完全一樣**,那就是同一個佈景拍了兩次。
  for page in ${PAGES}; do
    local l="${dir}/settings-p${page}-light.bmp"
    local d="${dir}/settings-p${page}-dark.bmp"
    [ -f "${l}" ] && [ -f "${d}" ] || continue
    if cmp -s "${l}" "${d}"; then
      red "第 ${page} 頁的深色與淺色**是同一張圖** —— 深色沒有切過去。
     (verify_installer.sh §12s 寫的是 appearance.appearance = dark;
      服務沒讀到?還是高對比壓過去了?)"
      bad=1
    else
      ok "第 ${page} 頁:深淺兩張不一樣(深色真的切過去了)"
    fi
  done

  if [ "${bad}" -ne 0 ]; then
    red "截圖沒有通過。${n} 個檔名裡有問題的見上面。"
    return 1
  fi
  ok "十張截圖都拍到畫面,而且深淺兩份不一樣"
  return 0
}

# ── 反向測試 ──────────────────────────────────────────────────────
#
# ⚠ 這一支自己也要被守。「守門的東西沒有人守」是這個專案的老毛病
#   (verify_product_ids.sh 6/6 全綠、check_ui_spec.sh 的 W29 過期後
#    蓋住訊號,兩個前科)。下面每一種壞法都**必須真的紅**。
make_bmp() {
  # make_bmp <路徑> <寬> <高> <uniform|varied> [色偏移]
  "${PY}" - "$1" "$2" "$3" "$4" "${5:-0}" <<'PYEOF'
import struct, sys
path, w, h, kind, shift = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
stride = ((w * 3) + 3) & ~3
px = bytearray()
for y in range(h):
    row = bytearray()
    for x in range(w):
        if kind == 'uniform':
            row += bytes((12, 12, 12))
        else:
            v = (x * 7 + y * 3 + shift) % 256
            row += bytes((v, (v * 3) % 256, (v * 5) % 256))
    row += b'\0' * (stride - w * 3)
    px += row
off = 54
fh = b'BM' + struct.pack('<IHHI', off + len(px), 0, 0, off)
ih = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(px), 2835, 2835, 0, 0)
open(path, 'wb').write(fh + ih + bytes(px))
PYEOF
}

self_check() {
  if [ -z "${PY}" ]; then
    red "--self-check 需要 python3 / python"
    return 1
  fi
  local root
  root="$(mktemp -d)"
  # shellcheck disable=SC2064
  trap "rm -rf '${root}'" RETURN
  local bad=0

  # 每一個植入:名字|準備函式。準備函式先造一份**好的**十張,再弄壞一處。
  seed_good() {
    local d="$1" mode page
    mkdir -p "${d}"
    for mode in ${MODES}; do
      for page in ${PAGES}; do
        # 深淺兩份用不同的色偏移 —— 不然它們會位元組相同,
        # 而那正是下面 `深淺同一張` 那一條要抓的東西。
        local shift=0
        [ "${mode}" = "dark" ] && shift=97
        make_bmp "${d}/settings-p${page}-${mode}.bmp" 660 460 varied \
                 $((shift + page))
      done
    done
  }

  # 先證明「好的那一份」是綠的。少了這一條,下面每一條都可能因為
  # **不相干的原因**而紅,而那時反向測試全部通過卻什麼都沒證明。
  local good="${root}/good"
  seed_good "${good}"
  if check_dir "${good}" >/dev/null 2>&1; then
    ok "現況(十張都好)→ 綠"
  else
    red "現況應該是綠的,但它紅了 —— 下面的反向測試不算數"
    check_dir "${good}" || true
    bad=1
  fi

  # run_case <名字> <在該目錄裡執行的指令字串>
  # ⚠ 每一個 case 都從一份**全新的、好的**十張長出來,只弄壞一處 ——
  #   共用同一份目錄的話,前一個 case 弄壞的東西會讓後一個 case 假紅。
  local n_case=0
  run_case() {
    local name="$1" cmd="$2"
    n_case=$((n_case + 1))
    local d="${root}/case${n_case}"
    seed_good "${d}"
    ( cd "${d}" && eval "${cmd}" )
    if check_dir "${d}" >/dev/null 2>&1; then
      red "植入「${name}」之後仍然是綠的 —— 這一條守門是假的"
      bad=1
    else
      ok "「${name}」→ 紅"
    fi
    rm -rf "${d}"
  }

  run_case "少一張(p3-dark 不見了)" \
           'rm -f settings-p3-dark.bmp'
  run_case "大小是 0(p0-light)" \
           ': > settings-p0-light.bmp'
  run_case "不是 BMP(開頭兩個位元組被改掉)" \
           "printf 'XX' | dd of=settings-p1-light.bmp bs=1 seek=0 conv=notrunc status=none"
  run_case "被截斷(像素資料不完整)" \
           "${PY} -c \"import io;d=io.open('settings-p2-dark.bmp','rb').read()[:200];io.open('settings-p2-dark.bmp','wb').write(d)\""
  run_case "整張同一個顏色(p4-light 全黑)" \
           "make_bmp settings-p4-light.bmp 660 460 uniform"
  # 深色沒切過去:把 dark 換成與 light 位元組相同的那一張。
  run_case "深色與淺色是同一張(p2)" \
           'cp settings-p2-light.bmp settings-p2-dark.bmp'
  run_case "尺寸太小,不像設定視窗(p0-dark 是 40x40)" \
           "make_bmp settings-p0-dark.bmp 40 40 varied 5"

  if [ "${bad}" -ne 0 ]; then
    red "反向測試沒有全過 —— 這一支守門不算數"
    return 1
  fi
  log "反向測試全部通過:每一種壞法都真的會紅"
  return 0
}

case "${1:-}" in
  --self-check) self_check ;;
  "") echo "用法: $0 <截圖目錄> | $0 --self-check" >&2; exit 2 ;;
  *)  check_dir "$1" ;;
esac
