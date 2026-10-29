#!/bin/sh
# chip RAM bench: passes/s (4 KB each) for read, write, fast-to-chip copy
cd /media/fat/_Test/armuae
B=${B:-./armuae5}
for m in 0 1 2; do
  r=$(timeout 30 taskset -c 0 $B -b $OPTS -r chip$m.rom -C 68020 -j 8192 -t 6 -a 0x400 2>&1 | grep "count" | grep "+" | tail -1 | sed 's/.*(+\([0-9]*\)\/s).*/\1/')
  echo "chip$m $OPTS: $r passes/s"
done
