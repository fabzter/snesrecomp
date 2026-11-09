#!/usr/bin/env python3
"""Run the ROM-free signed-OAM-coordinate PPU contract.

The unimplemented API must fail loudly, never look like a skipped pass: a
probe compile of the shared interface runs first and reports FAIL explicitly
when PpuWsSetOamCoordinates is not yet available."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

PROBE = """\
#include "snes/ppu.h"
int main(void) {
  PpuWsOamCoordinates c;
  Ppu p;
  (void)p;
  PpuWsSetOamCoordinates(&p, &c);
  PpuWsSetOamCoordinates(&p, 0);
  return (int)sizeof c.valid;
}
"""

FAIL_SRC = """\
#include <stdio.h>
int main(void) {
  fputs("ppu_oam_coordinates: FAIL "
        "PpuWsSetOamCoordinates/PpuWsOamCoordinates unavailable\\n",
        stderr);
  return 1;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='snes-oam-coords-') as folder:
        folder = Path(folder)
        probe_src = folder / 'probe.c'
        probe_src.write_text(PROBE)
        probe_bin = folder / 'probe.exe'
        probe = subprocess.run(
            [args.cc, '-std=c11', '-fsyntax-only',
             '-I' + str(root / 'runner/src'),
             '-I' + str(root / 'runner/src/snes'),
             str(probe_src)],
            capture_output=True, text=True)
        if probe.returncode != 0:
            # API not implemented: fail explicitly, never pass as "skipped".
            fail_src = folder / 'fail.c'
            fail_src.write_text(FAIL_SRC)
            fail_bin = folder / 'fail.exe'
            subprocess.run([args.cc, '-std=c11', str(fail_src),
                            '-o', str(fail_bin)], check=True)
            subprocess.run([str(fail_bin)])
            print(probe.stderr, file=sys.stderr)
            sys.exit(1)
        binary = folder / 'ppu_oam_coordinates.exe'
        command = [args.cc, '-std=c11', '-O2', '-Wall', '-Wextra',
                   '-DSNESRECOMP_REVERSE_DEBUG=0',
                   '-I' + str(root / 'runner/src'),
                   '-I' + str(root / 'runner/src/snes'),
                   str(root / 'tests/ppu/ppu_oam_coordinates_test.c'),
                   str(root / 'runner/src/snes/ppu.c'),
                   str(root / 'runner/src/snes/ppu_legacy.c'),
                   '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
