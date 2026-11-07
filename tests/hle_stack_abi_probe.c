/* CPU-only host ABI experiment: Horizon's two stack slots vs Darwin's packed uint32 arguments.
   Uses production hle_stack.c/.S; no game code, decoder, profile or window. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include "../games/totk/runtime/runtime.h"
extern uint64_t probe_horizon_call(void *);
_Noreturn void runtime_fail(const char *why) { fprintf(stderr,"%s\n",why);exit(2); }
uint64_t runtime_now_ns(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
void runtime_thread_note_hle(unsigned index) { (void)index; }
uint32_t runtime_thread_current_handle(void) { return 0; }
#define FIRST8 uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h
static uintptr_t observed_sp;
static __attribute__((noinline)) uint64_t narrow(FIRST8,uint32_t nine,uint32_t ten) {
    assert(a+b+c+d+e+f+g+h==36);__asm__ volatile("mov %0, sp":"=r"(observed_sp));return ((uint64_t)nine<<32)|ten;
}
static __attribute__((noinline)) uint64_t wide(FIRST8,uint64_t nine,uint64_t ten) {
    assert(a+b+c+d+e+f+g+h==36);__asm__ volatile("mov %0, sp":"=r"(observed_sp));return ((uint64_t)(uint32_t)nine<<32)|(uint32_t)ten;
}
int main(void) {
    pthread_key_t key;assert(!pthread_key_create(&key,NULL)&&key);
    runtime_hle_stack_init((uint64_t)key*8);
    size_t length=1u<<20;uint8_t *stack=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);assert(stack!=MAP_FAILED);
    uintptr_t top=(uintptr_t)stack+length-64;
    void *n=runtime_hle_wrap(narrow),*w=runtime_hle_wrap(wide);
    assert(runtime_hle_wrap(wide)==w&&runtime_hle_wrap(NULL)==NULL);
    unsigned cases=0;
    for(int trace=0;trace<3;trace++) {
        runtime_hle_trace_set(trace);
        for(int switched=0;switched<2;switched++) {
            runtime_hle_stack_set(switched?top:0);
            uint64_t raw=probe_horizon_call(n);
            assert(raw==((uint64_t)9<<32)); /* Darwin reads the padding at +4 as the tenth argument. */
            assert(((observed_sp>=(uintptr_t)stack)&&(observed_sp<top))==switched);
            assert((uintptr_t)pthread_getspecific(key)==(switched?top:0));
            uint64_t adapted=probe_horizon_call(w);
            assert(adapted==(((uint64_t)9<<32)|10));
            assert(((observed_sp>=(uintptr_t)stack)&&(observed_sp<top))==switched);
            assert((uintptr_t)pthread_getspecific(key)==(switched?top:0));
            cases++;
        }
    }
    runtime_hle_trace_set(0);assert(!pthread_key_delete(key));assert(!munmap(stack,length));
    printf("{\"status\":\"pass\",\"configurations\":%u,\"narrow_stack_argument_mismatch_demonstrated\":true,\"wide_slot_adapter_correct\":true,\"host_stack_switch_and_trace_checked\":true}\n",cases);
}
