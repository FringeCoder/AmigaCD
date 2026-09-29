#!/usr/bin/env python3
"""Generate compile_commands.json so clangd indexes the whole tree.

The .clangd file supplies the FLAGS. It does not supply the file list, and
clangd's background indexer only indexes translation units it knows about -- so
with .clangd alone a cross-file "find references" still comes back empty, which
is the exact failure this was meant to fix. It reads as "nothing uses this
symbol" rather than as an error, and that has already sent work in the wrong
direction more than once in this project.

Emitting a compilation database fixes it: clangd indexes every entry in the
background and cross-file references start resolving.

Written by hand rather than with bear or compiledb, for two reasons. The build
runs under WSL and would produce /mnt/c paths that Windows clangd cannot follow,
and the file list is derivable from the Makefile anyway -- it compiles everything
under the tracked source directories.

    python scripts/gen-compile-commands.py

Regenerate after adding source files or changing the Makefile's INCLUDE/DFLAGS.
The output is gitignored: it contains absolute paths to this checkout and to the
toolchain, so it is per-machine.
"""
import glob
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLCHAIN = 'gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf'

# Mirrors the Makefile: INCLUDE at lines 19-26, DFLAGS at 52. Kept in step by
# hand; the alternative is parsing the Makefile, which is more to go wrong than
# eight paths and ten defines.
INCLUDES = ['.', 'lib/libco', 'lib/miniz', 'lib/md5', 'lib/lzma',
            'lib/zstd/lib', 'lib/libchdr/include', 'lib/bluetooth']

DEFINES = ['_7ZIP_ST', 'PACKAGE_VERSION="1.3.3"', 'HAVE_LROUND',
           'HAVE_STDINT_H', 'HAVE_STDLIB_H', 'HAVE_SYS_PARAM_H',
           'ENABLE_64_BIT_WORDS=0', '_FILE_OFFSET_BITS=64',
           '_LARGEFILE64_SOURCE', 'VDATE="000000"']

# Where the sources live. tests/ is included deliberately: those files are the
# CALLERS of the pure modules, so leaving them out is precisely what makes a
# reference search on akiko_frame_valid come back with only its own file.
SOURCE_GLOBS = ['*.cpp', '*.c',
                'support/*/*.cpp', 'support/*/*.c',
                'tests/*.cpp']

# Vendored third-party code. Indexing it costs minutes and buys nothing: we do
# not navigate it and its diagnostics are not ours to act on.
SKIP_PREFIXES = ('lib/', 'build/', 'bin/', TOOLCHAIN)


def sysroot_flags():
    base = os.path.join(TOOLCHAIN, 'arm-none-linux-gnueabihf')
    if not os.path.isdir(os.path.join(REPO, base)):
        print('WARNING: %s is not present.' % base)
        print('         Run ./setup_default_toolchain.sh first, or clangd will')
        print('         fall back to host headers and most files will not parse.')
        return []
    cxx = os.path.join(base, 'include', 'c++')
    vers = sorted(os.listdir(os.path.join(REPO, cxx))) if os.path.isdir(os.path.join(REPO, cxx)) else []
    if not vers:
        print('WARNING: no C++ headers under %s' % cxx)
        return []
    v = vers[-1]
    return ['--target=arm-none-linux-gnueabihf',
            '--sysroot=' + os.path.join(base, 'libc'),
            '-isystem', os.path.join(cxx, v),
            '-isystem', os.path.join(cxx, v, 'arm-none-linux-gnueabihf')]


def main():
    os.chdir(REPO)

    files = []
    for pat in SOURCE_GLOBS:
        for f in glob.glob(pat):
            rel = f.replace(os.sep, '/')
            if rel.startswith(SKIP_PREFIXES):
                continue
            files.append(rel)
    files = sorted(set(files))
    if not files:
        print('no sources found -- is this the repository root?')
        return 1

    base = (['clang++'] + sysroot_flags() +
            # Plain char is UNSIGNED on the ARM ABI. Without this,
            # support/minimig/minimig_boot.cpp reports a wall of narrowing errors
            # for a font table that is perfectly correct for its target, and
            # drops out of the index.
            ['-funsigned-char', '-std=gnu++14'] +
            ['-D' + d for d in DEFINES] +
            ['-I' + i for i in INCLUDES])

    db = [{'directory': REPO,
           'file': os.path.join(REPO, f.replace('/', os.sep)),
           'arguments': base + [f]}
          for f in files]

    with open('compile_commands.json', 'w', encoding='utf-8') as fh:
        json.dump(db, fh, indent=1)

    print('compile_commands.json: %d translation unit(s)' % len(db))
    print('  including %d under tests/, which is what makes cross-file'
          ' references resolve' % len([f for f in files if f.startswith('tests/')]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
