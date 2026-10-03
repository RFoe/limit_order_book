#!/usr/bin/env bash
# Convert an ITCH 5.0 day into per-symbol .ops workloads, streaming through zcat
# (the ~10 GB decompressed file never touches the disk).
#   scripts/itch_convert.sh [file.gz] [SYMBOL ...]     (default: data/12302019..., AAPL)
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

in="${1:-$DATA_DIR/12302019.NASDAQ_ITCH50.gz}"
shift || true
symbols="${*:-AAPL}"
symbols="${symbols// /,}"
day="$(basename "$in" | cut -d. -f1)"

lob_build release >/dev/null
mkdir -p "$DATA_DIR" "$RESULTS_DIR"
out="$RESULTS_DIR/itch_convert_${LOB_PREFIX}_${day}.txt"

lob_log "converting $in for $symbols"
zcat "$in" | "$(lob_bin release book)" itch2ops - --symbols "$symbols" --out-dir "$DATA_DIR" \
    --tag "itch${day}" --top 20 | tee "$out"
lob_log "wrote $out"
