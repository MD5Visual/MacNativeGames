"""Native ARM64 execution of the production OoT timing veneer; no game files."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(platform.system()=='Darwin' and platform.machine()=='arm64','native Apple silicon required')
class OotFrameStep(unittest.TestCase):
    def test_measured_time_original_control_and_instruction_guards(self):
        with tempfile.TemporaryDirectory() as td:
            exe=Path(td)/'probe'
            built=subprocess.run(['xcrun','clang','-O2','-DGAME_OOT=1','-DGAME_NS2=1',str(ROOT/'tests/oot_frame_step_probe.c'),'-o',str(exe)],capture_output=True,text=True,timeout=60)
            self.assertEqual(built.returncode,0,built.stderr)
            env={k:v for k,v in os.environ.items() if not k.startswith(('TOTK_','DM_'))}
            for extra in ({},{'DM_FRAME_DELTA':'0'}):
                ran=subprocess.run([str(exe)],env=env|extra,capture_output=True,text=True,timeout=20)
                self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
                self.assertIn('native frame-step checks passed',ran.stdout)
            for mode,reason in [('bad-site','original instruction mismatch'),('bad-padding','padding not empty')]:
                ran=subprocess.run([str(exe),mode],env=env,capture_output=True,text=True,timeout=20)
                self.assertEqual(ran.returncode,2,ran.stderr)
                self.assertIn(reason,ran.stderr)
