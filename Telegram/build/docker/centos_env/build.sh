#!/bin/bash
set -e

cd Telegram
./configure.sh "$@"
# KEEP_GOING=1 lets ninja report every failing translation unit in one
# run instead of stopping at the first; the exit status still fails.
# Rocky8's python3.12 pyexpat clashes with the older libexpat that
# LD_LIBRARY_PATH exposes first (appstream changelog generator is the only
# consumer) — python3.11 from the image works fine for it
sed -i 's|/usr/bin/python3.12|/usr/bin/python3.11|g' ../out/build.ninja
cmake --build ../out --config "${CONFIG:-Release}" --target Telegram ${KEEP_GOING:+-- -k 0}
