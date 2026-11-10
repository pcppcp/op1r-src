import sys, subprocess, numpy as np
import os
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'mm_dump')
def render(args, exe=EXE):
    r = subprocess.run([exe, *map(str, args)], capture_output=True)
    if r.returncode: sys.stderr.write(r.stderr.decode()); raise SystemExit('FAIL')
    return np.frombuffer(r.stdout, dtype=np.int16).astype(np.float64)
if __name__ == '__main__':
    a = [float(x) if '.' in x else int(x) for x in sys.argv[1:]]
    x = render(a)
    print(f'peak {np.abs(x).max():.0f} ({20*np.log10(np.abs(x).max()/32768+1e-12):.1f} dBFS) rms {np.sqrt((x**2).mean()):.0f} ({20*np.log10(np.sqrt((x**2).mean())/32768+1e-12):.1f} dBFS) dc {x.mean():.0f}')
