#!/bin/sh

cd `dirname $0`/.. || exit 1
wd=`pwd`
test "${wd%%/keepalived}" != "$wd" -a -d debian || { echo >&2 "strange wd $wd"; exit 1; }

if [ "$1" = clean -o "$1" = clear ]; then
	shift
	dont_build=yes
	for f in * .*; do
		case "$f" in
			debian | . | ..) ;;
			*)
				echo removing $f ...
				rm -rf $f
			;;
		esac
	done
	./debian/rules clean
fi

if [ "$1" = build -o -z "$dont_build" ]; then
	./debian/rules get-orig-source # get source package
	dpkg-buildpackage -b -uc
fi
