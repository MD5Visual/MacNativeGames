/* Synthetic host threads and words, through the actual SVC dispatcher and wait queue. No game, GPU or profile.
 * Disable only virtual-core scheduling; address comparison, enqueueing, timeouts, selection and wakeup are production. */
#ifndef ADDRESS_WAIT_RUNTIME
#define ADDRESS_WAIT_RUNTIME "../games/totk/runtime/threads.c"
#endif
#include ADDRESS_WAIT_RUNTIME
#include <assert.h>
#include <unistd.h>

pthread_key_t runtime_keys[8];
uint64_t runtime_hle_tsd_off;
_Noreturn void runtime_fail(const char *why) { fprintf(stderr,"%s\n",why);exit(2); }
uint64_t runtime_now_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void probe_fault(int sig) { const char message[]="fault accessing synthetic test memory\n";write(2,message,sizeof message-1);_exit(128+sig); }

static void thread_record(thr_t *t,int prio) {
    memset(t,0,sizeof *t);t->cur_core=-1;t->prio=prio;t->mask=1;t->pt=pthread_self();t->handle=0x1002;
    assert(!pthread_cond_init(&t->cv,NULL));assert(!pthread_setspecific(g_keythr,t));
}
static uint64_t invoke(int svc,void *address,unsigned type,uint64_t value,int64_t limit) {
    uint64_t r[32],original[32];for(unsigned i=0;i<32;i++)r[i]=0x6100000000000000ULL+i;
    r[0]=(uintptr_t)address;r[1]=type;r[2]=value;r[3]=(uint64_t)limit;memcpy(original,r,sizeof r);
    assert(svc_thread(svc,r));for(unsigned i=1;i<32;i++)assert(r[i]==original[i]);return r[0];
}
typedef struct { thr_t record;pthread_t host;void *address;unsigned type;uint64_t value,result; } waiter;
static void *wait_main(void *p) {
    waiter *w=p;assert(!pthread_setspecific(g_keythr,&w->record));w->record.pt=pthread_self();
    w->result=invoke(0x34,w->address,w->type,w->value,2000000000);return NULL;
}
static void start_waiter(waiter *w,void *address,unsigned type,uint64_t value,int prio) {
    memset(w,0,sizeof *w);w->record.cur_core=-1;w->record.prio=prio;w->record.mask=1;
    assert(!pthread_cond_init(&w->record.cv,NULL));w->address=address;w->type=type;w->value=value;
    assert(!pthread_create(&w->host,NULL,wait_main,w));
    uint64_t end=runtime_now_ns()+1000000000;
    for(;;){pthread_mutex_lock(&G);int queued=w->record.wkind==3;pthread_mutex_unlock(&G);if(queued)break;
        assert(runtime_now_ns()<end);struct timespec pause={0,100000};nanosleep(&pause,NULL);}
}
static void finish_waiter(waiter *w) { assert(!pthread_join(w->host,NULL));assert(!w->result);assert(!pthread_cond_destroy(&w->record.cv)); }
static int was_woken(waiter *w) { pthread_mutex_lock(&G);int woke=w->record.woke;pthread_mutex_unlock(&G);return woke; }
int main(int argc,char **argv) {
    signal(SIGBUS,probe_fault);signal(SIGSEGV,probe_fault);
    assert(argc>=2);g_cores_on=0;assert(!pthread_key_create(&g_keythr,NULL));thr_t main_thread;thread_record(&main_thread,44);
    if(!strcmp(argv[1],"call")){
        assert(argc==8);int svc=(int)strtol(argv[2],NULL,0);unsigned type=(unsigned)strtoul(argv[3],NULL,0);
        uint64_t initial=strtoull(argv[4],NULL,0),value=strtoull(argv[5],NULL,0);int64_t limit=strtoll(argv[6],NULL,0);unsigned offset=(unsigned)strtoul(argv[7],NULL,0);assert(offset<=8);
        _Alignas(16) unsigned char memory[32];memset(memory,0xa5,sizeof memory);memcpy(memory+offset,&initial,8);
        uint64_t start=runtime_now_ns(),result=invoke(svc,memory+offset,type,value,limit),elapsed=runtime_now_ns()-start,after;
        memcpy(&after,memory+offset,8);for(unsigned i=0;i<sizeof memory;i++)if(i<offset||i>=offset+8)assert(memory[i]==0xa5);
        assert(!g_wait);printf("{\"result\":%llu,\"value\":%llu,\"elapsed_ns\":%llu}\n",(unsigned long long)result,(unsigned long long)after,(unsigned long long)elapsed);
    }else{
        _Alignas(8) uint64_t word=0x1234567800000007ULL,other=word;waiter w[3];
        if(!strcmp(argv[1],"wake64")){
            start_waiter(&w[0],&word,3,word,44);assert(!invoke(0x35,&word,0,0,1));finish_waiter(&w[0]);
        }else if(!strcmp(argv[1],"write-without-signal")){
            start_waiter(&w[0],&word,3,word,44);__atomic_store_n(&word,word+1,__ATOMIC_SEQ_CST);assert(!was_woken(&w[0]));
            assert(!invoke(0x35,&word,0,0,1));finish_waiter(&w[0]);
        }else if(!strcmp(argv[1],"wrong-address")){
            start_waiter(&w[0],&word,3,word,44);assert(!invoke(0x35,&other,0,0,1));assert(!was_woken(&w[0]));
            assert(!invoke(0x35,&word,0,0,1));finish_waiter(&w[0]);
        }else if(!strcmp(argv[1],"priority-fifo")){
            start_waiter(&w[0],&word,3,word,44);start_waiter(&w[1],&word,3,word,32);start_waiter(&w[2],&word,3,word,32);
            assert(!invoke(0x35,&word,0,0,1));assert(!was_woken(&w[0])&&was_woken(&w[1])&&!was_woken(&w[2]));
            assert(!invoke(0x35,&word,0,0,1));assert(!was_woken(&w[0])&&was_woken(&w[2]));
            assert(!invoke(0x35,&word,0,0,1));for(int i=0;i<3;i++)finish_waiter(&w[i]);
        }else if(!strcmp(argv[1],"mixed-width")){
            start_waiter(&w[0],&word,2,word,44);start_waiter(&w[1],&word,3,word,44);
            assert(!invoke(0x35,&word,0,0,0));finish_waiter(&w[0]);finish_waiter(&w[1]);
        }else if(!strcmp(argv[1],"negative-count")){
            start_waiter(&w[0],&word,3,word,44);start_waiter(&w[1],&word,3,word,44);
            assert(!invoke(0x35,&word,0,0,-1));finish_waiter(&w[0]);finish_waiter(&w[1]);
        }else if(!strcmp(argv[1],"signal32-wait64")){
            start_waiter(&w[0],&word,3,word,44);assert(!invoke(0x35,&word,1,7,1));finish_waiter(&w[0]);assert(word==0x1234567800000008ULL);
        }else if(!strcmp(argv[1],"modify32-all")){
            start_waiter(&w[0],&word,3,word,44);start_waiter(&w[1],&word,3,word,44);
            assert(!invoke(0x35,&word,2,7,0));finish_waiter(&w[0]);finish_waiter(&w[1]);assert(word==0x1234567800000005ULL);
        }else return 3;
        assert(!g_wait);printf("{\"result\":0,\"value\":%llu}\n",(unsigned long long)word);
    }
    assert(!pthread_cond_destroy(&main_thread.cv));return 0;
}
