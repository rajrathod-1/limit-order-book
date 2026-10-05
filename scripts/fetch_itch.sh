#!/usr/bin/env bash
#
# Fetch a Nasdaq TotalView-ITCH 5.0 day file and slice it into per-symbol
# replay corpora.
#
# Nasdaq publishes sample days at https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
# with no account, no key and no rate limit. They are large: the file below is
# 3.5 GB compressed and 8.25 GB of messages once decompressed, so the filter
# streams it and never writes the decompressed form to disk.
#
# Everything in this project also runs without any of this -- `itch_gen`
# produces a synthetic ITCH stream that goes through the same decoder.

set -euo pipefail

DAY="${DAY:-12302019}"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${DAY}.NASDAQ_ITCH50.gz"
DATA_DIR="${DATA_DIR:-data}"
ARCHIVE="${DATA_DIR}/${DAY}.NASDAQ_ITCH50.gz"
SYMBOLS=("${@:-AAPL MSFT TSLA AMD INTC}")

FILTER="${FILTER:-build/itch_filter}"
if [[ ! -x "$FILTER" ]]; then
    echo "error: $FILTER not found. Build first:" >&2
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    exit 1
fi

mkdir -p "$DATA_DIR" "$DATA_DIR/symbols"

if [[ -f "$ARCHIVE" ]]; then
    echo "==> $ARCHIVE already present, skipping download"
else
    echo "==> downloading $URL (~3.5 GB)"
    curl -L -C - --retry 3 -o "$ARCHIVE" "$URL"
fi

echo "==> verifying gzip integrity"
gzip -t "$ARCHIVE"

echo "==> filtering symbols: ${SYMBOLS[*]}"
args=()
for s in ${SYMBOLS[*]}; do args+=(--symbol "$s"); done
"$FILTER" --in "$ARCHIVE" --out-dir "$DATA_DIR/symbols" "${args[@]}"

echo
echo "==> done. Replay one with:"
echo "    build/lob_replay --data $DATA_DIR/symbols/AAPL.itch"
