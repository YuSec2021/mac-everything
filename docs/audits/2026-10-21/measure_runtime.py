import ctypes, json, pathlib, struct, subprocess, tempfile, time, urllib.request
libproc = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
class Timebase(ctypes.Structure):
    _fields_ = [('numer', ctypes.c_uint32), ('denom', ctypes.c_uint32)]
timebase = Timebase()
assert ctypes.CDLL('/usr/lib/libSystem.B.dylib').mach_timebase_info(ctypes.byref(timebase)) == 0
libproc.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
def usage(pid):
    buf = ctypes.create_string_buffer(1024)
    if libproc.proc_pid_rusage(pid, 2, buf) != 0:
        raise OSError(ctypes.get_errno())
    values = struct.unpack_from('=18Q', buf.raw, 16)
    return {'user_ticks': values[0], 'system_ticks': values[1], 'idle_wakeups': values[2], 'interrupt_wakeups': values[3], 'rss_bytes': values[6]}
with tempfile.TemporaryDirectory(prefix='maceverything-audit-') as directory:
    base = pathlib.Path(directory)
    root = base / 'fixture'
    root.mkdir()
    for i in range(1000):
        (root / f'example-{i}.txt').write_text('synthetic test data\n')
    with (base / 'stdout.log').open('w') as log:
        process = subprocess.Popen(['./maceverything-daemon', '--port', '19889', '--root', str(root), '--cache-dir', str(base / 'cache'), '--log-dir', str(base / 'logs')], stdout=log, stderr=log)
        try:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            for _ in range(150):
                if process.poll() is not None:
                    raise RuntimeError((base / 'stdout.log').read_text())
                try:
                    with opener.open('http://127.0.0.1:19889/api/status', timeout=1) as response:
                        status = json.load(response)
                    break
                except OSError:
                    time.sleep(.1)
            else:
                raise RuntimeError('startup timeout')
            time.sleep(2)
            start = usage(process.pid)
            started = time.monotonic()
            sockets = []
            for _ in range(4):
                sockets.append(subprocess.run(['lsof', '-nP', '-a', '-p', str(process.pid), '-i'], capture_output=True, text=True).stdout)
                time.sleep(5)
            elapsed = time.monotonic() - started
            end = usage(process.pid)
            cpu_ns = (end['user_ticks'] + end['system_ticks'] - start['user_ticks'] - start['system_ticks']) * timebase.numer / timebase.denom
            result = {'scope': 'source-built headless ServiceEngine, 1000 synthetic files, content indexing disabled; GUI/GPU not measured', 'status': status, 'mach_timebase': {'numer': timebase.numer, 'denom': timebase.denom}, 'elapsed_seconds': elapsed, 'cpu_seconds': cpu_ns / 1e9, 'cpu_percent_one_core': cpu_ns / 1e9 / elapsed * 100, 'start_usage': start, 'end_usage': end, 'socket_snapshots': sockets}
            sample = subprocess.run(['sample', str(process.pid), '2', '10', '-file', '/tmp/maceverything-idle-sample.txt'], capture_output=True, text=True)
            result['sample_exit_code'] = sample.returncode
            process.terminate()
            result['shutdown_exit_code'] = process.wait(timeout=10)
            pathlib.Path('/tmp/maceverything-runtime-audit.json').write_text(json.dumps(result, indent=2))
            print(json.dumps(result, indent=2))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
