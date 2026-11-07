/* Production offline services, synthetic counters and host threads only. No socket connection or game data. */
#ifndef OFFLINE_RUNTIME
#define OFFLINE_RUNTIME "../games/totk/runtime/offline_host.c"
#endif
#include OFFLINE_RUNTIME
#include <assert.h>
#include <stdatomic.h>
#include <errno.h>

static _Thread_local int guest_errno, core_held;
static _Atomic int away_calls, back_calls;
static pthread_mutex_t test_core = PTHREAD_MUTEX_INITIALIZER;
static int *guest_errno_location(void) { return &guest_errno; }
void *runtime_symbol(const char *name) { return !strcmp(name,"__errno_location") ? (void *)guest_errno_location : NULL; }
_Noreturn void runtime_fail(const char *why) { fprintf(stderr,"%s\n",why);exit(2); }
void hle_bind_system_event(void *event,int autoclear) { (void)event;(void)autoclear; }
void hle_signal_system_event(void *event) { (void)event; }
uint64_t runtime_now_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
int runtime_core_away(void) {
    atomic_fetch_add(&away_calls,1);
    int held=core_held;
    if(held){assert(!pthread_mutex_trylock(&evfd_lock));assert(!pthread_mutex_unlock(&evfd_lock));core_held=0;assert(!pthread_mutex_unlock(&test_core));}
    return held;
}
void runtime_core_back(int *away) { atomic_fetch_add(&back_calls,1);if(*away){assert(!pthread_mutex_lock(&test_core));core_held=1;*away=0;} }
static void sleep_ms(unsigned ms) { struct timespec t={ms/1000,(ms%1000)*1000000};nanosleep(&t,NULL); }
static void check_error(int result,int sk,int libc) { assert(result==-1);assert(socket_last_error()==sk);assert(guest_errno==libc); }
static void bit(unsigned set[32],int fd) { set[fd>>5]|=1u<<(fd&31); }
static int has(const unsigned set[32],int fd) { return !!(set[fd>>5]&(1u<<(fd&31))); }
typedef struct { int fd,read_it,require_core,close_it,reopen;uint64_t value; } action;
static void *later(void *arg) {
    action *a=arg;sleep_ms(20);
    if(a->require_core){assert(!pthread_mutex_lock(&test_core));core_held=1;}
    if(a->close_it){assert(!ev_close(a->fd));if(a->reopen){assert(ev_event_fd(9,4)==a->fd);}}
    else if(a->read_it){uint64_t out=0;assert(!ev_read(a->fd,&out));assert(out==EV_MAX);}
    else assert(!ev_write(a->fd,a->value));
    if(a->require_core){core_held=0;assert(!pthread_mutex_unlock(&test_core));}
    return NULL;
}
static void start(pthread_t *thread,action *a) { assert(!pthread_create(thread,NULL,later,a)); }
static void *errno_thread(void *arg) { (void)arg;check_error(ev_read(-1,NULL),9,9);return NULL; }
int main(int argc,char **argv) {
    assert(argc==2);const char *test=argv[1];uint64_t out=0x123456789abcdef0ull;
    if(!strcmp(test,"counter")){
        int fd=ev_event_fd(7,6);assert(fd>=512&&fd<1024);assert(!ev_read(fd,&out)&&out==7);
        check_error(ev_read(fd,&out),35,11);assert(out==7);
        assert(!ev_write(fd,2)&&!ev_write(fd,3));assert(!ev_read(fd,&out)&&out==5);assert(!ev_close(fd));
    }else if(!strcmp(test,"semaphore")){
        int fd=ev_event_fd(2,5);assert(!ev_read(fd,&out)&&out==1);assert(!ev_read(fd,&out)&&out==1);check_error(ev_read(fd,&out),35,11);
    }else if(!strcmp(test,"bounds")){
        int fd=ev_event_fd(EV_MAX,4);assert(!ev_write(fd,0));check_error(ev_write(fd,UINT64_MAX),22,22);check_error(ev_write(fd,1),35,11);
        assert(!ev_read(fd,&out)&&out==EV_MAX);check_error(ev_event_fd(UINT64_MAX,0),22,22);
    }else if(!strcmp(test,"flags")){
        int fd=ev_event_fd(0,0);assert(ev_fcntl(fd,3,0)==0);assert(!ev_fcntl(fd,4,4));assert(ev_fcntl(fd,3,0)==4);check_error(ev_read(fd,&out),35,11);
        assert(!ev_fcntl(fd,4,0));assert(!ev_fcntl(fd,1,0));assert(!ev_fcntl(fd,2,1));assert(ev_fcntl(fd,1,0)==1);check_error(ev_fcntl(fd,123,0),22,22);
    }else if(!strcmp(test,"poll")){
        int fd=ev_event_fd(1,4);struct {int fd;short events,revents;} p[3]={{fd,0x41,0},{-1,1,-1},{7,1,-1}};
        assert(ev_poll(p,3,0)==1&&p[0].revents==0x41&&!p[1].revents&&!p[2].revents);
        assert(!ev_read(fd,&out));p[0].events=1|4;assert(ev_poll(p,1,0)==1&&p[0].revents==4);
    }else if(!strcmp(test,"select")){
        int fd=ev_event_fd(1,4);unsigned r[32]={0},w[32]={0},e[32]={0};long tv[2]={0,0};bit(r,fd);bit(w,fd);bit(e,fd);
        assert(ev_select(1024,r,w,e,tv)==2&&has(r,fd)&&has(w,fd)&&!has(e,fd));
        bit(e,fd);assert(!ev_select(1024,NULL,NULL,e,tv)&&!has(e,fd));
    }else if(!strcmp(test,"select-late")){
        int fd=ev_event_fd(0,4);unsigned r[32]={0};bit(r,fd);long tv[2]={1,0};action a={.fd=fd,.value=3};pthread_t t;start(&t,&a);
        uint64_t begin=runtime_now_ns();assert(ev_select(1024,r,NULL,NULL,tv)==1&&has(r,fd));assert(runtime_now_ns()-begin<500000000);assert(!pthread_join(t,NULL));
    }else if(!strcmp(test,"timeout")){
        int fd=ev_event_fd(0,4);struct {int fd;short events,revents;} p={fd,1,-1};uint64_t begin=runtime_now_ns();
        assert(!ev_poll(&p,1,20)&&!p.revents);uint64_t elapsed=runtime_now_ns()-begin;assert(elapsed>=15000000&&elapsed<500000000);
        unsigned r[32]={0};bit(r,fd);long tv[2]={0,20000};begin=runtime_now_ns();assert(!ev_select(1024,r,NULL,NULL,tv)&&!has(r,fd));assert(runtime_now_ns()-begin>=15000000);
        assert(away_calls>0&&away_calls==back_calls);
    }else if(!strncmp(test,"core-",5)){
        int write_it=!strcmp(test,"core-write"),fd=ev_event_fd(write_it?EV_MAX:0,0);
        action a={.fd=fd,.value=3,.read_it=write_it,.require_core=1};pthread_t t;
        assert(!pthread_mutex_lock(&test_core));core_held=1;start(&t,&a);
        if(!strcmp(test,"core-read")){assert(!ev_read(fd,&out)&&out==3);}
        else if(write_it){assert(!ev_write(fd,1));assert(!ev_read(fd,&out)&&out==1);}
        else if(!strcmp(test,"core-poll")){struct {int fd;short events,revents;} p={fd,1,0};assert(ev_poll(&p,1,1000)==1&&p.revents==1);}
        else if(!strcmp(test,"core-select")){unsigned r[32]={0};bit(r,fd);long tv[2]={1,0};assert(ev_select(1024,r,NULL,NULL,tv)==1&&has(r,fd));}
        else return 3;
        core_held=0;assert(!pthread_mutex_unlock(&test_core));assert(!pthread_join(t,NULL));assert(away_calls>0&&away_calls==back_calls);
    }else if(!strcmp(test,"closed-wait")){
        int fd=ev_event_fd(0,0);action a={.fd=fd,.close_it=1,.reopen=1};pthread_t t;start(&t,&a);
        check_error(ev_read(fd,&out),9,9);assert(!pthread_join(t,NULL));assert(!ev_read(fd,&out)&&out==9);
    }else if(!strcmp(test,"errors")){
        int fd=ev_event_fd(0,4);check_error(ev_read(fd,&out),35,11);pthread_t t;assert(!pthread_create(&t,NULL,errno_thread,NULL));assert(!pthread_join(t,NULL));assert(socket_last_error()==35&&guest_errno==11);
        check_error(ev_select(-1,NULL,NULL,NULL,NULL),22,22);long tv[2]={0,1000000};check_error(ev_select(1024,NULL,NULL,NULL,tv),22,22);
        assert(socket_down()==-1&&socket_last_error()==50);
    }else if(!strcmp(test,"exports")){
        assert(runtime_offline_override("_ZN2nn6socket4PollEP6pollfdmi")==ev_poll);
        assert(runtime_offline_override("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz")==ev_fcntl);
        assert(runtime_offline_override("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE")==ev_select);
    }else return 3;
    printf("{\"status\":\"pass\",\"case\":\"%s\",\"away\":%d,\"back\":%d}\n",test,away_calls,back_calls);return 0;
}
