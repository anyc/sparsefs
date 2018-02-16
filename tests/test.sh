#! /bin/bash

qgrep() {
	grep $1 $2 >/dev/null || return 1
}

cleanup() {
	fusermount -zu ${FDIR}
	rmdir ${FDIR}
}

fail() {
	src=$1
	lineno=$2
	shift
	shift
	echo "In $src:$lineno: \"$(cat $src | head -n $lineno | tail -n1)\""
	tree ${FDIR}
	echo "ERROR"
	exit 1
}

FDIR=fuse

mkdir ${FDIR}
../sparsefs -s $(pwd)/test1/src1/ -s $(pwd)/test1/src2/ \
	${FDIR}

trap cleanup EXIT

qgrep source1 ${FDIR}/both12 || fail ${BASH_SOURCE} ${LINENO}
qgrep source1 ${FDIR}/path12/both12 || fail ${BASH_SOURCE} ${LINENO}

[ "$(ls -1 ${FDIR} | grep both12 | wc -l)" != "1" ] && fail ${BASH_SOURCE} ${LINENO}

ls ${FDIR} | qgrep source1 || fail ${BASH_SOURCE} ${LINENO}
ls ${FDIR} | qgrep source2 || fail ${BASH_SOURCE} ${LINENO}

qgrep source1 ${FDIR}/source1 && qgrep source2 ${FDIR}/source2 || fail ${BASH_SOURCE} ${LINENO}

ls ${FDIR}/path12 | qgrep source1 || fail ${BASH_SOURCE} ${LINENO}
ls ${FDIR}/path12 | qgrep source2 || fail ${BASH_SOURCE} ${LINENO}
[ "$(ls -1 ${FDIR}/path12 | wc -l)" != "3" ] && fail ${BASH_SOURCE} ${LINENO}

[ "$(ls -1 ${FDIR}/path1 | wc -l)" != "1" ] && fail ${BASH_SOURCE} ${LINENO}

qgrep source1 ${FDIR}/path12/source1 && qgrep source2 ${FDIR}/path12/source2 || fail ${BASH_SOURCE} ${LINENO}

qgrep source1 ${FDIR}/path1/source1 || fail ${BASH_SOURCE} ${LINENO}
qgrep source2 ${FDIR}/path2/source2 || fail ${BASH_SOURCE} ${LINENO}

cleanup


mkdir ${FDIR}
../sparsefs -s $(pwd)/test1/src1/ -s $(pwd)/test1/src2/ \
	-X $(pwd)/test1/src1/both12 \
	-X $(pwd)/test1/src1/path12/both12 \
	${FDIR}

qgrep source2 ${FDIR}/both12 || fail ${BASH_SOURCE} ${LINENO}
qgrep source2 ${FDIR}/path12/both12 || fail ${BASH_SOURCE} ${LINENO}

[ "$(ls -1 ${FDIR} | grep both12 | wc -l)" != "1" ] && fail ${BASH_SOURCE} ${LINENO}

ls ${FDIR} | qgrep source1 || fail ${BASH_SOURCE} ${LINENO}
ls ${FDIR} | qgrep source2 || fail ${BASH_SOURCE} ${LINENO}

qgrep source1 ${FDIR}/source1 && qgrep source2 ${FDIR}/source2 || fail ${BASH_SOURCE} ${LINENO}

ls ${FDIR}/path12 | qgrep source1 || fail ${BASH_SOURCE} ${LINENO}
ls ${FDIR}/path12 | qgrep source2 || fail ${BASH_SOURCE} ${LINENO}
[ "$(ls -1 ${FDIR}/path12 | wc -l)" != "3" ] && fail ${BASH_SOURCE} ${LINENO}

[ "$(ls -1 ${FDIR}/path1 | wc -l)" != "1" ] && fail ${BASH_SOURCE} ${LINENO}

qgrep source1 ${FDIR}/path12/source1 && qgrep source2 ${FDIR}/path12/source2 || fail ${BASH_SOURCE} ${LINENO}

qgrep source1 ${FDIR}/path1/source1 || fail ${BASH_SOURCE} ${LINENO}
qgrep source2 ${FDIR}/path2/source2 || fail ${BASH_SOURCE} ${LINENO}

tree ${FDIR}
