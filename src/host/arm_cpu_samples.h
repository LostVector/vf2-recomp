// Optional low-overhead ARMv7 CPU sampling. The signal handler only records
// instruction addresses in lock-free atomics; file I/O happens on the host loop.
#pragma once
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include "../core/tgp_sample_context.h"
#if defined(__arm__) && defined(__linux__)
#include <atomic>
#include <dlfcn.h>
#include <csignal>
#include <sys/time.h>
#include <ucontext.h>
#include <string>
#include <vector>
#include <time.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

class arm_cpu_samples {
    static constexpr unsigned slots=16384;
    inline static std::atomic<uint32_t> keys[slots]{},hits[slots]{},dropped{};
    inline static std::atomic<uint32_t> tgp_hits[4096]{},tgp_commands[256]{};
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    inline static std::atomic<uint32_t> copy_keys[2048]{},copy_hits[2048]{};
    inline static uint32_t copy_begin=0;
    std::string path;
    std::vector<uint32_t> previous_hits;
    uint64_t previous_frame=0;
    struct sigaction previous{};
    itimerval previous_timer{};
    bool active=false;
    bool thread_mode=false,thread_started=false;
    timer_t thread_timer{};clockid_t thread_clock{};timespec thread_began{};long thread_tid=0;
    static void sample(int,siginfo_t*,void *context){
        uint32_t pc=static_cast<ucontext_t*>(context)->uc_mcontext.arm_pc&~1u;
        // The target glibc ARM memcpy preserves LR throughout this first
        // 1 KiB (verified in its matching disassembly). Record callers only
        // there, rather than treating arbitrary LR values as stack traces.
        if(copy_begin&&pc-copy_begin<1024){
            const uint32_t lr=static_cast<ucontext_t*>(context)->uc_mcontext.arm_lr&~1u;
            unsigned at=((lr>>1)*2654435761u)>>(32-11);
            for(unsigned n=0;lr&&n<16;++n,at=(at+1)&2047){
                uint32_t key=copy_keys[at].load(std::memory_order_relaxed);
                if(!key){copy_keys[at].compare_exchange_strong(key,lr,std::memory_order_relaxed);key=copy_keys[at].load(std::memory_order_relaxed);}
                if(key==lr){copy_hits[at].fetch_add(1,std::memory_order_relaxed);break;}
            }
        }
        const uint32_t tgp=vf2_tgp_sample::location.load(std::memory_order_relaxed);
        if(tgp&0x80000000u){if(!(tgp&0x40000000u))tgp_hits[tgp&4095].fetch_add(1,std::memory_order_relaxed);tgp_commands[(tgp>>12)&255].fetch_add(1,std::memory_order_relaxed);}
        if(!pc)return;
        unsigned at=((pc>>1)*2654435761u)>>(32-14);
        for(unsigned n=0;n<16;n++,at=(at+1)&(slots-1)){
            uint32_t key=keys[at].load(std::memory_order_relaxed);
            if(!key){keys[at].compare_exchange_strong(key,pc,std::memory_order_relaxed);key=keys[at].load(std::memory_order_relaxed);}
            if(key==pc){hits[at].fetch_add(1,std::memory_order_relaxed);return;}
        }
        dropped.fetch_add(1,std::memory_order_relaxed);
    }
public:
    arm_cpu_samples(){
        const char *p=std::getenv("VF2_CPU_SAMPLES");if(!p||!*p)return;path=p;
        if(std::getenv("VF2_CPU_SAMPLE_WINDOWS"))previous_hits.resize(slots);
        if(std::getenv("VF2_CPU_COPY_CALLERS")){
            copy_begin=uint32_t(reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT,"memcpy")))&~1u;
            const auto *code=reinterpret_cast<const uint32_t*>(uintptr_t(copy_begin));
            if(!copy_begin||code[0]!=0xe1a0c000u||code[1]!=0xe3520040u||code[2]!=0x2a000019u)copy_begin=0;
            if(!copy_begin)std::fprintf(stderr,"Memcpy caller sampling disabled: unrecognized implementation\n");
            std::fprintf(stderr,"Memcpy caller sampling: %08x..%08x (first 1024 bytes, preserved LR)\n",copy_begin,copy_begin+1024);
        }
        struct sigaction action{};action.sa_sigaction=sample;action.sa_flags=SA_SIGINFO|SA_RESTART;sigemptyset(&action.sa_mask);
        if(sigaction(SIGPROF,&action,&previous)){std::perror("CPU sampling signal");return;}
        thread_mode=std::getenv("VF2_CPU_SAMPLES_THREAD")!=nullptr;
        if(thread_mode){active=true;return;}
        itimerval timer{};timer.it_interval.tv_usec=timer.it_value.tv_usec=5000;
        if(setitimer(ITIMER_PROF,&timer,&previous_timer)){sigaction(SIGPROF,&previous,nullptr);std::perror("CPU sampling timer");return;}
        active=true;
    }
    // Called by the simulation worker itself. Its CPU clock stops while the
    // thread sleeps; the signal is delivered to that exact Linux thread.
    bool start_thread(){
        if(!active||!thread_mode||thread_started)return true;
        thread_tid=syscall(SYS_gettid);
        if(pthread_getcpuclockid(pthread_self(),&thread_clock))return false;
        clock_gettime(thread_clock,&thread_began);
        sigevent event{};event.sigev_notify=SIGEV_THREAD_ID;event.sigev_signo=SIGPROF;event._sigev_un._tid=thread_tid;
        if(timer_create(CLOCK_THREAD_CPUTIME_ID,&event,&thread_timer)){std::perror("thread CPU sampling timer");return false;}
        itimerspec timer{};timer.it_interval.tv_nsec=timer.it_value.tv_nsec=5000000;
        if(timer_settime(thread_timer,0,&timer,nullptr)){timer_delete(thread_timer);std::perror("thread CPU sampling interval");return false;}
        thread_started=true;std::fprintf(stderr,"CPU samples target thread %ld\n",thread_tid);return true;
    }
    void dump(uint64_t frame=0){
        if(!active)return;
        FILE *out=std::fopen(path.c_str(),"w");if(!out)return;
        for(unsigned i=0;i<slots;i++){unsigned count=hits[i].load(std::memory_order_relaxed);if(count)std::fprintf(out,"%08x %u\n",keys[i].load(std::memory_order_relaxed),count);}
        std::fclose(out);
        if(!previous_hits.empty()&&frame>previous_frame){
            out=std::fopen((path+".windows").c_str(),previous_frame?"a":"w");
            if(out){
                for(unsigned i=0;i<slots;++i){
                    const auto total=hits[i].load(std::memory_order_relaxed);
                    const auto delta=total-previous_hits[i];
                    previous_hits[i]=total;
                    if(delta)std::fprintf(out,"%llu %08x %u\n",(unsigned long long)frame,keys[i].load(std::memory_order_relaxed),delta);
                }
                std::fclose(out);
                previous_frame=frame;
            }
        }
        out=std::fopen((path+".maps").c_str(),"w");FILE *in=std::fopen("/proc/self/maps","r");
        if(out&&in){char buffer[4096];size_t n;while((n=std::fread(buffer,1,sizeof(buffer),in)))std::fwrite(buffer,1,n,out);}
        if(in)std::fclose(in);if(out)std::fclose(out);
        if(std::getenv("VF2_TGP_PC_SAMPLES")){
            out=std::fopen((path+".tgp").c_str(),"w");
            if(out){for(unsigned i=0;i<4096;++i){unsigned n=tgp_hits[i].load(std::memory_order_relaxed);if(n)std::fprintf(out,"%04x %u\n",i,n);}std::fclose(out);}
        }
        if(std::getenv("VF2_TGP_PC_SAMPLES")||std::getenv("VF2_TGP_COMMAND_SAMPLES")){
            out=std::fopen((path+".tgp-commands").c_str(),"w");
            if(out){for(unsigned i=0;i<256;++i){unsigned n=tgp_commands[i].load(std::memory_order_relaxed);if(n)std::fprintf(out,"%02x %u\n",i,n);}std::fclose(out);}
        }
        if(copy_begin){
            out=std::fopen((path+".copy-callers").c_str(),"w");
            if(out){for(unsigned i=0;i<2048;++i){unsigned n=copy_hits[i].load(std::memory_order_relaxed);if(n)std::fprintf(out,"%08x %u\n",copy_keys[i].load(std::memory_order_relaxed),n);}std::fclose(out);}
        }
        if(thread_started){
            timespec now{};clock_gettime(thread_clock,&now);
            out=std::fopen((path+".thread").c_str(),"w");
            if(out){std::fprintf(out,"tid %ld\ncpu_seconds %.9f\ninterval_ns 5000000\npc_resolution_bytes 2\ndropped %u\n",thread_tid,double(now.tv_sec-thread_began.tv_sec)+double(now.tv_nsec-thread_began.tv_nsec)/1e9,dropped.load(std::memory_order_relaxed));std::fclose(out);}
        }
    }
    void reset(){if(active){for(auto &count:hits)count.store(0,std::memory_order_relaxed);for(auto &count:copy_hits)count.store(0,std::memory_order_relaxed);for(auto &count:tgp_hits)count.store(0,std::memory_order_relaxed);for(auto &count:tgp_commands)count.store(0,std::memory_order_relaxed);dropped.store(0,std::memory_order_relaxed);}}
    void stop(uint64_t frame=0){if(active){if(thread_started)timer_delete(thread_timer);if(!thread_mode){itimerval disabled{};setitimer(ITIMER_PROF,&disabled,nullptr);}dump(frame);sigaction(SIGPROF,&previous,nullptr);if(!thread_mode)setitimer(ITIMER_PROF,&previous_timer,nullptr);active=false;}}
    ~arm_cpu_samples(){stop();}
};
#else
class arm_cpu_samples {public:bool start_thread(){return true;}void dump(uint64_t=0){}void reset(){}void stop(uint64_t=0){}};
#endif
