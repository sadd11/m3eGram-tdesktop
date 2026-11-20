#!/bin/bash
set -e

cd Telegram

# Rocky 8's python3.12 pyexpat needs the expat>=2.4 API
# (XML_SetBillionLaughsAttackProtection*), but the image's only libexpat is
# the system expat 2.2.5, which does not export those symbols — so
# xml.etree can't import under 3.12 and the appstream changelog generator
# dies with ImportError. python3.11's pyexpat is built against expat 2.2.5
# and works. Pin the interpreter for every CMake-side Python consumer
# (appstream changelog, tl scheme generator, obj->binobj baking): FindPython3
# honors a pre-set absolute Python3_EXECUTABLE cache entry, so all
# ${Python3_EXECUTABLE} references land as 3.11 at configure time — this
# survives RERUN_CMAKE regenerations mid-build (CONFIGURE_DEPENDS globs).
./configure.sh "$@" -D Python3_EXECUTABLE=/usr/bin/python3.11

# Belt-and-suspenders: the generator is Ninja Multi-Config — custom command
# edges live in out/build-<Config>.ninja (Release/Debug/...), NOT in
# out/build.ninja, so a sed of build.ninja alone silently fixes nothing.
# With the -D above this is a no-op; it only guards against a stale out/
# CMakeCache still pointing at python3.12.
for f in ../out/build.ninja ../out/build-*.ninja; do
    if [ -f "$f" ]; then
        sed -i 's|/usr/bin/python3.12|/usr/bin/python3.11|g' "$f"
    fi
done

# Fail fast here (before 30 min of compiling) if 3.12 still leaks into any
# generated edge; also makes the outcome visible in the run log.
LEFT=$(grep -h '/usr/bin/python3.12' ../out/build.ninja ../out/build-*.ninja 2>/dev/null | wc -l)
echo "ninja files still referencing /usr/bin/python3.12: $LEFT"
if [ "$LEFT" -gt 0 ]; then
    echo "ERROR: python3.12 still referenced by generated build files" >&2
    exit 1
fi

# KEEP_GOING=1 lets ninja report every failing translation unit in one
# run instead of stopping at the first; the exit status still fails.
cmake --build ../out --config "${CONFIG:-Release}" --target Telegram ${KEEP_GOING:+-- -k 0}
