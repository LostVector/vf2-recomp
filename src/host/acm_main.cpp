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
#include "../gpu/render_size.h"
#include "../machine/geometry_math.h"
#include "input_script.h"
#include "png.h"
#include <GLES2/gl2.h>
#include <alsa/asoundlib.h>
#include <linux/input.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <bit>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <mutex>
#include <cstdlib>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>

#ifdef MODEL2_ORIGINAL
#include <cerrno>
#endif

namespace {
volatile std::sig_atomic_t stopping=0;
void stop(int){stopping=1;}
#ifdef MODEL2_ORIGINAL
bool cabinet_nvram(model2a &board,const std::string &path,bool save)
{
    const std::string file=save?path+".tmp":path;
    FILE *stream=std::fopen(file.c_str(),save?"wb":"rb");
    if(!stream){
        if(!save&&errno==ENOENT)return true;
        std::perror(file.c_str());
        return false;
    }
    auto &backup=board.backup_ram();
    std::array<u8,128> eeprom{};
    bool ok;
    if(save){
        for(size_t i=0;i<64;++i){
            const u16 cell=board.eeprom_cells()[i];
            eeprom[2*i]=u8(cell>>8);
            eeprom[2*i+1]=u8(cell);
        }
        ok=std::fwrite(backup.data(),1,backup.size(),stream)==backup.size()&&
           std::fwrite(eeprom.data(),1,eeprom.size(),stream)==eeprom.size();
    }else{
        ok=std::fread(backup.data(),1,backup.size(),stream)==backup.size()&&
           std::fread(eeprom.data(),1,eeprom.size(),stream)==eeprom.size()&&
           std::fgetc(stream)==EOF&&!std::ferror(stream);
        if(ok)for(size_t i=0;i<64;++i)board.eeprom_cells()[i]=(u16(eeprom[2*i])<<8)|eeprom[2*i+1];
    }
    if(std::fclose(stream)!=0)ok=false;
    if(save&&ok)ok=std::rename(file.c_str(),path.c_str())==0;
    if(!ok){
        std::fprintf(stderr,"Could not %s cabinet NVRAM %s\n",save?"save":"read",path.c_str());
        if(save)std::remove(file.c_str());
    }
    return ok;
}
#endif
GLuint compile(GLenum type,const char *source){
    GLuint s=glCreateShader(type);glShaderSource(s,1,&source,nullptr);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof(log),nullptr,log);std::fprintf(stderr,"Compositor shader: %s\n",log);glDeleteShader(s);return 0;}return s;
}
class compositor {
    GLuint program=0,vbo=0,textures[4]={},canvas_vbo=0;
#ifdef MODEL2_ORIGINAL
    const bool static_canvas_enabled=std::getenv("DAYTONA_STATIC_COMPOSITE")!=nullptr;
#else
    const bool static_canvas_enabled=std::getenv("VF2_STATIC_COMPOSITE")!=nullptr;
#endif
    bool static_canvas_active=false;
    uint64_t static_canvas_frames=0;
    static constexpr float canvas_vertices[]={-1.f,1.f,0.f,1.f, 1.f,1.f,1.f,1.f,
                                               -1.f,-1.f,0.f,0.f, 1.f,-1.f,1.f,0.f};
    GLint swizzle=-1,rotation=-1;
    // The composed picture is drawn across the 640x480 panel viewport while the
    // source art is 496x384, a 1.29x step. Nearest-neighbour magnification
    // duplicates whole pixels unevenly; linear magnification is the display
    // default. VF2_NEAREST_UPSCALE=1 restores nearest (for pixel comparisons).
    const bool linear_upscale=std::getenv("VF2_NEAREST_UPSCALE")==nullptr;
    int sizes[4][2]={};
public:
    ~compositor(){glDeleteTextures(4,textures);glDeleteBuffers(1,&vbo);if(canvas_vbo)glDeleteBuffers(1,&canvas_vbo);glDeleteProgram(program);}
    bool init(){
        const char *vs="attribute vec2 aPosition;attribute vec2 aUV;uniform vec2 uRotation;varying mediump vec2 uv;void main(){vec2 p=vec2(aPosition.x*uRotation.x-aPosition.y*uRotation.y,aPosition.x*uRotation.y+aPosition.y*uRotation.x);gl_Position=vec4(p,0.,1.);uv=aUV;}";
        const char *fs="precision mediump float;varying mediump vec2 uv;uniform sampler2D uImage;uniform float uSwizzle;void main(){vec4 c=texture2D(uImage,uv);gl_FragColor=uSwizzle>.5?c.bgra:c;}";
        GLuint a=compile(GL_VERTEX_SHADER,vs),b=compile(GL_FRAGMENT_SHADER,fs);if(!a||!b)return false;
        program=glCreateProgram();glAttachShader(program,a);glAttachShader(program,b);glBindAttribLocation(program,0,"aPosition");glBindAttribLocation(program,1,"aUV");glLinkProgram(program);glDeleteShader(a);glDeleteShader(b);
        GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)return false;
        swizzle=glGetUniformLocation(program,"uSwizzle");rotation=glGetUniformLocation(program,"uRotation");
        glGenBuffers(1,&vbo);glGenTextures(4,textures);
        if(static_canvas_enabled){
            glGenBuffers(1,&canvas_vbo);
            if(!canvas_vbo)return false;
            glBindBuffer(GL_ARRAY_BUFFER,canvas_vbo);
            glBufferData(GL_ARRAY_BUFFER,sizeof(canvas_vertices),canvas_vertices,GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER,0);
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program,"uImage"),0);
        }
        return true;
    }
    void begin(int w,int h,int angle,bool single_canvas=false){
        static_canvas_active=static_canvas_enabled&&single_canvas;
        glBindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);glViewport(0,0,w,h);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        float aspect=(angle==90||angle==270)?.75f:4.f/3.f;
        int vw=w,vh=int(w/aspect);if(vh>h){vh=h;vw=int(h*aspect);}
        int x=(w-vw)/2,y=(h-vh)/2;glViewport(x,y,vw,vh);glScissor(x,y,vw,vh);glEnable(GL_SCISSOR_TEST);
        glUseProgram(program);
        if(!static_canvas_enabled)glUniform1i(glGetUniformLocation(program,"uImage"),0);
        float c=angle==0?1.f:angle==180?-1.f:0.f,s=angle==90?1.f:angle==270?-1.f:0.f;glUniform2f(rotation,c,s);
        glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        glBindBuffer(GL_ARRAY_BUFFER,static_canvas_active?canvas_vbo:vbo);glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
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
    void canvas(GLuint texture,bool opaque){
        // The composed target is a full-screen picture: its size is the render
        // target's, not the 496x384 arcade canvas image() maps to. Drawing it
        // across the whole viewport makes a 640x480 target 1:1 on the panel
        // (the viewport is the 640x480 picture area, rotated).
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);glUniform1f(swizzle,0.f);
        if(opaque){
            if(linear_upscale)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glDisable(GL_BLEND);
        }
        if(static_canvas_active)++static_canvas_frames;
        else glBufferData(GL_ARRAY_BUFFER,sizeof(canvas_vertices),canvas_vertices,GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        if(opaque){
            glEnable(GL_BLEND);
            if(linear_upscale)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        }
    }

    void report_static_canvas()const{
        if(static_canvas_enabled)std::fprintf(stderr,"Static compositor canvas: %llu\n",(unsigned long long)static_canvas_frames);
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
#ifdef MODEL2_ORIGINAL
        set(0,0x04,active(KEY_F3));set(0,0x08,active(KEY_F4));
        set(0,0x20,active(KEY_C)||active(KEY_Z));set(0,0x40,active(KEY_D));
        set(0,0x80,active(KEY_E));set(1,1,active(KEY_F));
        set(2,1,active(KEY_LEFT));set(2,2,active(KEY_RIGHT));
        set(2,4,active(KEY_A)||active(KEY_X)||active(KEY_UP));
        set(2,8,active(KEY_B)||active(KEY_Y)||active(KEY_DOWN));
        for(int gear=0;gear<=4;++gear)set(3,1<<gear,active(gear?KEY_1+gear-1:KEY_0));
#else
        set(1,4,active(KEY_A)||active(KEY_X)); // Guard
        set(1,1,active(KEY_B)||active(KEY_Y)); // Punch
        set(1,2,active(KEY_C)||active(KEY_Z)); // Kick
        set(1,0x10,active(KEY_DOWN));set(1,0x20,active(KEY_UP));set(1,0x40,active(KEY_RIGHT));set(1,0x80,active(KEY_LEFT));
#endif
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
    // Detached mode: the sound worker owns the device and writes to it itself,
    // on its own core. A separate writer thread here lived on the render cores,
    // where a heavy combat frame starved it into dropped blocks.
    bool direct=false;
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
                if(n<0){
                    if(n==-EPIPE)++underruns;
                    if(snd_pcm_recover(pcm,n,1)<0){++errors;std::fprintf(stderr,"ALSA stopped: %s\n",snd_strerror(n));return;}
                    continue;
                }
                if(!n)break;at+=size_t(n);
            }
            ++played;played_samples+=at;submitted+=at;
        }
    }
public:
    // The device's own view, for the detached sound board's report.
    uint64_t submitted=0,underruns=0,errors=0;
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
                // The RAM kernel starts with this muted (or low), and the stock
                // gameapp that normally sets it is not running here. Raise it
                // to the top of the range (VF2_LINEOUT overrides) instead of
                // only lifting a hard mute, so output is not left quiet.
                const long lo=snd_ctl_elem_info_get_min(info),hi=snd_ctl_elem_info_get_max(info);
                long target=hi;
                if(const char *p=std::getenv("VF2_LINEOUT"))target=std::atol(p);
                target=std::clamp(target,lo,hi);
                unsigned count=snd_ctl_elem_info_get_count(info);
                bool raise=false;
                for(unsigned i=0;i<count;i++)raise|=snd_ctl_elem_value_get_integer(value,i)<target;
                if(raise){
                    for(unsigned i=0;i<count;i++)snd_ctl_elem_value_set_integer(value,i,target);
                    snd_ctl_elem_write(ctl,value);
                    std::fprintf(stderr,"ALSA lineout %ld of %ld..%ld\n",target,lo,hi);
                }
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
        unsigned buffer_us=100000;
#ifndef MODEL2_ORIGINAL
        if(direct) {
            if(const char *ms=std::getenv("VF2_SOUND_BUFFER_MS"))
                buffer_us=1000u*std::clamp(std::atoi(ms),80,220);
        }
#endif
        if(e>=0)e=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,2,44100,1,buffer_us);
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
        if(!direct)writer=std::thread([this]{drain();});
        return true;
    }
    void set_direct(bool d){direct=d;}

#ifndef MODEL2_ORIGINAL
    // Keep a PCM reserve so a late video frame cannot starve ALSA
    // while the board synthesizes the next block at the native sample rate.
    std::chrono::microseconds time_until_refill()
    {
        snd_pcm_sframes_t delay=0;
        if(snd_pcm_delay(pcm,&delay)<0)return {};
        static const snd_pcm_sframes_t reserve=[] {
            const char *value=std::getenv("VF2_SOUND_RESERVE_MS");
            const int ms=value?std::clamp(std::atoi(value),30,120):40;
            return snd_pcm_sframes_t(ms*441/10);
        }();
        if(delay<=reserve)return {};
        return std::chrono::microseconds((delay-reserve)*1000000ll/44100);
    }
#endif
    // A synchronous write from the caller's thread: in detached mode that is
    // the sound worker, which owns the core the device is serviced on.
    void write_direct(const std::vector<int16_t> &block)
    {
        if(!pcm||block.empty())return;
        size_t at=0,frames=block.size()/2;
        while(at<frames){
            snd_pcm_sframes_t n=snd_pcm_writei(pcm,block.data()+at*2,frames-at);
            if(n<0){
                if(n==-EPIPE)++underruns;
                if(snd_pcm_recover(pcm,n,1)<0){++errors;std::fprintf(stderr,"ALSA stopped: %s\n",snd_strerror(n));return;}
                continue;
            }
            if(!n)break;at+=size_t(n);
        }
        ++played;++enqueued;played_samples+=at;submitted+=at;
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

class detached_sound {
public:
    struct observation {
        uint64_t generated, submitted, underruns, errors, midi_bytes, queued;
        int64_t simulation_ns, write_ns;
    };

    detached_sound(romset::result roms, bool audible, bool trace, uint64_t frames)
        : machine(std::move(roms), false), audible(audible), trace(trace)
    {
        machine.set_sound(true);
        machine.set_sound_only(true);
        machine.set_byte_link(true);
        machine.set_draw_3d(false);
        if (std::getenv("VF2_SOUND_SCSP_STARVE")) machine.set_starve_scsp(true);
        machine.reset();
        cycles_at_start = machine.sound_68k_cycles();
        // Cost probe: which 68000 PCs the driver spends its frames on. A tight
        // poll loop here is the only lever left on a 12.8 ms/tick interpreter.
        if (std::getenv("VF2_68K_PCS")) {
            pc_hits.assign(65536, 0);
            machine.on_68k_instruction = [this](u32 pc) { ++pc_hits[pc & 0xffff]; ++pc_total; };
        }
        if (trace) records.reserve(frames);
    }

    ~detached_sound() { finish(false); }

    bool start()
    {
        if (audible) {
            speaker.set_direct(true);
            if (!speaker.open()) return false;
        }
        worker = std::thread([this] {
            // The board is a whole-core job (synth 14.2 ms of a 17.4 ms tick).
            // Give it a core of its own when asked, so render threads cannot
            // preempt it into an underrun, and optionally a real-time priority.
            if (const char *cpu = std::getenv("VF2_SOUND_CPU")) {
                cpu_set_t set;
                CPU_ZERO(&set);
                CPU_SET(std::atoi(cpu), &set);
                if (sched_setaffinity(0, sizeof(set), &set)) std::perror("sound CPU affinity");
            }
            if (const char *prio = std::getenv("VF2_SOUND_PRIO")) {
                sched_param param{};
                param.sched_priority = std::atoi(prio);
                if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param)) std::perror("sound priority");
            }
            process();
        });
        return true;
    }

    bool submit(const std::vector<model2a::timed_midi_byte> &events)
    {
        {
            std::unique_lock lock(mutex);
            if (pcm_proof) room.wait(lock, [&] { return failed || closing || queue.size() < maximum_queue; });
            if (failed || closing) return false;
            // A playable run must not end because the worker fell behind for a
            // moment. The queue is bounded; on a stall drop the oldest tick
            // (its audio is already late) and keep going, the way the ALSA
            // writer drops a block instead of stopping the device.
            if (queue.size() >= maximum_queue) {
                queue.pop_front();
                ++rejected;
            }
            queue.push_back({events, queue.size()});
        }
        ready.notify_one();
        return true;
    }

    bool finish(bool drain)
    {
        {
            std::lock_guard lock(mutex);
            closing = true;
            if (!drain) queue.clear();
        }
        ready.notify_one();
        if (worker.joinable()) worker.join();
        return !failed;
    }

    void report() const
    {
        std::fprintf(stderr, "Detached sound: %llu ticks, %llu MIDI bytes, %llu generated frames, %llu submitted frames, %llu underruns, %llu errors, %llu dropped ticks\n",
            (unsigned long long)processed, (unsigned long long)midi_bytes, (unsigned long long)generated,
            (unsigned long long)speaker.submitted, (unsigned long long)speaker.underruns,
            (unsigned long long)speaker.errors, (unsigned long long)rejected);
#ifndef MODEL2_ORIGINAL
        if (run_ahead) std::fprintf(stderr, "Detached sound refill: %llu extra ticks\n",
            (unsigned long long)refill_ticks);
#endif
        if(pcm_proof)std::fprintf(stderr,"Detached PCM proof: %016llx, %llu frames, %llu ticks\n",
            (unsigned long long)pcm_hash,(unsigned long long)generated,(unsigned long long)processed);
        if (records.empty()) return;
        int64_t synth_total = 0, write_total = 0, synth_max = 0, write_max = 0;
        size_t queue_max = 0;
        for (const observation &r : records) {
            synth_total += r.simulation_ns; write_total += r.write_ns;
            synth_max = std::max(synth_max, r.simulation_ns);
            write_max = std::max(write_max, r.write_ns);
            queue_max = std::max(queue_max, size_t(r.queued));
        }
        const double n = double(records.size());
        std::fprintf(stderr, "Detached sound worker: synth %.3f ms/tick (max %.3f), write %.3f ms/tick (max %.3f), queue max %zu of 16\n",
            synth_total / n / 1e6, synth_max / 1e6, write_total / n / 1e6, write_max / 1e6, queue_max);
        if (machine.sound_scsp_updates())
            std::fprintf(stderr, "Detached board: 68000 %.3f ms/tick, SCSP+convert %.3f ms/tick (%llu updates)\n",
                double(machine.sound_68k_ns()) / n / 1e6, double(machine.sound_scsp_ns()) / n / 1e6,
                (unsigned long long)machine.sound_scsp_updates());
        else
            std::fprintf(stderr, "Detached board: no sound-profile counters (run with --sound-profile)\n");
        if (!pc_hits.empty() && pc_total) {
            std::vector<uint32_t> order(65536);
            for (uint32_t i = 0; i < 65536; ++i) order[i] = i;
            std::partial_sort(order.begin(), order.begin() + 8, order.end(),
                [&](uint32_t a, uint32_t b) { return pc_hits[a] > pc_hits[b]; });
            std::fprintf(stderr, "68000 PCs: %llu instructions; top: ", (unsigned long long)pc_total);
            for (int k = 0; k < 8; ++k)
                std::fprintf(stderr, "%04x %.1f%% ", order[k], 100.0 * double(pc_hits[order[k]]) / double(pc_total));
            std::fprintf(stderr, "\n");
        }
        const uint64_t ran = machine.sound_68k_cycles() - cycles_at_start;
        constexpr double sound_cycles_per_tick = double(model2a::CYCLES_PER_FRAME) * model2a::SOUND_CPU_CLOCK / 25000000;
        std::fprintf(stderr, "Detached board: 68000 %.0f cycles/tick (real time is %.0f), %.1f ns/cycle\n",
            double(ran) / n, sound_cycles_per_tick, double(synth_total) / std::max<uint64_t>(ran, 1));
        if (machine.sound_68k_runs())
            std::fprintf(stderr, "Detached board: 68000 %llu dispatches/tick, %.0f cycles/dispatch\n",
                (unsigned long long)(double(machine.sound_68k_runs()) / n),
                double(ran) / double(std::max<uint64_t>(machine.sound_68k_runs(), 1)));
        if (throttle_mod)
            std::fprintf(stderr, "Detached sound starvation probe: 68000 skipped on %llu of %llu ticks (%zu of every %zu)\n",
                (unsigned long long)starved, (unsigned long long)processed, throttle_skip, throttle_mod);
    }

    std::vector<observation> records;

private:
    struct task {
        std::vector<model2a::timed_midi_byte> events;
        size_t queued;
    };

    void process()
    {
        using clock = std::chrono::steady_clock;
        // Starvation ladder for the headroom probe: "N/M" skips the 68000 on N
        // ticks of every M, while the SCSP keeps producing samples so the ALSA
        // stream stays continuous. "0/4" is the full board.
        if (const char *t = std::getenv("VF2_SOUND_THROTTLE")) {
            unsigned long n = 0, m = 0;
            if (std::sscanf(t, "%lu/%lu", &n, &m) == 2 && m && n <= m) {
                throttle_mod = m;
                throttle_skip = n;
            }
        }
        for (;;) {
            task next{};
            bool refill = false;
            {
                std::unique_lock lock(mutex);
                const auto has_work = [&] { return failed || closing || !queue.empty(); };
#ifndef MODEL2_ORIGINAL
                if (run_ahead && speaker.submitted && !speaker.errors && !has_work()) {
                    lock.unlock();
                    const auto wait = speaker.time_until_refill();
                    lock.lock();
                    refill = !ready.wait_for(lock, wait, has_work);
                }
#endif
                if (!refill) {
                    ready.wait(lock, has_work);
                    if (failed || queue.empty()) return;
                    next = std::move(queue.front());
                    queue.pop_front();
                }
            }
            if (pcm_proof) room.notify_one();
            const bool starve = throttle_mod && (processed % throttle_mod) < throttle_skip;
            if (starve) ++starved;
            machine.set_throttle_68k(starve);
            const auto begin = clock::now();
            if (!machine.run_sound_frame(next.events)) {
                std::lock_guard lock(mutex);
                failed = true;
                if (pcm_proof) room.notify_one();
                std::fprintf(stderr, "Detached sound rejected MIDI at tick %llu\n", (unsigned long long)processed+1);
                return;
            }
            const auto synthesized = clock::now();
            const uint64_t samples = machine.m_audio_out.size()/2;
            if (audible) speaker.write_direct(machine.m_audio_out);
            const auto written = clock::now();
            if(pcm_proof)for(int16_t value:machine.m_audio_out){
                const uint16_t sample=uint16_t(value);
                pcm_hash=(pcm_hash^(sample&0xff))*1099511628211ull;
                pcm_hash=(pcm_hash^(sample>>8))*1099511628211ull;
            }
            machine.m_audio_out.clear();
            ++processed;
            midi_bytes += next.events.size();
            generated += samples;
#ifndef MODEL2_ORIGINAL
            if (refill) ++refill_ticks;
#endif
            if (trace && !refill) {
                const auto ns = [](auto span) { return std::chrono::duration_cast<std::chrono::nanoseconds>(span).count(); };
                records.push_back({samples, speaker.submitted, speaker.underruns, speaker.errors,
                    next.events.size(), next.queued, ns(synthesized-begin), ns(written-synthesized)});
            }
        }
    }

    static constexpr size_t maximum_queue = 16;
    model2a machine;
    audio speaker;
    bool audible, trace;
    std::mutex mutex;
#ifndef MODEL2_ORIGINAL
    const bool run_ahead = audible && std::getenv("VF2_SOUND_REFILL");
    uint64_t refill_ticks = 0;
#endif
    std::condition_variable ready, room;
    const bool pcm_proof = !audible && std::getenv("VF2_SOUND_PCM_HASH");
    uint64_t pcm_hash = 14695981039346656037ull;
    std::deque<task> queue;
    bool failed = false, closing = false;
    uint64_t processed = 0, midi_bytes = 0, generated = 0, cycles_at_start = 0, pc_total = 0, rejected = 0, starved = 0;
    size_t throttle_skip = 0, throttle_mod = 0;
    std::vector<uint64_t> pc_hits;
    std::thread worker;
};
}
int main(int argc,char **argv){
    arm_cpu_samples cpu_samples;
    std::string rom,capture,scriptpath,load_state_path,save_state_path,detached_audio;bool headless=false,mute=false,probe=false,uncapped=false,release_rom=false,release_state=false,no_sound_board=false,simulation_only=false,graphics_only=false;uint64_t frames=0,report_every=300;int rotate=0;unsigned draw_count=1,draw_period=1;
    std::vector<unsigned long long> capture_frames;
#ifdef MODEL2_ORIGINAL
    std::string nvram_path;
    std::string coverage_path;
    std::string audio_capture_path;
    bool adaptive_draw=false;
#endif
    for(int i=1;i<argc;i++){
        std::string a=argv[i];auto next=[&](){if(i+1>=argc){std::fprintf(stderr,"Missing value after %s\n",a.c_str());std::exit(2);}return argv[++i];};
#ifdef MODEL2_ORIGINAL
        if(a=="--nvram"){nvram_path=next();continue;}
        if(a=="--i960-coverage"){coverage_path=next();continue;}
        if(a=="--audio-capture"){audio_capture_path=next();continue;}
        if(a=="--adaptive-draw"){adaptive_draw=true;continue;}
#endif
        if(a=="--headless")headless=true;else if(a=="--mute")mute=true;else if(a=="--probe")probe=true;else if(a=="--uncapped")uncapped=true;
        else if(a=="--detached-audio")detached_audio=next();
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
        else if(a=="--frames")frames=strtoull(next(),nullptr,10);else if(a=="--capture")capture=next();else if(a=="--capture-frames"){std::string v=next();size_t p=0;for(;;){size_t c=v.find(',',p);std::string tok=v.substr(p,c==std::string::npos?std::string::npos:c-p);if(!tok.empty())capture_frames.push_back(std::strtoull(tok.c_str(),nullptr,10));if(c==std::string::npos)break;p=c+1;}}else if(a=="--script")scriptpath=next();else if(a=="--rotate")rotate=atoi(next());
        else if(rom.empty()&&a[0]!='-')rom=a;else{std::fprintf(stderr,"Unknown argument %s\n",a.c_str());return 2;}
    }
#ifdef MODEL2_ORIGINAL
    if((rom.empty()&&!probe)||(rotate!=0&&rotate!=90&&rotate!=180&&rotate!=270)){
        std::fprintf(stderr,"usage: daytona-acm daytona.zip [--nvram FILE] [--frames N] [--capture FILE] [--script FILE] [--rotate 0|90|180|270] [--adaptive-draw] [--uncapped] [--headless]\n       daytona-acm --probe [--headless]\n");return 2;
    }
    if(!load_state_path.empty()&&!save_state_path.empty()){
        std::fprintf(stderr,"Choose one checkpoint direction\n");return 2;
    }
    if(!audio_capture_path.empty()&&(no_sound_board||!detached_audio.empty())){
        std::fprintf(stderr,"Audio capture requires the inline sound board\n");return 2;
    }
    if(!coverage_path.empty()&&std::getenv("VF2_GUEST_COUNTS")){
        std::fprintf(stderr,"Choose interpreter coverage or native guest counts\n");return 2;
    }
#else
    if((rom.empty()&&!probe)||(rotate!=0&&rotate!=90&&rotate!=180&&rotate!=270)){std::fprintf(stderr,"usage: vf2-acm vf2.zip [--frames N] [--capture FILE] [--script FILE] [--rotate 0|90|180|270] [--mute] [--uncapped] [--headless]\n       vf2-acm --probe [--headless]\n");return 2;}
#endif
    if(simulation_only&&graphics_only){std::fprintf(stderr,"Choose one isolation mode\n");return 2;}
    const bool reduced_cadence=draw_count<draw_period;
#ifdef MODEL2_ORIGINAL
    const bool can_adapt=!uncapped&&!headless&&!reduced_cadence&&!graphics_only&&!simulation_only&&!probe;
    if(adaptive_draw&&!can_adapt){
        std::fprintf(stderr,"--adaptive-draw requires paced gameplay without a fixed draw cadence\n");return 2;
    }
    adaptive_draw=adaptive_draw||(can_adapt&&std::getenv("DAYTONA_ADAPTIVE_DRAW"));
    if(adaptive_draw)std::fprintf(stderr,"Adaptive drawing enabled\n");
#endif
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
    const bool canvas=(std::getenv("VF2_GPU_CANVAS")||vf2_render_target_size().w!=496||
        vf2_render_target_size().h!=384)&&!simulation_only;
    if(pipeline&&!no_sound_board&&!audio_experiment){std::fprintf(stderr,"Experimental pipeline requires --no-sound-board\n");return 2;}
    if(!detached_audio.empty()&&(!pipeline||!snapshot_render||!no_sound_board||headless||
            !save_state_path.empty()||
            (detached_audio!="muted"&&detached_audio!="speaker"))){
        std::fprintf(stderr,"Detached audio requires snapshot pipelining with the main sound board held\n");
        return 2;
    }
    // A checkpoint carries no sound state, so the worker's board starts from
    // power-on while the loaded game is mid-fight: the sound phase is not
    // continuous with the game. Timing comparisons are still valid (that is
    // what the qualification runs measure); waveform identity against an
    // integrated board is not.
    if(!detached_audio.empty()&&!load_state_path.empty())
        std::fprintf(stderr,"Detached audio: worker board starts from power-on; the checkpoint carries no sound state\n");
    std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
    vf2_egl_context egl;if(!egl.open(headless)){std::fprintf(stderr,"EGL initialization failed: %x\n",eglGetError());return 1;}
    const bool queued_gpu=std::getenv("VF2_GPU_QUEUE")&&!simulation_only;
    const int simulation_core=std::getenv("VF2_SIM_CPU")?std::atoi(std::getenv("VF2_SIM_CPU")):-1;
    // Pinning the simulation to a core the render threads cannot use isolates it
    // (sim p95 17.18 vs 17.65) but costs the render pipeline a core, which is
    // worse for the frame interval. VF2_SIM_CPU_SHARE keeps the core in the
    // render set and relies on VF2_SIM_PRIO instead: the simulation runs FIFO
    // and always wins on its core, while the render threads use whatever the
    // simulation leaves idle.
    const bool simulation_share=std::getenv("VF2_SIM_CPU_SHARE")!=nullptr;
    const int simulation_prio=std::getenv("VF2_SIM_PRIO")?std::atoi(std::getenv("VF2_SIM_PRIO")):-1;
    if(simulation_core>=0){
        cpu_set_t render_set;
        if(!pipeline||simulation_core>=CPU_SETSIZE||sched_getaffinity(0,sizeof(render_set),&render_set)||!CPU_ISSET(simulation_core,&render_set))return 2;
        if(!simulation_share)CPU_CLR(simulation_core,&render_set);
        if(!CPU_COUNT(&render_set)||sched_setaffinity(0,sizeof(render_set),&render_set)){std::perror("render CPU affinity");return 1;}
    }
    if(!detached_audio.empty()&&std::getenv("VF2_SOUND_CPU")){
        // The board is a whole-core job: give it the core outright. The main
        // process (simulation, render threads, ALSA writer) stays off it so a
        // heavy combat frame cannot preempt the 68000 into a late tick.
        // VF2_SOUND_CPU_SHARE=1 skips that exclusion: the worker keeps the core
        // pinned at FIFO priority, and the main process may use whatever the
        // worker leaves idle (notably the blocking ALSA write).
        const bool share=std::getenv("VF2_SOUND_CPU_SHARE")!=nullptr;
        cpu_set_t main_set;
        const int sound_core=std::atoi(std::getenv("VF2_SOUND_CPU"));
        if(!sched_getaffinity(0,sizeof(main_set),&main_set)){
            if(!share)CPU_CLR(sound_core,&main_set);
            if(!CPU_COUNT(&main_set)||sched_setaffinity(0,sizeof(main_set),&main_set))std::perror("main CPU affinity");
            std::fprintf(stderr,"Main process cores: %d%s\n",CPU_COUNT(&main_set),share?" (shared with sound)":" (sound core excluded)");
        }
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
    if(!gpu->make_targets(vf2_render_target_size().w,vf2_render_target_size().h,1))return 1;
    std::unique_ptr<gpu2d> tile_gpu;
    if(std::getenv("VF2_GPU2D")||canvas){tile_gpu=gpu2d_create_gles2();if(!tile_gpu){std::fprintf(stderr,"GPU tile renderer initialization failed\n");return 1;}}
    compositor display;if(!display.init())return 1;
#ifdef MODEL2_ORIGINAL
    auto roms=romset::load_daytona(rom);
#else
    auto roms=romset::load(rom);
#endif
    if(!roms.ok){std::fprintf(stderr,"%s\n",roms.error.c_str());return 1;}
    if(release_rom){
        // The uploaded ZIP occupies unreclaimable initramfs memory in addition
        // to the decoded regions. Release only an explicitly temporary copy.
        if((rom.rfind("/run/",0)!=0&&rom.rfind("/tmp/",0)!=0)||rom.find("..")!=std::string::npos){std::fprintf(stderr,"--release-rom requires a /run/ or /tmp/ path\n");return 2;}
        if(unlink(rom.c_str())){std::perror("release RAM ROM");return 1;}
        std::fprintf(stderr,"Released temporary ROM archive after decoding\n");
    }
    const char *frame_trace_path=std::getenv("VF2_FRAME_TRACE");
    const bool trace_sim_waits=frame_trace_path&&std::getenv("VF2_TRACE_SIM_WAITS");
    std::unique_ptr<detached_sound> detached;
#ifdef MODEL2_ORIGINAL
    model2a machine(std::move(roms),coverage_path.empty());
    machine.configure_standalone_cabinet();
    if(!nvram_path.empty()&&!cabinet_nvram(machine,nvram_path,false))return 1;
    if(!std::getenv("DAYTONA_RAW_COLORS")&&machine.configure_twin_display_curve())
        std::fprintf(stderr,"Daytona Twin CRT color ramp correction enabled\n");
    std::set<std::pair<u32,u32>> coverage;
    u32 last_pc=0;
    if(!coverage_path.empty())machine.on_instruction=[&](u32 pc){
        if(last_pc!=pc&&last_pc+4!=pc&&pc<i960rec::runtime::LIMIT){
            const auto op=i960rec::decode(last_pc,[&](u32 addr){return machine.peek32(addr);});
            if(op.k!=i960rec::kind::ret&&pc!=last_pc+u32(op.len)&&(!op.has_target||pc!=op.target))
                coverage.emplace(last_pc,pc);
        }
        last_pc=pc;
    };
#else
    const char *rom_dedup=std::getenv("VF2_ROM_DEDUP");
    model2a machine(std::move(roms),true,!rom_dedup||std::strcmp(rom_dedup,"0")!=0);
#endif
    if(!detached_audio.empty()){
        // The sound board is the only reader of these two regions (snd_bus and
        // sound_reset), and the main machine runs with its board held. Move
        // them out after the constructor has hashed the full set (checkpoint
        // headers validate that hash) rather than copying them: a second copy
        // is 8.5 MB, which on a 245 MB device is the difference between
        // running and the OOM killer.
        romset::result sound_roms=machine.take_sound_regions();
        detached=std::make_unique<detached_sound>(std::move(sound_roms),detached_audio=="speaker",frame_trace_path,frames);
        if(!detached->start())return 1;
    }
    machine.release_unused_roms();machine.set_sound(!no_sound_board);
    // Match the desktop frontend: schedule UART byte arrivals rather than a
    // million idle clock events per emulated second. The bit-level mode is
    // retained by vf2run for exact reference validation.
#ifdef MODEL2_ORIGINAL
    machine.set_byte_link(!std::getenv("DAYTONA_BIT_UART"));
#else
    machine.set_byte_link(true);
#endif
    machine.reset();machine.defer_3d(true);
    if(camera_packets&&!machine.video().camera_packets(true)){std::fprintf(stderr,"Camera packets require the normal arcade camera\n");return 2;}
    if(std::getenv("VF2_GEOMETRY_WORKER")&&!machine.video().geometry_jobs(geometry_execution::worker)){std::fprintf(stderr,"Geometry worker requires camera packets\n");return 2;}
    const bool sample_geometry=std::getenv("VF2_CPU_SAMPLES_GEOMETRY")!=nullptr;
    if(sample_geometry&&!machine.video().observe_geometry_worker([&]{
        if(!cpu_samples.start_thread())std::abort();
    })){
        std::fprintf(stderr,"Geometry profiling requires the geometry worker\n");return 2;
    }
#ifdef MODEL2_ORIGINAL
    if(coverage_path.empty()&&!machine.recompiled()){
#else
    if(!machine.recompiled()){
#endif
        std::fprintf(stderr,"Native i960 compilation failed\n");return 1;
    }
    if((!load_state_path.empty()||!save_state_path.empty())&&!no_sound_board&&!audio_experiment){std::fprintf(stderr,"Checkpoint tests require --no-sound-board\n");return 2;}
    if(!load_state_path.empty()){
        std::string error;if(!acm_checkpoint::load(machine,load_state_path,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        std::fprintf(stderr,"Sound board at load: 68000 %llu cycles\n",(unsigned long long)machine.sound_68k_cycles());
        if(std::getenv("VF2_CHECK_LOADHASH")){
            // Re-serialize the in-memory state: if this equals the checkpoint's
            // own core hash, the load landed every byte and any divergence is in
            // runtime state the format never carried.
            std::vector<uint8_t> st;machine.save_state(st);
            std::fprintf(stderr,"LOADHASH core %016llx work %016llx\n",
                (unsigned long long)m2::state_hash(st.data()+model2a::STATE_HEADER,st.size()-model2a::STATE_HEADER),
                (unsigned long long)m2::state_hash(machine.workram().data(),machine.workram().size()));
        }
        if(release_state){
            if((load_state_path.rfind("/run/",0)!=0&&load_state_path.rfind("/tmp/",0)!=0)||load_state_path.find("..")!=std::string::npos){std::fprintf(stderr,"--release-state requires a temporary path\n");return 2;}
            if(unlink(load_state_path.c_str())){std::perror("release checkpoint");return 1;}
        }
        // Frames run here before the render pipeline is attached are simulated but
        // not drawn, which lets deferred pictures and RAM-change handling differ
        // from a continuous run. Zero warm-up starts the loop on the frame after
        // the save so every frame goes through the normal drawn path.
        int warm_up=3;
        if(const char *v=std::getenv("VF2_CHECK_WARMUP"))warm_up=std::atoi(v);
        for(int i=0;i<warm_up;i++)machine.run_frame();
        std::fprintf(stderr,"Resumed checkpoint at game frame %llu (warm-up %d)\n",(unsigned long long)machine.frame_number(),warm_up);
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
        frame.underlay=[](void *p,bool clear_depth){
            auto *tiles=static_cast<gpu2d*>(p);
            return tiles->paint(0,0,true,true,clear_depth)&&tiles->paint(1,0,true,false);
        };
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
        gpu3d_frame frame;frame.w=vf2_render_target_size().w;frame.h=vf2_render_target_size().h;layers(frame);
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
#ifdef MODEL2_ORIGINAL
    const bool post_swap_fence=queued_gpu&&std::getenv("DAYTONA_POST_SWAP_FENCE");
    const bool pace_preflush=present_at_tick&&post_swap_fence&&std::getenv("DAYTONA_PACE_PREFLUSH");
    const bool move_sim_launch=std::getenv("DAYTONA_MOVE_ON_SIM_DISPATCH")!=nullptr;
    cpu_set_t original_launch_mask{}, launch_mask{};
    unsigned moved_sim_launches=0;
    if(move_sim_launch){
        if(simulation_core<0||!simulation_share||
           sched_getaffinity(0,sizeof(original_launch_mask),&original_launch_mask)||
           !CPU_ISSET(simulation_core,&original_launch_mask)||CPU_COUNT(&original_launch_mask)<2)
            return 2;
        launch_mask=original_launch_mask;
        CPU_CLR(simulation_core,&launch_mask);
    }
#else
    constexpr bool post_swap_fence=false;
#endif
    // Wake-up latency from sleep_until is the remaining cadence jitter once the
    // swap is on the grid (build/acm/present-times). Sleeping short of the
    // deadline and spinning the last margin makes the present start on the tick.
    const int64_t pace_spin_ns=[](){const char *p=std::getenv("VF2_PACE_SPIN_US");return p?int64_t(std::atoll(p))*1000:0;}();
    // The present itself costs ~240 us at the median and ~390 us at p99 (EGL
    // swap to submission complete), so waking on the tick always submits after
    // it. The lead aims the *submission* at the tick by starting the present
    // that much earlier; the cadence (next_tick) is unchanged. 500 us covers
    // the present p99 plus wake jitter, so a frame whose work fits inside
    // tick - 500 us (i.e. work p99 < 16.88 ms) submits on its tick. Set
    // VF2_PRESENT_LEAD_US=0 to restore wake-on-tick behaviour.
    const int64_t present_lead_ns=[](){const char *p=std::getenv("VF2_PRESENT_LEAD_US");return p?int64_t(std::atoll(p))*1000:500000;}();
    struct frame_observation {
        int64_t wait_ns,simulation_ns,swap_ns,deadline_ns,swap_start_ns,sleep_end_ns;
        bool resynced;
        double stage[render_timing::count];
        double stage_cpu[render_timing::count];
        int64_t simulation_pool_wait_ns,simulation_cpu_ns;
        uint64_t geometry_job_wait_ns;
    };
    std::vector<frame_observation> frame_trace;
    if(frame_trace_path)frame_trace.reserve(frames);
    // A loaded checkpoint already has the machine at its saved frame. Start the
    // host frame counter there so scripted inputs resume at the right tick
    // instead of replaying the coin/start/menu presses mid-race.
    auto began=clock::now();auto next_tick=began;uint64_t f=load_state_path.empty()?0:machine.frame_number(),presented=0;bool captured=false;
    auto since=[](auto end,auto start){return std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();};
    double phase_ms[6]={};auto window_began=began;std::vector<uint32_t> capture_pixels;
    double simulation_job_ms=0,simulation_task_ms=0;uint64_t simulation_tasks=0;
    std::vector<model2a::timed_midi_byte> midi;
    if(detached)midi.reserve(64);
    bool audio_queued=true;
#ifdef MODEL2_ORIGINAL
    std::unique_ptr<FILE,decltype(&std::fclose)> audio_capture(nullptr,&std::fclose);
    if(!audio_capture_path.empty()){
        audio_capture.reset(std::fopen(audio_capture_path.c_str(),"wb"));
        if(!audio_capture){std::perror(audio_capture_path.c_str());return 1;}
    }
    bool audio_capture_ok=true;
    int steering=0x80, accelerator=0x20, brake=0x20;
    if(!load_state_path.empty()){
        // The host filters the analog pedals and steering itself; the machine
        // now carries the filtered latch, so continue the filter from it rather
        // than from the defaults a fresh process starts with.
        steering=machine.m_driving_analog[0];
        accelerator=machine.m_driving_analog[1];
        brake=machine.m_driving_analog[2];
        // Old checkpoints (and experiments) can still override.
        if(const char *v=std::getenv("VF2_LOAD_STEER"))steering=std::atoi(v);
        if(const char *v=std::getenv("VF2_LOAD_ACCEL"))accelerator=std::atoi(v);
        if(const char *v=std::getenv("VF2_LOAD_BRAKE"))brake=std::atoi(v);
        machine.m_driving_analog={uint8_t(steering),uint8_t(accelerator),uint8_t(brake)};
    }
    std::chrono::nanoseconds draw_estimate{};
    bool drew_previous=false;
#endif
    int64_t simulation_pool_wait_ns=0;
    int64_t simulation_cpu_ns=0;
    uint64_t geometry_job_wait_ns=0;
    const auto *trace_video=trace_sim_waits?&machine.video():nullptr;
    const auto thread_cpu_ns=[]() -> int64_t {
        timespec now{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID,&now);
        return int64_t(now.tv_sec)*1000000000+now.tv_nsec;
    };
    auto simulate=[&] {
        simulation_pool_wait_ns=0;
        row_pool::lock_wait_sample pool_wait(trace_sim_waits?&simulation_pool_wait_ns:nullptr);
        const auto geometry_before=trace_video?trace_video->geometry_job_wait_ns():0;
        const auto cpu_before=trace_sim_waits?thread_cpu_ns():0;
        if(detached){
            machine.run_frame_capturing_midi(midi);
            audio_queued=detached->submit(midi);
        }else machine.run_frame();
        if(trace_video){
            simulation_cpu_ns=thread_cpu_ns()-cpu_before;
            geometry_job_wait_ns=trace_video->geometry_job_wait_ns()-geometry_before;
        }
    };
    background_job simulation_job;bool simulation_pending=false;
    auto inputs=[&](uint64_t frame){
#ifdef MODEL2_ORIGINAL
        const auto &samples=machine.m_audio_out;
        if(audio_capture&&std::fwrite(samples.data(),sizeof(int16_t),samples.size(),audio_capture.get())!=samples.size())
            audio_capture_ok=false;
#endif
        if(!mute&&!headless&&!detached)sound.write(machine.m_audio_out);machine.m_audio_out.clear();
        uint8_t held[4],scripted[4];input.poll(held);script.apply(frame,scripted);
        machine.m_in0=held[0]&scripted[0];machine.m_in1=held[1]&scripted[1];machine.m_in2=held[2]&scripted[2];machine.m_sw=held[3]&scripted[3];
#ifdef MODEL2_ORIGINAL
        const auto pedal=[&](int value,unsigned bit){
            return std::clamp(value+((machine.m_in2&bit)?-24:18),0x20,0xe0);
        };
        const int direction=bool(machine.m_in2&1)-bool(machine.m_in2&2);
        if(direction)steering=std::clamp(steering+direction*10,0x20,0xe0);
        else steering+=std::clamp(0x80-steering,-16,16);
        accelerator=pedal(accelerator,4);
        brake=pedal(brake,8);
        machine.m_driving_analog={uint8_t(steering),uint8_t(accelerator),uint8_t(brake)};
        for(unsigned gear=0;gear<=4;++gear)if(!(machine.m_sw&(1<<gear)))machine.m_driving_gear=gear;
#endif
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
            else simulate();
            if(!audio_queued)return 1;
        }
        mark(0);timing.mark(render_timing::simulation);
        if(std::getenv("VF2_CHECK_TRACE")){
            // Before anything is drawn: what the simulation alone made of the RAM.
            const auto &w=machine.workram();
            std::fprintf(stderr,"CHECKPRE tick %llu game %llu ip %08x tgppc %04x",
                (unsigned long long)(f+1),(unsigned long long)machine.frame_number(),machine.cpu().ip(),machine.tgp().pc());
#ifdef MODEL2_ORIGINAL
            std::fprintf(stderr," gear %u ana %u,%u,%u",unsigned(machine.m_driving_gear),
                unsigned(machine.m_driving_analog[0]),unsigned(machine.m_driving_analog[1]),unsigned(machine.m_driving_analog[2]));
#endif
            std::fprintf(stderr," work %016llx\n",
                (unsigned long long)m2::state_hash(w.data(),w.size()));
            if(std::getenv("VF2_CHECK_REGIONS")){
                std::fprintf(stderr,"REG");
                const size_t n=16,per=(w.size()+n-1)/n;
                for(size_t i=0;i<n;i++){
                    const size_t a=i*per,b=std::min(w.size(),a+per);
                    std::fprintf(stderr," %016llx",(unsigned long long)m2::state_hash(w.data()+a,b-a));
                }
                std::fputc('\n',stderr);
            }
        }
        const auto wait_ns=since(phase,frame_start);
        const auto simulation_ns=waited_for_worker?int64_t(simulation_job_ms*1000000):wait_ns;
        const auto frame_pool_wait_ns=simulation_pool_wait_ns;
        const auto frame_simulation_cpu_ns=simulation_cpu_ns;
        const auto frame_geometry_wait_ns=geometry_job_wait_ns;
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
#ifdef MODEL2_ORIGINAL
                // Publishing a FIFO worker on the caller's core can preempt
                // the caller before it continues to camera projection.
                const bool move_launch=move_sim_launch&&sched_getcpu()==simulation_core;
                if(move_launch){
                    if(sched_setaffinity(0,sizeof(launch_mask),&launch_mask)){
                        std::perror("simulation launch affinity");
                        std::abort();
                    }
                    ++moved_sim_launches;
                }
#endif
                simulation_job.start([&]{
                    static thread_local bool pinned=false;
                    if(simulation_core>=0&&!pinned){
                        cpu_set_t set;CPU_ZERO(&set);CPU_SET(simulation_core,&set);
                        if(sched_setaffinity(0,sizeof(set),&set)){std::perror("simulation CPU affinity");std::abort();}
                        pinned=true;
                    }
                    static thread_local bool prioritized=false;
                    if(simulation_prio>=0&&!prioritized){
                        // The real-time simulation thread outranks the render
                        // threads that share its core; it still blocks between
                        // frames, so they run in the gap.
                        sched_param param{};param.sched_priority=simulation_prio;
                        if(pthread_setschedparam(pthread_self(),SCHED_FIFO,&param))std::perror("simulation priority");
                        prioritized=true;
                    }
                    if(!sample_geometry&&!cpu_samples.start_thread())std::abort();
                    auto start=clock::now();simulate();simulation_job_ms=std::chrono::duration<double,std::milli>(clock::now()-start).count();
                });
#ifdef MODEL2_ORIGINAL
                if(move_launch&&sched_setaffinity(0,sizeof(original_launch_mask),&original_launch_mask)){
                    std::perror("simulation launch affinity restore");
                    std::abort();
                }
#endif
                simulation_pending=true;
            }
        };
        // The board and inputs run on each tick. The last tick of a finite
        // capture is always drawn so its PNG corresponds to the state hashes.
        const bool capture_tick=!capture.empty()&&frames&&f+1==frames;
        // Reversible diagnostic: --capture-frames N,M,... forces a draw and writes
        // capture_path.NNNNNN.png at each listed frame, for exact-pixel A/B at
        // specific busy frames. Only active when the flag is passed.
        bool fence_after_swap=false;
        const bool frame_capture=!capture.empty()&&!capture_frames.empty()&&
            std::find(capture_frames.begin(),capture_frames.end(),(unsigned long long)(f+1))!=capture_frames.end();
#ifdef MODEL2_ORIGINAL
        const auto draw_start=phase;
        // Give a late draw one tick to catch up. The next tick always gets
        // a picture after a skip, and every tick still takes inputs.
        const bool skip_for_deadline=adaptive_draw&&drew_previous&&
            draw_start+draw_estimate+std::chrono::microseconds(500)>next_tick+tick_period;
        const bool skip_draw=!(capture_tick||frame_capture)&&(skip_for_deadline||
            (reduced_cadence&&(f%draw_period)*draw_count%draw_period>=draw_count));
#else
        const bool skip_draw=reduced_cadence&&(f%draw_period)*draw_count%draw_period>=draw_count&&!(capture_tick||frame_capture);
#endif
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
        if(std::getenv("VF2_CHECK_TRACE")){
            const auto &w=machine.workram();
            std::fprintf(stderr,"CHECK tick %llu game %llu shown %d polys %d separate %d has3d %d gpu3d %d cam %d mode %u gws %08x grs %08x work %016llx\n",
                (unsigned long long)(f+1),(unsigned long long)machine.frame_number(),machine.video().shown_index(),
                machine.video().polygons_shown(),separate?1:0,parts.has3d?1:0,parts.gpu3d?1:0,camera_pending?1:0,machine.game_mode(),
                machine.geo_write_start(),machine.geo_read_start(),
                (unsigned long long)m2::state_hash(w.data(),w.size()));
        }
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
            gpu3d_frame frame;frame.w=vf2_render_target_size().w;frame.h=vf2_render_target_size().h;layers(frame);
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
            gpu->settle();mark(2);display.begin(egl.width,egl.height,rotate,canvas_drawn);
            constexpr int pad=model2a::picture_parts::PAD;
            if(canvas_drawn)display.canvas(GLuint(reinterpret_cast<uintptr_t>(gpu->target_handle(0))),true);
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
        // Swapping submits the current context's work. Create the completion
        // fence afterward, before any next-frame texture use.
        fence_after_swap=post_swap_fence&&!capture_tick&&!frame_capture;
        if(queued_gpu){
            if(!fence_after_swap&&!gpu_queue.end_frame())return 1;
        }else if(std::getenv("VF2_SYNC_GPU"))glFinish();
        if(!fence_after_swap)timing.mark(render_timing::flush);
        if(capture_tick||frame_capture){
            std::vector<uint32_t> px(size_t(egl.width)*egl.height);std::vector<uint8_t> bytes(px.size()*4);
            glReadPixels(0,0,egl.width,egl.height,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());
            for(int y=0;y<egl.height;y++)for(int x=0;x<egl.width;x++){auto p=&bytes[(size_t(egl.height-1-y)*egl.width+x)*4];px[size_t(y)*egl.width+x]=0xff000000u|(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];}
            if(capture_tick)capture_pixels=px;
            if(frame_capture){char name[512];std::snprintf(name,sizeof name,"%s.%06llu.png",capture.c_str(),(unsigned long long)(f+1));if(!write_png(name,px.data(),egl.width,egl.height))std::fprintf(stderr,"Capture failed: %s\n",name);}
        }
        mark(3);
        if(!present_at_tick){
            if(frame_trace_path)swap_start_ns=since(clock::now(),began);
            if(!egl.swap()){std::fprintf(stderr,"EGL swap failed: %x\n",eglGetError());return 1;}
            ++presented;
            mark(4);swapped=phase;timing.mark(render_timing::swap);
            if(fence_after_swap){
                if(!gpu_queue.end_frame())return 1;
                timing.mark(render_timing::flush);
            }
        }
        }else if(skip_draw)next_simulation();
        if(!pipeline)inputs(f);
#ifdef MODEL2_ORIGINAL
        if(adaptive_draw){
            drew_previous=!skip_draw;
            if(drew_previous){
                const auto work=std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now()-draw_start);
                // React to a heavier scene immediately and relax as it gets cheaper.
                draw_estimate=std::max(work,draw_estimate+(work-draw_estimate)/4);
            }
        }
        // Submit the complete frame before sleeping. Its fence still follows the swap.
        if(pace_preflush&&fence_after_swap&&!skip_draw&&!headless&&!simulation_only&&!uncapped)glFlush();
#endif
        int64_t deadline_ns=0;
        bool resynced=false;
        if(!uncapped&&!headless){
            next_tick+=tick_period;
            if(frame_trace_path)deadline_ns=since(next_tick,began);
            const auto present_target=next_tick-std::chrono::nanoseconds(present_lead_ns);
            if(pace_spin_ns>0){
                std::this_thread::sleep_until(present_target-std::chrono::nanoseconds(pace_spin_ns));
                while(clock::now()<present_target){}
            }else std::this_thread::sleep_until(present_target);
            const auto now=clock::now();            if(frame_trace_path)sleep_end_ns=since(now,began);
            if(now-next_tick>std::chrono::milliseconds(100)){next_tick=now;resynced=true;}
        }
        if(present_at_tick&&!headless&&!simulation_only&&!skip_draw){
            if(fence_after_swap)timing.mark(render_timing::input);
            if(frame_trace_path)swap_start_ns=since(clock::now(),began);
            if(!egl.swap()){std::fprintf(stderr,"EGL swap failed: %x\n",eglGetError());return 1;}
            ++presented;swapped=clock::now();timing.mark(render_timing::swap);
            if(fence_after_swap){
                if(!gpu_queue.end_frame())return 1;
                timing.mark(render_timing::flush);
            }
        }
        if(frame_trace_path){
            frame_observation row{wait_ns,simulation_ns,swapped==clock::time_point{}?0:since(swapped,began),deadline_ns,swap_start_ns,sleep_end_ns,resynced,{}};
            std::copy(std::begin(timing.frame),std::end(timing.frame),row.stage);
            std::copy(std::begin(timing.frame_cpu),std::end(timing.frame_cpu),row.stage_cpu);
            row.simulation_pool_wait_ns=frame_pool_wait_ns;
            row.simulation_cpu_ns=frame_simulation_cpu_ns;
            row.geometry_job_wait_ns=frame_geometry_wait_ns;
            frame_trace.push_back(row);
        }
        mark(5);timing.mark(render_timing::input);
        if(f%report_every==report_every-1){
            auto now=clock::now();rusage usage{};getrusage(RUSAGE_SELF,&usage);
            double elapsed=std::chrono::duration<double>(now-began).count(),window=std::chrono::duration<double>(now-window_began).count();
            std::fprintf(stderr,"Frame %llu; %.2f fps average, %.2f recent; peak RSS %ld KiB; ms/frame CPU %.2f render %.2f sync %.2f composite %.2f swap %.2f audio/input/pace %.2f\n",(unsigned long long)(f+1),(f+1)/elapsed,report_every/window,usage.ru_maxrss,phase_ms[0]/report_every,phase_ms[1]/report_every,phase_ms[2]/report_every,phase_ms[3]/report_every,phase_ms[4]/report_every,phase_ms[5]/report_every);
            std::fill(phase_ms,phase_ms+6,0);window_began=now;
            cpu_samples.dump(f+1);
            if(std::getenv("VF2_ROM_RESIDENCY"))machine.rom_residency_report();
        }
    }
    if(simulation_pending){simulation_job.wait();simulation_pending=false;simulation_task_ms+=simulation_job_ms;++simulation_tasks;}
    if(queued_gpu&&!gpu_queue.drain())return 1;
    auto measured_end=clock::now();cpu_samples.stop(f);
#ifdef MODEL2_ORIGINAL
    if(!nvram_path.empty()&&!cabinet_nvram(machine,nvram_path,true))return 1;
    if(audio_capture&&std::fclose(audio_capture.release())!=0)audio_capture_ok=false;
    if(!audio_capture_ok){std::fprintf(stderr,"Could not write audio capture %s\n",audio_capture_path.c_str());return 1;}
    if(!no_sound_board)std::fprintf(stderr,"Model 1 sound peak: %d\n",machine.sound_peak());
    if(!coverage_path.empty()){
        FILE *file=std::fopen(coverage_path.c_str(),"w");
        if(!file){std::perror(coverage_path.c_str());return 1;}
        for(const auto &[from,to]:coverage)std::fprintf(file,"edge %06x %06x\n",from,to);
        const bool written=!std::ferror(file);
        if(std::fclose(file)!=0||!written)return 1;
    }
#endif
    if(detached){
        const bool finished=detached->finish(true);
        detached->report();
        if(!finished||!audio_queued||(frame_trace_path&&detached->records.size()!=f))return 1;
    }
    if(frame_trace_path){
        FILE *trace=std::fopen(frame_trace_path,"w");
        if(!trace){std::perror("frame trace");return 1;}
        std::fputs("frame,sim_wait_ns,sim_task_ns,swap_end_ns,pace_deadline_ns,pace_resynced,swap_start_ns,sleep_end_ns",trace);
        static const char *stage_names[]={"sim","capture","camera","packets","prior_gpu","tile_upload",
            "submit","gpu_3d","composite","gpu_composite","flush","swap","pace"};
        if(timing.enabled)for(const char *name:stage_names)std::fprintf(trace,",%s_ms",name);
        // Composite wall vs thread CPU: a long composite that used little CPU is
        // the thread waiting or preempted, not the driver copying pixels.
        if(timing.enabled)std::fputs(",composite_cpu_ms",trace);
        // The same thread-CPU split for every other stage, so the main thread's
        // frame cost can be attributed exactly instead of from wall times.
        if(timing.enabled)for(size_t s=0;s<size_t(render_timing::count);++s){
            if(s==size_t(render_timing::composite))continue;
            std::fprintf(trace,",%s_cpu_ms",stage_names[s]);
        }
        if(trace_sim_waits)std::fputs(",sim_pool_wait_ns,geometry_job_wait_ns,sim_task_cpu_ns",trace);
        std::fputc('\n',trace);
        for(size_t i=0;i<frame_trace.size();++i){
            const auto &row=frame_trace[i];
            std::fprintf(trace,"%zu,%lld,%lld,%lld,%lld,%d,%lld,%lld",i+1,(long long)row.wait_ns,
                (long long)row.simulation_ns,(long long)row.swap_ns,(long long)row.deadline_ns,row.resynced,
                (long long)row.swap_start_ns,(long long)row.sleep_end_ns);
            if(timing.enabled)for(double v:row.stage)std::fprintf(trace,",%.4f",v);
            if(timing.enabled)std::fprintf(trace,",%.4f",row.stage_cpu[render_timing::composite]);
            if(timing.enabled)for(size_t s=0;s<size_t(render_timing::count);++s){
                if(s==size_t(render_timing::composite))continue;
                std::fprintf(trace,",%.4f",row.stage_cpu[s]);
            }
            if(trace_sim_waits)std::fprintf(trace,",%lld,%llu,%lld",(long long)row.simulation_pool_wait_ns,
                (unsigned long long)row.geometry_job_wait_ns,(long long)row.simulation_cpu_ns);
            std::fputc('\n',trace);
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
    display.report_static_canvas();
#ifdef MODEL2_ORIGINAL
    if(move_sim_launch)std::fprintf(stderr,"Moved simulation launches: %u from CPU %d\n",moved_sim_launches,simulation_core);
#endif
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
    if(frames||!save_state_path.empty()){
        // The render queue is drained and any capture pixels are on the CPU.
        // Release renderer caches before hashing or serializing the final state.
        gpu.reset();
        tile_gpu.reset();
    }
    if(frames){
        std::vector<uint8_t> state;machine.save_state(state);
        std::fprintf(stderr,"Core state hash: %016llx\n",(unsigned long long)m2::state_hash(state.data()+model2a::STATE_HEADER,state.size()-model2a::STATE_HEADER));
        std::vector<uint8_t> geometry;machine.video().save_geometry(geometry);
        std::fprintf(stderr,"Geometry state hash: %016llx\n",(unsigned long long)m2::state_hash(geometry.data(),geometry.size()));
    }
    if(!save_state_path.empty()){
        // The checkpoint lands on RAM-backed /run. Release the captured pixels
        // first to keep the peak allocation down.
        if(!capture_pixels.empty()){
            captured=write_png(capture.c_str(),capture_pixels.data(),egl.width,egl.height);
            if(!captured)std::fprintf(stderr,"Capture failed: %s\n",capture.c_str());
            std::vector<uint32_t>().swap(capture_pixels);
        }
        std::string error;if(!acm_checkpoint::save(machine,save_state_path,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        std::fprintf(stderr,"Checkpoint saved: %s (game frame %llu)\n",save_state_path.c_str(),(unsigned long long)machine.frame_number());
    }
    if(!capture_pixels.empty()){captured=write_png(capture.c_str(),capture_pixels.data(),egl.width,egl.height);if(!captured)std::fprintf(stderr,"Capture failed: %s\n",capture.c_str());}
    double elapsed=std::chrono::duration<double>(measured_end-began).count();rusage usage{};getrusage(RUSAGE_SELF,&usage);
    if(early_tiles)std::fprintf(stderr,"VBLANK tiles: %llu prepared, %llu staged early, %llu staged after join, %llu ownership misses, %llu declines, %llu ineligible\n",
        (unsigned long long)tile_descriptors,(unsigned long long)early_tile_frames,(unsigned long long)joined_tile_frames,
        (unsigned long long)tile_ownership_misses,(unsigned long long)tile_stage_declines,(unsigned long long)tile_ineligible);
#ifdef MODEL2_ORIGINAL
    if(reduced_cadence||adaptive_draw)
#else
    if(reduced_cadence)
#endif
        std::fprintf(stderr,"Draw cadence: %llu presented, %llu skipped; %.2f presented fps\n",
        (unsigned long long)presented,(unsigned long long)(f-presented),presented/elapsed);
    std::fprintf(stderr,"%llu frames, %.3fs, %.2f fps, peak RSS %ld KiB, capture %s\n",(unsigned long long)f,elapsed,f/elapsed,usage.ru_maxrss,captured?"saved":"none");
    return 0;
}
