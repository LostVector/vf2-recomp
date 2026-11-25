// Independent old renderer versus camera-space handoff, using the user's fight.
#include "../../src/host/acm_checkpoint.h"
#include "../../src/gpu/geometry_frame.h"
#include "../../src/host/gpu3d_snapshot.h"
#include <bit>
#include <cstdio>
#include <cstdlib>

static void require(bool ok,const char *what){if(!ok){std::fprintf(stderr,"Camera packet check failed: %s\n",what);std::exit(1);}}
static std::vector<uint32_t> canonical(const std::vector<gpu3d_poly> &polys);
static void synthetic(){
    gpu3d_geometry_frame frame;gpu3d_camera_poly p;auto &g=p.material;g.n=3;
    g.x[0]=0;g.y[0]=0;g.x[1]=1;g.y[1]=0;g.x[2]=0;g.y[2]=1;
    for(int i=0;i<3;++i){g.z[i]=1;g.u[i]=g.x[i]*8;g.v[i]=g.y[i]*8;}
    p.center[0]=17;p.center[1]=19;p.planes[0][0]=1;p.planes[0][3]=0.25f;g.renderer=2;
    gpu3d_poly clipped;require(gpu3d_geometry_frame::project(p,clipped),"crossing-plane clip");
    require(clipped.n==3&&clipped.x[0]==17.25f&&clipped.x[1]==18&&clipped.x[2]==17.25f&&clipped.y[2]==18.25f,"clipped positions");
    require(clipped.u[0]==0.25f&&clipped.u[1]==1&&clipped.v[2]==0.75f,"clipped perspective texture coordinates");
    g.renderer=0;p.planes[0][3]=0;
    frame.polygons.resize(32770,p);frame.final_window=2;
    for(size_t i=0;i<frame.polygons.size();++i){auto &q=frame.polygons[i];q.window=i%4;q.depth=(i*7)%31;q.material.colour=uint32_t(i+1);}
    std::vector<gpu3d_poly> out;frame.prepare(out);size_t at=0;
    for(int w=2;w>=0;--w)for(unsigned z=0;z<31;++z)for(int i=32766;i>=0;--i){
        if(i%4!=w||unsigned(i*7)%31!=z)continue;
        require(at<out.size()&&out[at++].colour==unsigned(i+1),"window/depth/link order and first-survivor capacity");
    }
    require(at==out.size()&&gpu3d_geometry_frame::visible_count(frame.polygons)==32767,"feedback capacity includes hidden windows");
    frame.compact_polygons.resize(frame.polygons.size());
    for(size_t i=0;i<frame.polygons.size();++i){
        const auto &p=frame.polygons[i];auto &c=frame.compact_polygons[i];c.n=p.material.n;c.depth=p.depth;c.window=p.window;c.colour=p.material.colour;
        std::copy_n(p.center,2,c.center);
        for(unsigned j=0;j<4;++j)std::copy_n(p.planes[j],4,c.planes[j]);
        for(unsigned j=0;j<c.n;++j){c.position[j][0]=p.material.x[j];c.position[j][1]=p.material.y[j];c.position[j][2]=p.material.z[j];}
    }
    frame.compact=true;std::vector<gpu3d_poly> compact;frame.prepare(compact);
    require(canonical(compact)==canonical(out),"compact clipping/window/depth/capacity stream");
    frame.compact=false;frame.compact_polygons.clear();
    for(size_t limit:{size_t(1024),size_t(32767),size_t(32768)}){
        frame.polygons.resize(limit,p);
        for(size_t i=0;i<limit;++i){auto &q=frame.polygons[i];q.window=i%4;q.depth=(i*7)%31;q.material.colour=uint32_t(i+1);if(i%7==0)q.material.n=0;}
        std::vector<gpu3d_poly> direct;frame.prepare(out);frame.prepare(direct,true);
        require(canonical(direct)==canonical(out),"direct projection clipping/window/depth/capacity boundaries");
    }
    // Exercise the whole depth/window key, a single window and equal keys.
    frame.polygons.assign(8192,p);
    for(unsigned mode=0;mode<4;++mode){
        frame.final_window=mode==0?223:255;
        for(size_t i=0;i<frame.polygons.size();++i){
            auto &poly=frame.polygons[i];
            poly.window=mode==0?uint8_t((i*17)%256):mode==3?0:255;
            poly.depth=mode==2?32:mode==1?uint16_t((i*13)%89+0x7700):uint16_t(i*509);
            poly.material.colour=uint32_t(i+1);
        }
        std::vector<gpu3d_poly> direct;frame.prepare(out);frame.prepare(direct,true);
        require(canonical(direct)==canonical(out),"full depth/window keys and stable ties");
    }
    frame.polygons.clear();frame.prepare(out,true);require(out.empty(),"empty scene clears output");
}
static std::vector<uint32_t> canonical(const std::vector<gpu3d_poly> &polys){
    std::vector<uint32_t> words;words.push_back(uint32_t(polys.size()));
    for(const auto &p:polys){
        words.insert(words.end(),{p.n,p.renderer,p.checker});for(auto v:p.vp)words.push_back(uint32_t(v));
        for(unsigned i=0;i<p.n;++i){words.push_back(std::bit_cast<uint32_t>(p.x[i]));words.push_back(std::bit_cast<uint32_t>(p.y[i]));}
        if(!(p.renderer&2)){words.push_back(p.colour);continue;}
        for(unsigned i=0;i<p.n;++i)for(float v:{p.z[i],p.u[i],p.v[i]})words.push_back(std::bit_cast<uint32_t>(v));
        words.insert(words.end(),{p.pal,p.lumabase,p.luma,uint32_t(p.texlod),p.sheet,p.texw,p.texh,p.texx,p.texy,p.wrapx,p.wrapy,p.mirrorx,p.mirrory,p.utex,p.utexminlod,p.utexx,p.utexy});
    }
    return words;
}
static void setup(model2a &m,const char *state,bool camera){
    m.release_unused_roms();m.set_byte_link(true);m.set_sound(false);m.reset();m.defer_3d(true);
    require(m.video().camera_packets(camera),"camera configuration");
    if(camera&&std::getenv("VF2_GEOMETRY_WORKER"))require(m.video().geometry_jobs(geometry_execution::worker),"worker configuration");
    std::string error;require(acm_checkpoint::load(m,state,error),error.c_str());
    for(int i=0;i<3;++i)m.run_frame();
}
static void owned_input(){
    std::vector<uint32_t> source(gpu3d_geometry_job::words),reference(source.size());
    uint32_t unused32=0;uint16_t unused16=0;uint8_t unused8=0;
    auto make=[&](std::vector<uint32_t> &list){return std::make_unique<model2_video>(list.data(),&unused32,&unused32,&unused8,&unused16,&unused16,&unused8,reinterpret_cast<uint8_t*>(&unused16),2,reinterpret_cast<uint8_t*>(&unused32),4);};
    auto a=make(reference),b=make(source);
    for(auto *v:{a.get(),b.get()}){v->power_on();v->defer_drawing(true);require(v->camera_packets(true),"owned input camera");}
    require(b->geometry_jobs(geometry_execution::deferred),"deferred geometry backend");
    for(unsigned trial=0;trial<16;++trial){
        std::fill(source.begin(),source.end(),0);
        const unsigned offset=16+trial*17;
        source[0]=0x80000000u|offset*4;source[offset]=0x0bu<<23;
        for(unsigned i=0;i<12;++i)source[offset+1+i]=std::bit_cast<uint32_t>(float(trial*12+i+1));
        source[offset+13]=0x0fu<<23;reference=source;
        a->geo_parse();b->geo_parse();
        // Execution is deferred deliberately, so every original byte is gone
        // before the backend reads the captured list (including its jump).
        std::fill(source.begin(),source.end(),0xffffffffu);
        std::vector<uint8_t> expected,actual;a->save_geometry(expected);b->save_geometry(actual);
        require(expected==actual,"owned display list survives live buffer overwrite");
    }
    require(b->geometry_job_count()==16,"deferred jobs exercised");
    std::puts("Owned geometry list: 16 matrix/jump lists survive source overwrite");
}
static void indexed_contexts(){
    auto shared=std::make_shared<gpu3d_shared_camera_list>();
    gpu3d_geometry_frame indexed,reference;
    indexed.compact=reference.compact=true;
    shared->contexts.resize(3);
    shared->contexts[0].viewport[2]=495;shared->contexts[0].viewport[3]=384;
    shared->contexts[1]=shared->contexts[0];
    shared->contexts[1].viewport[0]=4;
    shared->contexts[1].center[0]=15;
    shared->contexts[1].planes[0][0]=1;
    shared->contexts[1].planes[0][3]=0.25f;
    shared->contexts[2]=shared->contexts[1];
    shared->contexts[2].planes[0][3]=100;
    std::array<uint16_t,0x2000> palette{};
    std::array<uint16_t,0x6000> xlat{};
    std::array<uint8_t,256> gamma{};
    gpu3d_projected_stream stream;
    std::vector<uint32_t> texture(0x80000);
    std::array<uint8_t,0x8000> luma{};
    gpu3d_rams colors{};
    colors.tex[0]=colors.tex[1]=texture.data();
    colors.luma=luma.data();colors.xlat=xlat.data();colors.gamma=gamma.data();
    gpu3d_snapshot snapshot;
    for(size_t limit:{size_t(9),size_t(32768),size_t(0),size_t(64)}){
        shared->polygons.clear();
        reference.compact_polygons.clear();
        palette[1]=0x6f34;
        for(size_t i=0;i<xlat.size();++i)xlat[i]=uint8_t(i+limit+33);
        for(size_t i=0;i<gamma.size();++i)gamma[i]=uint8_t(i*7+limit+2);
        ++colors.gen[3];
        snapshot.capture(colors);
        for(size_t i=0;i<limit;++i){
            auto &p=shared->polygons.emplace_back();
            p.n=3;p.position[0][2]=p.position[1][2]=p.position[2][2]=1;
            p.position[1][0]=1;p.position[2][1]=1;
            p.context=i%3;p.depth=uint16_t(i*509);p.window=i%4;p.header[0]=(i&1)?0x4000:0;
            p.palette_index=1;
            auto &r=reference.compact_polygons.emplace_back();
            static_cast<gpu3d_camera_attributes&>(r)=p;
            static_cast<gpu3d_camera_context&>(r)=shared->contexts[p.context];
            gpu3d_geometry_frame::resolve_compact(r,2,3,palette.data(),xlat.data(),gamma.data(),nullptr);
        }
        std::vector<gpu3d_poly> a,b;
        reference.prepare(a);
        for(bool share:{true,false}){
            const auto live_xlat=xlat;
            const auto live_gamma=gamma;
            indexed.retain_compact(shared,2,3,palette.data(),xlat.data(),gamma.data(),nullptr,
                share?&snapshot.rams():nullptr);
            // Later board writes must not alter the color of the held picture.
            xlat.fill(0);gamma.fill(0);
            for(bool direct:{false,true}){
                indexed.prepare(b,direct);
                require(canonical(a)==canonical(b),"retained picture translation and gamma");
            }
            require(indexed.prepare_projected(stream)==(limit<=32767),"packed projection capacity gate");
            if(limit<=32767){
                stream.expand(b);
                require(canonical(a)==canonical(b),"packed points and snapshot colors");
            }
            indexed.release_shared_compact();
            xlat=live_xlat;
            gamma=live_gamma;
        }
    }
    std::puts("Owned and shared colors survive live RAM mutation in both projection paths");
}
int main(int argc,char **argv){
    if(argc!=3)return 2;
    synthetic();indexed_contexts();owned_input();
    auto a=romset::load(argv[1]),b=romset::load(argv[1]);require(a.ok&&b.ok,"ROM load");
    model2a reference(std::move(a)),camera(std::move(b));setup(reference,argv[2],false);setup(camera,argv[2],true);
    gpu3d_geometry_frame frame,held;
    std::vector<gpu3d_poly> expected,actual,held_expected;uint64_t total=0;
    gpu3d_projected_stream packed;
    const bool shared=std::getenv("VF2_SHARED_CAMERA")!=nullptr;
    for(unsigned i=0;i<120;++i){
        reference.run_frame();if(!i)camera.run_frame();
        require(reference.video().polygon_count_r()==camera.video().polygon_count_r(),"visible polygon feedback");
        std::vector<uint8_t> reference_geometry,camera_geometry;
        reference.video().save_geometry(reference_geometry);camera.video().save_geometry(camera_geometry);
        require(reference_geometry==camera_geometry,"all serialized geometry registers and RAM");
        reference.video().export_picture(reference.video().shown_index(),expected);
        require(camera.video().capture_camera_picture(camera.video().shown_index(),frame),"capture");
        if(shared)require(frame.has_shared_compact(),"shared compact picture used");
        if(shared&&i==0){
            require(camera.video().capture_camera_picture(camera.video().shown_index(),held),"hold a second view");
            held_expected=expected;
        }
        // The owned packet must survive the entire next emulated frame.
        camera.run_frame();frame.prepare(actual,std::getenv("VF2_DIRECT_PROJECTION")!=nullptr);
        if(canonical(expected)!=canonical(actual)){
            std::fprintf(stderr,"Different packet stream at frame %u: %zu/%zu polygons\n",i,expected.size(),actual.size());
            for(size_t p=0;p<std::min(expected.size(),actual.size());++p)if(canonical({expected[p]})!=canonical({actual[p]})){std::fprintf(stderr,"First differing polygon %zu\n",p);break;}
            return 1;
        }
        if(shared){
            require(frame.prepare_projected(packed),"packed projection of retained picture");
            packed.expand(actual);
            require(canonical(expected)==canonical(actual),"packed projected points and materials");
        }
        total+=actual.size();
    }
    if(shared){
        held.prepare(actual,true);
        require(canonical(actual)==canonical(held_expected),"old picture survives 120 replacements");
    }
    // Also read feedback while the geometry list is pending, before presentation.
    reference.run_frame();reference.video().geo_parse();camera.video().geo_parse();
    require(reference.video().polygon_count_r()==camera.video().polygon_count_r(),"pending polygon feedback");
    if(std::getenv("VF2_DIRECT_GEOMETRY")){
        require(camera.video().direct_geometry_polygons()>0,"direct geometry path exercised");
        require(reference.video().direct_geometry_polygons()==0,"independent command-port reference");
        std::printf("Direct geometry polygons: %llu\n",(unsigned long long)camera.video().direct_geometry_polygons());
    }
    if(std::getenv("VF2_GEOMETRY_WORKER")){require(camera.video().geometry_job_count()>120,"geometry worker exercised");camera.video().report_geometry_jobs(stderr);}
    std::printf("Camera packets exact across 120 fight frames, %llu polygons; feedback and owned next-frame lifetime match\n",(unsigned long long)total);
}
