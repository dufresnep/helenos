#!/bin/sh

BOOT_OUTPUT="$1"
SRCDIR="$2"
DESTDIR="$3"

rm -rf ${DESTDIR}

mkdir -p ${DESTDIR}/boot
cp -t ${DESTDIR}/boot \
	${SRCDIR}/a.out/isofs.b \
	${SRCDIR}/a.out/second.b \
	${SRCDIR}/silo.conf

mkdir -p ${DESTDIR}/HelenOS
cp ${BOOT_OUTPUT} ${DESTDIR}/HelenOS/image.boot
