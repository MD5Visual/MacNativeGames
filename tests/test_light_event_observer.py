"""Actual production wrappers forward calls/return values with diagnostics off or on."""
import os,platform,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(platform.system()=='Darwin','Apple thread-id API')
class LightEventObserver(unittest.TestCase):
 def test_logging_preserves_calls_timeout_and_result(self):
  text=(ROOT/'games/totk/runtime/services.c').read_text();a=text.index('static void lv_log(');b=text.index('/* TEMP probe (OoT): the movie player',a)
  prefix='''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
static int count;static void *last;static uint64_t timeout;
static void call(void *p){count++;last=p;}
static uint32_t timed(void *p,uint64_t t){call(p);timeout=t;return 0xab123456;}
static void *runtime_symbol(const char *name){return strstr(name,"TimedWait")?(void*)timed:(void*)call;}
'''
  main='''int main(void){uint64_t ev=0;observed_light_wait(&ev);uint32_t r=observed_light_timedwait(&ev,0x123456789abcdef0ull);observed_light_signal(&ev);observed_light_clear(&ev);return !(count==4&&last==&ev&&r==0xab123456&&timeout==0x123456789abcdef0ull);}
'''
  with tempfile.TemporaryDirectory() as td:
   probe=next(line for line in text.splitlines() if line.startswith('static int oot_probe('))
   p=Path(td);(p/'test.c').write_text(prefix+probe+'\n'+text[a:b]+main)
   r=subprocess.run(['xcrun','clang','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'probe')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
   env={k:v for k,v in os.environ.items() if k!='TOTK_WAIT_LOG'}
   for on in (False,True):
    with self.subTest(logging=on):
     r=subprocess.run([str(p/'probe')],env=env|({'TOTK_WAIT_LOG':'1'} if on else {}),capture_output=True,text=True)
     self.assertEqual(r.returncode,0,r.stderr);self.assertEqual('[lv-probe]' in r.stderr,on)
