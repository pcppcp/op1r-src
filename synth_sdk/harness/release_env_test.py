#!/usr/bin/env python3
"""make_firmware.py --release refuses test build switches (OP1R_* at a non-default value), names them, and stops
before building; dev builds and the default values pass; every OP1R_* the script reads is classified.
python3 harness/release_env_test.py"""
import os, re, sys

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for k in [k for k in os.environ if k.startswith('OP1R_')]: del os.environ[k]   # module constants: the normal build
sys.path.insert(0, SDK)
import make_firmware as mf

fails = 0
def want(env, bad, what):
    global fails
    got = mf.release_env_check(env)
    if got != sorted(bad):
        fails += 1; print(f'FAIL {what}: {env} -> {got}, want {sorted(bad)}')

want({}, [], 'clean')
want({'PATH': '/bin', 'OP1R_GUISKIP': '1', 'OP1R_WDOG': '1', 'OP1R_L1_RUNTIME': '1'}, [], 'defaults spelled out')
want({'OP1R_CRASHTEST': '', 'OP1R_MIX': '', 'OP1R_UNRELATED': 'x'}, [], 'empty / not ours')
for k, v in [('OP1R_CRASHTEST', '1'), ('OP1R_FORCE_HP', '1'), ('OP1R_WDOG_NORESET', '1'), ('OP1R_TMXTEST', '1'),
             ('OP1R_WDOG', '0'), ('OP1R_WDOG', ''), ('OP1R_GUISKIP', '0'), ('OP1R_GUISKIP', 'hook_gsframe'),
             ('OP1R_GUISKIP', ''), ('OP1R_OUTSTAGE', 'check'), ('OP1R_MASTER', 'stock'), ('OP1R_EQ', 'stress'),
             ('OP1R_TAPEMIX', 'exact'), ('OP1R_L1_RUNTIME', '0'), ('OP1R_MIX', 'anything'), ('OP1R_L1READ', '1')]:
    want({k: v}, [k], f'{k}={v!r}')
want({'OP1R_CRASHTEST': '1', 'OP1R_WDOG': '0', 'OP1R_GUISKIP': '1'}, ['OP1R_CRASHTEST', 'OP1R_WDOG'], 'two at once')

src = open(os.path.join(SDK, 'make_firmware.py')).read()           # a new switch must be classified
read = set(re.findall(r'OP1R_[A-Z0-9_]+', src)) - set(re.findall(r"-D(OP1R_[A-Z0-9_]+)", src))   # -D: compiler flags, not variables
if read - set(mf.BUILD_ENV):
    fails += 1; print(f'FAIL: OP1R_* read but not in BUILD_ENV: {sorted(read - set(mf.BUILD_ENV))}')
if set(mf.BUILD_ENV) - read:
    fails += 1; print(f'FAIL: BUILD_ENV lists unused {sorted(set(mf.BUILD_ENV) - read)}')

def no_build(*a): raise AssertionError('build started')
mf.plugins.load = no_build                                         # main stops before any build step
for release, env, refused in ((True, {'OP1R_CRASHTEST': '1', 'OP1R_TMXTEST': '1'}, True), (False, {'OP1R_CRASHTEST': '1'}, False)):
    os.environ.update(env)
    try:
        mf.main(release=release); r = 'returned'
    except SystemExit as x:
        r = str(x)
    except AssertionError as x:
        r = str(x)
    for k in env: del os.environ[k]
    ok = ('OP1R_CRASHTEST=' in r and 'OP1R_TMXTEST=' in r) if refused else r == 'build started'
    if not ok:
        fails += 1; print(f'FAIL main(release={release}) {env}: {r}')
print(f'{fails} failed' if fails else 'release_env: ok')
sys.exit(1 if fails else 0)
