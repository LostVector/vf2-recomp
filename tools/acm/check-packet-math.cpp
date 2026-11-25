#include "../../src/gpu/gles2_tiles.h"
#include <climits>
#include <cstdio>
#include <cstring>
#include <random>
static void require(bool ok,const char *what){if(!ok){std::fprintf(stderr,"Packet math failed: %s\n",what);std::exit(1);}}
int main(){
    for(unsigned side=1;side<=UINT16_MAX;++side){
        const int expected=std::max(0,int(std::log2(side))-1);
        require(vf2_packet_math::texture_mip_limit(uint16_t(side),UINT16_MAX)==expected,"integer mip limit");
    }
    require(vf2_packet_math::texture_mip_limit(0,256)==0,"zero texture size");
    for(int64_t k=-65536;k<=65535;++k){
        if(std::bit_cast<uint64_t>(vf2_packet_math::lod_boundary(k))!=std::bit_cast<uint64_t>(std::exp2(-double(k)/256))){
            std::fprintf(stderr,"LOD mismatch at %lld\n",(long long)k);return 1;
        }
    }
    for(int64_t k:{INT64_MIN,INT64_MAX,int64_t(-65537),int64_t(65536)})
        require(vf2_packet_math::lod_boundary(k)==std::exp2(-double(k)/256),"outside-range fallback");
    for(unsigned level=0;level<256;++level)
        require(vf2_packet_math::micro_scale(level)==std::exp2(std::exp2(double(level))),"microtexture scale");
    for(double cell:{0.,-0.,1.,-1.,2.,-2.,4503599627370495.,4503599627370496.,9007199254740991.,9007199254740992.,double(INFINITY),double(-INFINITY),double(NAN)})
        require(vf2_packet_math::odd_cell(cell)==(std::fmod(std::abs(cell),2.)>=1),"mirror parity edge");
    unsigned transform_cases=0;
    for(int x:{INT_MIN,-16777217,-1000,-5,-2,-1,0,1,2,5,1000,16777217,INT_MAX})
        for(int y:{INT_MIN,-17,-2,-1,0,1,2,17,INT_MAX})
            for(int mode=0;mode<4;++mode)for(int level=0;level<=5;++level){
                gpu3d_poly p{};p.texw=256;p.texh=32;
                p.mirrorx=mode&1;p.mirrory=(mode>>1)&1;
                float expected[4],actual[4];
                vf2_tiles::transform(expected,p,level,double(x)+.5,double(y)+.5,false);
                vf2_tiles::cell_transform(actual,p,level,x,y);
                require(!std::memcmp(expected,actual,sizeof actual),"integer cell transform bytes");
                ++transform_cases;
            }
    std::mt19937 random(0x2ac701);
    uint64_t hash=1469598103934665603ull,vertices=0;
    vf2_tiles::indexed_packet cached;std::vector<vf2_tiles::vertex> cached_array;
    vf2_tiles::packet_storage shared,expected_shared;
    unsigned appended_bands=0;
    std::vector<vf2_tiles::indexed_packet> reference_slots,limited_slots,inactive_slots;
    int reference_count=0,limited_count=0,inactive_count=0;
    for(unsigned test=0;test<1000;++test){
        gpu3d_poly p{};p.n=4;p.renderer=2;p.texw=32u<<(test%3);p.texh=32u<<((test/3)%3);
        p.utexminlod=test%2;p.utex=(test>>1)&1;p.mirrorx=(test>>2)&1;p.mirrory=(test>>3)&1;p.wrapx=(test>>4)&1;p.wrapy=(test>>5)&1;
        p.texlod=int(random()%4001)-2000;
        const float x=float(int(random()%550)-50),y=float(int(random()%420)-40),w=1+random()%80,h=1+random()%60;
        const float u=float(int(random()%200)-100),v=float(int(random()%200)-100);
        for(unsigned i=0;i<4;++i){
            p.x[i]=x+((i==1||i==2)?w:0);p.y[i]=y+(i>=2?h:0);
            p.z[i]=0.5f+float(random()%1024)/1024;
            p.u[i]=(u+((i==1||i==2)?p.texw:0))*p.z[i];p.v[i]=(v+(i>=2?p.texh:0))*p.z[i];
        }
        const int max_level=std::max(0,int(std::log2(std::min(p.texw,p.texh)))-1);
        auto range_poly=p;
        if(test%4==0){
            range_poly.texlod=0;
            range_poly.z[0]=std::nextafter(1.f,0.f);
            range_poly.z[1]=1.f;
            range_poly.z[2]=std::nextafter(1.f,2.f);
            range_poly.z[3]=1.f;
        }
        int reference_first=0,limited_first=0,inactive_first=0;
        reference_count=vf2_tiles::sparse_indexed_packets<true>(reference_slots,reference_first,
            reference_count,range_poly,max_level,0,0,496,384);
        limited_count=vf2_tiles::sparse_indexed_packets<true>(limited_slots,limited_first,
            limited_count,range_poly,max_level,0,0,496,384,true);
        inactive_count=vf2_tiles::sparse_indexed_packets<true>(inactive_slots,inactive_first,
            inactive_count,range_poly,max_level,0,0,496,384,true,false,false,false,true);
        require(reference_first==limited_first&&reference_count==limited_count&&
                reference_slots.size()==limited_slots.size(),"mip scan range");
        require(inactive_first==reference_first&&inactive_count==reference_count,"live mip range with stale storage");
        for(int i=0;i<inactive_count;++i){
            const auto &a=reference_slots[i],&b=inactive_slots[i];
            require(a.indices==b.indices&&a.native_repeat==b.native_repeat&&a.vertices.size()==b.vertices.size(),
                "reused live packet shape");
            if(!a.vertices.empty())
                require(!std::memcmp(a.vertices.data(),b.vertices.data(),a.vertices.size()*sizeof(a.vertices[0])),
                    "reused live packet bytes");
        }
        for(size_t i=0;i<reference_slots.size();++i){
            const auto &a=reference_slots[i];
            const auto &b=limited_slots[i];
            require(a.indices==b.indices&&a.native_repeat==b.native_repeat&&a.vertices.size()==b.vertices.size(),"mip scan packet shape");
            if(!a.vertices.empty())
                require(!std::memcmp(a.vertices.data(),b.vertices.data(),a.vertices.size()*sizeof(a.vertices[0])),"mip scan packet bytes");
        }
        for(int band=-1;band<=max_level;++band){
            auto packet=vf2_tiles::packet(p,band,max_level,0,0,496,384,true);vertices+=packet.size();
            auto indexed=vf2_tiles::packet_indexed(p,band,max_level,0,0,496,384,true);
            vf2_tiles::indexed_packet cell_indexed,compact_indexed;
            const double clip_upper=band<0?INFINITY:vf2_packet_math::lod_boundary(int64_t(p.texlod)+int64_t(band)*128);
            const double clip_lower=band==max_level?0:vf2_packet_math::lod_boundary(int64_t(p.texlod)+int64_t(band+1)*128);
            const auto [minq,maxq]=vf2_tiles::depth_range(p);
            if(!(maxq<clip_lower||minq>clip_upper)){
                vf2_tiles::clip_packet_into<true,true>(cell_indexed,p,band,max_level,0,0,496,384,true,clip_lower,clip_upper,true);
                vf2_tiles::clip_packet_into<true,true>(compact_indexed,p,band,max_level,0,0,496,384,true,clip_lower,clip_upper,true,true);
                vf2_tiles::indexed_packet fast_viewport;
                vf2_tiles::clip_indexed_band<true>(fast_viewport,p,band,max_level,0,0,496,384,clip_lower,
                    clip_upper,false,true,true,true);
                const auto same_bytes=[](const auto &a,const auto &b){
                    return a.size()==b.size()&&(a.empty()||!std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0])));
                };
                require(fast_viewport.indices==compact_indexed.indices&&
                    fast_viewport.native_repeat==compact_indexed.native_repeat&&
                    same_bytes(fast_viewport.vertices,compact_indexed.vertices)&&
                    same_bytes(fast_viewport.ordinary,compact_indexed.ordinary),"fast viewport packet bytes");
                vf2_tiles::packet_writer writer(shared);
                vf2_tiles::clip_indexed_band<true>(writer,p,band,max_level,0,0,496,384,clip_lower,clip_upper,false,true,true);
                require(writer.vertices.size()==compact_indexed.vertices.size()&&
                    writer.ordinary.size()==compact_indexed.ordinary.size()&&
                    writer.indices.size()==compact_indexed.indices.size()&&writer.native_repeat==compact_indexed.native_repeat,
                    "shared packet extent and repeat mode");
                expected_shared.vertices.insert(expected_shared.vertices.end(),compact_indexed.vertices.begin(),compact_indexed.vertices.end());
                expected_shared.ordinary.insert(expected_shared.ordinary.end(),compact_indexed.ordinary.begin(),compact_indexed.ordinary.end());
                expected_shared.indices.insert(expected_shared.indices.end(),compact_indexed.indices.begin(),compact_indexed.indices.end());
                ++appended_bands;
            }
            require(cell_indexed.indices==compact_indexed.indices&&cell_indexed.native_repeat==compact_indexed.native_repeat&&
                cell_indexed.vertex_count()==compact_indexed.vertex_count(),"compact cell packet shape");
            for(size_t i=0;i<cell_indexed.vertex_count();++i){
                const auto expanded=compact_indexed.vertex_at(i);
                require(!std::memcmp(&expanded,&cell_indexed.vertices[i],sizeof(expanded)),"compact cell packet bytes");
            }
            require(indexed.indices==cell_indexed.indices&&indexed.native_repeat==cell_indexed.native_repeat&&
                indexed.vertices.size()==cell_indexed.vertices.size(),"integer cell packet shape");
            if(!indexed.vertices.empty())
                require(!std::memcmp(indexed.vertices.data(),cell_indexed.vertices.data(),
                    indexed.vertices.size()*sizeof(indexed.vertices[0])),"integer cell packet bytes");
            vf2_tiles::packet_into<true,true>(cached,p,band,max_level,0,0,496,384,true);
            vf2_tiles::packet_into<false,true>(cached_array,p,band,max_level,0,0,496,384,true);
            require(cached_array.size()==packet.size()&&cached.indices.size()==packet.size(),"stack packet count");
            if(!packet.empty())require(!std::memcmp(cached_array.data(),packet.data(),packet.size()*sizeof(packet[0])),"stack clipping equivalence");
            require(indexed.vertices.size()<=packet.size(),"only referenced fan vertices retained");
            require(indexed.indices.size()==packet.size(),"indexed triangle count");
            for(size_t i=0;i<packet.size();++i){
                require(indexed.indices[i]<indexed.vertices.size(),"index range");
                const auto &got=indexed.vertices[indexed.indices[i]];
                require(!std::memcmp(&got,&packet[i],sizeof(got)),"indexed vertex stream equivalence");
                require(cached.indices[i]<cached.vertices.size()&&!std::memcmp(&cached.vertices[cached.indices[i]],&packet[i],sizeof(got)),"stack indexed equivalence");
            }
            for(const auto &vertex:packet){
                const float values[]={vertex.p.x,vertex.p.y,vertex.p.q,vertex.p.u,vertex.p.v,vertex.a[0],vertex.a[1],vertex.a[2],vertex.a[3],vertex.b[0],vertex.b[1],vertex.b[2],vertex.b[3]};
                for(float value:values){hash^=std::bit_cast<uint32_t>(value);hash*=1099511628211ull;}
            }

        }
    }
    unsigned accepted=0,rejected=0;
    for(unsigned test=0;test<12000;++test){
        gpu3d_poly p{};p.n=3+test%6;p.texw=32u<<(test%5);p.texh=32u<<((test/5)%5);
        p.renderer=2;p.wrapx=test&1;p.wrapy=(test>>1)&1;
        p.mirrorx=(test>>2)&1;p.mirrory=(test>>3)&1;
        const int band=int(test%5)-1;
        const double lower=0.5,upper=band<0?INFINITY:2.0;
        for(int i=0;i<p.n;++i){
            const double angle=6.283185307179586*i/p.n;
            p.x[i]=float(248+248*std::cos(angle));p.y[i]=float(192+192*std::sin(angle));
            const float q=p.z[i]=.5f+float(random()%1024)/1024;
            p.u[i]=(q*p.texw)*float(random()%1024)/1024;
            p.v[i]=(q*p.texh)*float(random()%1024)/1024;
        }
        const int last=p.n-1;
        const int kind=int(test/5%10);
        switch(kind){
        case 0:p.u[last]=-0.f;p.v[last]=0.f;p.z[last]=float(lower);break;
        case 1:p.u[last]=std::nextafter(p.z[last]*p.texw,0.f);break;
        case 2:p.x[0]=std::nextafter(496.f,INFINITY);break;
        case 3:p.u[last]=p.z[last]*p.texw;break;
        case 4:p.utex=true;break;
        case 5:p.z[last]=std::nextafter(float(lower),0.f);break;
        case 6:p.x[1]=p.x[0];p.y[1]=p.y[0];break;
        case 7:p.v[last]=-std::numeric_limits<float>::min();break;
        case 8:p.z[last]=2.0f;p.u[last]=0.f;break;
        }
        const bool expect=kind!=2&&kind!=3&&kind!=5&&kind!=7&&!(kind==4&&band<0);
        vf2_tiles::indexed_packet baseline,fast;
        vf2_tiles::clip_packet_into<true,true>(baseline,p,band,5,0,0,496,384,true,lower,upper);
        const bool used=vf2_tiles::origin_indexed_fan(fast,p,band,0,0,496,384,lower,upper);
        require(used==expect,"origin membership");
        if(!used){
            ++rejected;
            require(fast.indices.empty()&&fast.vertices.empty(),"origin rejection leaves output untouched");
            vf2_tiles::clip_packet_into<true,true>(fast,p,band,5,0,0,496,384,true,lower,upper);
        }else ++accepted;
        require(baseline.indices==fast.indices&&baseline.native_repeat==fast.native_repeat&&
            baseline.vertices.size()==fast.vertices.size(),"origin packet shape");
        if(!baseline.vertices.empty())
            require(!std::memcmp(baseline.vertices.data(),fast.vertices.data(),
                baseline.vertices.size()*sizeof(baseline.vertices[0])),"origin packet bytes");
    }
    std::printf("Origin packet fixture: %u exact fans and %u fallback cases\n",accepted,rejected);
    unsigned retained=0,fallback=0;
    for(unsigned test=0;test<18000;++test){
        gpu3d_poly p{};
        p.n=3+test%6;p.renderer=2;p.texw=32u<<(test%5);p.texh=32u<<((test/5)%5);
        p.wrapx=test&1;p.wrapy=(test>>1)&1;p.mirrorx=(test>>2)&1;p.mirrory=(test>>3)&1;
        p.utex=(test>>4)&1;p.utexminlod=(test>>5)&3;
        const bool base=(test/7)%2==0;
        const double micro=base&&p.utex?vf2_packet_math::micro_scale(p.utexminlod):0;
        const double sx=micro?std::min(1.,128/(p.texw*micro)):1;
        const double sy=micro?std::min(1.,128/(p.texh*micro)):1;
        const int cx=int(test%17)-8,cy=int(test%19)-9;
        for(unsigned i=0;i<p.n;++i){
            const double angle=6.283185307179586*i/p.n;
            p.x[i]=float(248+220*std::cos(angle));p.y[i]=float(192+160*std::sin(angle));
            p.z[i]=base?1.2f+i*.01f:.8f+i*.01f;
            p.u[i]=float((cx+.2+i*.03)*sx*(p.z[i]*p.texw));
            p.v[i]=float((cy+.3+i*.03)*sy*(p.z[i]*p.texh));
        }
        const unsigned last=p.n-1,kind=(test/2)%11;
        if(kind==1)p.x[last]=-1;
        if(kind==2)p.u[last]=float((cx+1.2)*sx*(p.z[last]*p.texw));
        if(kind==3)p.z[last]=1.f;
        if(kind==4){p.x[1]=p.x[0];p.y[1]=p.y[0];}
        if(kind==5)p.z[last]=NAN;
        if(kind==6)p.v[last]=INFINITY;
        const bool expect=kind==0||kind>=7;
        const int max_level=vf2_packet_math::texture_mip_limit(p.texw,p.texh);
        const bool indexed_transform=test&1;
        vf2_tiles::retained_fan fan;
        const bool used=vf2_tiles::retain_fan(fan,p,max_level,0,0,496,384,indexed_transform);
        require(used==expect,"retained fan eligibility");
        std::array<gpu3d_projected_point,8> points{};
        for(unsigned i=0;i<p.n;++i)points[i]=projected_point(p,i);
        gpu3d_projected_poly packed{};
        packed.points=points.data();packed.n=p.n;packed.texw=p.texw;packed.texh=p.texh;
        packed.texlod=p.texlod;packed.utex=p.utex;packed.utexminlod=p.utexminlod;
        packed.wrapx=p.wrapx;packed.wrapy=p.wrapy;packed.mirrorx=p.mirrorx;packed.mirrory=p.mirrory;
        vf2_tiles::retained_fan fused;
        const bool used_fused=vf2_tiles::retain_fan(fused,packed,max_level,0,0,496,384,indexed_transform,false,true);
        require(used==used_fused,"fused retained fan eligibility");
        if(used)require(fused.band==fan.band&&!std::memcmp(fused.a,fan.a,sizeof fan.a)&&
                        !std::memcmp(fused.b,fan.b,sizeof fan.b),"fused retained transforms");
        vf2_tiles::retained_fan bounded;
        const bool used_bounds=vf2_tiles::retain_fan(bounded,p,max_level,0,0,496,384,indexed_transform,true);
        require(used==used_bounds,"cell-edge retained eligibility");
        if(!used){++fallback;continue;}
        ++retained;
        require(bounded.band==fan.band&&!std::memcmp(bounded.a,fan.a,sizeof fan.a)&&
                !std::memcmp(bounded.b,fan.b,sizeof fan.b),"cell-edge retained transforms");
        std::vector<vf2_tiles::indexed_packet> reference;
        int band=0;
        const int count=vf2_tiles::sparse_indexed_packets<true>(
            reference,band,0,p,max_level,0,0,496,384,true,true,indexed_transform);
        require(count==1&&fan.band==band&&fan.polygon==&p,"retained band and source");
        require(reference[0].vertices.size()==p.n&&reference[0].indices.size()==fan.index_count(),
                "retained topology and count");
        require(std::equal(reference[0].indices.begin(),reference[0].indices.end(),
                           vf2_tiles::fan_indices),"retained index order");
        for(unsigned i=0;i<p.n;++i){
            const auto &ref=reference[0].vertices[i];
            const auto pos=fan.position(i);
            require(!std::memcmp(&pos,&ref.p,sizeof pos)&&!std::memcmp(fan.a,ref.a,sizeof fan.a)&&
                    !std::memcmp(fan.b,ref.b,sizeof fan.b),"retained projected geometry and transforms");
        }
    }
    unsigned cell_edges=0;
    for(int cell:{-32768,-32767,-101,-2,-1,0,1,2,101,32766,32767}){
        for(unsigned j=0;j<100;++j)for(bool upper:{false,true})for(int side:{-1,0,1}){
            gpu3d_poly p{};p.n=3;p.renderer=2;p.texw=32;p.texh=64;
            p.x[1]=16;p.y[2]=16;
            const float q=p.z[0]=p.z[1]=p.z[2]=.75f+j*.001f;
            const float w=q*p.texw;
            p.u[0]=p.u[1]=float((cell+.5)*w);
            p.u[2]=float(double(cell+int(upper))*w);
            if(side)p.u[2]=std::nextafter(p.u[2],side>0?INFINITY:-INFINITY);
            vf2_tiles::retained_fan ref,fast;
            const bool a=vf2_tiles::retain_fan(ref,p,4,0,0,496,384,true);
            const bool b=vf2_tiles::retain_fan(fast,p,4,0,0,496,384,true,true);
            require(a==b,"signed cell edge membership");
            if(a)require(!std::memcmp(ref.a,fast.a,sizeof ref.a)&&!std::memcmp(ref.b,fast.b,sizeof ref.b),
                         "signed cell edge transforms");
            ++cell_edges;
        }
    }
    std::printf("Exact retained cell-edge checks: %u\n",cell_edges);
    std::printf("Retained fan checks: %u exact retained paths and %u fallback paths\n",retained,fallback);
    if(std::getenv("VF2_NATIVE_REPEAT_MATERIAL")){
        gpu3d_poly p{};p.n=4;p.renderer=2;p.texw=p.texh=256;p.texx=1792;
        p.wrapx=p.wrapy=true;
        for(unsigned i=0;i<4;++i){
            p.x[i]=i==1||i==2?100.f:50.f;p.y[i]=i>=2?100.f:50.f;
            p.z[i]=1.f;p.u[i]=p.x[i]*4.f;p.v[i]=p.y[i]*4.f;
        }
        vf2_tiles::indexed_packet before,after;
        vf2_tiles::clip_packet_into<true,true>(before,p,1,5,0,0,496,384,true,0,INFINITY);
        vf2_tiles::clip_packet_into<true,true>(after,p,1,5,0,0,496,384,true,0,INFINITY,true);
        require(before.native_repeat&&after.native_repeat&&before.indices==after.indices,"native repeat retained");
        require(!before.vertices.empty()&&before.vertices.size()==after.vertices.size(),"native repeat vertex count");
        require(!std::memcmp(before.vertices.data(),after.vertices.data(),
                            before.vertices.size()*sizeof(before.vertices[0])),"native repeat transform bytes");
    }
    vf2_tiles::stack_polygon spill;vf2_tiles::vector_polygon vector;
    for(unsigned i=0;i<100;++i){
        const double a=double(i)*6.283185307179586/100;
        vf2_tiles::point p{float(std::cos(a)*100),float(std::sin(a)*100),1,float(i),float(i*2)};
        spill.push_back(p);vector.push_back(p);
    }
    auto distance=[](const auto &p){return double(p.x)+20;};spill.clip_by(distance);vector.clip_by(distance);
    auto spill_copy=spill;auto vector_copy=vector;
    auto other=[](const auto &p){return 50-double(p.y);};spill_copy.clip_by(other);vector_copy.clip_by(other);
    require(spill_copy.size()==vector_copy.size(),"spill/copy count");
    for(size_t i=0;i<spill_copy.size();++i)require(!std::memcmp(&spill_copy[i],&vector_copy[i],sizeof(vf2_tiles::point)),"spill/copy bytes");
    require(vertices==29325&&hash==0x5e8e40c260fbe83full,"legacy vertex stream golden");
    std::printf("Packet math passed: 131072 exact boundaries, scale/parity edges; %llu vertices, hash %016llx\n",(unsigned long long)vertices,(unsigned long long)hash);
    std::printf("Cell transforms matched %u exact scale/bias pairs and 1000 packet streams\n",transform_cases);
    std::puts("Mip limits matched all positive u16 dimensions; limited scan matched 1000 reusable packet streams");
    std::puts("Keeping inactive slots matched 1000 live packet streams");
    const auto same_bytes=[](const auto &a,const auto &b){
        return a.size()==b.size()&&(a.empty()||!std::memcmp(a.data(),b.data(),a.size()*sizeof(a.front())));
    };
    require(same_bytes(shared.vertices,expected_shared.vertices)&&
        same_bytes(shared.ordinary,expected_shared.ordinary)&&shared.indices==expected_shared.indices,
        "all appended packet payloads after storage growth");
    std::printf("Shared packet storage matched %u appended bands\n",appended_bands);
}
