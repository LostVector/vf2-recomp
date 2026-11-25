// Optional wall/thread-CPU accounting; serialized drains are diagnostic only.
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <time.h>
class render_timing {
public:
    enum stage { simulation, capture, camera, packets, prior_gpu, tile_upload,
        submit, gpu_3d, composite, gpu_composite, flush, swap, input, count };
    const bool enabled=std::getenv("VF2_RENDER_STAGES")!=nullptr;
    const bool serialized=std::getenv("VF2_SERIAL_GPU_PASSES")!=nullptr;
private:
    struct sample {double wall=0,cpu=0;unsigned long long calls=0;} totals[count];
    unsigned long long fences[3][3]{}; // capture/camera/packets: absent/pending/complete
    using clock=std::chrono::steady_clock;
    clock::time_point wall_start;
    double cpu_start=0;
    static double cpu(){timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return t.tv_sec*1000.+t.tv_nsec/1e6;}
public:
    // Wall ms of each stage for the frame being built, for the per-frame trace.
    double frame[count]{};
    // Thread CPU for the same stages: a stage that is mostly wall but not CPU
    // is the thread waiting or being preempted, not doing work.
    double frame_cpu[count]{};
    void begin(){if(!enabled)return;for(auto &v:frame)v=0;for(auto &v:frame_cpu)v=0;wall_start=clock::now();cpu_start=cpu();}
    void mark(stage s){if(!enabled)return;auto now=clock::now();double c=cpu();const double wall=std::chrono::duration<double,std::milli>(now-wall_start).count();auto &t=totals[s];t.wall+=wall;t.cpu+=c-cpu_start;++t.calls;frame[s]=wall;frame_cpu[s]=c-cpu_start;wall_start=now;cpu_start=c;}
    void fence(unsigned phase,int state){if(enabled&&phase<3&&state>=-1&&state<=1)++fences[phase][state+1];}
    void report(unsigned long long frames)const{
        if(!enabled)return;
        const char *names[]={"simulation wait","picture capture","camera preparation","packet/material preparation","prior GPU drain","tile upload","3D submit","serialized 3D drain","composition submit","serialized composition drain","fence/flush","EGL swap","input/pace"};
        std::fprintf(stderr,"Render stages: wall/thread CPU ms per frame; serialized GPU passes %d\n",serialized);
        for(unsigned i=0;i<count;++i)std::fprintf(stderr,"Stage %-29s %.4f %.4f (%llu calls)\n",names[i],totals[i].wall/(frames?frames:1),totals[i].cpu/(frames?frames:1),totals[i].calls);
        for(unsigned i=0;i<3;++i)std::fprintf(stderr,"Prior fence at %s: absent %llu pending %llu complete %llu\n",i==0?"capture":i==1?"camera":"packets",fences[i][0],fences[i][1],fences[i][2]);
    }
};
