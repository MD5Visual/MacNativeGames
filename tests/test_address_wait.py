"""SVC address waits on synthetic memory/pthreads; production threads.c, no game files or window.

Signal type 0 is the documented wake for 64-bit waiters. Type 3 signal tests below characterize this fork only:
libnx currently declares only SignalType 0..2. They are not a claim of Switch hardware conformance.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
R_TIMEOUT=0xea01
R_BAD_STATE=0xfa01
R_BAD_ADDRESS=0xcc01
R_BAD_ENUM=0xf001


@unittest.skipUnless(sys.platform=='darwin','production host thread runtime requires macOS')
class AddressWait(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.directory.cleanup)
        cls.env={k:v for k,v in os.environ.items() if not k.startswith(('TOTK_','DM_'))}
        cls.programs=[]
        for variant,flags in [('base',[]),('ns2',['-DGAME_NS2=1','-DGAME_OOT=1'])]:
            program=Path(cls.directory.name)/variant;cls.build(program,flags);cls.programs.append(program)

    @classmethod
    def build(cls,program,flags=()):
        run=subprocess.run(['xcrun','clang','-std=c11','-O2','-Wall','-Wextra','-Werror',
                            '-Wno-deprecated-declarations','-Wl,-undefined,dynamic_lookup','-Wl,-dead_strip',
                            '-I'+str(ROOT/'games/totk/runtime'),
                            '-I'+str(ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn/include'),
                            *flags,str(ROOT/'tests/address_wait_probe.c'),'-o',str(program)],capture_output=True,text=True,timeout=90)
        if run.returncode:raise AssertionError(run.stdout+run.stderr)

    def probe(self,program,*args):
        run=subprocess.run([str(program),*map(str,args)],env=self.env,capture_output=True,text=True,timeout=6)
        self.assertEqual(run.returncode,0,run.stdout+run.stderr+f' (exit {run.returncode})')
        return json.loads(run.stdout)

    def call(self,program,svc,kind,initial,value,timeout=0,offset=0):
        return self.probe(program,'call',svc,kind,initial,value,timeout,offset)

    def test_64_bit_equality_includes_high_bits(self):
        for program in self.programs:
            for word in (0,1,0x1234567889abcdef,0x8000000000000000,0xffffffffffffffff):
                with self.subTest(variant=program.name,word=word):
                    self.assertEqual(self.call(program,0x34,3,word,word)['result'],R_TIMEOUT)
                    self.assertEqual(self.call(program,0x34,3,word,word^(1<<32))['result'],R_BAD_STATE)
                    self.assertEqual(self.call(program,0x34,3,word,word^1)['result'],R_BAD_STATE)

    def test_finite_timeout_leaves_queue_and_word_unchanged(self):
        for program in self.programs:
            got=self.call(program,0x34,3,0x1234567800000007,0x1234567800000007,2000000)
            self.assertEqual(got['result'],R_TIMEOUT);self.assertEqual(got['value'],0x1234567800000007)
            self.assertGreaterEqual(got['elapsed_ns'],1000000)

    def test_wake_count_priority_address_and_32_bit_interoperation(self):
        for program in self.programs:
            for case in ('wake64','write-without-signal','wrong-address','priority-fifo','mixed-width','negative-count','signal32-wait64','modify32-all'):
                with self.subTest(variant=program.name,case=case):self.assertEqual(self.probe(program,case)['result'],0)

    def test_legacy_32_bit_waits_ignore_high_operand_and_keep_signed_comparisons(self):
        for program in self.programs:
            cases=[(2,0xabcdef0100000007,0x9876543200000007,R_TIMEOUT,0xabcdef0100000007),
                   (0,0xabcdef01ffffffff,0,R_TIMEOUT,0xabcdef01ffffffff),
                   (0,0xabcdef0100000000,0,R_BAD_STATE,0xabcdef0100000000),
                   (1,0xabcdef01ffffffff,0,R_TIMEOUT,0xabcdef01fffffffe)]
            for kind,word,value,result,after in cases:
                with self.subTest(variant=program.name,kind=kind,word=word):
                    got=self.call(program,0x34,kind,word,value);self.assertEqual(got['result'],result);self.assertEqual(got['value'],after)

    def test_32_bit_waits_accept_four_byte_alignment(self):
        for program in self.programs:
            for kind,value in ((0,8),(1,8),(2,7)):
                with self.subTest(variant=program.name,kind=kind):
                    got=self.call(program,0x34,kind,0x1234567800000007,value,0,4)
                    self.assertEqual(got['result'],R_TIMEOUT)
                    self.assertEqual(got['value'],0x1234567800000006 if kind==1 else 0x1234567800000007)

    def test_legacy_32_bit_signal_updates_only_low_word(self):
        for program in self.programs:
            for kind in (1,2):
                got=self.call(program,0x35,kind,0xabcdef0100000007,7,0)
                self.assertEqual(got['result'],0);self.assertEqual(got['value'],0xabcdef0100000008)
                got=self.call(program,0x35,kind,0xabcdef0100000007,8,0)
                self.assertEqual(got['result'],R_BAD_STATE);self.assertEqual(got['value'],0xabcdef0100000007)

    def test_fork_signal_type3_characterization_not_hardware_conformance(self):
        for program in self.programs:
            for word in (0x12345678ffffffff,0xffffffffffffffff):
                got=self.call(program,0x35,3,word,word,0);self.assertEqual(got['result'],0);self.assertEqual(got['value'],(word+1)&((1<<64)-1))
                got=self.call(program,0x35,3,word,word^(1<<32),0);self.assertEqual(got['result'],R_BAD_STATE);self.assertEqual(got['value'],word)

    @unittest.expectedFailure  # 2e7e6036 reads the word without validating the required 8-byte alignment.
    def test_unaligned_64_bit_wait_returns_an_error_without_host_crash(self):
        got=self.call(self.programs[1],0x34,3,7,7,0,4)
        self.assertEqual(got['result'],R_BAD_ADDRESS);self.assertEqual(got['value'],7)

    @unittest.expectedFailure  # 2e7e6036 treats an unrecognised signal type as the 32-bit increment operation.
    def test_unknown_signal_type_rejects_without_modifying_memory(self):
        got=self.call(self.programs[1],0x35,4,0x1234567800000007,7,0)
        self.assertEqual(got['result'],R_BAD_ENUM);self.assertEqual(got['value'],0x1234567800000007)

    def test_regression_checks_detect_truncated_value_and_load(self):
        original=(ROOT/'games/totk/runtime/threads.c').read_text();needle='ld64(addr) == regs[2]';self.assertEqual(original.count(needle),1)
        for name,replacement,word,value,correct in [('w2','ld64(addr) == (uint64_t)(int64_t)(int32_t)regs[2]',0x1234567800000007,0x1234567800000007,R_TIMEOUT),
                                                   ('load32','ld32(addr) == (uint32_t)regs[2]',0x1234567800000007,0x8765432100000007,R_BAD_STATE)]:
            with self.subTest(mutant=name):
                source=Path(self.directory.name)/(name+'.c');source.write_text(original.replace(needle,replacement))
                program=Path(self.directory.name)/name;self.build(program,['-DGAME_NS2=1','-DGAME_OOT=1',f'-DADDRESS_WAIT_RUNTIME="{source}"'])
                self.assertNotEqual(self.call(program,0x34,3,word,value)['result'],correct)
