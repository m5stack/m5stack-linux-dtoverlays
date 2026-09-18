#!/bin/sh
set -eu
kernel=${1:?usage: sh run-kunit.sh /path/to/linux-source /path/to/test-output}
output=${2:?missing test output directory}
tests=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$output"
output=$(CDPATH= cd -- "$output" && pwd)
if [ ! -d "$output/source" ]; then
    cp -a --reflink=auto "$kernel" "$output/source"
    ln -s "$tests" "$output/source/lib/kunit/m5io-hub-tests"
    printf '\nobj-y += m5io-hub-tests/\n' >> "$output/source/lib/kunit/Makefile"
    printf '\nsource "lib/kunit/m5io-hub-tests/Kconfig"\n' >> "$output/source/lib/kunit/Kconfig"
fi
cd "$output/source"
python3 tools/testing/kunit/kunit.py run \
    --kunitconfig="$tests/kunit.config" --build_dir="$output/build" \
    --jobs=8 --timeout=120 'm5io-hub*'
