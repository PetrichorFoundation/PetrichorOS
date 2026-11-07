#!/bin/sh
# Builds the sample Windows programs for the GrapeJuice package (hello.exe, demo.exe) with clang and lld-link.
# Needs llvm-dlltool (Homebrew's `llvm` package) to make import libraries from the .def files below.
set -e
cd "$(dirname "$0")/.."
D=packages/grapejuice/files/share/grapejuice
T=$(mktemp -d)
DLLTOOL=${DLLTOOL:-$(command -v llvm-dlltool || echo /opt/homebrew/opt/llvm/bin/llvm-dlltool)}

printf 'LIBRARY kernel32.dll\nEXPORTS\nGetStdHandle\nWriteFile\nGetCommandLineA\nHeapAlloc\nGetProcessHeap\nExitProcess\n' >$T/kernel32.def
printf 'LIBRARY msvcrt.dll\nEXPORTS\nprintf\nsprintf\nmalloc\nfree\n' >$T/msvcrt.def
printf 'LIBRARY user32.dll\nEXPORTS\nMessageBoxA\n' >$T/user32.def
for l in kernel32 msvcrt user32; do
    "$DLLTOOL" -m i386:x86-64 -d $T/$l.def -l $T/$l.lib
done
for p in hello demo; do
    clang -target x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -nostdlib -O2 -c $D/src/$p.c -o $T/$p.o
done
lld-link /subsystem:console /entry:main /nodefaultlib /dynamicbase /fixed:no /out:$D/hello.exe $T/hello.o $T/kernel32.lib
lld-link /subsystem:console /entry:main /nodefaultlib /dynamicbase /fixed:no /out:$D/demo.exe $T/demo.o $T/msvcrt.lib $T/user32.lib
rm -rf $T
ls -la $D/*.exe
