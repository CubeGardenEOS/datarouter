#!/usr/bin/env bash
# Build the JNI shared library and run the Java lifecycle tests against a
# throwaway switch. Exits non-zero on test failure.
set -euo pipefail
cd "$(dirname "$0")"

echo "== building datarouter (release) ==" >&2
(cd .. && cargo build --release)

echo "== building JNI .so ==" >&2
cargo build --release

TMP=$(mktemp -d)
SWITCH_PID=""

cleanup() {
    if [ -n "$SWITCH_PID" ]; then
        kill -9 "$SWITCH_PID" 2>/dev/null || true
        wait "$SWITCH_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

echo "== compiling Java sources + tests ==" >&2
javac -d "$TMP" \
    java/src/main/java/io/blinkcms/datarouter/*.java \
    java/src/test/java/io/blinkcms/datarouter/*.java

SHM="/datarouter-zctest-$$"
"../build/datarouter-switch" --name "$SHM" --ttl-ms 60000 --allow portal,core \
    >"$TMP/switch.log" 2>&1 &
SWITCH_PID=$!
sleep 1
if ! kill -0 "$SWITCH_PID" 2>/dev/null; then
    echo "switch failed to start:" >&2
    cat "$TMP/switch.log" >&2
    exit 1
fi

echo "== running ZcLifecycleTest on $SHM ==" >&2
java -cp "$TMP" -Djava.library.path=target/release \
    io.blinkcms.datarouter.ZcLifecycleTest "$SHM"
