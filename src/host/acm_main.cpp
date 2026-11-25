// Small native fbdev/EGL frontend for the Astro City Mini. The board and the
// game's native code are unchanged; desktop SDL3/RmlUi are not needed here.
#include "../machine/model2a.h"
#include "../gpu/gpu3d.h"
#include "egl_context.h"
#include "gpu_frame_queue.h"
#include "render_timing.h"
#include "atlas_transition_plan.h"
#include "vf2_netplay.h"
#include "arm_cpu_samples.h"
#include "guest_pc_profile.h"
#include "acm_checkpoint.h"
#include "gpu3d_snapshot.h"
#include "../gpu/geometry_frame.h"
#include "../machine/geometry_math.h"
#include "input_script.h"
#include "png.h"
#include <GLES2/gl2.h>
#include <alsa/asoundlib.h>
#include <linux/input.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <bit>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <string>
#include <vector>

namespace {
volatile std::sig_atomic_t stopping=0;
void stop(int){stopping=1;}
GLuint compile(GLenum type,const char *source){
    GLuint s=glCreateShader(type);glShaderSource(s,1,&source,nullptr);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof(log),nullptr,log);std::fprintf(stderr,"Compositor shader: %s\n",log);glDeleteShader(s);return 0;}return s;
}
class compositor {
    GLuint program=0,vbo=0,textures[4]={};
    GLint swizzle=-1,rotation=-1;
    const bool linear_upscale=std::getenv("VF2_LINEAR_UPSCALE")!=nullptr;
    int sizes[4][2]={};
public:
    ~compositor(){glDeleteTextures(4,textures);glDeleteBuffers(1,&vbo);glDeleteProgram(program);}
    bool init(){
        const char *vs="attribute vec2 aPosition;attribute vec2 aUV;uniform vec2 uRotation;varying mediump vec2 uv;void main(){vec2 p=vec2(aPosition.x*uRotation.x-aPosition.y*uRotation.y,aPosition.x*uRotation.y+aPosition.y*uRotation.x);gl_Position=vec4(p,0.,1.);uv=aUV;}";
        const char *fs="precision mediump float;varying mediump vec2 uv;uniform sampler2D uImage;uniform float uSwizzle;void main(){vec4 c=texture2D(uImage,uv);gl_FragColor=uSwizzle>.5?c.bgra:c;}";
        GLuint a=compile(GL_VERTEX_SHADER,vs),b=compile(GL_FRAGMENT_SHADER,fs);if(!a||!b)return false;
        program=glCreateProgram();glAttachShader(program,a);glAttachShader(program,b);glBindAttribLocation(program,0,"aPosition");glBindAttribLocation(program,1,"aUV");glLinkProgram(program);glDeleteShader(a);glDeleteShader(b);
        GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)return false;
        swizzle=glGetUniformLocation(program,"uSwizzle");rotation=glGetUniformLocation(program,"uRotation");
        glGenBuffers(1,&vbo);glGenTextures(4,textures);return true;
    }
    void begin(int w,int h,int angle){
        glBindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);glViewport(0,0,w,h);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        float aspect=(angle==90||angle==270)?.75f:4.f/3.f;
        int vw=w,vh=int(w/aspect);if(vh>h){vh=h;vw=int(h*aspect);}
        int x=(w-vw)/2,y=(h-vh)/2;glViewport(x,y,vw,vh);glScissor(x,y,vw,vh);glEnable(GL_SCISSOR_TEST);
        glUseProgram(program);glUniform1i(glGetUniformLocation(program,"uImage"),0);
        float c=angle==0?1.f:angle==180?-1.f:0.f,s=angle==90?1.f:angle==270?-1.f:0.f;glUniform2f(rotation,c,s);
        glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        glBindBuffer(GL_ARRAY_BUFFER,vbo);glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),nullptr);glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    }
    GLuint upload(int slot,const uint32_t *pixels,int w,int h){
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,textures[slot]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        if(sizes[slot][0]!=w||sizes[slot][1]!=h){glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);sizes[slot][0]=w;sizes[slot][1]=h;}
        else glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        return textures[slot];
    }
    void image(GLuint texture,float x,float y,float w,float h,bool cpu,bool top_down=false,bool opaque=false){
        float x0=x/496*2-1,x1=(x+w)/496*2-1,y0=1-y/384*2,y1=1-(y+h)/384*2;
        float t=(cpu||top_down)?0.f:1.f,b=1-t;
        float vertices[]={x0,y0,0,t,x1,y0,1,t,x0,y1,0,b,x1,y1,1,b};
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);glUniform1f(swizzle,cpu?1.f:0.f);
        if(opaque){
            // Filter the composed picture at presentation time. The indexed
            // textures and the layers feeding the canvas keep their own filters.
            if(linear_upscale)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glDisable(GL_BLEND);
        }
        glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        if(opaque){
            glEnable(GL_BLEND);
            if(linear_upscale)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        }
    }
    void end(){glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisableVertexAttribArray(0);glDisableVertexAttribArray(1);glBindBuffer(GL_ARRAY_BUFFER,0);}
};
class controls {
    std::vector<int> fds;
    bool keys[KEY_MAX+1]={};
public:
    controls(){for(int n=0;n<32;n++){char p[64];std::snprintf(p,sizeof(p),"/dev/input/event%d",n);int fd=open(p,O_RDONLY|O_NONBLOCK);if(fd>=0){char name[128]={};ioctl(fd,EVIOCGNAME(sizeof(name)),name);std::fprintf(stderr,"Input %s: %s\n",p,name);fds.push_back(fd);}}}
    ~controls(){for(int fd:fds)close(fd);}
    void poll(uint8_t ports[4]){
        bool pressed[KEY_MAX+1]={};
        for(int fd:fds){input_event e;while(read(fd,&e,sizeof(e))==sizeof(e))if(e.type==EV_KEY&&e.code<=KEY_MAX){keys[e.code]=e.value!=0;pressed[e.code]|=e.value==1;}}
        auto active=[&](int key){return keys[key]||pressed[key];};
        for(int i=0;i<4;i++)ports[i]=255;
        auto set=[&](int port,uint8_t bit,bool held){if(held)ports[port]&=~bit;};
        set(0,1,active(KEY_F2)||active(KEY_5));set(0,0x10,active(KEY_F1)||active(KEY_ENTER));
        set(1,1,active(KEY_A)||active(KEY_X));set(1,2,active(KEY_B)||active(KEY_Y));set(1,4,active(KEY_C)||active(KEY_Z));
        set(1,0x10,active(KEY_DOWN));set(1,0x20,active(KEY_UP));set(1,0x40,active(KEY_RIGHT));set(1,0x80,active(KEY_LEFT));
        if(active(KEY_ESC)||active(KEY_POWER))stopping=1;
    }
};
class audio {
    snd_pcm_t *pcm=nullptr;
    // The frame loop must not wait on the device. A writer thread drains a
    // bounded queue; write() only enqueues. With the synchronous write, a
    // backed-up period added ~2.9 ms to the present-grid p95
    // (build/acm/pgo-audio2).
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::vector<int16_t>> queue;
    std::thread writer;
    bool quit=false;
    unsigned long dropped=0,played=0,enqueued=0;
    uint64_t played_samples=0;
    void drain(){
        for(;;){
            std::vector<int16_t> block;
            {
                std::unique_lock<std::mutex> held(lock);
                wake.wait(held,[&]{return quit||!queue.empty();});
                if(quit)return;
                block=std::move(queue.front());queue.pop_front();
            }
            size_t at=0,frames=block.size()/2;
            while(at<frames){
                snd_pcm_sframes_t n=snd_pcm_writei(pcm,block.data()+at*2,frames-at);
                if(n<0){if(snd_pcm_recover(pcm,n,1)<0){std::fprintf(stderr,"ALSA stopped: %s\n",snd_strerror(n));return;}continue;}
                if(!n)break;at+=size_t(n);
            }
            ++played;played_samples+=at;
        }
    }
public:
    ~audio(){
        {std::lock_guard<std::mutex> held(lock);quit=true;}
        wake.notify_all();
        if(writer.joinable())writer.join();
        if(pcm)snd_pcm_close(pcm);
        std::fprintf(stderr,"Audio: %lu blocks enqueued, %lu to the device (%llu samples), %lu dropped\n",
            enqueued,played,(unsigned long long)played_samples,dropped);
    }
    bool open(){
        // Match the stock gameapp's analog-codec initialization. Preserve a
        // nonzero setting; the RAM kernel commonly starts with this muted.
        snd_ctl_t *ctl=nullptr;
        if(snd_ctl_open(&ctl,"default",0)>=0){
            snd_ctl_elem_id_t *id;snd_ctl_elem_info_t *info;snd_ctl_elem_value_t *value;
            snd_ctl_elem_id_alloca(&id);snd_ctl_elem_info_alloca(&info);snd_ctl_elem_value_alloca(&value);
            snd_ctl_elem_id_set_interface(id,SND_CTL_ELEM_IFACE_MIXER);snd_ctl_elem_id_set_name(id,"Lineout volume control");
            snd_ctl_elem_info_set_id(info,id);snd_ctl_elem_value_set_id(value,id);
            if(snd_ctl_elem_info(ctl,info)>=0&&snd_ctl_elem_info_get_type(info)==SND_CTL_ELEM_TYPE_INTEGER&&snd_ctl_elem_read(ctl,value)>=0){
                bool muted=true;unsigned count=snd_ctl_elem_info_get_count(info);
                for(unsigned i=0;i<count;i++)muted&=snd_ctl_elem_value_get_integer(value,i)==0;
                if(muted){long level=std::clamp(40L,snd_ctl_elem_info_get_min(info),snd_ctl_elem_info_get_max(info));for(unsigned i=0;i<count;i++)snd_ctl_elem_value_set_integer(value,i,level);snd_ctl_elem_write(ctl,value);}
            }
            // The diagnostic kernel defaults to the headset route with the
            // speaker disabled. Select the cabinet's built-in speaker.
            struct route {const char *name;snd_ctl_elem_type_t type;unsigned setting;};
            for(auto r:{route{"Speaker Function",SND_CTL_ELEM_TYPE_ENUMERATED,1},route{"Audio speaker out",SND_CTL_ELEM_TYPE_BOOLEAN,1}}){
                snd_ctl_elem_id_set_name(id,r.name);snd_ctl_elem_info_set_id(info,id);snd_ctl_elem_value_set_id(value,id);
                if(snd_ctl_elem_info(ctl,info)<0||snd_ctl_elem_info_get_type(info)!=r.type||snd_ctl_elem_read(ctl,value)<0)continue;
                if(r.type==SND_CTL_ELEM_TYPE_ENUMERATED){
                    if(r.setting>=snd_ctl_elem_info_get_items(info))continue;
                    snd_ctl_elem_info_set_item(info,r.setting);
                    if(snd_ctl_elem_info(ctl,info)<0||std::strcmp(snd_ctl_elem_info_get_item_name(info),"spk"))continue;
                    snd_ctl_elem_value_set_enumerated(value,0,r.setting);
                }else snd_ctl_elem_value_set_boolean(value,0,r.setting);
                if(snd_ctl_elem_write(ctl,value)<0)std::fprintf(stderr,"ALSA route failed: %s\n",r.name);
            }
            snd_ctl_close(ctl);
        }
        const char *name=getenv("VF2_PCM");if(!name)name="default";
        int e=snd_pcm_open(&pcm,name,SND_PCM_STREAM_PLAYBACK,0);
        if(e>=0)e=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,2,44100,1,100000);
        if(e<0){std::fprintf(stderr,"ALSA: %s\n",snd_strerror(e));if(pcm)snd_pcm_close(pcm);pcm=nullptr;return false;}
        snd_pcm_hw_params_t *hp=nullptr;snd_pcm_hw_params_malloc(&hp);
        if(hp&&snd_pcm_hw_params_current(pcm,hp)>=0){
            unsigned rate=0,periods=0;
            snd_pcm_uframes_t period=0,buffer=0;
            snd_pcm_hw_params_get_rate(hp,&rate,0);
            snd_pcm_hw_params_get_period_size(hp,&period,0);
            snd_pcm_hw_params_get_buffer_size(hp,&buffer);
            snd_pcm_hw_params_get_periods(hp,&periods,0);
            std::fprintf(stderr,"Audio: %u Hz, period %u frames, buffer %u frames, %u periods\n",
                rate,period,buffer,periods);
        }
        snd_pcm_hw_params_free(hp);
        writer=std::thread([this]{drain();});
        return true;
    }
    void write(const std::vector<int16_t> &data){
        if(!pcm||data.empty())return;
        std::lock_guard<std::mutex> held(lock);
        // ~8 frames of slack. If the device falls further behind, drop the
        // oldest block rather than let the backlog grow without bound.
        if(queue.size()>=8){queue.pop_front();++dropped;}
        queue.push_back(data);++enqueued;
        wake.notify_one();
    }
};
}
int main(int argc,char **argv){
    arm_cpu_samples cpu_samples;
    std::string rom,capture,scriptpath,load_state_path,save_state_path;bool headless=false,mute=false,probe=false,uncapped=false,release_rom=false,release_state=false,no_sound_board=false,simulation_only=false,graphics_only=false;uint64_t frames=0,report_every=300;int rotate=0;unsigned draw_count=1,draw_period=1;
    for(int i=1;i<argc;i++){
        std::string a=argv[i];auto next=[&](){if(i+1>=argc){std::fprintf(stderr,"Missing value after %s\n",a.c_str());std::exit(2);}return argv[++i];};
        if(a=="--headless")headless=true;else if(a=="--mute")mute=true;else if(a=="--probe")probe=true;else if(a=="--uncapped")uncapped=true;
        else if(a=="--release-rom")release_rom=true;
        else if(a=="--release-state")release_state=true;
        else if(a=="--no-sound-board"){no_sound_board=true;mute=true;}
        else if(a=="--load-state")load_state_path=next();else if(a=="--save-state")save_state_path=next();
        else if(a=="--draw-cadence"){
            const std::string value=next();
            if(value.size()!=3||value[1]!='/'||value[0]<'1'||value[2]>'8'||value[0]>=value[2]){
                std::fprintf(stderr,"--draw-cadence expects D/N, with 1 <= D < N <= 8\n");return 2;
            }
            draw_count=unsigned(value[0]-'0');draw_period=unsigned(value[2]-'0');
        }
        else if(a=="--report-every")report_every=std::max(1ull,std::strtoull(next(),nullptr,10));
        else if(a=="--simulation-only"){simulation_only=true;mute=true;uncapped=true;}
        else if(a=="--graphics-only"){graphics_only=true;mute=true;uncapped=true;}
        else if(a=="--frames")frames=strtoull(next(),nullptr,10);else if(a=="--capture")capture=next();else if(a=="--script")scriptpath=next();else if(a=="--rotate")rotate=atoi(next());
        else if(rom.empty()&&a[0]!='-')rom=a;else{std::fprintf(stderr,"Unknown argument %s\n",a.c_str());return 2;}
    }
    if((rom.empty()&&!probe)||(rotate!=0&&rotate!=90&&rotate!=180&&rotate!=270)){std::fprintf(stderr,"usage: vf2-acm vf2.zip [--frames N] [--capture FILE] [--script FILE] [--rotate 0|90|180|270] [--mute] [--uncapped] [--headless]\n       vf2-acm --probe [--headless]\n");return 2;}
    if(simulation_only&&graphics_only){std::fprintf(stderr,"Choose one isolation mode\n");return 2;}
    const bool reduced_cadence=draw_count<draw_period;
    if(reduced_cadence&&(graphics_only||simulation_only||probe)){
        std::fprintf(stderr,"--draw-cadence requires normal gameplay\n");return 2;
    }
    if(std::getenv("VF2_GUEST_COUNTS")&&(!frames||frames>1200)){std::fprintf(stderr,"Guest counting requires 1..1200 frames\n");return 2;}
    // Experiment only: the pipeline and the checkpoint loader were written with the
    // sound board held. The board's 68000/SCSP run on the simulation side and the host
    // only reads m_audio_out after the simulation job is joined, so enabling both is
    // safe for a timing measurement; a checkpoint does not carry the board's state, so
    // the sound will not be phase-accurate after a load.
    const bool audio_experiment=std::getenv("VF2_AUDIO_EXPERIMENT")!=nullptr;
    const bool pipeline=(std::getenv("VF2_PIPELINE")||std::getenv("VF2_PIPELINE_RENDER"))&&!simulation_only&&!graphics_only;
    const bool snapshot_render=pipeline&&std::getenv("VF2_PIPELINE_RENDER");
    const bool camera_packets=std::getenv("VF2_CAMERA_PACKETS")!=nullptr;
    if(camera_packets&&!snapshot_render&&!simulation_only){std::fprintf(stderr,"Camera packets require snapshot rendering\n");return 2;}
    const bool canvas=std::getenv("VF2_GPU_CANVAS")&&!simulation_only;
    if(pipeline&&!no_sound_board&&!audio_experiment){std::fprintf(stderr,"Experimental pipeline requires --no-sound-board\n");return 2;}
    std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
    vf2_egl_context egl;if(!egl.open(headless)){std::fprintf(stderr,"EGL initialization failed: %x\n",eglGetError());return 1;}
    const bool queued_gpu=std::getenv("VF2_GPU_QUEUE")&&!simulation_only;
    const int simulation_core=std::getenv("VF2_SIM_CPU")?std::atoi(std::getenv("VF2_SIM_CPU")):-1;
    if(simulation_core>=0){
        cpu_set_t render_set;
        if(!pipeline||simulation_core>=CPU_SETSIZE||sched_getaffinity(0,sizeof(render_set),&render_set)||!CPU_ISSET(simulation_core,&render_set))return 2;
        CPU_CLR(simulation_core,&render_set);
        if(!CPU_COUNT(&render_set)||sched_setaffinity(0,sizeof(render_set),&render_set)){std::perror("render CPU affinity");return 1;}
    }
    if(queued_gpu&&(!snapshot_render||!canvas)){std::fprintf(stderr,"Bounded GPU queue requires snapshot render pipeline and canvas\n");return 2;}
    std::vector<atlas_state_recipe> state_prewarm;
    std::vector<atlas_preload_progress> preload_progress;
    std::vector<uint64_t> state_keys;
    if(const char *plan=std::getenv("VF2_ATLAS_STATE_PLAN")){
        if(!queued_gpu||!read_atlas_state_plan(plan,state_prewarm)){
            std::fprintf(stderr,"Invalid atlas state plan or renderer mode\n");return 2;
        }
        preload_progress.resize(state_prewarm.size());
        std::fprintf(stderr,"Atlas preload plan ready: %zu selections\n",state_prewarm.size());
    }
    render_timing timing;
    if(timing.serialized&&!timing.enabled){std::fprintf(stderr,"Serialized GPU passes require render-stage timing\n");return 2;}
    gpu_frame_queue gpu_queue;
    if(queued_gpu&&!gpu_queue.open()){std::fprintf(stderr,"Bounded GPU queue requires EGL/GL fence extensions\n");return 1;}
    std::fprintf(stderr,"EGL surface %dx%d; GPU %s; %s\n",egl.width,egl.height,glGetString(GL_RENDERER),glGetString(GL_VERSION));
    auto gpu=gpu3d_create_gles2();if(!gpu){std::fprintf(stderr,"GLES2 renderer initialization failed\n");return 1;}
    if(probe){
        GLint range[2]={},precision=0,units=0,texture_size=0;
        glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER,GL_HIGH_FLOAT,range,&precision);
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&units);glGetIntegerv(GL_MAX_TEXTURE_SIZE,&texture_size);
        std::fprintf(stderr,"Fragment highp: %d bits, range %d..%d; texture units %d, max size %d\n",precision,range[0],range[1],units,texture_size);
        std::fprintf(stderr,"Extensions: %s\n%s ready\n",glGetString(GL_EXTENSIONS),gpu->name());return 0;
    }
    if(!gpu->make_targets(496,384,1))return 1;
    std::unique_ptr<gpu2d> tile_gpu;
    if(std::getenv("VF2_GPU2D")||canvas){tile_gpu=gpu2d_create_gles2();if(!tile_gpu){std::fprintf(stderr,"GPU tile renderer initialization failed\n");return 1;}}
    compositor display;if(!display.init())return 1;
    auto roms=romset::load(rom);if(!roms.ok){std::fprintf(stderr,"%s\n",roms.error.c_str());return 1;}
    if(release_rom){
        // The uploaded ZIP occupies unreclaimable initramfs memory in addition
        // to the decoded regions. Release only an explicitly temporary copy.
        if((rom.rfind("/run/",0)!=0&&rom.rfind("/tmp/",0)!=0)||rom.find("..")!=std::string::npos){std::fprintf(stderr,"--release-rom requires a /run/ or /tmp/ path\n");return 2;}
        if(unlink(rom.c_str())){std::perror("release RAM ROM");return 1;}
        std::fprintf(stderr,"Released temporary ROM archive after decoding\n");
    }
    const char *rom_dedup=std::getenv("VF2_ROM_DEDUP");
    model2a machine(std::move(roms),true,!rom_dedup||std::strcmp(rom_dedup,"0")!=0);
    machine.release_unused_roms();machine.set_sound(!no_sound_board);
    // Match the desktop frontend: schedule UART byte arrivals rather than a
    // million idle clock events per emulated second. The bit-level mode is
    // retained by vf2run for exact reference validation.
    machine.set_byte_link(true);machine.reset();machine.defer_3d(true);
    if(camera_packets&&!machine.video().camera_packets(true)){std::fprintf(stderr,"Camera packets require the normal arcade camera\n");return 2;}
    if(std::getenv("VF2_GEOMETRY_WORKER")&&!machine.video().geometry_jobs(geometry_execution::worker)){std::fprintf(stderr,"Geometry worker requires camera packets\n");return 2;}
    const bool sample_geometry=std::getenv("VF2_CPU_SAMPLES_GEOMETRY")!=nullptr;
    if(sample_geometry&&!machine.video().observe_geometry_worker([&]{
        if(!cpu_samples.start_thread())std::abort();
    })){
        std::fprintf(stderr,"Geometry profiling requires the geometry worker\n");return 2;
    }
    if(!machine.recompiled()){std::fprintf(stderr,"Native i960 compilation failed\n");return 1;}
    if((!load_state_path.empty()||!save_state_path.empty())&&!no_sound_board&&!audio_experiment){std::fprintf(stderr,"Checkpoint tests require --no-sound-board\n");return 2;}
    if(!load_state_path.empty()){
        std::string error;if(!acm_checkpoint::load(machine,load_state_path,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        std::fprintf(stderr,"Sound board at load: 68000 %llu cycles\n",(unsigned long long)machine.sound_68k_cycles());
        if(release_state){
            if((load_state_path.rfind("/run/",0)!=0&&load_state_path.rfind("/tmp/",0)!=0)||load_state_path.find("..")!=std::string::npos){std::fprintf(stderr,"--release-state requires a temporary path\n");return 2;}
            if(unlink(load_state_path.c_str())){std::perror("release checkpoint");return 1;}
        }
        for(int i=0;i<3;i++)machine.run_frame();
        std::fprintf(stderr,"Resumed checkpoint at game frame %llu\n",(unsigned long long)machine.frame_number());
    }
    const auto &native=machine.native_stats();std::fprintf(stderr,"i960 native: %zu functions, %zu instructions, %zu bytes\n",native.functions,native.instructions,native.code_bytes);
    std::fprintf(stderr,"TGP: %s\n",machine.tgp().precompiled()?"precompiled instruction handlers":"interpreter");
    controls input;audio sound;if(!mute&&!headless)sound.open();
    input_script script;std::string error;if(!scriptpath.empty()&&!script.load(scriptpath,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
    using clock=std::chrono::steady_clock;
    auto milliseconds=[](clock::time_point start){return std::chrono::duration<double,std::milli>(clock::now()-start).count();};
    double tile_prepare_ms=0,scene_capture_ms=0,parts_ms=0,packet_prepare_ms=0,submit_ms=0;
    std::vector<gpu3d_poly> polygons;gpu3d_rams rams{};
    gpu3d_projected_stream projected;
    uint64_t projected_frames=0,projected_fallbacks=0,projected_bytes=0,full_projected_bytes=0;
    gpu3d_snapshot snapshot;bool snapshot_pending=false;
    const bool share_camera_colors=snapshot_render&&std::getenv("VF2_SHARED_CAMERA_COLORS");
    uint64_t shared_color_frames=0;
    gpu3d_geometry_frame camera_scene;bool camera_pending=false;double camera_prepare_ms=0;
    const bool draw_mesh=std::getenv("VF2_MESH_RENDER")!=nullptr;
    const bool check_mesh_handoff=std::getenv("VF2_MESH_HANDOFF")!=nullptr;
    const bool use_projected_stream=std::getenv("VF2_PROJECTED_STREAM")&&queued_gpu&&camera_packets&&!draw_mesh&&!check_mesh_handoff;
    uint64_t mesh_frames=0,mesh_vertices=0,mesh_polygons=0,mesh_fallbacks=0,mesh_projected=0;
    auto verify_mesh_handoff=[&](){
        if(!camera_scene.compact||camera_scene.mesh_polygons.size()!=camera_scene.compact_polygons.size())
            return false;
        for(size_t i=0;i<camera_scene.mesh_polygons.size();++i){
            const auto &mesh=camera_scene.mesh_polygons[i];
            if(mesh.object==gpu3d_mesh_polygon::invalid){++mesh_fallbacks;continue;}
            if(mesh.object>=camera_scene.mesh_objects.size())return false;
            const auto &object=camera_scene.mesh_objects[mesh.object];
            const auto &camera=camera_scene.compact_polygons[i];
            const geometry_math::matrix3x4 transform(object.matrix.data(),false);
            for(unsigned v=0;v<camera.n;++v){
                auto vertex=mesh.position[v];
                transform.apply(vertex[0],vertex[1],vertex[2],true);
                vertex[0]*=object.focus[0];
                vertex[1]*=object.focus[1];
                for(unsigned axis=0;axis<3;++axis){
                    if((std::bit_cast<uint32_t>(vertex[axis])&0xffffff00u)!=
                        std::bit_cast<uint32_t>(camera.position[v][axis])){
                        std::fprintf(stderr,"Mesh handoff mismatch object %x polygon %u vertex %u axis %u\n",
                            object.address,mesh.ordinal,v,axis);
                        return false;
                    }
                }
                ++mesh_vertices;
            }
            ++mesh_polygons;
        }
        ++mesh_frames;
        return true;
    };
    bool canvas_ready=false,canvas_drawn=false;
    auto layers=[&](gpu3d_frame &frame){
        if(!canvas_ready)return;
        frame.layers=tile_gpu.get();
        frame.underlay=[](void *p){auto *tiles=static_cast<gpu2d*>(p);return tiles->paint(0,0,true,true)&&tiles->paint(1,0,true,false);};
        frame.overlay=[](void *p){return static_cast<gpu2d*>(p)->paint(2,0,true,false);};
    };
    model2a::gpu3d_hook hook=[&](model2_video &video,int picture){
        const auto start=clock::now();
        video.gpu_rams(rams);
        if(share_camera_colors)snapshot.capture(rams);
        camera_pending=video.capture_camera_picture(picture,camera_scene,
            share_camera_colors?&snapshot.rams():nullptr);
        if(!camera_pending)video.export_picture(picture,polygons);
        if(snapshot_render){
            if(share_camera_colors)shared_color_frames+=camera_pending&&camera_scene.has_shared_compact();
            else snapshot.capture(rams);
            snapshot_pending=true;scene_capture_ms+=milliseconds(start);return true;
        }
        gpu->set_rams(rams);
        gpu3d_frame frame;frame.w=496;frame.h=384;layers(frame);
        bool ok=gpu->submit(polygons.data(),polygons.size(),frame,0);if(ok&&canvas_ready)canvas_drawn=true;return ok;
    };
    model2a::gpu2d_hook tile_hook=[&](const gpu2d_frame &frame){
        const auto start=clock::now();bool ok;
        if(canvas){canvas_ready=queued_gpu?tile_gpu->stage(frame):tile_gpu->prepare(frame);ok=canvas_ready;}
        else ok=tile_gpu->submit(frame);
        tile_prepare_ms+=milliseconds(start);return ok;
    };
    const bool early_tiles=std::getenv("VF2_EARLY_TILE_STAGING")&&queued_gpu&&pipeline&&
                           camera_packets&&!graphics_only&&!reduced_cadence;
    std::atomic<bool> tile_renderer_idle{false};
    auto tile_capture=model2a::vblank_tiles::unavailable;
    gpu2d_frame pending_tiles;
    uint64_t tile_descriptors=0,early_tile_frames=0,joined_tile_frames=0;
    uint64_t tile_ownership_misses=0,tile_stage_declines=0,tile_ineligible=0;
    const std::function<void()> vblank_tiles=[&]{
        if(!machine.prepare_vblank_tiles(pending_tiles)){
            ++tile_ineligible;
            return;
        }
        ++tile_descriptors;
        tile_capture=model2a::vblank_tiles::decoded;
        if(!tile_renderer_idle.load(std::memory_order_acquire)){
            ++tile_ownership_misses;
            return;
        }
        if(tile_gpu->stage(pending_tiles)){
            tile_capture=model2a::vblank_tiles::staged;
            ++early_tile_frames;
        }else ++tile_stage_declines;
    };
    if(early_tiles)machine.set_vblank_tile_hook(&vblank_tiles);
    if(simulation_only){
        if(!camera_packets)machine.video().draw_deferred();machine.set_draw_3d(false);
        std::fprintf(stderr,"Diagnostic: simulation and geometry, no rasterization or display\n");
    }
    if(graphics_only)std::fprintf(stderr,"Diagnostic: graphics only, game simulation frozen\n");
    guest_pc_profile guest_counts;guest_counts.start(machine);
    cpu_samples.reset();machine.reset_scheduler_stats();machine.video().reset_geometry_job_stats();
    if(!pipeline&&!sample_geometry&&!cpu_samples.start_thread())return 1;
    constexpr auto tick_period=std::chrono::nanoseconds(17384000);
    // Opt-in: present on the tick grid instead of at tick + work. The frame is
    // already complete when the pacing sleep starts, so moving the swap after
    // the sleep makes the interval tick_period + swap jitter instead of
    // tick_period + frame work jitter.
    const bool present_at_tick=std::getenv("VF2_PRESENT_AT_TICK")!=nullptr;
    // Wake-up latency from sleep_until is the remaining cadence jitter once the
    // swap is on the grid (build/acm/present-times). Sleeping short of the
    // deadline and spinning the last margin makes the present start on the tick.
    const int64_t pace_spin_ns=[](){const char *p=std::getenv("VF2_PACE_SPIN_US");return p?int64_t(std::atoll(p))*1000:0;}();
    const char *frame_trace_path=std::getenv("VF2_FRAME_TRACE");
    struct frame_observation {
        int64_t wait_ns,simulation_ns,swap_ns,deadline_ns,swap_start_ns,sleep_end_ns;
        bool resynced;
    };
    std::vector<frame_observation> frame_trace;
    if(frame_trace_path)frame_trace.reserve(frames);
    auto began=clock::now();auto next_tick=began;uint64_t f=0,presented=0;bool captured=false;
    auto since=[](auto end,auto start){return std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();};
    double phase_ms[6]={};auto window_began=began;std::vector<uint32_t> capture_pixels;
    background_job simulation_job;bool simulation_pending=false;
    double simulation_job_ms=0,simulation_task_ms=0;uint64_t simulation_tasks=0;
    auto inputs=[&](uint64_t frame){
        if(!mute&&!headless)sound.write(machine.m_audio_out);machine.m_audio_out.clear();
        uint8_t held[4],scripted[4];input.poll(held);script.apply(frame,scripted);
        machine.m_in0=held[0]&scripted[0];machine.m_in1=held[1]&scripted[1];machine.m_in2=held[2]&scripted[2];machine.m_sw=held[3]&scripted[3];
    };
    if(pipeline)std::fprintf(stderr,"Pipeline: next simulation overlaps current %s; CPU phase reports remaining wait\n",snapshot_render?"render preparation and presentation":"presentation");
    if(std::getenv("VF2_CPU_SAMPLES_RENDER")&&!cpu_samples.start_thread())return 1;
    for(;!stopping&&(!frames||f<frames);f++){
        timing.begin();
        auto phase=clock::now();
        const auto frame_start=phase;
        bool waited_for_worker=false;
        auto mark=[&](int i){auto now=clock::now();phase_ms[i]+=std::chrono::duration<double,std::milli>(now-phase).count();phase=now;};
        if(!graphics_only){
            if(simulation_pending){simulation_job.wait();simulation_pending=false;simulation_task_ms+=simulation_job_ms;++simulation_tasks;waited_for_worker=true;}
            else machine.run_frame();
        }
        mark(0);timing.mark(render_timing::simulation);
        const auto wait_ns=since(phase,frame_start);
        const auto simulation_ns=waited_for_worker?int64_t(simulation_job_ms*1000000):wait_ns;
        auto swapped=clock::time_point{};
        int64_t swap_start_ns=0,sleep_end_ns=0;
        bool next_launched=false;
        auto next_simulation=[&](){
            if(!pipeline||next_launched)return;
            next_launched=true;
            inputs(f);
            if(!stopping&&(!frames||f+1<frames)){
                if(early_tiles){
                    tile_capture=model2a::vblank_tiles::unavailable;
                    tile_renderer_idle.store(false,std::memory_order_relaxed);
                }
                simulation_job.start([&]{
                    static thread_local bool pinned=false;
                    if(simulation_core>=0&&!pinned){
                        cpu_set_t set;CPU_ZERO(&set);CPU_SET(simulation_core,&set);
                        if(sched_setaffinity(0,sizeof(set),&set)){std::perror("simulation CPU affinity");std::abort();}
                        pinned=true;
                    }
                    if(!sample_geometry&&!cpu_samples.start_thread())std::abort();
                    auto start=clock::now();machine.run_frame();simulation_job_ms=std::chrono::duration<double,std::milli>(clock::now()-start).count();
                });
                simulation_pending=true;
            }
        };
        // The board and inputs run on each tick. The last tick of a finite
        // capture is always drawn so its PNG corresponds to the state hashes.
        const bool capture_tick=!capture.empty()&&frames&&f+1==frames;
        const bool skip_draw=reduced_cadence&&(f%draw_period)*draw_count%draw_period>=draw_count&&!capture_tick;
        if(!simulation_only&&!skip_draw){
        model2a::picture_parts parts;
        canvas_drawn=false;
        snapshot_pending=false;
        const auto parts_start=clock::now();
        if(tile_capture==model2a::vblank_tiles::decoded){
            // The prior frame and the simulation job have both joined.
            // Preserve the descriptor when retrying an early validation decline.
            if(tile_gpu->stage(pending_tiles)){
                tile_capture=model2a::vblank_tiles::staged;
                ++joined_tile_frames;
            }
        }
        canvas_ready=tile_capture==model2a::vblank_tiles::staged;
        const auto *capture_tiles=tile_gpu&&tile_capture!=model2a::vblank_tiles::decoded?&tile_hook:nullptr;
        bool separate=machine.render_parts(-1,nullptr,0,false,parts,&hook,true,false,capture_tiles,tile_capture);
        parts_ms+=milliseconds(parts_start);timing.mark(render_timing::capture);
        if(!state_prewarm.empty()){
            const uint64_t tick=f+1;
            const auto &work=machine.workram();
            const std::array<unsigned,5> identity{machine.game_mode(),work[vf2net::STAGE],
                work[vf2net::VERSUS],atlas_fighter_id(work,0),atlas_fighter_id(work,1)};
            for(size_t i=0;i<state_prewarm.size();++i){
                const auto &recipe=state_prewarm[i];
                auto &progress=preload_progress[i];
                if(progress.batch>=recipe.batches.size())continue;
                if(identity!=recipe.identity||
                   ((recipe.lock_mask&1)&&!atlas_selection_locked(work,0))||
                   ((recipe.lock_mask&2)&&!atlas_selection_locked(work,1))){
                    progress.changed=0;
                    if(progress.batch)progress.batch=recipe.batches.size();
                    continue;
                }
                gpu3d_rams live{};
                machine.video().gpu_rams(live);
                const auto &source=snapshot_pending?snapshot.rams():live;
                const std::array<uint32_t,2> revisions{source.gen[0],source.gen[1]};
                if(!progress.changed||revisions!=progress.revisions){
                    progress.changed=tick;
                    progress.revisions=revisions;
                    progress.wait_for_revision=false;
                }
                // A write on either sheet delays the next batch. Individual
                // source signatures still determine which tiles are ready.
                if(progress.wait_for_revision||tick-progress.changed<recipe.stable_ticks)continue;
                const auto start=clock::now();
                const auto &batch=recipe.batches[progress.batch];
                if(!progress.batch&&!gles2_check_atlas_sources(source,&recipe.sentinel,1)){
                    progress.wait_for_revision=true;
                    continue;
                }
                state_keys.clear();
                if(!gles2_check_atlas_sources(source,batch.data(),batch.size())){
                    progress.wait_for_revision=true;
                    if(progress.batch)
                        std::fprintf(stderr,"Atlas state waiting for source at tick %llu\n",(unsigned long long)tick);
                    continue;
                }
                for(const auto &tile:batch)state_keys.push_back(tile.key);
                if(!snapshot_pending)snapshot.capture(live);
                if(!gpu_queue.drain())return 1;
                if(!gles2_prewarm_atlas(*gpu,snapshot.rams(),state_keys.data(),state_keys.size())){
                    std::fprintf(stderr,"Atlas state preload stopped at tick %llu\n",(unsigned long long)tick);
                    progress.batch=recipe.batches.size();
                }else{
                    if(!progress.batch)
                        std::fprintf(stderr,"Atlas state recipe: %zu %u %u\n",i,identity[3],identity[4]);
                    ++progress.batch;
                    std::fprintf(stderr,"Atlas state batch: %llu %zu %.4f ms\n",
                        (unsigned long long)tick,state_keys.size(),milliseconds(start));
                }
                break;
            }
        }
        if(timing.enabled&&queued_gpu)timing.fence(0,gpu_queue.pending_status());
        if(separate&&parts.has3d&&!parts.gpu3d)separate=false;
        if(separate&&snapshot_pending){
            next_simulation();
            if(check_mesh_handoff&&(!camera_pending||!verify_mesh_handoff())){
                std::fprintf(stderr,"Invalid native mesh handoff\n");
                return 1;
            }
            bool packed_projection=false;
            if(camera_pending){
                const auto start=clock::now();
                const bool direct=std::getenv("VF2_DIRECT_PROJECTION")!=nullptr;
                packed_projection=use_projected_stream&&direct&&camera_scene.prepare_projected(projected);
                if(!packed_projection)camera_scene.prepare(polygons,direct);
                camera_prepare_ms+=milliseconds(start);
            }
            timing.mark(render_timing::camera);
            if(timing.enabled&&queued_gpu)timing.fence(1,gpu_queue.pending_status());
            if(check_mesh_handoff){
                if(camera_scene.mesh_projected_sources.size()!=polygons.size())return 1;
                for(size_t i=0;i<polygons.size();++i){
                    const uint32_t source=camera_scene.mesh_projected_sources[i];
                    if(source==gpu3d_mesh_polygon::invalid)continue;
                    if(source>=camera_scene.compact_polygons.size())return 1;
                    const auto &r=camera_scene.compact_polygons[source];
                    const auto &p=polygons[i];
                    if(p.n!=r.n)return 1;
                    for(unsigned v=0;v<p.n;++v){
                        const float z=r.position[v][2]+std::numeric_limits<float>::min();
                        if(p.x[v]!=r.center[0]+r.position[v][0]/z||p.y[v]!=r.center[1]-r.position[v][1]/z){
                            std::fprintf(stderr,"Mesh provenance did not survive projection and sorting\n");
                            return 1;
                        }
                    }
                    ++mesh_projected;
                }
            }
            gpu3d_frame frame;frame.w=496;frame.h=384;layers(frame);
            if(draw_mesh&&camera_pending)frame.mesh_scene=&camera_scene;
            if(queued_gpu){
                const auto packet_start=clock::now();
                if(packed_projection){
                    ++projected_frames;
                    full_projected_bytes+=projected.polygons.size()*sizeof(gpu3d_poly);
                    projected_bytes+=projected.polygons.size()*sizeof(gpu3d_projected_poly);
                    for(const auto &lane:projected.lanes)projected_bytes+=lane.size()*sizeof(gpu3d_projected_point);
                    if(!gles2_prepare_projected(*gpu,projected,frame,snapshot.rams())){
                        ++projected_fallbacks;
                        projected.expand(polygons);
                        packed_projection=false;
                    }
                }
                if(!packed_projection&&!gpu->prepare_frame_cpu(polygons.data(),polygons.size(),frame,snapshot.rams()))return 1;
                packet_prepare_ms+=milliseconds(packet_start);timing.mark(render_timing::packets);
                if(timing.enabled)timing.fence(2,gpu_queue.pending_status());
                if(!gpu_queue.drain()){std::fprintf(stderr,"Queued renderer drain failed\n");return 1;}
                timing.mark(render_timing::prior_gpu);
                if(canvas_ready&&!tile_gpu->upload()){std::fprintf(stderr,"Queued renderer upload failed\n");return 1;}
                timing.mark(render_timing::tile_upload);
            }
            const auto submit_start=clock::now();
            gpu->set_rams(snapshot.rams());
            const bool submitted=packed_projection?gles2_submit_projected(*gpu,projected,frame,0):
                gpu->submit(polygons.data(),polygons.size(),frame,0);
            if(!submitted){std::fprintf(stderr,"Snapshot renderer submission failed\n");return 1;}
            submit_ms+=milliseconds(submit_start);timing.mark(render_timing::submit);
            if(timing.serialized)glFinish();
            timing.mark(render_timing::gpu_3d);
            if(canvas_ready)canvas_drawn=true;
        }
        if(separate&&canvas_ready&&!canvas_drawn){
            if(queued_gpu&&(!gpu_queue.drain()||!tile_gpu->upload()))return 1;
            next_simulation();uintptr_t target=reinterpret_cast<uintptr_t>(gpu->target_handle(0));
            if(!tile_gpu->paint(0,target,true,true)||!tile_gpu->paint(1,target,true,false)||!tile_gpu->paint(2,target,true,false)){std::fprintf(stderr,"Tile canvas failed\n");return 1;}
            canvas_drawn=true;
        }
        mark(1);
        if(separate){
            next_simulation();
            gpu->settle();mark(2);display.begin(egl.width,egl.height,rotate);
            constexpr int pad=model2a::picture_parts::PAD;
            if(canvas_drawn)display.image(GLuint(reinterpret_cast<uintptr_t>(gpu->target_handle(0))),0,0,496,384,false,false,true);
            else {
            if(parts.gpu2d){
                display.image(GLuint(tile_gpu->target_handle(0)),0,0,496,384,false,true);
                display.image(GLuint(tile_gpu->target_handle(1)),0,0,496,384,false,true);
            }
            if(parts.backdrop)display.image(display.upload(0,parts.backdrop,parts.w+2*pad,384+2*pad),-parts.bdx-pad,-parts.bdy-pad,parts.w+2*pad,384+2*pad,true);
            if(parts.mid)display.image(display.upload(1,parts.mid,parts.w,384),0,0,496,384,true);
            if(parts.gpu3d&&parts.has3d)display.image(GLuint(reinterpret_cast<uintptr_t>(gpu->target_handle(0))),0,0,496,384,false);
            if(parts.top)display.image(display.upload(2,parts.top,parts.w,384),0,0,496,384,true);
            if(parts.gpu2d)display.image(GLuint(tile_gpu->target_handle(2)),0,0,496,384,false,true);
            }
        }else{
            if(camera_packets){std::fprintf(stderr,"Camera packet composition unavailable\n");return 1;}
            if(queued_gpu&&!gpu_queue.drain())return 1;
            const auto &picture=machine.render();display.begin(egl.width,egl.height,rotate);display.image(display.upload(3,picture.data(),picture.width(),picture.height()),0,0,496,384,true);
            next_simulation();
        }
        display.end();
        // The previous upload and all tile paint calls have consumed their CPU data.
        if(early_tiles)tile_renderer_idle.store(true,std::memory_order_release);
        timing.mark(render_timing::composite);
        if(timing.serialized)glFinish();
        timing.mark(render_timing::gpu_composite);
        if(queued_gpu){if(!gpu_queue.end_frame())return 1;}
        else if(std::getenv("VF2_SYNC_GPU"))glFinish();
        timing.mark(render_timing::flush);
        if(capture_tick){
            capture_pixels.resize(size_t(egl.width)*egl.height);std::vector<uint8_t> bytes(capture_pixels.size()*4);
            glReadPixels(0,0,egl.width,egl.height,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());
            for(int y=0;y<egl.height;y++)for(int x=0;x<egl.width;x++){auto p=&bytes[(size_t(egl.height-1-y)*egl.width+x)*4];capture_pixels[size_t(y)*egl.width+x]=0xff000000u|(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];}
        }
        mark(3);
        if(!present_at_tick){
            if(frame_trace_path)swap_start_ns=since(clock::now(),began);
            if(!egl.swap()){std::fprintf(stderr,"EGL swap failed: %x\n",eglGetError());return 1;}
            ++presented;
            mark(4);swapped=phase;timing.mark(render_timing::swap);
        }
        }else if(skip_draw)next_simulation();
        if(!pipeline)inputs(f);
        int64_t deadline_ns=0;
        bool resynced=false;
        if(!uncapped&&!headless){
            next_tick+=tick_period;
            if(frame_trace_path)deadline_ns=since(next_tick,began);
            if(pace_spin_ns>0){
                std::this_thread::sleep_until(next_tick-std::chrono::nanoseconds(pace_spin_ns));
                while(clock::now()<next_tick){}
            }else std::this_thread::sleep_until(next_tick);
            const auto now=clock::now();            if(frame_trace_path)sleep_end_ns=since(now,began);
            if(now-next_tick>std::chrono::milliseconds(100)){next_tick=now;resynced=true;}
        }
        if(present_at_tick&&!headless&&!simulation_only&&!skip_draw){
            // The sleep is charged to the pace stage by the mark(5) below.
            if(frame_trace_path)swap_start_ns=since(clock::now(),began);
            if(!egl.swap()){std::fprintf(stderr,"EGL swap failed: %x\n",eglGetError());return 1;}
            ++presented;swapped=clock::now();timing.mark(render_timing::swap);
        }
        if(frame_trace_path)frame_trace.push_back({wait_ns,simulation_ns,swapped==clock::time_point{}?0:since(swapped,began),deadline_ns,swap_start_ns,sleep_end_ns,resynced});
        mark(5);timing.mark(render_timing::input);
        if(f%report_every==report_every-1){
            auto now=clock::now();rusage usage{};getrusage(RUSAGE_SELF,&usage);
            double elapsed=std::chrono::duration<double>(now-began).count(),window=std::chrono::duration<double>(now-window_began).count();
            std::fprintf(stderr,"Frame %llu; %.2f fps average, %.2f recent; peak RSS %ld KiB; ms/frame CPU %.2f render %.2f sync %.2f composite %.2f swap %.2f audio/input/pace %.2f\n",(unsigned long long)(f+1),(f+1)/elapsed,report_every/window,usage.ru_maxrss,phase_ms[0]/report_every,phase_ms[1]/report_every,phase_ms[2]/report_every,phase_ms[3]/report_every,phase_ms[4]/report_every,phase_ms[5]/report_every);
            std::fill(phase_ms,phase_ms+6,0);window_began=now;
            cpu_samples.dump(f+1);
        }
    }
    if(simulation_pending){simulation_job.wait();simulation_pending=false;simulation_task_ms+=simulation_job_ms;++simulation_tasks;}
    if(queued_gpu&&!gpu_queue.drain())return 1;
    auto measured_end=clock::now();cpu_samples.stop(f);
    if(frame_trace_path){
        FILE *trace=std::fopen(frame_trace_path,"w");
        if(!trace){std::perror("frame trace");return 1;}
        std::fputs("frame,sim_wait_ns,sim_task_ns,swap_end_ns,pace_deadline_ns,pace_resynced,swap_start_ns,sleep_end_ns\n",trace);
        for(size_t i=0;i<frame_trace.size();++i){
            const auto &row=frame_trace[i];
            std::fprintf(trace,"%zu,%lld,%lld,%lld,%lld,%d,%lld,%lld\n",i+1,(long long)row.wait_ns,
                (long long)row.simulation_ns,(long long)row.swap_ns,(long long)row.deadline_ns,row.resynced,
                (long long)row.swap_start_ns,(long long)row.sleep_end_ns);
        }
        if(std::ferror(trace)){std::fclose(trace);std::perror("frame trace write");return 1;}
        if(std::fclose(trace)){std::perror("frame trace close");return 1;}
    }
    if(!guest_counts.finish(machine)){std::perror("guest count output");return 1;}
    if(check_mesh_handoff)std::fprintf(stderr,"Mesh handoff: %llu frames, %llu polygons, %llu exact vertices, %llu fallback polygons, %llu unclipped render entries\n",
        (unsigned long long)mesh_frames,(unsigned long long)mesh_polygons,
        (unsigned long long)mesh_vertices,(unsigned long long)mesh_fallbacks,(unsigned long long)mesh_projected);
    if(queued_gpu)gpu_queue.report();
    if(use_projected_stream)
        std::fprintf(stderr,"Projected stream: %llu frames, %llu plan fallbacks, %llu packed bytes, %llu full bytes\n",
            (unsigned long long)projected_frames,(unsigned long long)projected_fallbacks,
            (unsigned long long)projected_bytes,(unsigned long long)full_projected_bytes);
    timing.report(f);
    if(camera_packets)std::fprintf(stderr,"Camera-space geometry preparation: %.2f ms/frame\n",camera_prepare_ms/(f?f:1));
    if(snapshot_render&&std::getenv("VF2_GLES_STATS"))std::fprintf(stderr,"Texture snapshot bytes copied: %llu\n",(unsigned long long)snapshot.copied_texture_bytes());
    if(snapshot_render&&std::getenv("VF2_GLES_STATS"))std::fprintf(stderr,"Render preparation ms/frame: tiles %.2f, scene capture %.2f, other board %.2f, CPU packets %.2f, GPU submit %.2f\n",tile_prepare_ms/(f?f:1),scene_capture_ms/(f?f:1),(parts_ms-tile_prepare_ms-scene_capture_ms)/(f?f:1),packet_prepare_ms/(f?f:1),submit_ms/(f?f:1));
    machine.video().report_geometry_jobs(stderr);
    if(share_camera_colors)std::fprintf(stderr,"Shared camera colors: %llu frames\n",(unsigned long long)shared_color_frames);
    machine.report_scheduler_stats(stderr);
    machine.report_cooperative_fifo(stderr);
    if(pipeline&&simulation_tasks)std::fprintf(stderr,"Pipeline simulation tasks: %llu, %.2f ms/task\n",(unsigned long long)simulation_tasks,simulation_task_ms/simulation_tasks);
    if(machine.video().direct_geometry_polygons())std::fprintf(stderr,"Native geometry polygon submissions: %llu\n",(unsigned long long)machine.video().direct_geometry_polygons());
    if(frames)std::fprintf(stderr,"State signature: game frame %llu, IP %08x, TGP %llu, work %016llx\n",(unsigned long long)machine.frame_number(),machine.cpu().ip(),(unsigned long long)machine.m_tgp_instructions,(unsigned long long)m2::state_hash(machine.workram().data(),machine.workram().size()));
    if(frames)std::fprintf(stderr,"TGP execution: %llu precompiled, %llu interpreted instructions\n",(unsigned long long)machine.tgp().compiled_instructions(),(unsigned long long)machine.tgp().interpreted_instructions());
    if(machine.modular_scheduler_frames())std::fprintf(stderr,"Modular scheduler frames: %llu\n",(unsigned long long)machine.modular_scheduler_frames());
    if(machine.tgp().native_instructions())std::fprintf(stderr,"Native DSP translation: %llu instructions, %llu regions, %llu helper calls\n",(unsigned long long)machine.tgp().native_instructions(),(unsigned long long)machine.tgp().native_entries(),(unsigned long long)machine.tgp().native_helpers());
    if(machine.tgp().native_translations())
        std::fprintf(stderr,"Native DSP translations: %llu (%llu resumable)\n",
                     (unsigned long long)machine.tgp().native_translations(),
                     (unsigned long long)machine.tgp().resumed_translations());
    if(machine.tgp().native_rotations())std::fprintf(stderr,"Native DSP rotations: %llu\n",(unsigned long long)machine.tgp().native_rotations());
    if(machine.tgp().resumed_rotations())std::fprintf(stderr,"Resumable DSP rotations: %llu\n",(unsigned long long)machine.tgp().resumed_rotations());
    if(machine.tgp().resumed_matrices())std::fprintf(stderr,"Resumable DSP matrix compositions: %llu\n",(unsigned long long)machine.tgp().resumed_matrices());
    if(machine.tgp().native_matrices())std::fprintf(stderr,"Native DSP matrix compositions: %llu\n",(unsigned long long)machine.tgp().native_matrices());
    if(frames)std::fprintf(stderr,"Polygon count register reads: %llu\n",(unsigned long long)machine.polygon_count_reads());
    if(frames&&machine.tgp().compiled_blocks_run())std::fprintf(stderr,"TGP blocks: %llu runs, %llu instructions, %.2f instructions/run\n",(unsigned long long)machine.tgp().compiled_blocks_run(),(unsigned long long)machine.tgp().block_instructions(),double(machine.tgp().block_instructions())/machine.tgp().compiled_blocks_run());
if(frames&&machine.sound_scsp_updates())std::fprintf(stderr,"Sound board: 68000 %.3f ms/frame (%llu cycles, %.0f/frame; real time is 196258), SCSP+convert %.3f ms/frame (%llu updates, %.3f ms/update), peak %d\n",
    double(machine.sound_68k_ns())/f/1e6,(unsigned long long)machine.sound_68k_cycles(),
    double(machine.sound_68k_cycles())/f,
    double(machine.sound_scsp_ns())/f/1e6,
    (unsigned long long)machine.sound_scsp_updates(),double(machine.sound_scsp_ns())/machine.sound_scsp_updates()/1e6,machine.sound_peak());
    if(frames){
        std::vector<uint8_t> state;machine.save_state(state);
        std::fprintf(stderr,"Core state hash: %016llx\n",(unsigned long long)m2::state_hash(state.data()+model2a::STATE_HEADER,state.size()-model2a::STATE_HEADER));
        std::vector<uint8_t> geometry;machine.video().save_geometry(geometry);
        std::fprintf(stderr,"Geometry state hash: %016llx\n",(unsigned long long)m2::state_hash(geometry.data(),geometry.size()));
    }
    if(!save_state_path.empty()){
        std::string error;if(!acm_checkpoint::save(machine,save_state_path,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        std::fprintf(stderr,"Checkpoint saved: %s (game frame %llu)\n",save_state_path.c_str(),(unsigned long long)machine.frame_number());
    }
    if(!capture_pixels.empty()){captured=write_png(capture.c_str(),capture_pixels.data(),egl.width,egl.height);if(!captured)std::fprintf(stderr,"Capture failed: %s\n",capture.c_str());}
    double elapsed=std::chrono::duration<double>(measured_end-began).count();rusage usage{};getrusage(RUSAGE_SELF,&usage);
    if(early_tiles)std::fprintf(stderr,"VBLANK tiles: %llu prepared, %llu staged early, %llu staged after join, %llu ownership misses, %llu declines, %llu ineligible\n",
        (unsigned long long)tile_descriptors,(unsigned long long)early_tile_frames,(unsigned long long)joined_tile_frames,
        (unsigned long long)tile_ownership_misses,(unsigned long long)tile_stage_declines,(unsigned long long)tile_ineligible);
    if(reduced_cadence)std::fprintf(stderr,"Draw cadence: %llu presented, %llu skipped; %.2f presented fps\n",
        (unsigned long long)presented,(unsigned long long)(f-presented),presented/elapsed);
    std::fprintf(stderr,"%llu frames, %.3fs, %.2f fps, peak RSS %ld KiB, capture %s\n",(unsigned long long)f,elapsed,f/elapsed,usage.ru_maxrss,captured?"saved":"none");
    return 0;
}
