#include "../../src/gpu/gpu3d.h"
#include "../../src/gpu/gles2_atlas.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
static void require(bool ok,const char *what){if(!ok){std::fprintf(stderr,"FAIL: %s (GL %x EGL %x)\n",what,glGetError(),eglGetError());std::exit(1);}}
static gpu3d_poly quad(uint32_t colour){
    gpu3d_poly p{};p.n=4;p.colour=colour;p.vp[1]=p.vp[3]=31;
    p.x[1]=p.x[2]=32;p.y[2]=p.y[3]=32;
    for(int i=0;i<4;i++){p.z[i]=1;p.u[i]=p.x[i];p.v[i]=p.y[i];}
    return p;
}
int main(){
    EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLint major,minor;
    require(eglInitialize(display,&major,&minor),"EGL initialize");
    require(eglBindAPI(EGL_OPENGL_ES_API),"EGL API");
    EGLint attributes[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count;require(eglChooseConfig(display,attributes,&config,1,&count)&&count,"EGL config");
    EGLint context_attributes[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attributes);
    EGLint size[]={EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE};EGLSurface surface=eglCreatePbufferSurface(display,config,size);
    require(eglMakeCurrent(display,surface,surface,context),"EGL current");
    std::printf("Renderer: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    auto gpu=gpu3d_create_gles2();require(bool(gpu),"GLES2 shader program");
    std::vector<uint32_t> sheet0(0x80000,0x66666666),sheet1(0x80000,0x66666666);
    std::array<uint8_t,0x8000> luma{};std::array<uint16_t,0x6000> xlat{};std::array<uint8_t,256> gamma{};
    for(unsigned i=0;i<luma.size();i++)luma[i]=i&255;
    for(int c=0;c<3;c++)for(int p=0;p<32;p++)for(int l=0;l<256;l++)xlat[c*0x2000+p*256+l]=std::min(255,p*8+l);
    for(int i=0;i<256;i++)gamma[i]=i;
    gpu3d_rams ram{};ram.tex[0]=sheet0.data();ram.tex[1]=sheet1.data();ram.luma=luma.data();ram.xlat=xlat.data();ram.gamma=gamma.data();gpu->set_rams(ram);
    std::array<uint32_t,gpu3d_rams::texture_pages> page_versions[2];
    for(unsigned i=0;i<2;++i){page_versions[i].fill(1);ram.tex_page_gen[i]=page_versions[i].data();}
    gpu->set_rams(ram);
    gpu3d_poly polys[]={quad(0xffff0000),quad(0xff00ff00)};
    std::array<uint32_t,32*32> owners{},rgba{};
    auto draw=[&](){require(gpu->draw(polys,2,32,32,1,owners.data(),32,rgba.data(),32,nullptr),"draw/readback");};
    draw();for(unsigned i=0;i<rgba.size();i++)require(rgba[i]==0xffff0000&&owners[i]==0,"nearest polygon owns pixel");
    polys[0].checker=1;draw();
    for(int y=0;y<32;y++)for(int x=0;x<32;x++){bool red=(x^y)&1;require(rgba[y*32+x]==(red?0xffff0000u:0xff00ff00u)&&owners[y*32+x]==(red?0u:1u),"checker parity/ownership");}
    polys[0].checker=0;polys[0].vp[0]=8;polys[0].vp[1]=15;polys[0].vp[2]=4;polys[0].vp[3]=11;draw();
    for(int y=0;y<32;y++)for(int x=0;x<32;x++)require(rgba[y*32+x]==((x>=8&&x<16&&y>=4&&y<12)?0xffff0000u:0xff00ff00u),"inclusive Model2 viewport");
    polys[0]=quad(0);polys[0].renderer=2;polys[0].texw=polys[0].texh=32;polys[0].wrapx=polys[0].wrapy=1;
    polys[0].pal=31|(10<<5)|(2<<10);polys[0].lumabase=1;polys[0].luma=256;polys[0].texlod=2000;draw();
    for(auto colour:rgba)require(colour==0xffff8141,"texture index/luma/palette/gamma");
    sheet0.back()=0x77777777;++page_versions[0].back();++ram.gen[0];gpu->set_rams(ram);draw();
    for(auto colour:rgba)require(colour==0xffff8141,"unrelated texture page leaves sampled tile unchanged");
    sheet0[0]=0x55555555;++page_versions[0][0];++ram.gen[0];gpu->set_rams(ram);draw();
    require(rgba[0]==0xffff7939,"modified texture page refreshes sampled tile");
    sheet0[0]=0x66666666;++page_versions[0][0];++ram.gen[0];gpu->set_rams(ram);draw();
    for(auto colour:rgba)require(colour==0xffff8141,"restored texture page refreshes sampled tile");
    // Repeated coordinates cross negative and positive cells. Samples land
    // inside four-texel constant blocks, avoiding filter-boundary ambiguity.
    std::fill(sheet0.begin(),sheet0.end(),0);
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){
        const unsigned xo=x>>1,shift=((xo&1)<<4)+((~x&1)<<2)+((~y&1)<<3);
        sheet0[((((y>>1)<<9)|xo)>>1)&0x7ffff]|=((x/4+3*(y/4))%15)<<shift;
    }
    for(auto &revision:page_versions[0])++revision;
    ram.gen[0]++;gpu->set_rams(ram);
    polys[0].pal=1|(2<<5)|(3<<10);polys[0].luma=128;
    for(int i=0;i<4;++i){polys[0].u[i]=polys[0].x[i]*4-64;polys[0].v[i]=polys[0].y[i]*4-64;}
    for(unsigned mode=0;mode<4;++mode){
        polys[0].mirrorx=mode&1;polys[0].mirrory=mode>>1;draw();
        for(int y=0;y<32;++y)for(int x=0;x<32;++x){
            auto coord=[](int v,bool mirror){v=(v%64+64)%64;return mirror&&v>=32?63-v:v%32;};
            const unsigned tx=coord(x*4-62,mode&1),ty=coord(y*4-62,mode&2);
            const unsigned index=(tx/4+3*(ty/4))%15,l=index*4;
            const uint32_t expected=0xff000000u|((8+l)<<16)|((16+l)<<8)|(24+l);
            require(rgba[y*32+x]==expected&&owners[y*32+x]==0,"patterned repeat/mirror mapping");
        }
    }
    polys[0].mirrorx=polys[0].mirrory=0;
    // Distinct constant-index sheets make mip/microtexture blend arithmetic
    // observable through the palette, independently of UV interpolation.
    std::fill(sheet0.begin(),sheet0.end(),0x44444444);
    std::fill(sheet1.begin(),sheet1.end(),0xcccccccc);
    for(unsigned bank=0;bank<2;++bank){for(auto &revision:page_versions[bank])++revision;++ram.gen[bank];}
    gpu->set_rams(ram);
    polys[0]=quad(0);polys[0].renderer=2;polys[0].texw=polys[0].texh=32;
    polys[0].wrapx=polys[0].wrapy=1;polys[0].pal=1|(2<<5)|(3<<10);polys[0].luma=128;polys[0].lumabase=1;
    auto colour_for_index=[](unsigned index){unsigned l=(1+index/2)/2;return 0xff000000u|((8+l)<<16)|((16+l)<<8)|(24+l);};
    for(int lod:{-1,-31,-32,-63,-64,-95,-96,-127}){
        polys[0].texlod=lod;draw();const auto expected=colour_for_index(64-lod);
        for(auto colour:rgba)require(colour==expected,"adjacent mip blend/palette arithmetic");
    }
    polys[0].utex=1;polys[0].texlod=64;
    for(unsigned level=0;level<4;++level){
        polys[0].utexminlod=level;draw();const auto expected=colour_for_index(64+(64>>(level+1)));
        for(auto colour:rgba)require(colour==expected,"microtexture scale/blend/palette arithmetic");
    }
    std::fill(sheet0.begin(),sheet0.end(),0xffffffff);for(auto &revision:page_versions[0])++revision;
    ram.gen[0]++;gpu->set_rams(ram);polys[0].renderer=3;draw();
    for(unsigned i=0;i<rgba.size();i++)require(rgba[i]==0xff00ff00&&owners[i]==1,"transparent texel reveals polygon behind");
    polys[0].renderer=1;draw();for(auto colour:rgba)require(colour==0xff00ff00,"solid translucent polygon is skipped");
    // Native frame composition: preserve the underlay through 3D's depth
    // clear, then paint an overlay before restoring the caller's framebuffer.
    require(gpu->make_targets(32,32,1),"canvas target");
    gpu3d_frame frame;frame.w=frame.h=32;
    frame.underlay=[](void*){glDisable(GL_SCISSOR_TEST);glClearColor(16/255.f,32/255.f,48/255.f,1);glClear(GL_COLOR_BUFFER_BIT);return glGetError()==GL_NO_ERROR;};
    frame.overlay=[](void*){glEnable(GL_SCISSOR_TEST);glScissor(28,28,4,4);glClearColor(1,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glDisable(GL_SCISSOR_TEST);return glGetError()==GL_NO_ERROR;};
    polys[0]=quad(0xffff0000);polys[0].vp[0]=8;polys[0].vp[1]=15;polys[0].vp[2]=4;polys[0].vp[3]=11;
    require(gpu->submit(polys,1,frame,0)&&gpu->read_target(0,rgba.data(),32),"canvas hooks");
    for(int y=0;y<32;y++)for(int x=0;x<32;x++)require(rgba[y*32+x]==((x>=28&&y<4)?0xffffff00u:(x>=8&&x<16&&y>=4&&y<12)?0xffff0000u:0xff102030u),"underlay/3D/overlay ordering");
    if(std::getenv("VF2_GLES_INDEXED")){
        // More than 65,536 unique vertices must use array batches, with no
        // 16-bit wrapping or expensive per-polygon fallback.
        std::vector<gpu3d_poly> many(16385,quad(0xffff0000));
        for(auto &p:many)p.vp[1]=15;
        many.back().vp[1]=31;many.back().colour=0xff00ff00;
        require(gpu->draw(many.data(),many.size(),32,32,1,owners.data(),32,rgba.data(),32,nullptr),"indexed capacity fallback");
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
            require(rgba[y*32+x]==(x<16?0xffff0000u:0xff00ff00u)&&owners[y*32+x]==(x<16?0u:16384u),"large indexed scene ordering/colour");
    }
    {
        // Validate skipped source reads as well as invalidation side effects.
        vf2_batch::tile_atlas cache;require(cache.init(),"page-cache test atlas");
        auto data=sheet0;auto revisions=page_versions[0];gpu3d_rams r=ram;
        r.tex[0]=data.data();r.tex_page_gen[0]=revisions.data();r.gen[0]=1;
        gpu3d_poly t=quad(0);t.renderer=2;t.texw=t.texh=32;t.wrapx=t.wrapy=1;
        auto get=[&](){require(cache.get(r,t,0,false)!=nullptr,"page-cache tile lookup");};
        get();uint64_t uploads=cache.uploaded,checks=cache.source_checks;
        data.back()^=0x11111111;++revisions.back();++r.gen[0];get();
        require(cache.uploaded==uploads,"unrelated page avoids texture upload");
        if(std::getenv("VF2_ATLAS_PAGE_CACHE"))require(cache.page_hits==1&&cache.source_checks==checks,"unrelated page avoids source-word snapshot");
        data[0]^=0x11111111;++revisions[0];++r.gen[0];get();
        require(cache.uploaded>uploads,"changed source page updates atlas");uploads=cache.uploaded;
        ++revisions[0];++r.gen[0];get();
        require(cache.uploaded==uploads,"unchanged source data avoids upload after revision advance");
        auto alternate=revisions;r.tex_page_gen[0]=alternate.data();
        data[0]^=0x11111111;++r.gen[0];get();
        require(cache.uploaded>uploads,"replacement revision array cannot reuse old stamps");uploads=cache.uploaded;
        r.tex_page_gen[0]=nullptr;data[0]^=0x11111111;++r.gen[0];get();
        require(cache.uploaded>uploads,"missing page revisions use word comparison");uploads=cache.uploaded;
        r.tex_page_gen[0]=revisions.data();data[0]^=0x11111111;++revisions[0];++r.gen[0];get();
        require(cache.uploaded>uploads,"restored page metadata refreshes source");
        const auto hits=cache.page_hits;checks=cache.source_checks;
        data.back()^=0x11111111;++revisions.back();++r.gen[0];get();
        if(std::getenv("VF2_ATLAS_PAGE_CACHE"))require(cache.page_hits==hits+1&&cache.source_checks==checks,"restored metadata resumes page validation");
    }
    {
        vf2_batch::tile_atlas cache;
        cache.defer_uploads(true);
        auto polygon=quad(0);
        polygon.renderer=2;
        polygon.texw=polygon.texh=8;
        polygon.texx=2048;polygon.texy=1024;
        const auto *first=cache.get(ram,polygon,0,false);
        require(first,"initial atlas lookup");
        const auto page=first->page;
        for(int i=1;i<=256;++i){
            polygon.texx=2048+(i%256)*8;
            polygon.texy=1024+(i/256)*8;
            require(cache.get(ram,polygon,0,false),"atlas growth insertion");
        }
        polygon.texx=2048;polygon.texy=1024;
        require(cache.resident()==257&&cache.get(ram,polygon,0,false)==first&&first->page==page,
                "atlas growth preserves returned tile addresses");
        cache.clear();
        require(cache.resident()==0&&cache.clears==1,"atlas index clear");
        require(cache.get(ram,polygon,0,false)&&cache.resident()==1,"atlas lookup after clear");
    }
    if(std::getenv("VF2_STAGE_MATERIALS")){
        require(std::getenv("VF2_GLES_BATCHED"),"staged tests require batches");
        {
            // Sample a live atlas directly: queued CPU changes must not alter
            // its GPU contents, bindings, or lifetime until explicit flush.
            auto compile=[](GLenum kind,const char *text){GLuint shader=glCreateShader(kind);glShaderSource(shader,1,&text,nullptr);glCompileShader(shader);GLint ok;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);require(ok,"atlas sampler shader");return shader;};
            GLuint vs=compile(GL_VERTEX_SHADER,"attribute vec2 p;void main(){gl_Position=vec4(p,0.,1.);}");
            GLuint fs=compile(GL_FRAGMENT_SHADER,"precision mediump float;uniform sampler2D image;uniform vec2 uv;void main(){gl_FragColor=texture2D(image,uv);}");
            GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glBindAttribLocation(program,0,"p");glLinkProgram(program);GLint linked;glGetProgramiv(program,GL_LINK_STATUS,&linked);require(linked,"atlas sampler link");glDeleteShader(vs);glDeleteShader(fs);
            vf2_batch::tile_atlas cache;cache.defer_uploads(true);
            std::vector<uint32_t> data(0x80000,0x66666666);gpu3d_rams source=ram;source.tex[0]=data.data();source.tex_page_gen[0]=nullptr;source.gen[0]=1;
            auto polygon=quad(0);polygon.renderer=2;polygon.texw=polygon.texh=32;polygon.wrapx=polygon.wrapy=1;
            const float points[]={-1,-1,1,-1,-1,1,1,1};
            auto sample=[&](const vf2_batch::tile &tile){
                glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,32,32);glDisable(GL_DEPTH_TEST);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
                glUseProgram(program);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,cache.handle(tile.page));glUniform1i(glGetUniformLocation(program,"image"),0);glUniform2f(glGetUniformLocation(program,"uv"),(tile.x+1.5f)/tile.side,(tile.y+1.5f)/tile.side);
                glBindBuffer(GL_ARRAY_BUFFER,0);glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,points);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
                std::array<uint8_t,4> pixel;glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());glDisableVertexAttribArray(0);require(glGetError()==GL_NO_ERROR,"atlas sample");return pixel;
            };
            glEnable(0xffff);const auto *first=cache.get(source,polygon,0,false);require(first,"deferred tile planned");
            require(glGetError()==GL_INVALID_ENUM&&cache.handle(first->page)==0&&cache.uploaded==0,"CPU plan does not allocate/upload textures or consume GL errors");
            auto tile=*first;require(cache.flush(),"first atlas flush");require(sample(tile)==std::array<uint8_t,4>{96,96,96,255},"first tile pixels");
            const auto bytes=cache.uploaded;data.assign(data.size(),0x55555555);++source.gen[0];require(cache.get(source,polygon,0,false),"replacement tile planned");
            require(cache.uploaded==bytes&&sample(tile)==std::array<uint8_t,4>{96,96,96,255},"unflushed replacement preserves old GPU contents");
            require(cache.flush()&&sample(tile)==std::array<uint8_t,4>{80,80,80,255},"flush replaces GPU contents");
            const GLuint old=cache.handle(tile.page);cache.clear();require(glIsTexture(old),"CPU cache clear retains in-flight texture");
            data.assign(data.size(),0x44444444);++source.gen[0];const auto *fresh=cache.get(source,polygon,0,false);require(fresh&&cache.handle(fresh->page)==0,"replacement page allocated logically");tile=*fresh;
            require(cache.flush()&&sample(tile)==std::array<uint8_t,4>{64,64,64,255},"retired page and replacement upload");
            if(std::getenv("VF2_FIXED_ATLAS_LUMA")){
                auto transparent=polygon;
                transparent.renderer=3;
                const auto *second=cache.get(source,transparent,0,false);
                require(second&&second->x==tile.x+tile.w+2&&second->y==tile.y&&
                        second->side==tile.side,"mixed atlas backings preserve shelf layout");
                require(cache.flush()&&cache.handle(second->page)!=cache.handle(tile.page),
                        "mixed atlas backings bind different textures");
                require(sample(tile)==sample(*second),"mixed atlas formats preserve the opaque shade");
                vf2_batch::tile_atlas bounded;
                bounded.defer_uploads(true);
                require(bounded.add_page(520)&&bounded.add_page(2210)&&bounded.add_page(1032),
                        "leave room for an existing shelf but not another backing");
                require(!bounded.get(source,polygon,0,false),"opaque backing exceeds the allocation limit");
                transparent.texh=16;
                const auto *after_failure=bounded.get(source,transparent,0,false);
                require(after_failure&&after_failure->x==0&&after_failure->y==0,
                        "failed backing allocation leaves the first shelf available");
            }
            vf2_batch::tile_atlas capacity;capacity.defer_uploads(true);require(!capacity.add_page(4096)&&capacity.page_count()==0,"deferred page capacity limit");require(capacity.add_page(520)&&capacity.handle(0)==0,"bounded logical page");capacity.clear();require(capacity.flush()&&capacity.page_count()==0,"cancelled allocation stays empty");
            glUseProgram(0);glDeleteProgram(program);
        }
        setenv("VF2_REQUIRE_STAGED_PLAN","1",1);
        auto staged=gpu3d_create_gles2();require(bool(staged)&&staged->make_targets(32,32,1),"staged renderer target");
        gpu3d_frame f;f.w=f.h=32;
        std::array<gpu3d_poly,2> scene{quad(0),quad(0xff00ff00)};
        scene[0].renderer=2;scene[0].texw=scene[0].texh=32;scene[0].wrapx=scene[0].wrapy=1;
        scene[0].pal=1|(2<<5)|(3<<10);scene[0].luma=128;scene[0].lumabase=1;scene[0].texlod=2000;
        std::array<uint32_t,1024> expected{},actual{};
        std::vector<uint32_t> replacement(0x80000,0x88888888);
        auto compare=[&](){
            gpu->set_rams(ram);require(gpu->draw(scene.data(),scene.size(),32,32,1,nullptr,0,expected.data(),32,nullptr),"reference frame");
            glActiveTexture(GL_TEXTURE3);glBindTexture(GL_TEXTURE_2D,reinterpret_cast<uintptr_t>(staged->target_handle(0)));glPixelStorei(GL_UNPACK_ALIGNMENT,8);
            GLint binding,framebuffer,program;glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);glGetIntegerv(GL_CURRENT_PROGRAM,&program);
            glEnable(0xffff); // a marker error must survive CPU-only planning
            require(staged->prepare_frame_cpu(scene.data(),scene.size(),f,ram),"complete CPU material plan");
            require(glGetError()==GL_INVALID_ENUM,"planning does not consume GL errors");
            GLint value;glGetIntegerv(GL_ACTIVE_TEXTURE,&value);require(value==GL_TEXTURE3,"planning preserves active texture");
            glGetIntegerv(GL_TEXTURE_BINDING_2D,&value);require(value==binding,"planning preserves texture binding");
            glGetIntegerv(GL_UNPACK_ALIGNMENT,&value);require(value==8,"planning preserves unpack alignment");
            glGetIntegerv(GL_FRAMEBUFFER_BINDING,&value);require(value==framebuffer,"planning preserves framebuffer");
            glGetIntegerv(GL_CURRENT_PROGRAM,&value);require(value==program,"planning preserves program");
            glPixelStorei(GL_UNPACK_ALIGNMENT,4);
            staged->set_rams(ram);
            require(staged->submit(scene.data(),scene.size(),f,0)&&staged->read_target(0,actual.data(),32),"planned upload/draw");
            require(actual==expected,"planned versus immediate frame pixels");
        };
        std::fill(sheet0.begin(),sheet0.end(),0x66666666);++ram.gen[0];compare();
        std::fill(sheet0.begin(),sheet0.end(),0x55555555);for(auto &v:page_versions[0])++v;++ram.gen[0];compare();
        scene[0].renderer=3;std::fill(sheet0.begin(),sheet0.end(),0xffffffff);for(auto &v:page_versions[0])++v;++ram.gen[0];compare();
        scene[0].renderer=2;luma.fill(120);++ram.gen[2];compare();
        for(auto &v:xlat)v=(v+7)&255;++ram.gen[3];for(unsigned i=0;i<256;++i)gamma[i]=255-i;compare();
        ram.tex[0]=replacement.data();compare(); // pointer invalidation and retired textures
        if(std::getenv("VF2_PREPARED_DRAWS")){
            scene[0].renderer=0;scene[0].colour=0xff804020;compare();
            scene[0].renderer=2;compare();  // same storage slot returns to indexed textures
        }
        scene[0].texw=128;scene[0].texh=64;scene[0].texlod=32;scene[0].utex=1;
        for(unsigned i=0;i<4;++i){scene[0].u[i]=scene[0].x[i]*5-40;scene[0].v[i]=scene[0].y[i]*3-20;scene[0].z[i]=1.f/(1+i*.125f);}compare();
        // An unrepresentable packed LOD retains the individual-tile fallback.
        // Its textures must then retire safely when a later staged frame
        // invalidates their source generation.
        unsetenv("VF2_REQUIRE_STAGED_PLAN");scene[0].texlod=40000;compare();
        setenv("VF2_REQUIRE_STAGED_PLAN","1",1);scene[0].texlod=2000;++ram.gen[0];compare();
        // A mismatching submit must use its own geometry, not the old plan.
        require(staged->prepare_frame_cpu(scene.data(),scene.size(),f,ram),"plan before mismatching submit");
        auto different=quad(0xff123456);
        require(staged->submit(&different,1,f,0)&&staged->read_target(0,actual.data(),32),"mismatched plan fallback");
        for(auto c:actual)require(c==0xff123456,"fallback uses new geometry");
        if(std::getenv("VF2_GLES_INDEXED")){
            std::vector<gpu3d_poly> many(16385,quad(0xffff0000));for(auto &q:many)q.vp[1]=15;
            many.back().vp[1]=31;many.back().colour=0xff00ff00;
            require(staged->prepare_frame_cpu(many.data(),many.size(),f,ram),"staged index-capacity fallback plan");
            require(staged->submit(many.data(),many.size(),f,0)&&staged->read_target(0,actual.data(),32),"staged index-capacity fallback draw");
            for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)require(actual[y*32+x]==(x<16?0xffff0000u:0xff00ff00u),"staged large scene ordering");
        }
        ram.tex[0]=sheet0.data();++ram.gen[0];gpu->set_rams(ram);
        unsetenv("VF2_REQUIRE_STAGED_PLAN");
        std::puts("Staged frame tests passed: CPU-only GL boundary, RAM/texture/palette/gamma changes, retired resources and fallback");
    }
    gpu.reset();eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroySurface(display,surface);eglDestroyContext(display,context);eglTerminate(display);
    std::puts("GLES2 raster tests passed: ordering, ownership, checker, clipping, indexed colour and transparent texels");
}
