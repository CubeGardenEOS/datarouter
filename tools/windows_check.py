#!/usr/bin/env python3
"""Compile and link the native Windows implementation using the WSL host MSVC.
No services are started. Invoked by the cgeos-cli security profile.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def windows_path(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()


def windows_local_target(root, commit):
    local_app_data = subprocess.check_output(
        ['/mnt/c/Windows/System32/cmd.exe', '/d', '/c', 'echo %LOCALAPPDATA%'],
        text=True).strip()
    if not local_app_data or '%' in local_app_data:
        raise RuntimeError('Windows LOCALAPPDATA is unavailable')
    target = local_app_data.rstrip('\\/') + '\\CGEOS\\datarouter\\target-' + root.name + '-' + commit[:12]
    target_unix = Path(subprocess.check_output(['wslpath', '-u', target], text=True).strip())
    target_unix.mkdir(parents=True, exist_ok=True)
    if not os.access(target_unix, os.W_OK):
        raise RuntimeError('Windows-local Cargo target is not writable: ' + target)
    return target


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--datarouter', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = args.datarouter.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    result = {'runtime': 'not-run', 'status': 'failed'}
    try:
        vswhere = Path('/mnt/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')
        install = subprocess.check_output([vswhere, '-latest', '-products', '*', '-requires',
                                           'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                           '-property', 'installationPath'], text=True).strip()
        if not install:
            raise RuntimeError('MSVC installation not found')
        result['installation'] = install
        paths = [root/'src'/(name+'.c') for name in (
            'common', 'keep', 'port', 'registry', 'router', 'ring', 'scheduler', 'signal',
            'tlv', 'validation', 'platform_windows', 'config', 'switch')]
        command = 'cl /nologo /std:c11 /experimental:c11atomics /O2 /D_CRT_SECURE_NO_WARNINGS /DDATAROUTER_UNSAFE '
        command += '/I"'+windows_path(root/'include')+'" /I"'+windows_path(root/'src')+'" '
        command += ' '.join('"'+windows_path(path)+'"' for path in paths)
        command += ' /Fedatarouter-switch.exe /link kernel32.lib user32.lib'
        commit = subprocess.check_output(['git','-C',root,'rev-parse','HEAD'], text=True).strip()
        (out/'version.h').write_text('#define CGEOS_GIT_COMMIT \"'+commit+'\"\n')
        command = command.replace(' /link ', ' /FI\"'+windows_path(out/'version.h')+'\" /link ')
        rust_target = windows_local_target(root, commit)
        rust_command = subprocess.list2cmdline(['cargo','check','--lib','--offline','--locked',
            '--manifest-path',windows_path(root/'Cargo.toml'),'--target-dir',rust_target])
        batch = out/'compile.cmd'
        batch.write_text('@echo off\r\nsetlocal\r\nset VSCMD_SKIP_SENDTELEMETRY=1\r\nset CARGO_INCREMENTAL=0\r\n'
                         'call "'+install+'\\VC\\Auxiliary\\Build\\vcvars64.bat"\r\n'
                         'if errorlevel 1 exit /b 1\r\n'
                         'pushd "'+windows_path(out)+'"\r\n'
                         'if errorlevel 1 exit /b 1\r\nset CGEOS_GIT_COMMIT='+commit+'\r\n'+command+'\r\n'
                         'set DR_COMPILE_RESULT=%ERRORLEVEL%\r\nif not %DR_COMPILE_RESULT%==0 goto finished\r\n'+rust_command+'\r\n'
                         'set DR_COMPILE_RESULT=%ERRORLEVEL%\r\n:finished\r\npopd\r\nexit /b %DR_COMPILE_RESULT%\r\n')
        result['rust_command'] = rust_command
        result['rust_target_dir'] = rust_target
        result['git_commit'] = commit
        rc = subprocess.call(['/mnt/c/Windows/System32/cmd.exe', '/d', '/c', windows_path(batch)])
        result.update(exit=rc, status='passed' if rc == 0 else 'failed')
    except Exception as exc:
        result['error'] = str(exc)
    (out/'results.json').write_text(json.dumps(result, indent=2)+'\n')
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
