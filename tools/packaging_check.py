#!/usr/bin/env python3
"""Build a wheel from the actual sdist, then inspect its bundled native ABI."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--datarouter', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    root, out = args.datarouter.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    python = os.environ.get('DATAROUTER_PACKAGE_PYTHON', sys.executable)
    result = dict(status='failed', checks=[], python=python)
    env = dict(os.environ, CARGO_NET_OFFLINE='true')
    def run(argv, name):
        result['checks'].append(dict(name=name, argv=list(map(str, argv))))
        subprocess.run(argv, check=True, env=env, cwd=out)
    try:
        result['tools'] = json.loads(subprocess.check_output([python, '-c',
            'import json,sys; from importlib.metadata import version; '
            'print(json.dumps(dict(python=sys.version, **{n:version(n) for n in ("build","setuptools","wheel")})))'], text=True))
        run([python, '-m', 'build', '--sdist', '--no-isolation', '--outdir', out/'dist', root/'python'], 'sdist')
        packages = list((out/'dist').glob('*.tar.gz'))
        if len(packages) != 1: raise RuntimeError('missing or ambiguous sdist')
        source = out/'source'
        source.mkdir()
        with tarfile.open(packages[0]) as archive:
            archive.extractall(source, filter='data')
        projects = list(source.iterdir())
        if len(projects) != 1: raise RuntimeError('unexpected sdist roots')
        project = projects[0]
        native = project/'_native_source'
        for name in ('src/keep.c','src/tlv.c','src/common.c','Cargo.lock','LICENSE','.cargo_vcs_info.json'):
            if not (native/name).is_file(): raise RuntimeError('missing bundled source: '+name)
        commit = json.loads((native/'.cargo_vcs_info.json').read_text())['git']['sha1']
        if (project/'LICENSE').read_bytes() != (root/'LICENSE').read_bytes():
            raise RuntimeError('Python license differs from repository license')
        run([python, '-m', 'build', '--wheel', '--no-isolation', '--outdir', out/'wheels', project], 'wheel-from-sdist')
        wheels = list((out/'wheels').glob('*.whl'))
        if len(wheels) != 1 or wheels[0].name.endswith('-any.whl'):
            raise RuntimeError('wheel missing or not platform-specific')
        with zipfile.ZipFile(wheels[0]) as wheel:
            library = out/'libblinkcms_datarouter.so'
            target = 'blinkcms_datarouter/_native/libblinkcms_datarouter.so'
            libraries = [name for name in wheel.namelist() if name == target or
                         name.endswith(('.data/purelib/'+target, '.data/platlib/'+target))]
            if len(libraries) != 1: raise RuntimeError('missing or ambiguous wheel native library')
            library.write_bytes(wheel.read(libraries[0]))
            if not any('/licenses/LICENSE' in name or name.endswith('.dist-info/LICENSE') for name in wheel.namelist()):
                raise RuntimeError('wheel license missing')
        lib = ctypes.CDLL(str(library))
        lib.datarouter_version.restype = ctypes.c_char_p
        if lib.datarouter_version().decode() != commit: raise RuntimeError('wheel lost Git build identity')
        for symbol in ('datarouter_frontend_open_v3','datarouter_protocol_frame_layout','datarouter_keep_get_ref'):
            getattr(lib, symbol)
        result.update(status='passed', git_commit=commit, artifacts={
            str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (packages[0],wheels[0],library)})
    except Exception as exc:
        result['error'] = str(exc)
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    return 0 if result['status']=='passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
