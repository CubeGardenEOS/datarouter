#!/usr/bin/env python3
"""CLI security checks. Each check is isolated; failures never suppress later checks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import re
import shutil
import subprocess
import time
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['common', 'keep', 'port', 'registry', 'router', 'ring', 'scheduler',
           'signal', 'tlv', 'validation', 'platform_linux']

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--profile', choices=['security'], default='security')
    ap.add_argument('--datarouter', type=Path, required=True)
    ap.add_argument('--baseline-datarouter', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    result = {'contract_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (ROOT/'tests').glob('test_*.c')},
              'profile': 'security', 'partial_coverage': True, 'checks': [],
              'performance_gate': 'not-run', 'windows_runtime': 'not-run'}
    sys.path.insert(0, str(ROOT/'php'))
    from verify import Processes, native_build
    cproc = Processes(out)
    c_clean = False
    def interrupt(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for signum in (signal.SIGINT, signal.SIGTERM): signal.signal(signum, interrupt)
    def run(argv, name, cwd, env=None, timeout=120):
        argv = list(map(str, argv))
        child = cproc.spawn(argv, name, cwd=cwd, env=env)
        error = None
        try: cproc.wait(child, timeout)
        except (RuntimeError, subprocess.TimeoutExpired) as exc: error = str(exc)
        row = {'name': name, 'argv': argv, 'exit': child.returncode}
        if error: row['error'] = error
        result['checks'].append(row)
        return child.returncode == 0 and error is None
    try:
        for label, source in [('baseline', a.baseline_datarouter), ('candidate', a.datarouter)]:
            if source is None: continue
            source = source.resolve()
            result[label] = {'commit': subprocess.check_output(
                ['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip(),
                'sources': {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for d in ('src', 'include') for p in (source/d).rglob('*') if p.is_file()}}
            common = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DDATAROUTER_UNSAFE',
                      '-I'+str(source/'include'), '-I'+str(source/'src'), '-pthread']
            sources = [source/'src'/(s+'.c') for s in SOURCES]
            if os.environ.get('SANITIZER_LIB_DIR'):
                common += ['-L'+os.environ['SANITIZER_LIB_DIR'], '-Wl,-rpath,'+os.environ['SANITIZER_LIB_DIR']]
            result[label]['compiler'] = subprocess.check_output(['cc', '--version'], text=True).splitlines()[0]
            static_out = out/(label+'-static')
            static_out.mkdir()
            run(common + ['-Wall', '-Wextra', '-Werror', '-fanalyzer', '-c'] + sources + [source/'src/config.c', source/'src/switch.c'],
                label+'-static', static_out)
            for test in ['test_security', 'test_ring', 'test_layout', 'test_protocol_fuzz', 'test_signal', 'test_clock']:
                binary = out/(label+'-'+test)
                # Use the candidate's regression contract against both source trees.
                test_source = ROOT/'tests'/(test+'.c')
                copied = out/(label+'-'+test+'.c')
                copied.write_text(test_source.read_text().replace('../src/', str(source/'src')+'/'))
                if run(common + ['-g', '-O1', '-fno-omit-frame-pointer',
                                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                 copied] + sources + ['-o', binary], label+'-'+test+'-build', source):
                    result[label][test+'_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
                    cases = ['tlvs', 'layout', 'chunk', 'ref', 'keep', 'empty', 'ttl', 'lease', 'wrap', 'prefix', 'generation', 'recovery', 'writer_lifetime', 'pin_lifetime'] if test == 'test_security' else ['all']
                    for case in cases:
                        run([binary, case], label+'-'+test+'-'+case, source,
                            dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                                 UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
        result['c_cleanup'] = cproc.cleanup()
        c_clean = True
        runtime_out = out/'runtime'
        runtime_out.mkdir()
        proc = Processes(runtime_out)
        try:
            native_build(proc)
            try:
                proc.run(['cargo', 'test', '--release', '--tests', '--', '--test-threads=1'],
                         'rust-tests', 600, cwd=ROOT)
                result['checks'].append(dict(name='candidate-rust-tests', exit=0))
            except (RuntimeError, subprocess.TimeoutExpired) as exc:
                result['checks'].append(dict(name='candidate-rust-tests', exit=1, error=str(exc)))
            # Build real consumers from their current working sources without
            # changing their manifests, lockfiles, or submodule checkouts.
            assembly = ROOT.parent/'CGEOS'
            for relative in ('core', 'services/portal', 'services/cgeai'):
                consumer = assembly/relative
                name = 'candidate-consumer-'+consumer.name
                if not (consumer/'Cargo.toml').is_file():
                    result['checks'].append(dict(name=name, exit=1, error='consumer checkout missing'))
                    continue
                stage = runtime_out/('consumer-'+consumer.name)
                stage.mkdir()
                for path in consumer.iterdir():
                    if path.name not in ('Cargo.toml', 'Cargo.lock', '.git', 'target'):
                        (stage/path.name).symlink_to(path.resolve(), target_is_directory=path.is_dir())
                manifest = (consumer/'Cargo.toml').read_text()
                dependency = 'path = "lib/datarouter"'
                if dependency not in manifest:
                    result['checks'].append(dict(name=name, exit=1, error='unrecognized DataRouter dependency'))
                    continue
                manifest = re.sub(r'path\s*=\s*"([^"]+)"',
                                  lambda m: 'path = '+json.dumps(str(ROOT if m[1] == 'lib/datarouter'
                                                                     else (consumer/m[1]).resolve())), manifest)
                (stage/'Cargo.toml').write_text(manifest)
                if (consumer/'Cargo.lock').is_file():
                    shutil.copy2(consumer/'Cargo.lock', stage/'Cargo.lock')
                try:
                    proc.run(['cargo', 'check', '--offline', '--manifest-path', stage/'Cargo.toml',
                              '--target-dir', ROOT/'target/consumer-check'], name, 900, cwd=stage)
                    result['checks'].append(dict(name=name, exit=0))
                except (RuntimeError, subprocess.TimeoutExpired) as exc:
                    result['checks'].append(dict(name=name, exit=1, error=str(exc)))
            try:
                proc.run(['python3', ROOT/'tools/windows_check.py', '--datarouter', ROOT,
                          '--out', runtime_out/'windows'], 'windows-compile', 600, cwd=ROOT)
                result['checks'].append(dict(name='candidate-windows-compile', exit=0))
            except (RuntimeError, subprocess.TimeoutExpired) as exc:
                result['checks'].append(dict(name='candidate-windows-compile', exit=1, error=str(exc)))
            try:
                proc.run(['cargo', 'package', '--allow-dirty', '--locked', '--offline'],
                         'cargo-package', 600, cwd=ROOT)
                result['checks'].append(dict(name='candidate-cargo-package', exit=0))
            except (RuntimeError, subprocess.TimeoutExpired) as exc:
                result['checks'].append(dict(name='candidate-cargo-package', exit=1, error=str(exc)))
            try:
                proc.run(['python3', ROOT/'tools/packaging_check.py', '--datarouter', ROOT,
                          '--out', runtime_out/'python-packaging'], 'python-packaging', 900, cwd=ROOT)
                result['checks'].append(dict(name='candidate-python-packaging', exit=0))
            except (RuntimeError, subprocess.TimeoutExpired) as exc:
                result['checks'].append(dict(name='candidate-python-packaging', exit=1, error=str(exc)))
        except BaseException as exc:
            result['checks'].append(dict(name='candidate-rust-tests', exit=1, error=str(exc)))
        finally:
            result['cleanup'] = proc.cleanup()
        result['status'] = 'passed'  if all(c['exit'] == 0 for c in result['checks']
                                          if c['name'].startswith('candidate-')) else 'failed'
    except BaseException as exc:
        result.update(status='failed', error=str(exc))
    finally:
        for signum in (signal.SIGINT, signal.SIGTERM): signal.signal(signum, signal.SIG_IGN)
        if not c_clean:
            try: result['c_cleanup'] = cproc.cleanup()
            except BaseException as exc: result.update(status='failed', cleanup_error=str(exc))
        (out/'results.json').write_text(json.dumps(result, indent=2)+'\n')
    print(result['status']+': '+str(out/'results.json'), flush=True)
    return 0 if result['status'] == 'passed' else 1

if __name__ == '__main__':
    raise SystemExit(main())
