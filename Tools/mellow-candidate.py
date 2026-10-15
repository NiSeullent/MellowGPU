#!/usr/bin/env python3
"""Stage a new immutable source/patch/config evidence bundle; never builds or installs."""
# SPDX-License-Identifier: MIT
import argparse
from pathlib import Path
import sys
from mellow_port.candidate import PortError, canonical, stage_candidate


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('stage',))
    parser.add_argument('--lock', type=Path, default=Path(__file__).resolve().parents[1] / 'porting/upstreams.lock.json')
    parser.add_argument('--component', required=True)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--project-root', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        result = stage_candidate(args.lock, args.component, args.source_root, args.out, args.project_root)
        sys.stdout.write(canonical(result)); return 0
    except (PortError, OSError, ValueError) as error:
        sys.stderr.write(canonical({'status': 'failed', 'error': str(error), 'ready': False, 'build_eligible': False,
                                   'hardware_qualified': False, 'production_promotion_available': False})); return 1


if __name__ == '__main__': raise SystemExit(main())
