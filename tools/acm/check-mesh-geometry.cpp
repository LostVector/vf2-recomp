// Independent link topology, bounded decode and immutable catalog ownership.
#include "../../src/gpu/mesh_geometry.h"
#include <cstdio>
#include <cstdlib>
static void check(bool ok,const char *s){if(!ok){std::fprintf(stderr,"FAIL mesh geometry: %s\n",s);std::exit(1);}}
int main(){
 std::vector<uint32_t> words;auto point=[&](float x){for(float v:{x,x+10,x+20})words.push_back(std::bit_cast<uint32_t>(v));};
 point(0);point(1);
 const unsigned attrs[]={1|(2<<8),2|(1<<8),1|(3<<8),2,1|(2<<8)};
 for(unsigned i=0;i<5;++i){words.push_back(attrs[i]);point(90+i);point(2+i*2);point(3+i*2);}words.push_back(0);
 const float expected[][4]={{1,0,2,3},{3,2,4,4},{4,2,6,7},{4,7,8,8},{8,8,10,11}};
 vf2_mesh::geometry mesh;
 for(unsigned mode=0;mode<4;++mode){
  check(vf2_mesh::decode(words,0,mode,mesh)&&mesh.polygons.size()==5&&mesh.terminated,"all modes and terminal marker");
  check(mesh.generated_normals==bool(mode&2),"normal source metadata");
  for(unsigned i=0;i<5;++i){const auto &p=mesh.polygons[i];check(p.n==((attrs[i]&1)?4:3)&&p.attributes==attrs[i],"triangle/quad attributes");for(unsigned v=0;v<4;++v)for(unsigned a=0;a<3;++a)check(p.position[v][a]==expected[i][v]+a*10,"link topology and vertex winding");}
 }
 check(vf2_mesh::decode(words,1,0,mesh)&&mesh.polygons.size()==1&&!mesh.terminated&&mesh.source_words==16,"explicit object count");
 for(size_t n=0;n<words.size();++n)check(!vf2_mesh::decode(std::span(words).first(n),0,0,mesh)&&mesh.polygons.empty(),"truncated input rejects partial mesh");
 check(!vf2_mesh::decode(words,0,0,mesh,sizeof(vf2_mesh::polygon)*2)&&mesh.polygons.empty(),"decode allocation bound");
 vf2_mesh::catalog cache(sizeof(vf2_mesh::polygon)*2);const vf2_mesh::key id{0x800000,1,0};
 const auto *kept=cache.get(id,words);check(kept&&kept->polygons.size()==1,"catalog first decode");
 const auto original=words;words.assign(words.size(),0);check(cache.get(id,words)==kept&&kept->polygons[0].position[0][0]==1,"catalog owns immutable mesh");words=original;
 check(cache.get({id.address,1,1},words)&&cache.size()==2,"mode belongs to cache key");
 check(!cache.get({id.address,5,0},words)&&cache.bytes()<=sizeof(vf2_mesh::polygon)*2,"catalog storage bound and count key");
 check(cache.hits==1&&cache.rejected==1,"catalog counters");
 std::puts("Mesh geometry passed: four link types, triangle/quad winding, all normal modes, counts, truncation, owned data and bounded allocation");
}
