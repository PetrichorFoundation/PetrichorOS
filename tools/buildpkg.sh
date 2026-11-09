#!/bin/sh
# Builds one package directory into a .ppk (called by `make package`).
#   buildpkg.sh PKGDIR OUTDIR      environment: CC UCFLAGS LD LDFLAGS ULIB B MKPPK
#
# PKGDIR/package: VERSION, DEPS, DESC, and optionally CFLAGS and LIBS (user-library objects, e.g. "gfx").
#   src/<prog>.c        -> bin/<prog>
#   src/<prog>/**.c     -> one program bin/<prog>; a file named `cflags` in any directory adds compiler flags for
#                          the sources below it ("@DIR@" stands for that directory)
#   files/              -> copied as-is (relative to /disk)
set -e
PKG=$1
OUT=$2
p=$(basename "$PKG")
[ -n "$p" ] && [ -f "$PKG/package" ]
st="$B/pkgstage/$p"
ob="$B/pkgobj/$p"
rm -rf "$B/pkgstage/$p" "$B/pkgobj/$p"
mkdir -p "$st" "$ob"
CFLAGS=
LIBS=
. "$PKG/package"
extra=
for l in $LIBS; do extra="$extra $B/user/lib/$l.o"; done

for s in "$PKG"/src/*.c; do
    [ -f "$s" ] || continue
    n=$(basename "$s" .c)
    mkdir -p "$st/bin"
    echo "  CC  $p/$n"
    $CC $UCFLAGS $CFLAGS -c "$s" -o "$ob/$n.o"
    $LD $LDFLAGS -T user/user.ld -o "$st/bin/$n" "$ob/$n.o" $extra $ULIB
done

for d in "$PKG"/src/*/; do
    [ -d "$d" ] || continue
    d=${d%/}
    n=$(basename "$d")
    mkdir -p "$st/bin" "$ob/$n"
    objs=
    count=0
    for s in $(find "$d" -name '*.c' | sort); do
        fl=
        dd=$(dirname "$s")
        while :; do
            [ -f "$dd/cflags" ] && fl="$fl $(sed "s#@DIR@#$dd#g" "$dd/cflags" | tr '\n' ' ')"
            [ "$dd" = "$d" ] && break
            dd=$(dirname "$dd")
        done
        rel=${s#$d/}
        o="$ob/$n/$(echo "$rel" | tr / _).o"
        $CC $UCFLAGS $CFLAGS $fl -I"$d" -c "$s" -o "$o"
        objs="$objs $o"
        count=$((count + 1))
    done
    echo "  LD  $p/$n ($count files)"
    $LD $LDFLAGS -T user/user.ld -o "$st/bin/$n" $objs $extra $ULIB
done

if [ -d "$PKG/files" ]; then cp -R "$PKG/files/." "$st/"; fi
"$MKPPK" "$OUT" "$st" "$p" "$VERSION" "$DEPS" "$DESC"
