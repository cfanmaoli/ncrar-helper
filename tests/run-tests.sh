#!/bin/sh
# Regression tests for ncrar-helper. The fixtures in tests/data were produced with RAR 7.30.
#
# Usage: tests/run-tests.sh build/ncrar-helper
set -u

HELPER=${1:?usage: tests/run-tests.sh <path-to-ncrar-helper>}
[ -x "${HELPER}" ] || { echo "no such helper: ${HELPER}" >&2; exit 2; }

# The missing-volume cases need a hard timeout; macOS has no timeout(1) unless coreutils is installed.
TIMEOUT=$(command -v timeout || command -v gtimeout) || {
    echo "timeout(1) is required (brew install coreutils)" >&2
    exit 2
}

DIR=$( cd "$(dirname "$0")" >/dev/null 2>&1 && pwd )
DATA="${DIR}/data"
TMP=$(mktemp -d)
trap 'rm -rf "${TMP}"' EXIT

failures=0
fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

# Runs the helper and stores its exit code in 'code', stdout in 'out'.
run() {
    out=$("$@" 2>/dev/null)
    code=$?
}

echo "== version"
run "${HELPER}" version
[ "${code}" -eq 0 ] || fail "version exit ${code}"
echo "${out}" | grep -q '"protocol":1' || fail "protocol field"
echo "${out}" | grep -q '"unrar":' || fail "unrar field"

echo "== list"
run "${HELPER}" list "${DATA}/plain.rar"
[ "${code}" -eq 0 ] || fail "list exit ${code}"
echo "${out}" | tail -1 | grep -q '"done":true,"entries":5' || fail "list terminator"
echo "${out}" | grep -q '"name":"文件.txt"' || fail "unicode entry name"
echo "${out}" | grep -q '"link_type":1' || fail "symlink entry"
echo "${out}" | grep -q '"link_target":"a.txt"' || fail "symlink target in listing"

echo "== read"
run "${HELPER}" read "${DATA}/plain.rar" 0
[ "${code}" -eq 0 ] || fail "read exit ${code}"
[ "${out}" = "hello world" ] || fail "read payload: '${out}'"

echo "== link"
run "${HELPER}" link "${DATA}/plain.rar" 3
[ "${code}" -eq 0 ] || fail "link exit ${code}"
[ "${out}" = '{"target":"a.txt"}' ] || fail "link target: '${out}'"

echo "== passwords"
out=$(NCRAR_PASSWORD='päss' "$HELPER" read "${DATA}/encrypted.rar" 0 2>/dev/null)
code=$?
[ "${code}" -eq 0 ] || fail "unicode password via env, exit ${code}"
[ "${out}" = "secret data" ] || fail "unicode password payload"
printf 'päss\n' | "${HELPER}" --password-stdin read "${DATA}/encrypted.rar" 0 > "${TMP}/stdin.out" 2>/dev/null
code=$?
[ "${code}" -eq 0 ] || fail "unicode password via stdin, exit ${code}"
[ "$(cat "${TMP}/stdin.out")" = "secret data" ] || fail "unicode password via stdin payload"
out=$(NCRAR_PASSWORD='wrong' "$HELPER" read "${DATA}/encrypted.rar" 0 2>/dev/null)
code=$?
[ "${code}" -eq 5 ] || fail "wrong password exit ${code}"

echo "== multi-volume"
run "${HELPER}" list "${DATA}/multi.part1.rar"
[ "${code}" -eq 0 ] || fail "multi-volume list exit ${code}"
echo "${out}" | tail -1 | grep -q '"entries":1' || fail "multi-volume entries"
cp "${DATA}/multi.part1.rar" "${TMP}/"
run "${TIMEOUT}" 5 "${HELPER}" list "${TMP}/multi.part1.rar"
[ "${code}" -eq 4 ] || fail "missing volume exit ${code} (expected 4, and it must not hang)"

# A FIFO in place of the next volume must be refused, not opened with a blocking read.
rm -f "${TMP}"/multi.part*.rar
cp "${DATA}/multi.part1.rar" "${TMP}/"
mkfifo "${TMP}/multi.part2.rar"
run "${TIMEOUT}" 5 "${HELPER}" list "${TMP}/multi.part1.rar"
rm -f "${TMP}/multi.part2.rar"
# 4 when the volume is unreachable, 3 when it opens but isn't an archive - either way it must not
# wait for a FIFO writer.
{ [ "${code}" -eq 4 ] || [ "${code}" -eq 3 ]; } ||
    fail "FIFO volume exit ${code} (expected 3 or 4, and it must not hang)"

# A volume that exists but can't be opened must fail the same way instead of being retried.
rm -f "${TMP}"/multi.part*.rar
cp "${DATA}"/multi.part*.rar "${TMP}/"
chmod 000 "${TMP}/multi.part2.rar"
run "${TIMEOUT}" 5 "${HELPER}" list "${TMP}/multi.part1.rar"
chmod 644 "${TMP}/multi.part2.rar"
[ "${code}" -eq 4 ] || fail "unreadable volume exit ${code} (expected 4, and it must not hang)"

echo "== malformed input"
run "${HELPER}" list "${DATA}/garbage.bin"
[ "${code}" -eq 3 ] || fail "garbage exit ${code}"
run "${HELPER}" list "${TMP}/does-not-exist.rar"
[ "${code}" -eq 3 ] || fail "missing archive exit ${code}"

echo "== entry index parsing"
for bad in abc -1 999999999999; do
    run "${HELPER}" read "${DATA}/plain.rar" "${bad}"
    [ "${code}" -eq 2 ] || fail "index '${bad}' exit ${code}"
done

if [ "${failures}" -ne 0 ]; then
    echo "${failures} test(s) failed" >&2
    exit 1
fi
echo "all tests passed"
