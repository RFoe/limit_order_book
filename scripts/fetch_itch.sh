#!/usr/bin/env bash
# Download one NASDAQ TotalView-ITCH 5.0 sample day into data/ (resumable).
# usage: scripts/fetch_itch.sh [MMDDYYYY]      (default 12302019, ~3.5 GB, the smallest sample)
# Integrity: the server serves the .md5sum files as HTML, so we verify the
# size against Content-Length and the gzip CRC32 (gzip -t) instead.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

day="${1:-12302019}"
base="${LOB_ITCH_BASE_URL:-https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH}"
file="${day}.NASDAQ_ITCH50.gz"
url="$base/$file"

mkdir -p "$DATA_DIR"
cd "$DATA_DIR"

size="$(curl -sSIL "$url" | awk 'tolower($1)=="content-length:" {v=$2} END {print v}' | tr -d '\r')"
[[ -n "$size" ]] || { lob_log "cannot get Content-Length for $url"; exit 1; }
have=0
[[ -f "$file" ]] && have="$(stat -c %s "$file")"
avail="$(df -B1 --output=avail . | tail -1)"
need=$((size - have + (1 << 30))) # keep 1 GiB headroom
if ((need > avail)); then
    lob_log "not enough disk: need $((need >> 20)) MiB, have $((avail >> 20)) MiB"
    exit 1
fi

if ((have < size)); then
    lob_log "downloading $url ($((size >> 20)) MiB, resuming from $((have >> 20)) MiB)"
    # emi.nasdaq.com throttles each connection (~30 KB/s measured from a CN
    # network); aria2c with 16 connections reached ~650 KB/s. Both resume.
    if command -v aria2c >/dev/null; then
        aria2c --no-conf -c --file-allocation=none -x 16 -s 16 -k 1M \
            --summary-interval=60 --console-log-level=warn -o "$file" "$url"
    else
        wget -c -q --show-progress -O "$file" "$url"
    fi
fi

got="$(stat -c %s "$file")"
[[ "$got" == "$size" ]] || { lob_log "size mismatch: $got != $size"; exit 1; }
lob_log "size ok ($got bytes); checking gzip CRC (gzip -t) ..."
gzip -t "$file"
lob_log "ok: $DATA_DIR/$file"
