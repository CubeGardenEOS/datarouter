#!/usr/bin/env python3
"""Build only PHP against this checkout, using the official phpize flow."""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def command(args, **kw):
    return subprocess.check_output([str(x) for x in args], text=True, stderr=subprocess.STDOUT, **kw).strip()

def tool(env, name):
    value = os.environ.get(env, name)
    path = shutil.which(value)
    if not path:
        raise RuntimeError(f"missing dependency {env}={value}")
    return str(Path(path).resolve())

def environment():
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        raise RuntimeError('requires Linux x86_64')
    t = {key: tool(key, name) for key, name in [('PHP', 'php'), ('PHP_FPM', 'php-fpm'), ('PHPIZE', 'phpize'), ('PHP_CONFIG', 'php-config')]}
    info = json.loads(command([t['PHP'], '-n', '-r', 'echo json_encode([PHP_VERSION, PHP_ZTS, PHP_INT_SIZE]);']))
    if not info[0].startswith('8.2.') or info[1] or info[2] != 8:
        raise RuntimeError(f'requires PHP 8.2 NTS 64-bit: {info}')
    if command([t['PHP_CONFIG'], '--version']) != info[0] or f'PHP {info[0]} ' not in command([t['PHP_FPM'], '-n', '-v']):
        raise RuntimeError('CLI/FPM/php-config version mismatch')
    api = re.search(r'^PHP API => (\d+)$', command([t['PHP'], '-n', '-i']), re.M).group(1)
    fpm_info = command([t['PHP_FPM'], '-n', '-i'])
    if f'PHP API => {api}' not in fpm_info or 'Thread Safety => disabled' not in fpm_info:
        raise RuntimeError('FPM ABI/NTS mismatch')
    if api not in command([t['PHPIZE'], '--version']):
        raise RuntimeError('phpize ABI mismatch')
    inc = Path(command([t['PHP_CONFIG'], '--include-dir']))
    header = (inc / 'main/php_version.h').read_text()
    if f'"{info[0]}"' not in header:
        raise RuntimeError('development headers version mismatch')
    return dict(compiler=command(['cc','--version']).splitlines()[0], rustc=command(['rustc','--version']), affinity=sorted(os.sched_getaffinity(0)), cpu_info=Path('/proc/cpuinfo').read_text().split('\n\n',1)[0], build_environment={k:os.environ.get(k,'') for k in ('PATH','PERL5LIB')}, tools=t, version=info[0], abi=api, machine=platform.platform(), jit=False, php_configure=command([t['PHP_CONFIG'], '--configure-options']), tool_sha256={k:fingerprint(v) for k,v in t.items()})

def fingerprint(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def build(meta=None):
    meta = meta or environment()
    dest = ROOT / 'build/php'
    dest.mkdir(parents=True, exist_ok=True)
    for name in ('config.m4', 'datarouter.c'):
        shutil.copy2(ROOT / 'php' / name, dest / name)
    commit = command(['git', '-C', ROOT, 'rev-parse', 'HEAD'])
    (dest / 'version.h').write_text(f'#define CGEOS_GIT_COMMIT "{commit}"\n')
    for args in ([meta['tools']['PHPIZE']], ['./configure', f"--with-php-config={meta['tools']['PHP_CONFIG']}", f'--with-datarouter={ROOT}'], ['make', '-j4']):
        with (dest / 'build.log').open('a') as log:
            subprocess.run(args, cwd=dest, stdout=log, stderr=subprocess.STDOUT, check=True)
    extension = dest / 'modules/datarouter.so'
    meta.update(commit=commit, extension=str(extension), extension_sha256=fingerprint(extension), library_sha256=fingerprint(ROOT / 'build/libblinkcms_datarouter.so'), lock_sha256=fingerprint(ROOT / 'Cargo.lock'), source_sha256={str(p.relative_to(ROOT)): fingerprint(p) for p in sorted((ROOT / 'php').rglob('*')) if p.is_file() and '__pycache__' not in p.parts})
    (dest / 'manifest.json').write_text(json.dumps(meta, indent=2)+'\n')
    command([meta['tools']['PHP'], '-n', '-d', f'extension={extension}', '-r', 'if (!extension_loaded("datarouter")) exit(1);'])
    return meta

if __name__ == '__main__':
    print(json.dumps(build(), indent=2))
