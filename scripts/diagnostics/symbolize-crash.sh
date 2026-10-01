#!/usr/bin/env bash
# Map a crash-summary.txt fault offset to the nearest function using the COFF
# symbol table of the exact shipped chiaki.exe (Release builds carry no DWARF).
# Usage: symbolize-crash.sh <chiaki.exe of the same build> <fault_offset hex, e.g. 0x1A2B3C>
set -euo pipefail
exe="$1"; off="$2"
base=$(objdump -p "$exe" | awk '/^ImageBase/{print $2}')
addr=$(( 0x$base + off ))
nm -C --defined-only "$exe" | awk -v a="$addr" '
  $2 ~ /^[tT]$/ && $3 !~ /^\./ { v = strtonum("0x" $1); if (v <= a && v > best) { best = v; line = $0 } }
  END { if (line) printf "0x%X = %s + 0x%X\n", a, substr(line, index(line, $3)), a - best; else print "no symbol" }'
