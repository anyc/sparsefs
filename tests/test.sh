#! /bin/bash

set -euo pipefail

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

qgrep() {
	case $# in
		1) grep -- "$1" >/dev/null ;;
		2) grep -- "$1" "$2" >/dev/null ;;
		*) return 2 ;;
	esac
}

cleanup() {
	fusermount -zu "$FDIR" >/dev/null 2>&1 || true
	rmdir "$FDIR" >/dev/null 2>&1 || true
}

fail() {
	src=$1
	lineno=$2
	shift
	shift
	echo "In $src:$lineno: \"$(sed -n "${lineno}p" "$src")\""
	tree "$FDIR"
	echo "ERROR"
	exit 1
}

FDIR=$(mktemp -d "${TMPDIR:-/tmp}/sparsefs-test.XXXXXX")
trap cleanup EXIT

"$TEST_DIR/../sparsefs" --source="$TEST_DIR/test1/src1/" \
	--source="$TEST_DIR/test1/src2/" \
	"$FDIR"

qgrep source1 "$FDIR/both12" || fail "${BASH_SOURCE[0]}" "$LINENO"
qgrep source1 "$FDIR/path12/both12" || fail "${BASH_SOURCE[0]}" "$LINENO"

[ "$(find "$FDIR" -maxdepth 1 -name both12 | wc -l)" != "1" ] && fail "${BASH_SOURCE[0]}" "$LINENO"

find "$FDIR" -maxdepth 1 -printf '%f\n' | qgrep source1 || fail "${BASH_SOURCE[0]}" "$LINENO"
find "$FDIR" -maxdepth 1 -printf '%f\n' | qgrep source2 || fail "${BASH_SOURCE[0]}" "$LINENO"

qgrep source1 "$FDIR/source1" && qgrep source2 "$FDIR/source2" || fail "${BASH_SOURCE[0]}" "$LINENO"

find "$FDIR/path12" -maxdepth 1 -printf '%f\n' | qgrep source1 || fail "${BASH_SOURCE[0]}" "$LINENO"
find "$FDIR/path12" -maxdepth 1 -printf '%f\n' | qgrep source2 || fail "${BASH_SOURCE[0]}" "$LINENO"
[ "$(find "$FDIR/path12" -mindepth 1 -maxdepth 1 | wc -l)" != "3" ] && fail "${BASH_SOURCE[0]}" "$LINENO"

[ "$(find "$FDIR/path1" -mindepth 1 -maxdepth 1 | wc -l)" != "1" ] && fail "${BASH_SOURCE[0]}" "$LINENO"

qgrep source1 "$FDIR/path12/source1" && qgrep source2 "$FDIR/path12/source2" || fail "${BASH_SOURCE[0]}" "$LINENO"

qgrep source1 "$FDIR/path1/source1" || fail "${BASH_SOURCE[0]}" "$LINENO"
qgrep source2 "$FDIR/path2/source2" || fail "${BASH_SOURCE[0]}" "$LINENO"

cleanup

FDIR=$(mktemp -d "${TMPDIR:-/tmp}/sparsefs-test.XXXXXX")
"$TEST_DIR/../sparsefs" --source="$TEST_DIR/test1/src1/" \
	--source="$TEST_DIR/test1/src2/" \
	-X "$TEST_DIR/test1/src1/both12" \
	-X "$TEST_DIR/test1/src1/path12/both12" \
	"$FDIR"

qgrep source2 "$FDIR/both12" || fail "${BASH_SOURCE[0]}" "$LINENO"
qgrep source2 "$FDIR/path12/both12" || fail "${BASH_SOURCE[0]}" "$LINENO"

[ "$(find "$FDIR" -maxdepth 1 -name both12 | wc -l)" != "1" ] && fail "${BASH_SOURCE[0]}" "$LINENO"

find "$FDIR" -maxdepth 1 -printf '%f\n' | qgrep source1 || fail "${BASH_SOURCE[0]}" "$LINENO"
find "$FDIR" -maxdepth 1 -printf '%f\n' | qgrep source2 || fail "${BASH_SOURCE[0]}" "$LINENO"

qgrep source1 "$FDIR/source1" && qgrep source2 "$FDIR/source2" || fail "${BASH_SOURCE[0]}" "$LINENO"

find "$FDIR/path12" -maxdepth 1 -printf '%f\n' | qgrep source1 || fail "${BASH_SOURCE[0]}" "$LINENO"
find "$FDIR/path12" -maxdepth 1 -printf '%f\n' | qgrep source2 || fail "${BASH_SOURCE[0]}" "$LINENO"
[ "$(find "$FDIR/path12" -mindepth 1 -maxdepth 1 | wc -l)" != "3" ] && fail "${BASH_SOURCE[0]}" "$LINENO"

[ "$(find "$FDIR/path1" -mindepth 1 -maxdepth 1 | wc -l)" != "1" ] && fail "${BASH_SOURCE[0]}" "$LINENO"

qgrep source1 "$FDIR/path12/source1" && qgrep source2 "$FDIR/path12/source2" || fail "${BASH_SOURCE[0]}" "$LINENO"

qgrep source1 "$FDIR/path1/source1" || fail "${BASH_SOURCE[0]}" "$LINENO"
qgrep source2 "$FDIR/path2/source2" || fail "${BASH_SOURCE[0]}" "$LINENO"

find "$FDIR" -print
