// Inventory immutable ROM object reuse from the existing owned geometry records.
#include "../../src/host/acm_checkpoint.h"
#include "../../src/host/input_script.h"
#include "../../src/gpu/geometry_frame.h"
#include "../../src/gpu/mesh_geometry.h"
#include "../../src/gpu/gles2_tiles.h"
#include <iterator>
#include <optional>
#include <string>
#include "../../src/machine/geometry_math.h"
#include <map>
#include <set>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct entry {uint64_t uses=0,frames=0,reused=0;unsigned max_instances=0;std::array<float,12> first{};bool matrix_changes=false;};
using position_key=std::array<uint32_t,13>;
using matrix_key=std::array<uint32_t,3>;

struct mesh_origin {
 unsigned slot;
 unsigned polygon;
 uint32_t texture_point,texture_header;

 bool has_static_material()const{return ((texture_point|texture_header)&0x800000)==0;}

};

struct packet_inventory {
 uint64_t drawn=0,drawn_vertices=0,drawn_indices=0;
 uint64_t attributed=0,immutable_input=0,eligible=0,eligible_vertices=0,eligible_indices=0;
 uint64_t clipped=0,multiple_bands=0,changed_fan=0;
 uint64_t matrix_groups=0,eligible_recipes=0,grouped_recipes=0,all_recipes=0;
 unsigned peak_matrix_groups=0,peak_grouped_recipes=0;
 uint64_t packed_groups_sum=0;
 unsigned peak_packed_groups=0;
 std::map<unsigned,unsigned> frame_packed_slots;
 uint64_t repeat_variants=0,consecutive_variants=0;
 using variant=std::array<uint32_t,5>;
 std::set<variant> seen_variants,previous_variants,frame_variants;

 using recipe=std::array<unsigned,9>;
 using recipe_pair=std::pair<recipe,recipe>;
 std::set<unsigned> frame_groups;
 std::set<recipe_pair> frame_all_recipes,frame_eligible_recipes,frame_fallback_recipes;
 std::map<uint64_t,unsigned> atlas_pages;
 struct draw_counts {
  unsigned baseline,fallback,retained,separate,mixed,single_palette,packed_mixed,atlas_packed_mixed,capacity_floor;
 };
 std::vector<draw_counts> page_draws;
 std::map<unsigned,std::set<recipe_pair>> frame_object_recipes;
 unsigned missing_atlas_keys=0;
 bool flat_draw=false;

 bool load_atlas(const char *path){
  FILE *in=std::fopen(path,"r");
  if(!in)return false;
  char format[32]{};
  unsigned clears=0;
  bool valid=std::fscanf(in,"%31s %u",format,&clears)==2&&
      std::strcmp(format,"VF2_ATLAS_LAYOUT_V1")==0&&clears==0;
  unsigned long long key;
  unsigned page;
  const bool luma_backing=std::getenv("VF2_FIXED_ATLAS_LUMA")!=nullptr;
  int status=0;
  while(valid&&(status=std::fscanf(in,"%llx %u",&key,&page))==2){
   // The layout stores logical pages. Opaque tiles address the luma
   // backing of the same page when the Mini's fixed atlas mode is active.
   if(luma_backing&&!(key&(1ull<<48)))page|=0x8000;
   valid=atlas_pages.emplace(key,page).second;
  }
  valid=valid&&status==EOF&&!std::ferror(in)&&!atlas_pages.empty();
  std::fclose(in);
  return valid;
 }

 std::optional<unsigned> page_for(const recipe &r){
  const uint64_t key=r[0]|(uint64_t(r[1])<<11)|(uint64_t(r[2])<<21)
      |(uint64_t(r[3])<<33)|(uint64_t(r[4])<<45)|(uint64_t(bool(r[5]))<<46)
      |(uint64_t(bool(r[6]))<<47)|(uint64_t(bool(r[7]))<<48)|(uint64_t(bool(r[8]))<<49);
  const auto it=atlas_pages.find(key);
  if(it!=atlas_pages.end())return it->second;
  ++missing_atlas_keys;
  return std::nullopt;
 }

 void finish_page_draws(){
  if(atlas_pages.empty())return;
  using page_pair=std::pair<unsigned,unsigned>;
  auto lookup=[&](const recipe_pair &pair)->std::optional<page_pair>{
   const auto a=page_for(pair.first),b=page_for(pair.second);
   if(!a||!b)return std::nullopt;
   return page_pair{*a,*b};
  };
  std::set<page_pair> baseline,fallback,retained_without_groups;
  std::set<std::pair<unsigned,page_pair>> retained,packed_retained;
  if(flat_draw){baseline.emplace(0,0);fallback.emplace(0,0);}
  for(const auto &pair:frame_all_recipes)if(auto pages=lookup(pair))baseline.insert(*pages);
  for(const auto &pair:frame_fallback_recipes)if(auto pages=lookup(pair))fallback.insert(*pages);
  for(const auto &[group,pair]:frame_grouped_recipes)
   if(auto pages=lookup(pair)){
    retained.emplace(group,*pages);
    retained_without_groups.insert(*pages);
   }
  for(const auto &[group,pair]:frame_packed_recipes)
   if(auto pages=lookup(pair))packed_retained.emplace(group,*pages);
  // A mixed-format shader could append fallback vertices to any retained
  // draw using the same pages. This counts the ideal case, not a live path.
  unsigned uncovered=0;
  for(const auto &pages:fallback)uncovered+=!retained_without_groups.contains(pages);
  // Each retained object occupies a single transform slot and can
  // touch multiple page pairs. Group objects that can share draws first.
  std::vector<std::set<page_pair>> object_pages;
  for(const auto &[object,recipes]:frame_object_recipes){
   auto &to=object_pages.emplace_back();
   for(const auto &pair:recipes)if(auto pages=lookup(pair))to.insert(*pages);
  }
  std::stable_sort(object_pages.begin(),object_pages.end(),
   [](const auto &a,const auto &b){return a.size()>b.size();});
  const size_t group_count=(object_pages.size()+63)/64;
  std::vector<std::set<page_pair>> atlas_groups(group_count);
  std::vector<unsigned> slots(group_count);
  for(const auto &pages:object_pages){
   size_t best=group_count,lowest=~size_t(0);
   for(size_t g=0;g<group_count;++g){
    if(slots[g]==64)continue;
    const size_t added=std::count_if(pages.begin(),pages.end(),
                                 [&](const auto &p){return !atlas_groups[g].contains(p);});
    if(added<lowest){best=g;lowest=added;}
   }
   atlas_groups[best].insert(pages.begin(),pages.end());
   ++slots[best];
  }
  std::map<page_pair,unsigned> object_counts;
  for(const auto &pairs:object_pages)for(const auto &pair:pairs)++object_counts[pair];
  size_t capacity_floor=uncovered;
  for(const auto &[pair,objects]:object_counts)capacity_floor+=(objects+63)/64;
  size_t atlas_group_draws=0;
  for(const auto &group:atlas_groups)atlas_group_draws+=group.size();
  page_draws.push_back({unsigned(baseline.size()),unsigned(fallback.size()),
      unsigned(retained.size()),unsigned(fallback.size()+retained.size()),
      unsigned(retained.size()+uncovered),unsigned(fallback.size()+retained_without_groups.size()),
      unsigned(packed_retained.size()+uncovered),unsigned(atlas_group_draws+uncovered),unsigned(capacity_floor)});
 }
 std::set<std::pair<unsigned,recipe_pair>> frame_grouped_recipes,frame_packed_recipes;

 static recipe tile_recipe(const gpu3d_poly &p,int level,bool micro,bool native_repeat){
  return {
   micro?p.utexx:((uint32_t(p.texx)-2048)>>level)&2047,
   micro?p.utexy:((uint32_t(p.texy)-1024)>>level)&1023,
   micro?128u:unsigned(p.texw>>level),micro?128u:unsigned(p.texh>>level),
   unsigned(p.sheet^(micro?1:(level&1))),p.wrapx,p.wrapy,unsigned(p.renderer&1),native_repeat
  };
 }

 void inspect(const gpu3d_compact_camera_poly &raw,const mesh_origin *origin){
  gpu3d_camera_poly source;
  gpu3d_geometry_frame::expand_compact(raw,source);
  if(source.material.renderer==1)return;
  gpu3d_poly projected;
  if(!gpu3d_geometry_frame::project(source,projected))return;
  const int left=std::max(0,int(projected.vp[0]));
  const int right=std::min(496,int(projected.vp[1])+1);
  const int top=std::max(0,int(projected.vp[2]));
  const int bottom=std::min(384,int(projected.vp[3])+1);
  if(right<=left||bottom<=top)return;
  if(!(source.material.renderer&2)){
   if(!flat_draw&&!atlas_pages.empty()){
    vf2_tiles::stack_polygon polygon;
    for(unsigned i=0;i<projected.n;++i)polygon.push_back({projected.x[i],projected.y[i],1,0,0});
    polygon.clip_by([&](const auto &v){return double(v.x)-left;});
    polygon.clip_by([&](const auto &v){return right-double(v.x);});
    polygon.clip_by([&](const auto &v){return double(v.y)-top;});
    polygon.clip_by([&](const auto &v){return bottom-double(v.y);});
    flat_draw=polygon.size()>=3;
   }
   return;
  }
  const int max_level=std::max(0,int(std::log2(std::min(projected.texw,projected.texh)))-1);
  vf2_tiles::indexed_packet packet;
  recipe_pair sole_recipe{};
  std::vector<recipe_pair> polygon_recipes;
  unsigned bands=0;
  int sole_band=-1;
  size_t vertices=0,indices=0;
  bool same_fan=true;
  for(int band=-1;band<=max_level;++band){
   vf2_tiles::packet_into<true,true>(packet,projected,band,max_level,left,top,right,bottom,true);
   if(packet.indices.empty())continue;
   ++bands;
   sole_band=band;
   vertices+=packet.vertices.size();
   indices+=packet.indices.size();
   sole_recipe={tile_recipe(projected,std::max(0,band),false,packet.native_repeat),
                tile_recipe(projected,band<0?0:std::min(band+1,max_level),band<0&&projected.utex,packet.native_repeat)};
   frame_all_recipes.insert(sole_recipe);
   if(!atlas_pages.empty())polygon_recipes.push_back(sole_recipe);
   same_fan &= packet.vertices.size()==projected.n && packet.indices.size()==size_t(3*(projected.n-2));
   for(unsigned i=0;same_fan&&i<projected.n;++i){
    const auto &v=packet.vertices[i].p;
    same_fan=v.x==projected.x[i]&&v.y==projected.y[i]&&v.q==projected.z[i]&&
        v.u==projected.u[i]&&v.v==projected.v[i];
   }
  }
  if(!bands)return;
  ++drawn;
  drawn_vertices+=vertices;
  drawn_indices+=indices;
  bool can_retain=false;
  if(origin){
   ++attributed;
   if(origin->has_static_material()){
    ++immutable_input;
    bool inside=true;
    for(const auto &plane:source.planes){
     for(unsigned i=0;i<raw.n;++i){
      const auto &v=raw.position[i];
      inside&=(v[0]*plane[0]+v[1]*plane[1])+v[2]*plane[2]>=plane[3];
     }
    }
    if(!inside)++clipped;
    else if(bands!=1)++multiple_bands;
    else if(!same_fan)++changed_fan;
    else can_retain=true;
   }
  }
  if(!can_retain){
   frame_fallback_recipes.insert(polygon_recipes.begin(),polygon_recipes.end());
   return;
  }
  ++eligible;
  eligible_vertices+=vertices;
  eligible_indices+=indices;
  const unsigned group=origin->slot/64;
  frame_groups.insert(group);
  frame_eligible_recipes.insert(sole_recipe);
  frame_grouped_recipes.emplace(group,sole_recipe);
  const unsigned packed_slot=frame_packed_slots.try_emplace(origin->slot,unsigned(frame_packed_slots.size())).first->second;
  frame_packed_recipes.emplace(packed_slot/64,sole_recipe);
  if(!atlas_pages.empty())frame_object_recipes[origin->slot].insert(sole_recipe);

  const variant id{origin->slot,origin->polygon,origin->texture_point,origin->texture_header,uint32_t(sole_band+1)};
  repeat_variants+=!seen_variants.insert(id).second;
  consecutive_variants+=previous_variants.contains(id);
  frame_variants.insert(id);
 }

 void end_frame(){
  matrix_groups+=frame_groups.size();
  const unsigned packed_groups=(frame_packed_slots.size()+63)/64;
  packed_groups_sum+=packed_groups;
  peak_packed_groups=std::max(peak_packed_groups,packed_groups);
  peak_matrix_groups=std::max(peak_matrix_groups,unsigned(frame_groups.size()));
  all_recipes+=frame_all_recipes.size();
  eligible_recipes+=frame_eligible_recipes.size();
  grouped_recipes+=frame_grouped_recipes.size();
  peak_grouped_recipes=std::max(peak_grouped_recipes,unsigned(frame_grouped_recipes.size()));
  previous_variants.swap(frame_variants);
  frame_variants.clear();
  finish_page_draws();
  frame_fallback_recipes.clear();
  flat_draw=false;
  frame_groups.clear();
  frame_all_recipes.clear();
  frame_eligible_recipes.clear();
  frame_grouped_recipes.clear();
  frame_packed_recipes.clear();
  frame_packed_slots.clear();
  frame_object_recipes.clear();
 }

 void write(FILE *out)const{
  std::fputs(",\n\"packet_inventory\":{",out);
  auto field=[&](const char *name,uint64_t value){std::fprintf(out,"\n\"%s\":%llu,",name,(unsigned long long)value);};
  field("drawn_textured_polygons",drawn);
  field("drawn_vertices",drawn_vertices);
  field("drawn_indices",drawn_indices);
  field("uniquely_attributed_polygons",attributed);
  field("immutable_uv_header_polygons",immutable_input);
  field("single_band_unchanged_fans",eligible);
  field("distinct_eligible_mesh_material_band_variants",seen_variants.size());
  field("repeated_eligible_variants",repeat_variants);
  field("consecutive_eligible_variants",consecutive_variants);
  field("eligible_vertices",eligible_vertices);
  field("eligible_indices",eligible_indices);
  field("frustum_clip_fallback",clipped);
  field("mip_crossing_fallback",multiple_bands);
  field("viewport_cell_or_fan_fallback",changed_fan);
  field("matrix_groups_sum",matrix_groups);
  field("matrix_groups_peak",peak_matrix_groups);
  field("frame_packed_groups_sum",packed_groups_sum);
  field("frame_packed_groups_peak",peak_packed_groups);
  field("all_tile_recipe_pairs_sum",all_recipes);
  field("eligible_tile_recipe_pairs_sum",eligible_recipes);
  field("eligible_grouped_tile_recipe_pairs_sum",grouped_recipes);
  field("eligible_grouped_tile_recipe_pairs_peak",peak_grouped_recipes);
  field("missing_atlas_keys",missing_atlas_keys);
  std::fputs("\n\"atlas_draw_columns\":[\"baseline\",\"fallback\",\"retained\",\"separate\",\"mixed_lower_bound\",\"single_palette_separate\",\"frame_packed_mixed_lower_bound\",\"atlas_packed_mixed_lower_bound\",\"per_pair_capacity_floor\"],",out);
  std::fputs("\n\"atlas_draw_counts\":[",out);
  for(size_t i=0;i<page_draws.size();++i){
   const auto &row=page_draws[i];
   std::fprintf(out,"%s[%u,%u,%u,%u,%u,%u,%u,%u,%u]",i?",":"",row.baseline,row.fallback,
       row.retained,row.separate,row.mixed,row.single_palette,row.packed_mixed,row.atlas_packed_mixed,row.capacity_floor);
  }
  std::fputs("]\n}",out);
 }
};

static position_key camera_position_key(const gpu3d_compact_camera_poly &poly){
 position_key words{};
 words[0]=poly.n;
 for(unsigned i=0;i<poly.n;++i)
  for(unsigned axis=0;axis<3;++axis)words[1+i*3+axis]=std::bit_cast<uint32_t>(poly.position[i][axis]);
 return words;
}

int main(int argc,char **argv){
 if(argc<5){std::fprintf(stderr,"rom state frames output.json [atlas-layout.txt] [--input-script path]\n");return 2;}
 packet_inventory inventory;
 input_script inputs;
 bool scripted=false,atlas=false;
 for(int arg=5;arg<argc;++arg){
  if(std::strcmp(argv[arg],"--input-script")==0){
   std::string error;
   if(scripted||arg+1>=argc){std::fprintf(stderr,"Expected one input script\n");return 2;}
   if(!inputs.load(argv[++arg],error)){std::fprintf(stderr,"%s\n",error.c_str());return 2;}
   scripted=true;
  }else{
   if(atlas||!inventory.load_atlas(argv[arg])){
    std::fprintf(stderr,"Expected one valid atlas layout with zero atlas clears\n");return 2;
   }
   atlas=true;
  }
 }
 auto rom=romset::load(argv[1]);if(!rom.ok){std::fprintf(stderr,"%s\n",rom.error.c_str());return 1;}
 model2a m(std::move(rom));m.release_unused_roms();m.set_byte_link(true);m.set_sound(false);m.reset();m.defer_3d(true);if(!m.video().camera_packets(true))return 1;
 std::string error;if(!acm_checkpoint::load(m,argv[2],error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
 m.video().set_smooth(true);for(unsigned i=0;i<3;++i)m.run_frame();
 const unsigned frames=std::strtoul(argv[3],nullptr,10);uint64_t total=0,rom_uses=0,ram_uses=0,cache_hits=0,temporal=0,records=0;unsigned max_objects=0;std::map<uint32_t,entry> entries;std::map<uint32_t,unsigned> previous;
 vf2_mesh::catalog meshes;uint64_t matched_positions=0,unmatched_positions=0;
 uint64_t last=~0ull;gpu3d_geometry_frame camera;
 std::map<matrix_key,unsigned> stable_slots;
 for(unsigned frame=0;frame<frames;++frame){
  m.run_frame();
  if(scripted){
   uint8_t ports[input_script::N_PORTS];
   inputs.apply(frame,ports);
   m.m_in0=ports[0];m.m_in1=ports[1];m.m_in2=ports[2];m.m_sw=ports[3];
  }
  if(!m.video().capture_camera_picture(m.video().shown_index(),camera)){std::fprintf(stderr,"No camera picture at %u\n",frame);return 1;}
  auto record=m.video().shown_list();if(!record){std::fprintf(stderr,"No geometry record at %u\n",frame);return 1;}if(record->frame==last)continue;last=record->frame;++records;
  if(!camera.compact){std::fprintf(stderr,"Mesh packet inventory requires VF2_COMPACT_CAMERA\n");return 2;}
  if(camera.compact_polygons.size()>32767){std::fprintf(stderr,"Mesh packet inventory exceeds polygon capacity\n");return 2;}
  std::multiset<position_key> positions;
  std::multimap<position_key,mesh_origin> origins;
  std::multiset<matrix_key> frame_matrices;
  for(const auto &o:record->objects)frame_matrices.insert({o.oba,o.count,o.mode});
  std::map<uint32_t,unsigned> current;max_objects=std::max<unsigned>(max_objects,record->objects.size());
  for(const auto &o:record->objects){++total;const bool immutable=!(o.oba&0x01000000)&&(o.oba&0x00800000);if(immutable)++rom_uses;else ++ram_uses;
   if(immutable){
    const uint32_t offset=o.oba&record->geo.polygon_rom_mask;
    const auto *mesh=meshes.get({o.oba,o.count,o.mode},{record->geo.polygon_rom+offset,size_t(record->geo.polygon_rom_mask)+1-offset});
    if(mesh){
     const matrix_key object{o.oba,o.count,o.mode};
     const unsigned slot=stable_slots.try_emplace(object,unsigned(stable_slots.size())).first->second;
     const bool unique_instance=frame_matrices.count(object)==1;
     uint32_t texture_point=o.tpa,texture_header=o.tha;
     geometry_math::matrix3x4 matrix(o.m,false);
     for(unsigned ordinal=0;ordinal<mesh->polygons.size();++ordinal){
      const auto &poly=mesh->polygons[ordinal];
      const mesh_origin origin{unique_instance?slot:~0u,ordinal,texture_point,texture_header};
      texture_point+=poly.n*2;
      const int delta=int((poly.attributes>>12)&31);
      texture_header+=(delta>=16?delta-32:delta)*4;
      if(!((poly.attributes>>8)&3))continue;
      position_key words{};
      words[0]=poly.n;
      for(unsigned i=0;i<poly.n;++i){
       auto v=poly.position[i];
       matrix.apply(v[0],v[1],v[2],true);
       v[0]*=o.focus[0];
       v[1]*=o.focus[1];
       for(unsigned axis=0;axis<3;++axis)
        words[1+i*3+axis]=std::bit_cast<uint32_t>(v[axis])&0xffffff00u;
      }
      positions.insert(words);
      // Preserve ambiguous position keys rather than guessing provenance.
      origins.emplace(words,origin);
     }
    }
   }
   auto &e=entries[o.oba];if(e.uses&&immutable)++cache_hits;else if(!e.uses)std::copy_n(o.m,12,e.first.begin());++e.uses;++current[o.oba];e.matrix_changes|=std::memcmp(e.first.data(),o.m,48)!=0;
  }
  std::map<position_key,unsigned> raw_counts;
  for(const auto &poly:camera.compact_polygons)++raw_counts[camera_position_key(poly)];
  for(const auto &poly:camera.compact_polygons){
   const auto words=camera_position_key(poly);
   const auto found=positions.find(words);if(found==positions.end())++unmatched_positions;else{++matched_positions;positions.erase(found);}
   if(poly.window<=camera.final_window){
    const auto [first,last]=origins.equal_range(words);
    const bool unique=first!=last&&std::next(first)==last&&raw_counts.at(words)==1&&first->second.slot!=~0u;
    const auto *origin=unique?&first->second:nullptr;
    inventory.inspect(poly,origin);
   }
  }
  inventory.end_frame();
  for(auto [id,n]:current){auto &e=entries[id];++e.frames;e.max_instances=std::max(e.max_instances,n);const unsigned reused=std::min(n,previous[id]);e.reused+=reused;if(!(id&0x01000000)&&(id&0x00800000))temporal+=reused;}
  previous=std::move(current);
 }
 if(inventory.missing_atlas_keys){
  std::fprintf(stderr,"Atlas layout omitted %u lookups. Collect layout from the matching replay.\n",inventory.missing_atlas_keys);
  return 1;
 }
 FILE *out=std::fopen(argv[4],"wb");if(!out)return 1;
 std::fprintf(out,"{\n \"records\":%llu,\n \"object_uses\":%llu,\n \"rom_uses\":%llu,\n \"ram_uses\":%llu,\n \"rom_cache_hits\":%llu,\n \"rom_temporal_reuse\":%llu,\n \"max_frame_objects\":%u,\n \"objects\":[\n",(unsigned long long)records,(unsigned long long)total,(unsigned long long)rom_uses,(unsigned long long)ram_uses,(unsigned long long)cache_hits,(unsigned long long)temporal,max_objects);
 bool first=true;for(auto &[id,e]:entries){std::fprintf(out,"%s{\"address\":%u,\"uses\":%llu,\"frames\":%llu,\"max_instances\":%u,\"matrix_changes\":%s}",first?"":",\n",id,(unsigned long long)e.uses,(unsigned long long)e.frames,e.max_instances,e.matrix_changes?"true":"false");first=false;}
 std::fprintf(out,"\n],\n\"catalog_meshes\":%zu,\"catalog_bytes\":%zu,\"catalog_hits\":%llu,\"catalog_misses\":%llu,\"catalog_rejected\":%llu,\"matched_camera_polygons\":%llu,\"unmatched_camera_polygons\":%llu",meshes.size(),meshes.bytes(),(unsigned long long)meshes.hits,(unsigned long long)meshes.misses,(unsigned long long)meshes.rejected,(unsigned long long)matched_positions,(unsigned long long)unmatched_positions);
 inventory.write(out);std::fputs("\n}\n",out);std::fclose(out);
 std::printf("Mesh reuse: %llu records, %llu objects, %llu ROM uses, %llu RAM uses, %zu unique sources, %llu ROM cache hits, %u max objects/frame\n",(unsigned long long)records,(unsigned long long)total,(unsigned long long)rom_uses,(unsigned long long)ram_uses,entries.size(),(unsigned long long)cache_hits,max_objects);
 std::printf("Catalog: %zu meshes %zu bytes, %llu hits %llu rejected; camera positions %llu matched / %llu unmatched\n",meshes.size(),meshes.bytes(),(unsigned long long)meshes.hits,(unsigned long long)meshes.rejected,(unsigned long long)matched_positions,(unsigned long long)unmatched_positions);
 std::vector<uint8_t> core,geometry;m.save_state(core);m.video().save_geometry(geometry);
 std::printf("State: frame %llu IP %08x TGP %llu work %016llx core %016llx geometry %016llx\n",(unsigned long long)m.frame_number(),m.cpu().ip(),(unsigned long long)m.m_tgp_instructions,(unsigned long long)m2::state_hash(m.workram().data(),m.workram().size()),(unsigned long long)m2::state_hash(core.data()+model2a::STATE_HEADER,core.size()-model2a::STATE_HEADER),(unsigned long long)m2::state_hash(geometry.data(),geometry.size()));
}
