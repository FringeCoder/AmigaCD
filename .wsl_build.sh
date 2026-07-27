#!/bin/bash
set -e
export PATH=/opt/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf/bin:/usr/local/bin:/usr/bin:/bin
cd /mnt/c/LLM/MiSTer/CD32/research/repos/Main_MiSTer
rm -f bin/support/minimig/akiko_cd32.cpp.o bin/support/minimig/akiko_cd32.cpp.d
make 2>&1 | tail -20
ls -la bin/MiSTer
md5sum bin/MiSTer
