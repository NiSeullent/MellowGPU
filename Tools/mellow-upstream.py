#!/usr/bin/env python3
"""Validate/verify pinned upstreams and report candidates; never install or promote."""
# SPDX-License-Identifier: MIT
import argparse
import os
from pathlib import Path
import sys
from mellow_port.upstreams import PortError, canonical, discover, load_lock, report, verify_source


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('operation', choices=('validate', 'verify-source', 'discover', 'report'))
    p.add_argument('--lock', type=Path, default=Path(__file__).resolve().parents[1] / 'porting/upstreams.lock.json')
    p.add_argument('--component')
    p.add_argument('--source-root', type=Path)
    p.add_argument('--project-root', type=Path)
    p.add_argument('--candidate-lock', type=Path)
    p.add_argument('--output', type=Path, help='New exclusive output; existing reports and locks are never overwritten')
    args = p.parse_args(argv)
    try:
        lock = load_lock(args.lock)
        if args.operation == 'verify-source':
            if not args.component or args.source_root is None:
                raise PortError('verify-source requires --component and --source-root')
            result = verify_source(lock, args.component, args.source_root, args.project_root)
        elif args.operation == 'discover': result = discover(lock)
        elif args.operation == 'report': result = report(lock, load_lock(args.candidate_lock) if args.candidate_lock else None)
        else:
            result = {'schema_version': 1, 'lock_valid': True, 'components': len(lock['components']),
                      'source_verified': False, 'hardware_qualified': False, 'production_promotion_available': False}
        payload = canonical(result)
        if args.output:
            # No parent creation, symlink traversal, replacement, or production-lock mutation.
            from mellow_port.core import no_links
            target = no_links(args.output)
            with target.open('x', encoding='utf-8') as stream:
                stream.write(payload); stream.flush(); os.fsync(stream.fileno())
        else: sys.stdout.write(payload)
        return 0
    except (PortError, OSError, ValueError) as e:
        sys.stderr.write(canonical({'error': str(e), 'hardware_qualified': False, 'production_promotion_available': False}))
        return 1


if __name__ == '__main__': raise SystemExit(main())
