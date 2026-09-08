#!/usr/bin/env python3
"""cgeos-cli owned PHP gate. No missing result, timeout or skipped case passes."""
from __future__ import annotations
import argparse
import concurrent.futures
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
from build import ROOT, environment, build, fingerprint, command

class Processes:
    def __init__(self, out):
        self.out = out
        self.children = []
        self.shm = set()
        self.known = set()
        self.before = set(Path('/dev/shm').iterdir())
        self.done = threading.Event()
        ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)  # become child subreaper on Linux
        self.thread = threading.Thread(target=self.watch, daemon=True)
        self.thread.start()
    def watch(self):
        while not self.done.wait(.02):
            self.scan()
    def scan(self):
        # Track only descendants, never unrelated workspace services.
        ids = {p.pid for p in self.children} | self.known
        entries = {}
        for path in Path('/proc').glob('[0-9]*/stat'):
            try:
                tail = path.read_text().rsplit(')', 1)[1].split()
                entries[int(path.parent.name)] = int(tail[1])
            except (OSError, ValueError, IndexError): pass
        for _ in range(8):
            new = {pid for pid, parent in entries.items() if parent in ids}
            if new <= ids: break
            ids |= new
        self.known |= ids
        for pid in ids:
            try:
                for line in Path(f'/proc/{pid}/maps').read_text().splitlines():
                    if '/dev/shm/' in line:
                        path = Path(line.split('/dev/shm/', 1)[1].removesuffix(' (deleted)'))
                        path = Path('/dev/shm') / path
                        if path not in self.before: self.shm.add(path)
            except OSError: pass
    def spawn(self, args, name, cwd=ROOT, env=None):
        args = [str(x) for x in args]
        with (self.out / 'commands.jsonl').open('a') as f:
            f.write(json.dumps(dict(argv=args, cwd=str(cwd)))+'\n')
        log = (self.out / f'{name}.log').open('wb')
        try:
            p = subprocess.Popen(args, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        finally: log.close()
        self.children.append(p)
        return p
    def wait(self, p, timeout=120):
        try:rc = p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.stop(p);raise
        if rc:self.stop(p)
        if rc: raise RuntimeError(f'child pid={p.pid} exit={rc}; see logs')
    def run(self, args, name, timeout=120, **kw):
        p = self.spawn(args, name, **kw); self.wait(p, timeout); return p
    def stop(self, p):
        try: os.killpg(p.pid, signal.SIGTERM)
        except ProcessLookupError: pass
        try: p.wait(3)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL); p.wait(3)
        # A completed parent can still have live grandchildren in its group.
        try: os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError: pass
    def cleanup(self):
        self.scan(); self.done.set(); self.thread.join()
        for p in reversed(self.children): self.stop(p)
        # Killed grandchildren (e.g. compiler servers) can take a short time
        # to exit and become waitable after their original parent has exited.
        deadline=time.monotonic()+5
        while True:
            while True:
                try:
                    pid, _ = os.waitpid(-1, os.WNOHANG)
                    if not pid: break
                except ChildProcessError: break
            alive=[]
            for p in self.children:
                try:os.killpg(p.pid,0);alive.append(p.pid)
                except ProcessLookupError:pass
            if not alive or time.monotonic()>=deadline:break
            time.sleep(.02)
        for path in self.shm: path.unlink(missing_ok=True)
        if alive: raise RuntimeError(f'leftover process groups: {alive}')
        return dict(process_groups=len(self.children), shared_memory=len(self.shm), remaining=0)
    def switch(self, name, token=False):
        shm = '/dr-php-'+uuid.uuid4().hex
        path = Path('/dev/shm') / shm[1:]
        if path.exists(): raise RuntimeError('shared memory collision')
        self.shm.add(path)
        args = [ROOT/'build/datarouter-switch', '--name', shm, '--allow-any', '--max-ports', '8', '--ttl-ms', '60000']
        if token: args += ['--token', 'secret']
        if name=='restart-switch':args += ['--reap-dead-processes']
        p = self.spawn(args, name)
        deadline = time.monotonic()+5
        while not path.exists():
            if p.poll() is not None or time.monotonic()>deadline: raise RuntimeError('switch did not start')
            time.sleep(.01)
        time.sleep(.05)
        return shm, p

def native_build(proc):
    target = ROOT/'build/php-native-target'
    if not (ROOT/'Cargo.lock').is_file():proc.run(['cargo','generate-lockfile'],'native-lockfile')
    shutil.copy2(ROOT/'Cargo.lock',proc.out/'native-Cargo.lock')
    proc.run(['cargo','build','--release','--locked','--target-dir',target], 'native-build', 300)
    switches = list((target/'release/build').glob('*/out/datarouter-switch'))
    if len(switches) != 1: raise RuntimeError(f'ambiguous source-built switch: {switches}')
    shutil.copy2(switches[0], ROOT/'build/datarouter-switch')
    proc.run(['cc','-shared','-o',ROOT/'build/libblinkcms_datarouter.so','-Wl,--whole-archive',target/'release/libblinkcms_datarouter.a','-Wl,--no-whole-archive','-lrt','-lpthread','-ldl'], 'native-link')

def require_result(path, **expected):
    obj = json.loads(path.read_text())
    if obj.get('errors') != 0 or obj.get('fatal'): raise RuntimeError(f'failed result: {path}: {obj}')
    for key, value in expected.items():
        if obj.get(key) != value: raise RuntimeError(f'{path}: {key} expected {value}, got {obj.get(key)}')
    return obj

def fcgi(sock, script, rid):
    def record(kind, data):
        return struct.pack('!BBHHBB',1,kind,1,len(data),0,0)+data
    def length(n): return bytes([n]) if n<128 else struct.pack('!I',n|0x80000000)
    params = dict(SCRIPT_FILENAME=str(script), REQUEST_METHOD='GET', QUERY_STRING=str(rid), SERVER_PROTOCOL='HTTP/1.1', GATEWAY_INTERFACE='CGI/1.1', CONTENT_LENGTH='0', SERVER_NAME='localhost', SERVER_PORT='80')
    data = b''
    for k,v in params.items():
        k=k.encode();v=v.encode();data+=length(len(k))+length(len(v))+k+v
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(15);s.connect(str(sock));s.sendall(record(1,struct.pack('!HB5x',1,0))+record(4,data)+record(4,b'')+record(5,b''))
        output=b''
        def read(n):
            b=b''
            while len(b)<n:
                part=s.recv(n-len(b))
                if not part: raise RuntimeError('truncated FastCGI response')
                b+=part
            return b
        while True:
            version,kind,request,n,pad,_=struct.unpack('!BBHHBB',read(8))
            if version!=1 or request!=1: raise RuntimeError('invalid FastCGI header')
            body=read(n);read(pad)
            if kind==6: output+=body
            if kind==3:
                if len(body)!=8 or struct.unpack('!IB3x',body)!=(0,0): raise RuntimeError('FastCGI request failed')
                break
    header,body=output.split(b'\r\n\r\n',1)
    if b'Status: 500' in header: raise RuntimeError(body.decode())
    obj=json.loads(body)
    if obj.get('errors')!=0 or obj.get('rid')!=str(rid): raise RuntimeError(f'bad FPM response: {obj}')
    return obj

def fpm_test(proc, meta, php, workers, count):
    name=f'fpm-{workers}'
    shm, sw=proc.switch(name+'-switch')
    plane=1 if workers>1 else 0  # native direct ring permits one writer only
    echo=proc.spawn(php+[ROOT/'php/examples/cli.php',shm,'server',count,plane],name+'-echo')
    sock=proc.out/(name+'.sock')
    conf=proc.out/(name+'.conf')
    conf.write_text(f'''[global]
daemonize = no
error_log = {proc.out}/{name}-error.log
[php]
listen = {sock}
pm = static
pm.max_children = {workers}
pm.max_requests = 100
clear_env = no
catch_workers_output = yes
''')
    env=dict(os.environ,DR_SHM=shm,DR_PLANE=str(plane))
    fpm=proc.spawn([meta['tools']['PHP_FPM'],'-n','-d','extension='+meta['extension'],'-y',conf,'-F'],name,env=env)
    try:
        deadline=time.monotonic()+10
        while not sock.exists():
            if fpm.poll() is not None or time.monotonic()>deadline: raise RuntimeError('FPM startup failure')
            time.sleep(.01)
        start=time.perf_counter()
        def request(i):
            t=time.perf_counter();obj=fcgi(sock,ROOT/'php/examples/fpm.php',i)
            if obj['php']!=meta['version'] or obj['version']!=meta['commit']: raise RuntimeError('FPM loaded wrong version')
            return obj['pid'],(time.perf_counter()-t)*1e6
        pool=concurrent.futures.ThreadPoolExecutor(max_workers=workers)
        try:replies=list(pool.map(request,range(count),timeout=300))
        finally:pool.shutdown(wait=True,cancel_futures=True)
        proc.wait(echo,20)
        echo_result=json.loads((proc.out/(name+'-echo.log')).read_text().splitlines()[-1])
        if echo_result != dict(count=count,errors=0): raise RuntimeError('FPM echo count mismatch')
        pids={pid for pid,_ in replies}
        if count>workers*100 and len(pids)<=workers: raise RuntimeError('FPM workers not recycled')
        if workers>1 and len(pids)<workers: raise RuntimeError('FPM did not use all workers')
        samples=sorted(t for _,t in replies)
        return dict(case=name,data_plane='routed' if plane else 'direct',requests=count,errors=0,worker_pids=sorted(pids),elapsed_s=time.perf_counter()-start,p50_request_us=samples[len(samples)//2],p99_request_us=samples[int(len(samples)*.99)])
    finally:
        proc.stop(fpm);proc.stop(echo);proc.stop(sw);sock.unlink(missing_ok=True)

def cli_example(proc, php):
    shm,sw=proc.switch('cli-example-switch')
    server=proc.spawn(php+[ROOT/'php/examples/cli.php',shm,'server',5,0],'cli-example-server')
    try:
        time.sleep(.1)
        proc.run(php+[ROOT/'php/examples/cli.php',shm,'client',5,0],'cli-example-client')
        proc.wait(server,10)
        for side in ('server','client'):
            obj=json.loads((proc.out/f'cli-example-{side}.log').read_text().splitlines()[-1])
            if obj!=dict(count=5,errors=0):raise RuntimeError('CLI example count mismatch')
        return dict(case='cli-example',requests=5,errors=0)
    finally:proc.stop(server);proc.stop(sw)


def lifecycle_test(proc, php):
    checks=0
    for mode in ('success','failure','timeout','interrupt'):
        shm,sw=proc.switch('lifecycle-'+mode+'-switch')
        marker=proc.out/('lifecycle-'+mode+'.ready')
        p=proc.spawn(php+[ROOT/'php/tests/lifecycle.php',shm,mode,marker],'lifecycle-'+mode)
        try:
            end=time.monotonic()+5
            while not marker.exists():
                if p.poll() is not None or time.monotonic()>end:raise RuntimeError('lifecycle startup failure')
                time.sleep(.01)
            if mode=='success':
                if p.wait(5)!=0:raise RuntimeError('success exit mismatch')
            elif mode=='failure':
                if p.wait(5)!=42:raise RuntimeError('failure exit mismatch')
            elif mode=='timeout':
                try:p.wait(.05)
                except subprocess.TimeoutExpired:proc.stop(p)
                else:raise RuntimeError('timeout did not occur')
                if p.returncode!=-signal.SIGTERM:raise RuntimeError('timeout signal mismatch')
            else:
                os.killpg(p.pid,signal.SIGINT)
                if p.wait(5)!=-signal.SIGINT:raise RuntimeError('interrupt signal mismatch')
            checks+=1
        finally:proc.stop(p);proc.stop(sw)
    # Reproduce a completed build parent leaving a compiler-server child.
    script="import subprocess; subprocess.Popen(['sleep','30'])"
    parent=proc.spawn([sys.executable,'-c',script],'orphan-parent')
    proc.wait(parent,5)
    proc.stop(parent)
    checks+=1
    return dict(case='lifecycle',checks=checks,errors=0)


def restart_test(proc, php):
    directory=proc.out/'restart';directory.mkdir()
    shm,sw=proc.switch('restart-switch')
    victim_marker=directory/'victim-ready'
    victim=proc.spawn(php+[ROOT/'php/tests/lifecycle.php',shm,'interrupt',victim_marker],'restart-victim')
    child=proc.spawn(php+[ROOT/'php/tests/restart.php',shm,directory],'restart-client')
    def marker(name):
        end=time.monotonic()+15
        while not (directory/name).exists():
            if child.poll() is not None or time.monotonic()>end:raise RuntimeError('restart marker missing: '+name)
            time.sleep(.01)
    try:
        marker('ready')
        end=time.monotonic()+5
        while not victim_marker.exists():
            if time.monotonic()>end:raise RuntimeError('victim startup timeout')
            time.sleep(.01)
        os.killpg(victim.pid,signal.SIGKILL);victim.wait(5)
        time.sleep(1.1)
        # Peer exited abruptly; then close handles and recreate the switch.
        (directory/'down').write_text('1');marker('closed')
        proc.stop(sw)
        Path('/dev/shm',shm[1:]).unlink(missing_ok=True)
        sw=proc.spawn([ROOT/'build/datarouter-switch','--name',shm,'--allow-any','--max-ports','8'],'restart-new-switch')
        end=time.monotonic()+5
        while not Path('/dev/shm',shm[1:]).exists():
            if sw.poll() is not None or time.monotonic()>end:raise RuntimeError('restart failed')
            time.sleep(.01)
        end=time.monotonic()+5
        while 'start pid=' not in (proc.out/'restart-new-switch.log').read_text():
            if sw.poll() is not None or time.monotonic()>end:raise RuntimeError('switch not initialized')
            time.sleep(.01)
        (directory/'up').write_text('1');proc.wait(child,20)
        return require_result(directory/'result.json',case='restart',checks=2)
    finally:proc.stop(child);proc.stop(victim);proc.stop(sw)


def regressions(proc):
    target=ROOT/'build/php-native-target'
    failures=[]
    try:proc.run(['cargo','test','--locked','--no-fail-fast','--target-dir',target,'--','--test-threads=1'], 'native-regressions',300)
    except (RuntimeError,subprocess.TimeoutExpired) as e:failures.append(str(e))
    # Existing C tests are compiled verbatim with the existing C implementation.
    sources=['common','keep','port','registry','router','ring','scheduler','signal','tlv','validation','platform_linux','config']
    for name in ['test_config','test_layout','test_ring','test_switch']:
        dest=ROOT/'build'/name
        proc.run(['cc','-O2','-std=c11','-D_POSIX_C_SOURCE=200809L','-DDATAROUTER_UNSAFE','-Iinclude','-Isrc',f'tests/{name}.c']+[f'src/{s}.c' for s in sources]+['-lrt','-lpthread','-o',dest],name+'-build')
        try:proc.run([dest],name,180)
        except (RuntimeError,subprocess.TimeoutExpired) as e:failures.append(f'{name}: {e}')
    return failures

def frozen_sources(benchmark):
    result={}
    for repo, prefixes in [(ROOT,('src/','include/','tests/','go/','jni/','python/','benches/')), (benchmark,('languages/c/','languages/cpp/','languages/csharp/','languages/go/','languages/java/','languages/python/','languages/rust/','scenarios/default.json','scenarios/cross-language.json'))]:
        for name in command(['git','-C',repo,'ls-files']).splitlines():
            path=repo/name
            if name.startswith(prefixes) and path.is_file():result[str(path)]=fingerprint(path)
    return result


def task_sources(benchmark):
    paths=[]
    for directory in (ROOT/'php',benchmark/'languages/php'):
        paths += [p for p in directory.rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    paths += [benchmark/'scenarios/php.json',benchmark/'scenarios/php-cross.json',benchmark/'scenarios/php-native.json',benchmark/'build.sh',benchmark/'run.py']
    return {str(p):fingerprint(p) for p in paths}


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--profile',choices=['smoke','full'],default='smoke')
    ap.add_argument('--benchmark',type=Path,default=ROOT.parent/'datarouter-benchmark')
    ap.add_argument('--out',type=Path)
    ap.add_argument('--only',choices=['faults','benchmarks'],help='targeted retest, not a full gate')
    a=ap.parse_args()
    out=(a.out or a.benchmark/'results'/('php-'+time.strftime('%Y%m%d-%H%M%S'))).resolve()
    out.mkdir(parents=True,exist_ok=False)
    proc=Processes(out);result=dict(profile=a.profile,coverage=a.only or 'all',status='failed',cases=[])
    frozen=None;task_snapshot=None
    def interrupted(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    try:
        a.benchmark=a.benchmark.resolve()
        frozen=frozen_sources(a.benchmark)
        result['frozen_sources']=frozen
        task_snapshot=task_sources(a.benchmark)
        result['task_sources']=task_snapshot
        result['benchmark_commit']=command(['git','-C',a.benchmark,'rev-parse','HEAD'])
        meta=environment();print('PHP environment checked',flush=True)
        native_build(proc)
        spec=importlib.util.spec_from_file_location('php_benchmark',a.benchmark/'languages/php/verify.py')
        bench=importlib.util.module_from_spec(spec);spec.loader.exec_module(bench)
        bench.build_c(proc,a.benchmark)
        if a.only!='faults':result['affinity']=bench.configure_affinity(proc)
        if a.profile=='full' and not a.only:result['native_regression_failures']=regressions(proc)
        native_fingerprint=fingerprint(ROOT/'build/libblinkcms_datarouter.so')
        if a.profile=='full' and a.only!='faults':baseline=bench.matrix(proc,a.benchmark,meta,False,'baseline')
        # Run phpize/make in a managed process group too.
        proc.run([sys.executable,ROOT/'php/build.py'],'php-build',180)
        meta=json.loads((ROOT/'build/php/manifest.json').read_text());result['environment']=meta
        php=[meta['tools']['PHP'],'-n','-d','opcache.enable_cli=0','-d','opcache.jit=0','-d','extension='+meta['extension']]
        if not a.only:
            for case,expected in [('basic',26),('auth',2),('routed',2)]:
                shm,sw=proc.switch(case+'-switch',case=='auth');echo=None
                try:
                    if case=='routed':
                        echo=proc.spawn(php+[ROOT/'php/examples/cli.php',shm,'server',2,1],case+'-echo');time.sleep(.1)
                    path=out/(case+'.json')
                    proc.run(php+[ROOT/'php/tests/cases.php',shm,case,path],case,40)
                    if echo: proc.wait(echo,20)
                    result['cases'].append(require_result(path,case=case,checks=expected))
                finally:
                    if echo:proc.stop(echo)
                    proc.stop(sw)
            result['cases'].append(cli_example(proc,php))
            result['cases'].append(fpm_test(proc,meta,php,1,5 if a.profile=='smoke' else 201))
            if a.profile=='full':
                result['cases'].append(fpm_test(proc,meta,php,4,10000))
        if a.only=='faults' or (not a.only and a.profile=='full'):
            result['cases'].append(restart_test(proc,php))
            result['cases'].append(lifecycle_test(proc,php))
        if a.only!='faults':result['benchmarks']=bench.matrix(proc,a.benchmark,meta,True,'php',quick=a.profile=='smoke')
        if a.profile=='full' and a.only!='faults':
            if fingerprint(ROOT/'build/libblinkcms_datarouter.so')!=native_fingerprint:raise RuntimeError('native library changed between baseline and candidate')
            candidate=bench.matrix(proc,a.benchmark,meta,False,'candidate')
            result['performance']=bench.compare(baseline,candidate,out)
            if not result['performance']['passed']:raise RuntimeError('native performance regression exceeds 5%; merge blocked')
        expected=2 if a.only=='faults' else (0 if a.only=='benchmarks' else (5 if a.profile=='smoke' else 8))
        if len(result['cases'])!=expected:raise RuntimeError('missing cases')
        if result.get('native_regression_failures'):raise RuntimeError('native regression failures; merge blocked')
        result['status']='passed'
    except BaseException as e:
        result['error']=str(e);print(f'FAIL: {e}',file=sys.stderr,flush=True)
    finally:
        for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,signal.SIG_IGN)
        try:result['cleanup']=proc.cleanup()
        except BaseException as e:result['status']='failed';result['cleanup_error']=str(e)
        try:result['frozen_unchanged']=frozen is not None and frozen==frozen_sources(a.benchmark)
        except Exception:result['frozen_unchanged']=False
        if not result['frozen_unchanged']:result['status']='failed';result['freeze_error']='frozen source files changed during verification'
        try:result['task_sources_unchanged']=task_snapshot is not None and task_snapshot==task_sources(a.benchmark)
        except Exception:result['task_sources_unchanged']=False
        if not result['task_sources_unchanged']:result['status']='failed';result['source_error']='task sources changed during verification'
        result['full_gate']=result['status']=='passed' and a.profile=='full' and not a.only
        (out/'verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(f"{result['status']} ({result['coverage']}): {out/'verification.json'}",flush=True)
    return 0 if result['status']=='passed' else 1
if __name__=='__main__':sys.exit(main())
