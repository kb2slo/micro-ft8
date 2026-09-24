#!/usr/bin/env bash
# Rebuild every decoder variant scored in RFC 0006 section 4.1, then score them.
#
#   tools/decode_bench/build_variants.sh OUT_DIR
#
# Needs: git, make, a C compiler, python3, and jt9 (WSJT-X) on PATH.
# Throwaway, like compare_decoders.py; B25 replaces both with the real harness.
#
# Every variant is howard0su/ft8_lib at a pinned SHA, changed by one sed recipe, so
# each number in the RFC traces to an exact diff. The subtraction fix (sub_minus8)
# is the base for the ablations: the upstream macro is undefined behaviour and its
# result depends on compiler and CPU (RFC 0006 section 4.1).

set -euo pipefail

OUT=${1:?usage: build_variants.sh OUT_DIR}
HOWARD_SHA=55ccf01
REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

UPSTREAM_SUB='#define SUB_WF_ELEM_MAG(x, y) do {(x) -= y \* 2 + 240; } while(0)'
SUB_MINUS8='#define SUB_WF_ELEM_MAG(x, y) do { int _v = (int)(x) - (int)(2.0f * (y) + 0.5f) + 16; (x) = (uint8_t)(_v < 0 ? 0 : (_v > 255 ? 255 : _v)); } while(0)'
SUB_FULL='#define SUB_WF_ELEM_MAG(x, y) do { int _v = (int)(x) - (int)(2.0f * (y) + 0.5f); (x) = (uint8_t)(_v < 0 ? 0 : _v); } while(0)'
SUB_HALF='#define SUB_WF_ELEM_MAG(x, y) do { int _v = (int)(x) - (int)(1.0f * (y) + 0.5f); (x) = (uint8_t)(_v < 0 ? 0 : _v); } while(0)'
SUB_ZERO='#define SUB_WF_ELEM_MAG(x, y) do { (void)(y); (x) = 0; } while(0)'

# Edit in place with a pattern that must match, so a drifted source fails loudly.
edit() {
    local file=$1 from=$2 to=$3
    grep -q -- "$from" "$file" || { echo "pattern not found in $file: $from" >&2; exit 1; }
    sed -i.bak "s|$from|$to|" "$file" && rm -f "$file.bak"
}

build() {
    (cd "$1" && { make clean >/dev/null 2>&1 || true; } \
        && make decode_ft8 >"$1.build.log" 2>&1) || { echo "build failed: $1 (see $1.build.log)" >&2; exit 1; }
}

# kgoba: our own submodule pin, demo defaults.
rm -rf "$OUT/kgoba"
cp -R "$REPO/components/ft8_lib/vendor" "$OUT/kgoba"
sed -i.bak '1i\
#include <stdio.h>
' "$OUT/kgoba/demo/decode_ft8.c" && rm -f "$OUT/kgoba/demo/decode_ft8.c.bak"
build "$OUT/kgoba"

# howard0su at the pinned SHA.
if [ ! -d "$OUT/howard_src" ]; then
    git clone -q https://github.com/howard0su/ft8_lib.git "$OUT/howard_src"
fi
git -C "$OUT/howard_src" checkout -q "$HOWARD_SHA"

variant() {
    local name=$1
    rm -rf "$OUT/$name"
    cp -R "$OUT/howard_src" "$OUT/$name"
    rm -rf "$OUT/$name/.git"
}

# name  | subtraction | time_osr freq_osr candidates | passes | OSD | subtract on
make_variant() {
    local name=$1 sub=$2 tosr=$3 fosr=$4 cand=$5 passes=$6 osd=$7 subtract=$8
    variant "$name"
    local d="$OUT/$name"
    [ "$sub" = upstream ] || edit "$d/ft8/decode.h" "$UPSTREAM_SUB" "$sub"
    edit "$d/demo/decode_ft8.c" 'const int kTime_osr = 4;' "const int kTime_osr = $tosr;"
    edit "$d/demo/decode_ft8.c" 'const int kFreq_osr = 2;' "const int kFreq_osr = $fosr;"
    edit "$d/demo/decode_ft8.c" 'const int kMax_candidates = 256;' "const int kMax_candidates = $cand;"
    edit "$d/demo/decode_ft8.c" 'pass < 3; pass++' "pass < $passes; pass++"
    [ "$osd" = on ] || edit "$d/ft8/decode.c" '#define kMaxLDPCErrors 32' '#define kMaxLDPCErrors -1'
    [ "$subtract" = on ] || edit "$d/ft8/decode.c" \
        'message->snr = ftx_substract(wf, cand, tones, desc->num_symbols);' 'message->snr = 0;'
    build "$d"
}

M8=$SUB_MINUS8
#            name              subtraction   t f cand  passes OSD subtract
make_variant howard_upstream   upstream      4 2 256   3      on  on
make_variant sub_minus8        "$M8"         4 2 256   3      on  on
make_variant sub_full          "$SUB_FULL"   4 2 256   3      on  on
make_variant sub_half          "$SUB_HALF"   4 2 256   3      on  on
make_variant sub_zero          "$SUB_ZERO"   4 2 256   3      on  on
make_variant no_osd            "$M8"         4 2 256   3      off on
make_variant no_subtract       "$M8"         4 2 256   3      on  off
make_variant one_pass          "$M8"         4 2 256   1      on  on
make_variant time_osr2         "$M8"         2 2 256   3      on  on
make_variant ladder_today      "$M8"         2 1 50    1      off off
make_variant ladder_c256       "$M8"         2 1 256   1      off off
make_variant ladder_c256_sub   "$M8"         2 1 256   3      off on
make_variant ladder_swap       "$M8"         1 2 256   3      off on
make_variant ladder_t2f2       "$M8"         2 2 256   3      off on

bins=()
for v in kgoba howard_upstream sub_minus8 sub_full sub_half sub_zero no_osd no_subtract one_pass time_osr2 \
         ladder_today ladder_c256 ladder_c256_sub ladder_swap ladder_t2f2; do
    bins+=("$v=$OUT/$v/decode_ft8")
done

echo "== in-tree corpus (components/ft8_lib/vendor/test/wav)"
python3 "$REPO/tools/decode_bench/compare_decoders.py" "$REPO/components/ft8_lib/vendor/test/wav" "${bins[@]}"
echo "== howard0su 20m_busy corpus"
python3 "$REPO/tools/decode_bench/compare_decoders.py" "$OUT/howard_src/test/wav/20m_busy" "${bins[@]}"
