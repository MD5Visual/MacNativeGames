"""Actual offline service implementation; generated counters and pthreads, no game/network/window."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'requires the macOS host runtime headers')
class OfflineEventFd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.programs = []
        cls.env = {k:v for k,v in os.environ.items() if not k.startswith(('TOTK_', 'DM_'))}
        for variant, flags in [('dkb', ['-DGAME_DKB=1']), ('oot', ['-DGAME_NS2=1', '-DGAME_OOT=1'])]:
            program = Path(cls.temp.name) / variant
            run = subprocess.run(['xcrun','clang','-std=c11','-O2','-Wall','-Wextra','-Werror',
                                  '-Wno-deprecated-declarations','-I'+str(ROOT/'games/totk/runtime'),
                                  *flags,str(ROOT/'tests/offline_eventfd_probe.c'),'-o',str(program)],
                                 capture_output=True,text=True,timeout=90)
            if run.returncode:
                raise AssertionError(run.stdout+run.stderr)
            cls.programs.append(program)

    def case(self, name):
        for program in self.programs:
            with self.subTest(variant=program.name):
                run = subprocess.run([str(program),name],env=self.env,capture_output=True,text=True,timeout=4)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                self.assertEqual(json.loads(run.stdout)['status'],'pass')

    def test_counter_result_value_sum_and_reset(self): self.case('counter')
    def test_semaphore(self): self.case('semaphore')
    def test_zero_maximum_and_overflow_writes(self): self.case('bounds')
    def test_bsd_fcntl_flags(self): self.case('flags')
    def test_poll_readiness_and_offline_descriptors(self): self.case('poll')
    def test_select_returns_each_ready_bit_and_no_false_exceptions(self): self.case('select')
    def test_select_retains_request_during_wait(self): self.case('select-late')
    def test_monotonic_timeouts_and_balanced_handoff(self): self.case('timeout')
    def test_blocking_read_releases_core_for_producer(self): self.case('core-read')
    def test_full_write_releases_core_for_consumer(self): self.case('core-write')
    def test_poll_releases_core_for_producer(self): self.case('core-poll')
    def test_select_releases_core_for_producer(self): self.case('core-select')
    def test_closed_wait_does_not_consume_reused_descriptor(self): self.case('closed-wait')
    def test_thread_local_socket_and_guest_errno(self): self.case('errors')
    def test_exports_reach_local_event_service(self): self.case('exports')
