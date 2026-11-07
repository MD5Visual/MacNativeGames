#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include "../games/totk/runtime/game_patches_ns2.c"
static int checks;
_Noreturn void runtime_fail(const char *s) { fprintf(stderr,"%s\n",s); exit(2); }
#define check(ok) do { if (!(ok)) { fprintf(stderr,"check line %d failed\n",__LINE__); runtime_fail("frame step assertion"); } checks++; } while(0)
extern float execute_step(void *, float, float);
__asm__(".text\n.align 2\n_execute_step:\n"
        "stp x29,x30,[sp,#-32]!\nstr d8,[sp,#16]\nblr x0\n"
        "ldr d8,[sp,#16]\nldp x29,x30,[sp],#32\nret\n");
int main(int argc,char **argv) {
    const size_t size=0x804000, site=0x6e6cc4;
    uint8_t *base=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    check(base!=MAP_FAILED);
    Module m={.name="main",.base=base,.text_end=0x6fff00,.file_size=0x800000,.size=size,.patch_off=0x700000};
    uint32_t original=0x1e201828, tail[]={0x1e204100,0xd65f03c0}; /* fmov s0,s8; ret */
    memcpy(base+site,&original,4);memcpy(base+site+4,tail,sizeof tail);
    if(argc>1&&!strcmp(argv[1],"bad-site"))base[site]^=1;
    if(argc>1&&!strcmp(argv[1],"bad-padding"))base[m.text_end]=1;
    runtime_game_patches(&m);
    check(!mprotect(base,0x704000,PROT_READ|PROT_EXEC));sys_icache_invalidate(base,0x704000);
    check(execute_step(base+site,60,60)==1);
    uint64_t now=1000000000;runtime_frame_step_update(now);
    for(int i=0;i<40;i++){now+=50000000;runtime_frame_step_update(now);}
    int original_mode=getenv("DM_FRAME_DELTA")&&!strcmp(getenv("DM_FRAME_DELTA"),"0");
    check(fabsf(execute_step(base+site,60,60)-(original_mode?1:3))<1e-5);
    check(fabsf(execute_step(base+site,30,30)-(original_mode?1:1.5f))<1e-5);
    for(int i=0;i<40;i++){now+=16666667;runtime_frame_step_update(now);}
    check(execute_step(base+site,60,60)==1);
    check(execute_step(base+site,60,30)==0.5f);
    if(!original_mode){
        for(int i=0;i<40;i++){now+=1000000000;runtime_frame_step_update(now);}
        check(fabsf(execute_step(base+site,60,60)-4)<1e-5);
        now-=1000000000;runtime_frame_step_update(now);
        check(execute_step(base+site,60,60)==1);
    }
    OotFrameStep clock={0};now=1000000000;check(oot_frame_seconds(&clock,now)==0);
    for(int fps=15;fps<=120;fps+=15){clock=(OotFrameStep){0};oot_frame_seconds(&clock,now);
        for(int i=0;i<1000;i++){now+=1000000000/fps;float step=oot_frame_seconds(&clock,now);
            double want=fps>=60?0:fmin(1.0/fps,4.0/60);check(fabs(step-want)<1e-6);}}
    munmap(base,size);printf("%d native frame-step checks passed%s\n",checks,original_mode?" (original-step control)":"");
}
