"""Native Horizon/Darwin stack argument contract, through the production host-stack bridge."""
import json
import platform
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin' and platform.machine() == 'arm64',
                     'requires Apple silicon and Darwin thread-specific slots')
class HostStackABI(unittest.TestCase):
    def test_horizon_slots_switch_trace_and_darwin_adapter(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'hle-stack-abi'
            command = ['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                       str(ROOT / 'games/totk/runtime/hle_stack.c'),
                       str(ROOT / 'games/totk/runtime/hle_stack.S'),
                       str(ROOT / 'tests/hle_stack_abi_probe.c'),
                       str(ROOT / 'tests/hle_stack_abi_call.S'), '-o', str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            result = json.loads(ran.stdout)
            self.assertEqual(result['status'], 'pass')
            self.assertEqual(result['configurations'], 6)
            self.assertTrue(result['narrow_stack_argument_mismatch_demonstrated'])
            self.assertTrue(result['wide_slot_adapter_correct'])
            self.assertTrue(result['host_stack_switch_and_trace_checked'])
