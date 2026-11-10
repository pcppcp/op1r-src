#!/bin/sh
# Builds the host test tools with the real plugin source (address + undefined-behaviour sanitizers):
#   build/mm_dump    one note -> raw int16 (see mm_dump.c)       build/mm_filter  the ladder alone (see mm_filter.c)
# Analysis (needs numpy + scipy): python3 levels.py | filter_response.py | osc_check.py
set -e
cd "$(dirname "$0")"
mkdir -p build
CF="-O1 -g -Wall -Wextra -Wno-unknown-pragmas -fsanitize=address,undefined -fno-sanitize-recover=undefined -DOP1_HOST -DMM_CHECK -I../../.. -I.."
cc $CF mm_dump.c -lm -o build/mm_dump
cc $CF mm_filter.c -lm -o build/mm_filter
cc $CF mm_osc.c -lm -o build/mm_osc
cc $CF mm_env.c -lm -o build/mm_env
cc $CF mm_poly.c -lm -o build/mm_poly
cc $CF mm_ui.c -lm -o build/mm_ui
cc $CF mm_views.c -lm -o build/mm_views
cc $CF mm_scope.c -lm -o build/mm_scope
echo built build/mm_dump build/mm_filter build/mm_osc build/mm_env build/mm_poly build/mm_ui build/mm_views build/mm_scope
