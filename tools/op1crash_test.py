#!/usr/bin/env python3
"""python3 tools/op1crash_test.py: symbol map (synth_sdk/symmap.py, make_plugin.funcs) and crash screen decoder
(op1crash.py) on a made-up build; host only (make_plugin.funcs needs pyelftools and cc, else skipped)."""
import json, os, shutil, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = [os.path.join(ROOT, 'tools'), os.path.join(ROOT, 'synth_sdk')]
import op1crash, symmap

NM = """01e00000 t _l1v_metro
01e00020 00000040 T _dispatcher_render
01e00060 00000010 t _helper
01e00100 D _plug_imgs
01e00400 A __image_end
01e00800 B _plug_img_n
019b975c t _hook_namecmp
ffa030f0 000001a0 T _eq_stage
"""
# what crash.c prints ("%s sp %#x", "%#x", "%s+%#x"): a stock call site, core, asm label, plugin, L1, an unknown plugin
SCREEN = """SEQSTAT : RETX
0x00000024 : 0x01a85c8e
op1r 64 sp 0x1fe3c40
0x1a85c8e 0x1e00030
0x1e00004 clouds+0x1c4
0xffa03100 bogus+0x10
0x1e00078 0x1aff000
"""


class Map(unittest.TestCase):
    def test_parse_nm(self):
        syms, end = symmap.parse_nm(NM)
        self.assertEqual(end, 0x01E00400)
        self.assertEqual(syms[0], (0x01E00000, 0, 'l1v_metro'))                 # hook.S labels at R246 sites: stock()
        self.assertIn((0x01E00020, 0x40, 'dispatcher_render'), syms)
        self.assertNotIn('plug_imgs', [s[2] for s in syms])         # data, not code

    def test_stock_names(self):
        st = dict(symmap.stock())
        self.assertIn('MALLOC', st[0x01A9F348])                    # function pointer macro
        self.assertIn('db2::exec', st[0x0196F950])                 # Class::method (0x...) in a comment
        self.assertIn('[hook_crash]', st[0x0191995C])              # make_firmware.py HOOKS + comment
        self.assertIn('vc_down_body', st[0x01A85754])              # link.ld
        self.assertTrue(all(0x01900000 <= a < 0x01B00000 or 0xFFA00000 <= a < 0xFFA14000 for a in st))

    def test_make_json(self):
        m = json.loads(json.dumps(symmap.make('64', NM, {'clouds': [[0x100, 0x100, 'clouds_render']]})))
        self.assertEqual((m['op1r'], m['image_end'], m['plugins']['clouds']), ('64', 0x01E00400, [[0x100, 0x100, 'clouds_render']]))
        self.assertEqual(m['core'], sorted(m['core']))


class Decode(unittest.TestCase):
    def setUp(self):
        self.m = symmap.make('64', NM, {'clouds': [[0x100, 0x100, 'clouds_render']]})

    def test_parse(self):
        p = op1crash.parse(SCREEN)
        self.assertEqual((p['version'], p['sp']), ('64', 0x1FE3C40))
        self.assertEqual([t for t, _, _ in p['addrs']], ['0x01a85c8e', '0x1a85c8e', '0x1e00030', '0x1e00004', 'clouds+0x1c4',
                                                      '0xffa03100', 'bogus+0x10', '0x1e00078', '0x1aff000'])

    def test_decode(self):
        d = lambda f, a: op1crash.decode(self.m, f, a)
        self.assertEqual(d(None, 0x01E00030), ('op1r', 'dispatcher_render+0x10'))
        self.assertEqual(d(None, 0x01E00004), ('op1r', 'l1v_metro+0x4'))          # asm label: up to the next symbol
        self.assertEqual(d(None, 0x01E00078), ('op1r', 'op1r core'))              # past helper's size
        self.assertEqual(d(None, 0xFFA03100), ('op1r', 'eq_stage+0x10'))           # our code in L1
        self.assertEqual(d('clouds', 0x1C4), ('plugin', 'clouds: clouds_render+0xc4'))
        self.assertEqual(d('bogus', 0x10), ('plugin', 'bogus: +0x10 (no symbols for this plugin)'))
        w, t = d(None, 0x01A85C8E)
        self.assertEqual(w, 'R246'); self.assertTrue(t.startswith('near VoiceCtl::on_midi_rx') and t.endswith('+0x4'), t)
        self.assertEqual(d(None, 0x01AFF000), ('R246', 'unnamed'))
        self.assertEqual(op1crash.decode(None, None, 0x01E00030), ('op1r', 'op1r core (no symbol map)'))

    def test_report(self):
        r = op1crash.report(SCREEN, dict(self.m, op1r='63')).splitlines()
        self.assertEqual(r[0], 'op1r 64, sp 0x1fe3c40 (symbol map is op1r 63)')
        self.assertEqual(len(r), 10)


class PluginFuncs(unittest.TestCase):
    def test_funcs(self):
        try: import make_plugin
        except ImportError: self.skipTest('pyelftools not installed')
        if not shutil.which('cc'): self.skipTest('no cc')
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, 'p.c')
            open(src, 'w').write('int tab[4];\nint f1(int x) { return x + tab[x]; }\nint f2(int x) { return f1(x) * 3; }\n')
            subprocess.run(['cc', '-O1', '-c', src, '-o', os.path.join(d, 'p.o')], check=True)
            fs = make_plugin.funcs(make_plugin.elf(os.path.join(d, 'p.o')), 0)
        self.assertEqual([f[2] for f in fs], ['f1', 'f2'])                        # code only, by offset
        self.assertEqual(fs[0][0], 0); self.assertTrue(fs[0][1] > 0 and fs[1][0] >= fs[0][1])


if __name__ == '__main__':
    unittest.main()
