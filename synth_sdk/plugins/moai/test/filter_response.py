"""Small-signal response of the ladder (white noise through the real lad_step): DC gain, resonance peak position and
height, -3 dB point, vs the cutoff the table was solved for."""
import subprocess, os, numpy as np
from scipy.signal import welch
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'mm_filter')
AMP = 400                                                  # input +-AMP of F (y is scaled x4 on output)
def run(mode, hz, emp, n, amp):
    r = subprocess.run([EXE, mode, str(hz), str(emp), str(n), str(amp)], capture_output=True)
    if r.returncode: raise SystemExit(r.stderr.decode())
    return np.frombuffer(r.stdout, dtype=np.int16).astype(float)
def response(hz, emp):
    y = run('noise', hz, emp, 1 << 20, AMP)
    f, p = welch(y, 44100, nperseg=8192)
    px = (4 * AMP) ** 2 / 3 / 22050                        # input density (uniform noise, same x4 scale)
    return f, 10 * np.log10(p / px + 1e-30)
if __name__ == '__main__':
    print(' target Hz | emph | DC dB | peak Hz (err) | peak dB re DC | -3 dB Hz')
    for hz in (100, 300, 1000, 3000, 8000):
        for emp in (0.0, 0.5, 0.8, 0.95):
            f, g = response(hz, emp)
            dc = g[1:6].mean()
            ip = np.argmax(g[1:]) + 1
            idx = np.where(g[1:] < dc - 3)[0]
            f3 = f[1:][idx[0]] if len(idx) else float('nan')
            pk = f'{f[ip]:6.0f} ({100 * (f[ip] / hz - 1):+4.0f}%)' if emp > 0.5 else '       -     '
            print(f'{hz:9d} | {emp:4.2f} | {dc:5.1f} | {pk} | {g[ip] - dc:6.1f} | {f3:7.0f}')
