// Indexed Model 2 tiles packed into small GLES2 texture pages. A one-pixel border
// reproduces each tile's independent repeat/clamp filtering behaviour.
#pragma once
#include "gpu3d.h"
#include "gles2_atlas_decode.h"
#include "gles2_atlas_edges.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <bitset>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <utility>
#include <vector>
namespace vf2_batch {
struct tile {unsigned x,y,w,h,page,side;float transform[4]{};};
struct tile_hash {size_t operator()(uint64_t key)const noexcept{return uint32_t(key)^(uint32_t(key>>32)*0x9e3779b9u);}};
class tile_atlas {
    struct shelf {unsigned y,height,x;};
    struct backing {GLuint name=0;bool claimed=false;};
    struct page {
        backing texture[2];
        unsigned side,next_y=0;
        std::vector<shelf> shelves;
        bool repeat=false;
    };
    // Mesh draw keys use the high 16 bits for the mesh group.
    static constexpr unsigned luma_bit=1u<<15;
    static size_t atlas_limit(){
        static const size_t value=[]{
            if(const char *text=std::getenv("VF2_ATLAS_LIMIT_MB")){
                const long mb=std::strtol(text,nullptr,10);
                if(mb>0)return size_t(mb)*1024*1024;
            }
            return size_t(12)*1024*1024;
        }();
        return value;
    }
    const size_t limit=atlas_limit();
    static GLenum format(bool luma){return luma?GL_LUMINANCE:GL_LUMINANCE_ALPHA;}
    bool luma_image(unsigned id)const{return fixed_luma&&(id&luma_bit);}
    unsigned logical_page(unsigned id)const{return fixed_luma?id&~luma_bit:id;}
    const backing &image(unsigned id)const{return pages[logical_page(id)].texture[luma_image(id)];}
    std::vector<page> pages;
    struct page_stamp {unsigned index;uint32_t revision;};
    struct cached_tile {
        tile placed;
        uint32_t generation;
        std::vector<uint32_t> source;
        std::vector<page_stamp> dependencies;
        const uint32_t *page_source=nullptr;
        unsigned uniform_axes=0;
    };
    // The records stay at stable addresses when another tile is inserted.
    // The slot keeps the immutable placement and its generation inline so a
    // steady-state lookup costs one cache line instead of chasing the record.
    struct index_slot { uint64_t key=0; cached_tile *entry=nullptr; tile placed{}; uint32_t generation=0; };
    std::deque<cached_tile> entries;
    std::vector<index_slot> lookup_slots;

    static auto &slot_for(auto &slots,uint64_t key){
        uint32_t hash=uint32_t(tile_hash{}(key));
        hash^=hash>>16;
        hash*=0x7feb352du;
        hash^=hash>>15;
        const size_t mask=slots.size()-1;
        size_t at=hash&mask;
        while(slots[at].entry&&slots[at].key!=key)at=(at+1)&mask;
        return slots[at];
    }

    cached_tile *find_entry(uint64_t key){
        return lookup_slots.empty()?nullptr:slot_for(lookup_slots,key).entry;
    }

    cached_tile &insert_entry(uint64_t key,cached_tile entry){
        // Keep at least half the slots free for short probe chains.
        if(entries.size()*2>=lookup_slots.size()){
            std::vector<index_slot> larger(lookup_slots.empty()?64:lookup_slots.size()*2);
            for(const auto &slot:lookup_slots)
                if(slot.entry)slot_for(larger,slot.key)=slot;
            lookup_slots.swap(larger);
        }
        auto &record=entries.emplace_back(std::move(entry));
        auto &row=slot_for(lookup_slots,key);
        row.key=key;row.entry=&record;row.placed=record.placed;row.generation=record.generation;
        return record;
    }

    size_t allocated=0;
    struct upload {unsigned page,x,y,w,h;std::vector<uint8_t> bytes;};
    std::vector<upload> uploads;
    std::vector<GLuint> retired;
    bool deferred=false;
    const bool snapshot_decode=std::getenv("VF2_ATLAS_SNAPSHOT_DECODE")!=nullptr;
    static bool create_texture(page &p,bool luma){
        auto &texture=p.texture[luma].name;
        glActiveTexture(GL_TEXTURE0);glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,p.repeat?GL_REPEAT:GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,p.repeat?GL_REPEAT:GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,format(luma),p.side,p.side,0,format(luma),GL_UNSIGNED_BYTE,nullptr);
        if(glGetError()!=GL_NO_ERROR){glDeleteTextures(1,&texture);texture=0;return false;}
        return true;
    }

    bool claim_backing(page &p,bool luma){
        auto &back=p.texture[luma];
        if(back.claimed)return true;
        const size_t bytes=size_t(p.side)*p.side*(luma?1:2);
        if(allocated+bytes>limit || (!deferred&&!create_texture(p,luma))){++claim_failures;return false;}
        allocated+=bytes;
        back.claimed=true;
        return true;
    }

    const bool fixed_luma=std::getenv("VF2_FIXED_ATLAS_LUMA")!=nullptr;
    const bool page_cache=std::getenv("VF2_ATLAS_PAGE_CACHE")!=nullptr;
    const bool record_uniform_axes=std::getenv("VF2_ATLAS_UNIFORM_AXES")!=nullptr;
    struct tile_request {
        unsigned w,h,x,y,sheet;
        bool transparent;
        uint64_t key;
    };
    template<class Material>
    static tile_request describe(const Material &p,int level,bool micro,bool repeat){
        const unsigned w=micro?128:p.texw>>level,h=micro?128:p.texh>>level;
        const unsigned x=micro?p.utexx:((uint32_t(p.texx)-2048)>>level)&2047;
        const unsigned y=micro?p.utexy:((uint32_t(p.texy)-1024)>>level)&1023;
        const unsigned sheet=p.sheet^(micro?1:(level&1));
        const bool transparent=p.renderer&1;
        const uint64_t key=x|(uint64_t(y)<<11)|(uint64_t(w)<<21)|(uint64_t(h)<<33)|(uint64_t(sheet)<<45)|
            (uint64_t(bool(p.wrapx))<<46)|(uint64_t(bool(p.wrapy))<<47)|(uint64_t(transparent)<<48)|(uint64_t(repeat)<<49);
        return {w,h,x,y,sheet,transparent,key};
    }
public:
    bool records_uniform_axes()const{return record_uniform_axes;}
    template<class Material>
    unsigned current_uniform_axes(const gpu3d_rams &rams,const Material &p,int level)const{
        if(lookup_slots.empty()||p.sheet>1)return 0;
        const auto request=describe(p,level,false,false);
        const auto &slot=slot_for(lookup_slots,request.key);
        if(!slot.entry||slot.generation!=rams.gen[request.sheet])return 0;
        return slot.entry->uniform_axes;
    }
    static size_t capacity_bytes(){return atlas_limit();}
    size_t allocated_bytes()const{return allocated;}
    // Counts atlas page claims that the byte budget refused. A frame that
    // trips this can discard the stale pages and rebuild in one pass.
    uint64_t claim_failures=0;
    uint64_t uploaded=0,clears=0;
    uint64_t page_hits=0,source_checks=0,source_words=0;
    tile_atlas()=default;
    tile_atlas(const tile_atlas &)=delete;
    tile_atlas &operator=(const tile_atlas &)=delete;
    ~tile_atlas(){
        if(const char *path=std::getenv("VF2_ATLAS_LAYOUT")){
            if(FILE *out=std::fopen(path,"w")){
                std::fprintf(out,"VF2_ATLAS_LAYOUT_V1 %llu\n",(unsigned long long)clears);
                for(const auto &slot:lookup_slots)
                    if(slot.entry)std::fprintf(out,"%016llx %u\n",(unsigned long long)slot.key,logical_page(slot.entry->placed.page));
                if(std::fclose(out))std::perror("close atlas layout");
            }else std::perror("write atlas layout");
        }
        deferred=false;
        clear();
    }
    bool init(){return true;}
    bool add_page(unsigned side,bool repeat=false,bool luma=false){
        if(fixed_luma&&pages.size()>=luma_bit)return false;
        page p{{},side,0,{},repeat};
        if(!claim_backing(p,luma))return false;
        pages.push_back(std::move(p));return true;
    }
    void defer_uploads(bool on){deferred=on;}
    // The caller invokes this only after its prior-frame GPU fence completes.
    bool flush(){
        for(GLuint texture:retired)glDeleteTextures(1,&texture);retired.clear();
        for(auto &p:pages)for(bool luma:{false,true}){
            const auto &back=p.texture[luma];
            if(back.claimed&&!back.name&&!create_texture(p,luma))return false;
        }
        for(const auto &u:uploads){
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,image(u.page).name);
            glPixelStorei(GL_UNPACK_ALIGNMENT,1);
            glTexSubImage2D(GL_TEXTURE_2D,0,u.x,u.y,u.w,u.h,format(luma_image(u.page)),GL_UNSIGNED_BYTE,u.bytes.data());
            uploaded+=u.bytes.size();
        }
        if(!uploads.empty())glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        uploads.clear();return glGetError()==GL_NO_ERROR;
    }
    GLuint handle(unsigned index)const{return image(index).name;}
    bool prewarm(const gpu3d_rams &rams,uint64_t key){
        // The key encodes the final mip or microtile, so reconstruct its
        // coordinates as a level-zero material on the recorded sheet.
        struct recipe {
            unsigned texw,texh,texx,texy,sheet,renderer,wrapx,wrapy;
            unsigned utexx=0,utexy=0;
        };
        const recipe p{unsigned((key>>21)&4095),unsigned((key>>33)&4095),
            unsigned(key&2047)+2048,unsigned((key>>11)&1023)+1024,
            unsigned((key>>45)&1),unsigned((key>>48)&1),unsigned((key>>46)&1),unsigned((key>>47)&1)};
        tile placed;
        return get(rams,p,0,false,key&(1ull<<49),placed);
    }
    size_t page_count()const{return pages.size();}
    size_t resident()const{return entries.size();}
    void clear(){
        if(resident())clears++;
        lookup_slots.clear();entries.clear();uploads.clear();
        for(auto &p:pages)for(auto &back:p.texture)if(back.name){
            if(deferred)retired.push_back(back.name);else glDeleteTextures(1,&back.name);
        }
        if(!deferred){for(GLuint texture:retired)glDeleteTextures(1,&texture);retired.clear();}
        pages.clear();allocated=0;
    }
    static void map(float transform[4],const tile &t){
        float sx=float(t.w)/t.side,sy=float(t.h)/t.side;
        transform[0]*=sx;transform[1]*=sy;
        transform[2]=transform[2]*sx+float(t.x+1)/t.side;
        transform[3]=transform[3]*sy+float(t.y+1)/t.side;
    }
    template<class Material>
    bool get_pair(const gpu3d_rams &rams,const Material &p,
                  int band,int max_level,bool native_repeat,tile &a,tile &b){
        const int first=std::max(0,band);
        const int second=band<0?0:std::min(band+1,max_level);
        const bool micro=band<0&&p.utex;
        // Preserve the original atlas placements even when rendering this
        // pair from standalone textures.
        if(!get(rams,p,first,false,false,a))return false;
        // The top mip pairs with itself; reuse it instead of resolving twice.
        if(second==first&&!micro)b=a;
        else if(!get(rams,p,second,micro,false,b))return false;
        if(!native_repeat)return true;
        tile repeat_a,repeat_b;
        if(!get(rams,p,first,false,true,repeat_a))return false;
        if(second==first&&!micro)repeat_b=repeat_a;
        else if(!get(rams,p,second,micro,true,repeat_b))return false;
        a=repeat_a;b=repeat_b;
        return true;
    }
    template<class Material>
    bool pair_current(const gpu3d_rams &rams,const Material &p,int band,int max_level,bool native_repeat)const{
        if(lookup_slots.empty())return false;
        const int first=std::max(0,band);
        const int second=band<0?0:std::min(band+1,max_level);
        const bool micro=band<0&&p.utex;
        const auto current=[&](int level,bool use_micro,bool repeat){
            const auto request=describe(p,level,use_micro,repeat);
            const auto &slot=slot_for(lookup_slots,request.key);
            return slot.entry&&slot.generation==rams.gen[request.sheet];
        };
        if(!current(first,false,false)||((first!=second||micro)&&!current(second,micro,false)))return false;
        return !native_repeat||(current(first,false,true)&&
            ((first==second&&!micro)||current(second,micro,true)));
    }
    template<class Material>
    bool get(const gpu3d_rams &rams,const Material &p,int level,bool micro,bool repeat,tile &out){
        const auto [w,h,x,y,sheet,transparent,key]=describe(p,level,micro,repeat);
        const bool luma=fixed_luma&&!transparent;
        const unsigned channels=luma?1:2;
        // The slot carries the placement and generation, so a warm lookup only
        // touches the slot instead of the record it points at.
        cached_tile *found=nullptr;
        index_slot *slot=lookup_slots.empty()?nullptr:&slot_for(lookup_slots,key);
        if(slot)found=slot->entry;
        const uint32_t generation=rams.gen[sheet];
        if(found&&slot->generation==generation){out=slot->placed;return true;}
        const uint32_t *revisions=page_cache?rams.tex_page_gen[sheet]:nullptr;
        if(found&&revisions&&found->page_source==revisions&&!found->dependencies.empty()){
            bool same=true;
            for(const auto &stamp:found->dependencies)if(revisions[stamp.index]!=stamp.revision){same=false;break;}
            if(same){++page_hits;found->generation=generation;if(slot)slot->generation=generation;out=slot?slot->placed:found->placed;return true;}
        }
        // A packed word covers four columns by two rows. Retain all words
        // touched by the tile (including partial edge words), so generation
        // changes can be checked without decoding every pixel again.
        unsigned bw=((x&3)+w+3)/4,bh=((y&1)+h+1)/2;
        std::vector<uint32_t> source;source.reserve(size_t(bw)*bh);
        std::vector<page_stamp> dependencies;
        const bool collect=revisions&&(!found||found->dependencies.empty());
        std::bitset<gpu3d_rams::texture_pages> touched;
        if(revisions&&!collect){
            dependencies=found->dependencies;
            for(auto &stamp:dependencies)stamp.revision=revisions[stamp.index];
        }
        for(unsigned v=0;v<bh;v++)for(unsigned u=0;u<bw;u++){
            unsigned px=((x&~3u)+u*4)&2047,py=((y&~1u)+v*2)&1023,right=px>=1024?1024:0,xo=(px-right)>>1;
            const unsigned address=(((((py>>1)<<9)^(right<<8))|xo)>>1)&0x7ffff;
            source.push_back(rams.tex[sheet][address]);
            if(collect){
                const unsigned page=address/gpu3d_rams::texture_page_words;
                if(!touched.test(page)){touched.set(page);dependencies.push_back({page,revisions[page]});}
            }
        }
        ++source_checks;source_words+=source.size();
        if(found&&found->source==source){
            found->dependencies=std::move(dependencies);found->page_source=revisions;
            found->generation=generation;if(slot)slot->generation=generation;out=slot?slot->placed:found->placed;return true;
        }
        tile placed{};
        if(found)placed=found->placed;
        else if(repeat){
            if(w!=h||(w&(w-1))||!w||!add_page(w,true,luma))return false;
            placed={0,0,w,h,unsigned(pages.size()-1),w};
            placed.transform[0]=placed.transform[1]=1.f;
        }else {
        shelf *row=nullptr;
        unsigned index=0;
        // A 520 page holds four bordered 256 tiles. Smaller coordinates retain
        // substantially more interpolation precision than a 2048 atlas.
        unsigned side=520;while(side<w+2||side<h+2)side=side*2-8;
        for(;index<pages.size();index++){
            auto &p=pages[index];if(p.side!=side||p.repeat)continue;
            for(auto &s:p.shelves)if(s.height==h+2&&s.x+w+2<=side){row=&s;break;}
            if(!row&&p.next_y+h+2>side)continue;
            if(!claim_backing(p,luma))return false;
            if(!row){
                p.shelves.push_back({p.next_y,h+2,0});
                p.next_y+=h+2;
                row=&p.shelves.back();
            }
            break;
        }
        if(!row){
            if(!add_page(side,false,luma))return false;
            index=pages.size()-1;auto &p=pages.back();p.shelves.push_back({0,h+2,0});p.next_y=h+2;row=&p.shelves.back();
        }
        placed={row->x,row->y,w,h,index,side};row->x+=w+2;
        placed.transform[0]=float(w)/side;placed.transform[1]=float(h)/side;
        placed.transform[2]=float(placed.x+1)/side;placed.transform[3]=float(placed.y+1)/side;
        }
        if(luma)placed.page|=luma_bit;
        std::vector<uint8_t> data(size_t(w)*h*channels);
        if(snapshot_decode){
            decode_atlas_snapshot(data.data(),source.data(),x,y,w,h,channels,transparent);
        }else{
            for(unsigned v=0;v<h;v++)for(unsigned u=0;u<w;u++){
                unsigned px=(x+u)&2047,py=(y+v)&1023,right=px>=1024?1024:0,xo=(px-right)>>1;
                unsigned shift=((xo&1)<<4)+((~u&1)<<2)+((~v&1)<<3);
                unsigned index=(rams.tex[sheet][(((((py>>1)<<9)^(right<<8))|xo)>>1)&0x7ffff]>>shift)&15;
                const size_t at=(size_t(v)*w+u)*channels;
                data[at]=index<<4;
                if(!luma)data[at+1]=transparent&&index==15?0:255;
            }
        }
        unsigned uniform_axes=0;
        if(record_uniform_axes&&!transparent&&w&&h&&w<=128&&h<=128){
            // Bit 0 means each row is constant. Bit 1 means each column is constant.
            uniform_axes=3;
            for(unsigned v=0;v<h&&uniform_axes;++v){
                const auto *row=data.data()+size_t(v)*w*channels;
                for(unsigned u=0;u<w&&uniform_axes;++u){
                    const size_t offset=size_t(u)*channels;
                    if(row[offset]!=row[0])uniform_axes&=~1u;
                    if(row[offset]!=data[offset])uniform_axes&=~2u;
                }
            }
        }
        static const bool row_copy=std::getenv("VF2_ATLAS_TILE_EDGES")!=nullptr;
        if(transparent&&row_copy){
            // Alpha is immutable. Only opaque texels contribute a colour,
            // and the fill never changes those texels.
            fill_transparent_edges(data.data(),w,h,p.wrapx,p.wrapy);
        }else if(transparent){
            auto original=data;
            for(unsigned v=0;v<h;v++)for(unsigned u=0;u<w;u++)if(!original[(size_t(v)*w+u)*2+1]){
                const int offsets[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
                for(auto &offset:offsets){
                    int a=int(u)+offset[0],b=int(v)+offset[1];
                    a=p.wrapx?(a+int(w))%w:std::clamp(a,0,int(w)-1);
                    b=p.wrapy?(b+int(h))%h:std::clamp(b,0,int(h)-1);
                    size_t at=(size_t(b)*w+a)*2;
                    if(original[at+1]){data[(size_t(v)*w+u)*2]=original[at];break;}
                }
            }
        }
        const unsigned upload_w=repeat?w:w+2,upload_h=repeat?h:h+2;
        std::vector<uint8_t> bordered;
        if(repeat)bordered=std::move(data);
        else bordered.resize(size_t(upload_w)*upload_h*channels);
        if(!repeat&&row_copy)copy_tile_border(bordered.data(),data.data(),w,h,channels,p.wrapx,p.wrapy);
        else if(!repeat)for(unsigned v=0;v<h+2;v++)for(unsigned u=0;u<w+2;u++){
            int a=int(u)-1,b=int(v)-1;
            a=p.wrapx?(a+int(w))%w:std::clamp(a,0,int(w)-1);
            b=p.wrapy?(b+int(h))%h:std::clamp(b,0,int(h)-1);
            size_t from=(size_t(b)*w+a)*channels,to=(size_t(v)*(w+2)+u)*channels;
            bordered[to]=data[from];
            if(!luma)bordered[to+1]=data[from+1];
        }
        if(deferred)uploads.push_back({placed.page,placed.x,placed.y,upload_w,upload_h,std::move(bordered)});
        else{
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,image(placed.page).name);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
            glTexSubImage2D(GL_TEXTURE_2D,0,placed.x,placed.y,upload_w,upload_h,format(luma),GL_UNSIGNED_BYTE,bordered.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT,4);uploaded+=bordered.size();
        }
        if(found){
            found->dependencies=std::move(dependencies);found->page_source=revisions;
            found->uniform_axes=uniform_axes;
            found->source=std::move(source);found->generation=generation;if(slot)slot->generation=generation;out=found->placed;return true;
        }
        out=insert_entry(key,{placed,generation,std::move(source),std::move(dependencies),revisions,uniform_axes}).placed;return true;
    }
};
}
