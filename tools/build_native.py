#!/usr/bin/env python3
"""Rebuild and select the switch from Cargo's current build-script message."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

def build(root, target):
    root, target = root.resolve(), target.resolve()
    result = subprocess.run(['cargo', 'build', '--release', '--locked',
                             '--target-dir', str(target), '--message-format=json'],
                            cwd=root, text=True, stdout=subprocess.PIPE, check=True)
    messages = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    outputs = [Path(m['out_dir']) for m in messages
               if m.get('reason') == 'build-script-executed'
               and 'blinkcms-datarouter@' in m.get('package_id', '')]
    if len(outputs) != 1:
        raise RuntimeError(f'expected one current DataRouter build script, got {outputs}')
    switch = outputs[0]/'datarouter-switch'
    if not switch.is_file(): raise RuntimeError('current build did not produce a switch')
    out = root/'build'
    out.mkdir(exist_ok=True)
    shutil.copy2(switch, out/'datarouter-switch')
    archive = target/'release/libblinkcms_datarouter.a'
    compiler = os.environ.get('CC') or 'cc'
    link_argv = [compiler, '-shared', '-o', str(out/'libblinkcms_datarouter.so'),
                    '-Wl,--whole-archive', str(archive), '-Wl,--no-whole-archive',
                    '-lrt', '-lpthread', '-ldl']
    subprocess.run(link_argv, check=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    commands = outputs[0]/'build-commands.txt'
    if 'build-commands.txt' in (root/'build.rs').read_text() and not commands.is_file():
        raise RuntimeError('current build is missing its native compiler command record')
    record = dict(commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
                  rustc=subprocess.check_output(['rustc', '--version'], text=True).strip(),
                  cc=subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
                  cargo_argv=result.args, out_dir=str(outputs[0]),
                  native_commands=commands.read_text() if commands.is_file() else None,
                  shared_link_argv=link_argv,
                  build_inputs={name:sha(root/name) for name in ('build.rs','Cargo.toml','Cargo.lock')},
                  environment={k: os.environ.get(k) for k in ('CC', 'CFLAGS', 'RUSTFLAGS', 'DATAROUTER_CHANNEL_SIZE', 'DATAROUTER_WAIT_DIAGNOSTICS')},
                  sources={str(p.relative_to(root)): sha(p) for d in ('src', 'include')
                           for p in (root/d).rglob('*') if p.is_file()},
                  binaries={p.name: sha(p) for p in (switch, archive, out/'libblinkcms_datarouter.so')})
    (out/'native-build.json').write_text(json.dumps(record, indent=2)+'\n')
    return record

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--datarouter', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--target-dir', type=Path)
    args = ap.parse_args()
    build(args.datarouter, args.target_dir or args.datarouter/'build/native-target')
