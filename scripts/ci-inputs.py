"""Freeze and verify the prepared inputs used for cross-host native verification."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

NAMES = ('tier0.bin', 'tier0.flags', 'tier0.flags.layout', 'tier0.highway',
         'tier0.highway.layout', 'tier0.highway.keys', 'tier0.highway.nodes')


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('freeze', 'verify'))
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    if args.operation == 'freeze':
        t0 = Path(os.environ['LAPLACE_TIER0'])
        stem = str(t0.with_suffix(''))
        flags = Path(os.environ.get('LAPLACE_FLAGS') or stem + '.flags')
        highway = Path(os.environ.get('LAPLACE_HIGHWAY') or stem + '.highway')
        sources = (t0, flags, Path(str(flags) + '.layout'), highway,
                   Path(str(highway) + '.layout'), Path(str(highway) + '.keys'), Path(str(highway) + '.nodes'))
        # Use the installation's cooperative lock, also held by deployment and preparation.
        with open(os.environ['LAPLACE_HOST_LOCK'], 'a') as lock:
            subprocess.run(['flock', '-s', '-w', '300', str(lock.fileno())], pass_fds=(lock.fileno(),), check=True)
            directory.mkdir(parents=True, exist_ok=False)
            for name, source in zip(NAMES, sources):
                if not source.is_file() or not source.stat().st_size:
                    raise ValueError(f'Missing prepared input: {source}')
                shutil.copyfile(source, directory / name)
            manifest = {name: {'sha256': digest(directory / name), 'bytes': (directory / name).stat().st_size}
                        for name in NAMES}
            (directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            fcntl.flock(lock, fcntl.LOCK_UN)
    manifest = json.loads((directory / 'manifest.json').read_text())
    if set(manifest) != set(NAMES):
        raise ValueError('The prepared-input manifest has an incomplete or unexpected closure')
    for name in NAMES:
        path = directory / name
        if path.stat().st_size != manifest[name]['bytes'] or digest(path) != manifest[name]['sha256']:
            raise ValueError(f'Prepared input digest mismatch: {name}')
    with open(os.environ['GITHUB_ENV'], 'a') as env:
        for name, filename in (('LAPLACE_TIER0', 'tier0.bin'), ('LAPLACE_FLAGS', 'tier0.flags'), ('LAPLACE_HIGHWAY', 'tier0.highway')):
            env.write(f'{name}={directory / filename}\n')
    print(json.dumps({'prepared_input_manifest': digest(directory / 'manifest.json'), 'files': len(NAMES)}))


if __name__ == '__main__':
    main()
