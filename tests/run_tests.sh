#!/bin/bash
# ─────────────────────────────────────────────────────
#  amalgame-database-postgresql — Test Runner
#  Usage: ./tests/run_tests.sh [/path/to/amc]
#
#  Double-gated:
#    1. libpq-dev is installed (header probe via gcc -E)
#    2. A PostgreSQL server is reachable on $PGHOST:$PGPORT
#       (default 127.0.0.1:5432)
#
#  Either missing → every case SKIPs cleanly.
#
#  Start a server locally:
#    docker run --rm -p 5432:5432 \
#      -e POSTGRES_PASSWORD=test -e POSTGRES_DB=amctest \
#      postgres:16-alpine
#  Then export PGHOST=127.0.0.1 PGUSER=postgres PGPASSWORD=test \
#                PGDATABASE=amctest
# ─────────────────────────────────────────────────────

set -u

if [ $# -ge 1 ]; then
    AMC="$1"
elif [ -n "${AMC:-}" ]; then
    :
elif command -v amc >/dev/null 2>&1; then
    AMC="$(command -v amc)"
else
    echo "ERROR: amc not found." >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_RUNTIME="$PKG_ROOT/runtime"

AMC_DIR="$(cd "$(dirname "$AMC")" && pwd)"
if [ -d "$AMC_DIR/runtime" ]; then
    AMC_RUNTIME="$AMC_DIR/runtime"
elif [ -n "${AMC_RUNTIME:-}" ]; then
    :
else
    echo "ERROR: amc runtime/ not found. Set AMC_RUNTIME=..." >&2
    exit 2
fi

BUILD_DIR="$(mktemp -d -t apg-XXXXXX)"
trap 'rm -rf "$BUILD_DIR"' EXIT
PROJ_DIR="$BUILD_DIR/proj"
mkdir -p "$PROJ_DIR"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
NC='\033[0m'
PASS=0; FAIL=0; SKIP=0

echo ""
echo "════════════════════════════════════════════"
echo "  amalgame-database-postgresql — Tests"
echo "════════════════════════════════════════════"
echo "  amc:     $AMC ($("$AMC" --version 2>&1 | head -1))"
echo "  runtime: $AMC_RUNTIME"
echo ""

# ── Gate 1 : libpq-dev present? ────────────────────────
echo "── Probing libpq-dev ───────────────────────"
LIBPQ_OK=0
echo '#include <libpq-fe.h>' > "$BUILD_DIR/_inc.c"
if gcc -E "$BUILD_DIR/_inc.c" >/dev/null 2>&1; then
    LIBPQ_OK=1
    echo "  libpq-fe.h:        found"
fi
if [ "$LIBPQ_OK" = "0" ]; then
    echo '#include <postgresql/libpq-fe.h>' > "$BUILD_DIR/_inc.c"
    if gcc -E "$BUILD_DIR/_inc.c" >/dev/null 2>&1; then
        LIBPQ_OK=1
        echo "  postgresql/libpq-fe.h: found"
    fi
fi
if [ "$LIBPQ_OK" = "0" ]; then
    echo "  libpq-fe.h:        NOT FOUND (install libpq-dev / libpq-devel)"
fi
echo ""

# ── Gate 2 : Postgres server reachable? ──────────────
PGHOST="${PGHOST:-127.0.0.1}"
PGPORT="${PGPORT:-5432}"
PG_OK=0
if (echo > /dev/tcp/$PGHOST/$PGPORT) 2>/dev/null; then
    PG_OK=1
    echo "  postgres:          reachable at $PGHOST:$PGPORT"
else
    echo "  postgres:          NOT reachable at $PGHOST:$PGPORT"
fi
echo ""

# ── Stage fake cache for the test fixture ────────────
FAKE_CACHE="$BUILD_DIR/cache"
PKG_GIT="github.com/amalgame-lang/amalgame-database-postgresql"
PKG_TAG="${PKG_TAG:-v0.1.0}"
FAKE_SHA="deadbeefcafebabe0000000000000000000000ab"
SHORT_SHA="${FAKE_SHA:0:8}"
PKG_CACHE_DIR="$FAKE_CACHE/$PKG_GIT/${PKG_TAG}_${SHORT_SHA}"

mkdir -p "$(dirname "$PKG_CACHE_DIR")"
rm -rf "$PKG_CACHE_DIR"
ln -s "$PKG_ROOT" "$PKG_CACHE_DIR"

cat > "$PROJ_DIR/amalgame.lock" <<EOF
[[package]]
name = "amalgame-database-postgresql"
git  = "$PKG_GIT"
tag  = "$PKG_TAG"
rev  = "$FAKE_SHA"
EOF

export AMALGAME_PACKAGES_DIR="$FAKE_CACHE"

run_test() {
    local name="$1"
    local expected="$2"
    printf "  %-38s" "$name"
    if [ "$LIBPQ_OK" = "0" ]; then
        echo -e "${YELLOW}SKIP${NC} (libpq-dev not installed)"
        SKIP=$((SKIP + 1)); return
    fi
    if [ "$PG_OK" = "0" ]; then
        echo -e "${YELLOW}SKIP${NC} (no postgres at $PGHOST:$PGPORT)"
        SKIP=$((SKIP + 1)); return
    fi
    cp "$SCRIPT_DIR/stdlib_pg.am" "$PROJ_DIR/test.am"
    local out_base="$PROJ_DIR/test"
    local out
    out=$(cd "$PROJ_DIR" && "$AMC" -o test test.am 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (amc)"; echo "$out" | head -3 | sed 's/^/    /'
        FAIL=$((FAIL + 1)); return
    fi
    if [ ! -f "$out_base.c" ]; then
        echo -e "${RED}FAIL${NC} (no .c)"; FAIL=$((FAIL + 1)); return
    fi
    gcc -O2 -w \
        -I"$AMC_RUNTIME" -I"$PKG_RUNTIME" \
        "$out_base.c" \
        -lgc -lm -ldl -lpthread -lpq \
        -o "$out_base" 2>"$BUILD_DIR/link.log"
    if [ ! -x "$out_base" ]; then
        echo -e "${RED}FAIL${NC} (gcc link)"
        cat "$BUILD_DIR/link.log" | head -3 | sed 's/^/    /'
        FAIL=$((FAIL + 1)); return
    fi
    local run_output
    run_output=$("$out_base" 2>&1)
    if echo "$run_output" | grep -qF "$expected"; then
        echo -e "${GREEN}PASS${NC}"; PASS=$((PASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "    expected: $expected"
        echo "    got:      $(echo "$run_output" | head -3 | tr '\n' '|')"
        FAIL=$((FAIL + 1))
    fi
}

echo "── PostgreSQL ──────────────────────────────"
run_test "open via PG* env"      "[PASS] open via PG* env"
run_test "ServerVersion"         "[PASS] ServerVersion"
run_test "CREATE TABLE"          "[PASS] CREATE TABLE"
run_test "INSERT Changes"        "[PASS] INSERT Changes==1"
run_test "SELECT 3 rows"         "[PASS] SELECT 3 rows"
run_test "SELECT cell content"   "[PASS] SELECT cell content"
run_test "SERIAL id starts at 1" "[PASS] SERIAL id starts at 1"
run_test "UPDATE Changes==2"     "[PASS] UPDATE Changes==2"
run_test "bad SQL reports error" "[PASS] bad SQL reports error"

# ── v0.3: parameter binding + transactions ──
run_test "execbind insert"           "[PASS] execbind insert"
run_test "execbind injection foiled" "[PASS] execbind injection foiled"
run_test "querybindall fetch"        "[PASS] querybindall fetch"
run_test "begin"                     "[PASS] begin"
run_test "rollback"                  "[PASS] rollback"
run_test "rollback drops insert"     "[PASS] rollback drops insert"
run_test "commit"                    "[PASS] commit"
run_test "commit persists insert"    "[PASS] commit persists insert"

run_test "Close drops connection" "[PASS] Close drops connection"

echo ""
echo "────────────────────────────────────────────"
echo -e "  ${GREEN}PASS: $PASS${NC}  |  ${RED}FAIL: $FAIL${NC}  |  ${YELLOW}SKIP: $SKIP${NC}"
echo "────────────────────────────────────────────"
echo ""

[ $FAIL -eq 0 ] && exit 0 || exit 1
