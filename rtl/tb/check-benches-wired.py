#!/usr/bin/env python3
"""Refuse a testbench in rtl/tb/ that nothing runs.

This repository has a second bench tree, separate from the core repo's rtl/sim/,
with its own naming convention: *_tb.v here, tb_*.sv there. The gate in the core
repo scans only its own tree and its glob would not match these names, so this one
covers the ss_* and snac_* benches -- which is to say the save-state controller,
the DMA, the CRC, the quiesce handshake, the serialiser, the register shadow and
the state fanout. The most bug-prone code in the project.

All ten are wired today. This is here so they stay that way, because the failure
mode is silent: in the core repo tb_akiko_txrx_dma failed 36 of its 48 checks for
months while unreferenced, and tb_ss_regbus_mux was red across four PRs. A bench
nobody runs reads as coverage.

No allowlist, unlike the core repo's copy: everything here builds under Icarus and
there is nothing to excuse. If that ever changes, add the mechanism then rather
than leaving a hole now.

Run from the repository root.
"""
import glob
import os
import sys


def main():
    benches = sorted(glob.glob(os.path.join('rtl', 'tb', '*_tb.v')))
    if not benches:
        print('no benches found in rtl/tb -- run this from the repository root')
        return 1

    workflows = (glob.glob(os.path.join('.github', 'workflows', '*.yml')) +
                 glob.glob(os.path.join('.github', 'workflows', '*.yaml')))
    if not workflows:
        print('no workflow files found -- run this from the repository root')
        return 1

    text = []
    for w in workflows:
        with open(w, encoding='utf-8') as fh:
            text.append(fh.read())
    # Runner scripts a workflow may call instead of naming the bench directly.
    runners = []
    for r in glob.glob(os.path.join('rtl', 'tb', 'run*.sh')):
        with open(r, encoding='utf-8', errors='ignore') as fh:
            runners.append((r.replace(os.sep, '/'), fh.read()))
    wf = '\n'.join(text)

    unwired = []
    for path in benches:
        name = os.path.basename(path)
        if name in wf:
            continue
        # A runner counts only if the workflow calls THAT script, matched by path
        # rather than basename -- a basename match could credit the wrong bench.
        if any(name in body and rp in wf for rp, body in runners):
            continue
        unwired.append(path)

    if unwired:
        print('These benches exist and nothing runs them:')
        for p in unwired:
            print('    - %s' % p)
        print()
        print('Wire each into .github/workflows/, or into a runner script the')
        print('workflow calls.')
        return 1

    print('%d bench(es) in rtl/tb, all run by the workflow' % len(benches))
    return 0


if __name__ == '__main__':
    sys.exit(main())
