// Compare GPU tiles to the existing pixel renderer, including split/line modes.
#include "../../src/machine/s24tile.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>

static void require(bool condition,const char *message){if(!condition){std::fprintf(stderr,"FAIL GPU2D: %s, GL=%x\n",message,glGetError());std::exit(1);}}
int main(){
    EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLint a,b;require(eglInitialize(d,&a,&b),"EGL init");eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};EGLConfig config;EGLint count;
    require(eglChooseConfig(d,ca,&config,1,&count)&&count,"EGL config");EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE},size[]={EGL_WIDTH,496,EGL_HEIGHT,384,EGL_NONE};
    auto context=eglCreateContext(d,config,EGL_NO_CONTEXT,ctx);auto surface=eglCreatePbufferSurface(d,config,size);require(eglMakeCurrent(d,surface,surface,context),"EGL current");
    auto gpu=gpu2d_create_gles2();require(bool(gpu),"init");
    segas24_tile tiles;std::mt19937 rng(0x24ac1234);std::array<uint32_t,4096> pens;
    for(auto &p:pens)p=0xff000000u|(rng()&0xffffff);
    for(size_t i=0;i<tiles.char_ram.size();i++)tiles.char_w(i,rng(),0xffff);
    for(int i=0;i<0x4000;i++)tiles.tile_w(i,rng(),0xffff);
    for(int i=0x6000;i<0x7000;i++)tiles.tile_w(i,rng(),0xffff);
    bitmap_rgb32 cpu[3];for(auto &bm:cpu)bm.allocate(496,384);
    GLuint read;glGenFramebuffers(1,&read);std::vector<uint8_t> pixels(496*384*4);
    GLuint canvas;glGenTextures(1,&canvas);glBindTexture(GL_TEXTURE_2D,canvas);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,496,384,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    constexpr unsigned dense_cases=24,sparse_cases=8;
    const bool rotating=std::getenv("VF2_ROTATE_TILE_TEXTURES")!=nullptr;
    for(unsigned test=0;test<dense_cases+sparse_cases;test++){
        if(test==dense_cases){
            // Empty maps with isolated tiles in each priority, including
            // opposite atlas quadrants at their shared edges.
            for(size_t i=0;i<tiles.char_ram.size();i++)tiles.char_w(i,0,0xffff);
            for(int i=16;i<32;i++)tiles.char_w(i,0x1203,0xffff);
            for(int i=0;i<0x4000;i++)tiles.tile_w(i,0,0xffff);
            for(int map=0;map<4;++map){
                const int corner=(map&1?0:63)+(map&2?0:63)*64;
                tiles.tile_w(map*4096+corner,0x8001,0xffff);
                tiles.tile_w(map*4096+32*64+32,0x0001,0xffff);
            }
        }
        if(test>=dense_cases){
            for(int l=0;l<4;++l){
                const int scroll=(test-dense_cases)*8;
                tiles.tile_w(0x5000+l,scroll,0xffff);
                tiles.tile_w(0x5004+l,(512-scroll)&511,0xffff);
            }
            const int old_tile=32*64+32;
            tiles.tile_w(old_tile,test&1?0x8001:0x0001,0xffff);
        }else{
            // Normal scroll, line scroll, vertical split, horizontal split and
            // disabled layers; mutate palette/graphics after the first full upload.
            for(int l=0;l<4;l++){
                tiles.tile_w(0x5000+l,(rng()&511)|((test&1)?0x8000:0),0xffff);
                tiles.tile_w(0x5004+l,rng()&511,0xffff);
                for(int y=0;y<384;y++)tiles.tile_w(0x4000+l*512+y,rng()&1023,0xffff);
            }
            for(int l=0;l<4;l+=2)tiles.tile_w(0x5004+l,(tiles.tile_r(0x5004+l)&511)|((test/2%4)<<13)|((test==23)?0x8000:0),0xffff);
            if(test&&(!rotating||(test!=8&&test!=9))){
                for(int i=0;i<13;i++)tiles.tile_w(rng()%0x4000,rng(),0xffff);
                for(int i=0;i<11;i++)tiles.char_w(rng()%tiles.char_ram.size(),rng(),0xffff);
                pens[rng()%pens.size()]=0xff000000u|(rng()&0xffffff);
            }
        }
        const bool small_block=test==28,large_block=test==29,full_width=test==30,spread=test==31;
        auto change=[&](unsigned map,unsigned x,unsigned y){
            const unsigned address=4096*map+64*y+x;
            tiles.tile_w(address,tiles.tile_r(address)^1,0xffff);
        };
        if(small_block||large_block){
            for(unsigned map=0;map<(large_block?3u:1u);++map)
                for(unsigned y=33;y<41;++y)for(unsigned x=22;x<34;++x)
                    change(map,x,y);
        }
        if(full_width)for(unsigned x=0;x<64;++x)change(0,x,63);
        if(spread){
            for(unsigned map=0;map<4;++map)
                for(unsigned i=0;i<36;++i)change(map,(i*29)%64,(i*19)%64);
        }
        if(test==3){
            for(auto &pen:pens)pen^=0x0055cc33;
            for(unsigned pair=0;pair<2;++pair)for(unsigned i=0;i<1536;++i){
                const unsigned address=0x6000+pair*0x800+i;
                tiles.tile_w(address,tiles.tile_r(address)^0xffff,0xffff);
            }
        }
        tiles.update(pens.data());gpu2d_frame frame;tiles.gpu_data(frame);
        if(rotating&&(test==8||test==9))require(frame.dirty_count==0,"unchanged frame between texture changes");
        if(small_block||full_width)require(frame.dirty_count>=64&&frame.dirty_count<=128,"bounded dirty rectangles");
        if(large_block||spread)require(frame.dirty_count>128,"large dirty bounds");
        for(int pass=0;pass<3;pass++){
            cpu[pass].fill(pass==0?pens[0]:0,rectangle(0,495,0,383));
            int first=pass==1?1:3,last=pass==0?2:0;
            for(int l=first;l>=last;l--){
                int layer=(l<<1)|(pass==2),flags=pass==0?segas24_tile::DRAW_OPAQUE:0;
                tiles.draw(cpu[pass],rectangle(0,495,0,383),layer,0,flags);
                tiles.gpu_draw(frame.passes[pass],cpu[pass],layer,flags);
            }
        }
        // Exercise version-gap recovery after a CPU-only frame.
        if(test==12){tiles.update(pens.data());tiles.gpu_data(frame);}
        if(test==3){
            auto rejected=frame;
            gpu2d_rect invalid{};
            invalid.x1=invalid.y1=1;
            invalid.map=1;
            invalid.line_scroll=true;
            rejected.passes[2].push_back(invalid);
            require(!gpu->stage(rejected),"reject unsupported scroll after tile, palette and mask changes");
        }
        require(gpu->submit(frame),"submit");glBindFramebuffer(GL_FRAMEBUFFER,read);
        for(int pass=0;pass<3;pass++){
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,gpu->target_handle(pass),0);
            glReadPixels(0,0,496,384,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            size_t different=0;for(int y=0;y<384;y++)for(int x=0;x<496;x++){
                const auto *p=&pixels[(y*496+x)*4];uint32_t got=uint32_t(p[3])<<24|uint32_t(p[0])<<16|uint32_t(p[1])<<8|p[2];
                if(got!=cpu[pass].pix(y,x)){if(different<2)std::fprintf(stderr,"case %u pass %d (%d,%d) CPU %08x GPU %08x\n",test,pass,x,y,cpu[pass].pix(y,x),got);different++;}
            }
            require(different==0,"pixel equality");
        }
        // The staged path must own its upload data while the board advances.
        // Change every source plane, palette and mask before uploading, then
        // restore the board for the next case. The canvas must remain exact.
        if(rotating&&test%5!=0){glBindFramebuffer(GL_FRAMEBUFFER,0);continue;}
        require(gpu->stage(frame),"CPU-only staging");
        auto perturb=[&](){
            for(unsigned map=0;map<4;++map)for(size_t i=0;i<512*512;++i){
                const_cast<uint16_t*>(frame.pixels[map])[i]^=0x1ff;
                const_cast<uint8_t*>(frame.flags[map])[i]^=0x10;
            }
            for(auto &pen:pens)pen^=0xffffff;
            for(unsigned pair=0;pair<2;++pair)for(unsigned i=0;i<1536;++i)
                const_cast<uint16_t*>(frame.tile_ram)[0x6000+pair*0x800+i]^=0xffff;
        };
        perturb();require(gpu->upload(),"staged upload with changed board inputs");perturb();
        require(gpu->paint(0,canvas,true,true)&&gpu->paint(1,canvas,true,false),"canvas underlay");
        glBindFramebuffer(GL_FRAMEBUFFER,read);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,canvas,0);
        glEnable(GL_SCISSOR_TEST);glScissor(120,384-192,120,96);glClearColor(49/255.f,151/255.f,227/255.f,1);glClear(GL_COLOR_BUFFER_BIT);glDisable(GL_SCISSOR_TEST);
        require(gpu->paint(2,canvas,true,false),"canvas overlay");
        glReadPixels(0,0,496,384,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        for(int y=0;y<384;y++)for(int x=0;x<496;x++){
            const auto *p=&pixels[((383-y)*496+x)*4];uint32_t got=uint32_t(p[3])<<24|uint32_t(p[0])<<16|uint32_t(p[1])<<8|p[2];
            uint32_t top=cpu[2].pix(y,x),mid=cpu[1].pix(y,x);
            uint32_t expected=top?top:(x>=120&&x<240&&y>=96&&y<192)?0xff3197e3u:mid?mid:cpu[0].pix(y,x);
            if(got!=expected){std::fprintf(stderr,"canvas case %u (%d,%d): %08x vs %08x\n",test,x,y,got,expected);require(false,"canvas pixel equality");}
        }
        glBindFramebuffer(GL_FRAMEBUFFER,0);
    }
    glDeleteTextures(1,&canvas);glDeleteFramebuffers(1,&read);gpu.reset();std::puts("GPU2D passed: 32 scroll/window/priority/update cases, layers and combined canvas pixel-exact");
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(d,context);eglDestroySurface(d,surface);eglTerminate(d);
}
