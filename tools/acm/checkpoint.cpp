// Create a reusable in-game checkpoint and verify it in a separate process.
#include "../../src/host/acm_checkpoint.h"
#include "../../src/host/arcade_stage_selection.h"
#include "../../src/host/input_script.h"
#include <charconv>
#include <fstream>
#include <iomanip>
#include <string_view>
int main(int argc,char **argv){
#ifdef MODEL2_ORIGINAL
    if(argc!=5||std::string_view(argv[1])!="dump-tgp"){
        std::fprintf(stderr,"usage: daytona-tgp-dump dump-tgp daytona.zip checkpoint output.bin\n");return 2;
    }
#endif
    if(argc<4){std::fprintf(stderr,"usage: vf2checkpoint create|verify vf2.zip checkpoint [frame script [versus-stage]]\n");return 2;}
    bool create=std::string(argv[1])=="create",dump=std::string(argv[1])=="dump-tgp";
    if(!create&&!dump&&std::string(argv[1])!="verify")return 2;
#ifdef MODEL2_ORIGINAL
    auto roms=romset::load_daytona(argv[2]);
#else
    auto roms=romset::load(argv[2]);
#endif
    if(!roms.ok){std::fprintf(stderr,"%s\n",roms.error.c_str());return 1;}
    model2a m(std::move(roms));m.release_unused_roms();m.set_byte_link(true);m.set_sound(false);m.reset();m.defer_3d(true);
    std::string error,path=argv[3];
    if(create){
        if(argc<6||argc>7)return 2;
        int stage = 0;
        if (argc == 7) {
            const std::string_view argument(argv[6]);
            const char *end = argument.data() + argument.size();
            const auto parsed = std::from_chars(argument.data(), end, stage);
            if (parsed.ec != std::errc{} || parsed.ptr != end || stage < 1 || stage > vf2net::STAGES) {
                std::fprintf(stderr, "Versus stage must be 1–%d\n", vf2net::STAGES);
                return 2;
            }
        }
        input_script script;if(!script.load(argv[5],error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        unsigned frames=std::strtoul(argv[4],nullptr,10);
        bool stage_pending = false, stage_applied = false;
        for(unsigned i=0;i<frames;i++){
            if (stage) stage_applied |= select_arcade_versus_stage(m, stage, stage_pending);
            m.run_frame();uint8_t ports[4];script.apply(i,ports);m.m_in0=ports[0];m.m_in1=ports[1];m.m_in2=ports[2];m.m_sw=ports[3];
        }
        if (stage && (!stage_applied || m.game_mode() != 9 || m.workram()[vf2net::VERSUS] == 0 ||
                      m.workram()[vf2net::STAGE] != stage)) {
            std::fprintf(stderr, "Versus fight on stage %d was not reached by frame %u (mode %u, versus %u, stage %u)\n",
                         stage, frames, unsigned(m.game_mode()), unsigned(m.workram()[vf2net::VERSUS]),
                         unsigned(m.workram()[vf2net::STAGE]));
            return 1;
        }
        if(!acm_checkpoint::save(m,path,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        std::fprintf(stderr,"Checkpoint created at frame %llu\n",(unsigned long long)m.frame_number());
    }else if(!acm_checkpoint::load(m,path,error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
    if(dump){
        if(argc<5)return 2;
        FILE *out=std::fopen(argv[4],"wb");if(!out)return 1;
        bool ok=true;for(unsigned i=0;i<4096;i++){uint32_t word=m.tgp().program_word(i);ok&=std::fwrite(&word,1,4,out)==4;}
        if(std::fclose(out))ok=false;return ok?0:1;
    }
    // Rebuild the display pipeline, then compare both core state and the full
    // rendered frame against an uninterrupted run from the same checkpoint.
    uint64_t hooks=0;const bool check_hooks=std::getenv("VF2_CHECK_SCHED_HOOKS")!=nullptr;
    if(check_hooks)m.on_instruction=[&](offs_t){++hooks;};
    for(int i=0;i<20;i++){
        if(check_hooks&&i==10)m.on_instruction=nullptr;
        m.run_frame();m.m_in0=m.m_in1=m.m_in2=m.m_sw=255;
    }
    if(check_hooks){if(!hooks){std::fprintf(stderr,"Instruction hook never ran\n");return 1;}std::fprintf(stderr,"Scheduler hook enabled/removed: %llu callbacks\n",(unsigned long long)hooks);}
    const auto &picture=m.render();
    uint64_t pixels=m2::state_hash(reinterpret_cast<const uint8_t*>(picture.data()),size_t(picture.width())*picture.height()*4);
    const char *dump_path=std::getenv("VF2_CHECKPOINT_DUMP");
    std::vector<uint8_t> state;std::vector<m2::StateSection> sections;m.save_state(state,dump_path?&sections:nullptr);
    if(dump_path){
        std::ofstream out(dump_path,std::ios::binary);out.write(reinterpret_cast<const char*>(state.data()),state.size());if(!out)return 1;
        std::ofstream names(std::string(dump_path)+".sections");for(const auto &s:sections)names<<s.offset<<' '<<s.name<<'\n';if(!names)return 1;
        std::vector<uint8_t> geometry;m.video().save_geometry(geometry);
        std::fprintf(stderr,"Diagnostic work %016llx geometry %016llx\n",(unsigned long long)m2::state_hash(m.workram().data(),m.workram().size()),(unsigned long long)m2::state_hash(geometry.data(),geometry.size()));
    }
    uint64_t core=m2::state_hash(state.data()+model2a::STATE_HEADER,state.size()-model2a::STATE_HEADER);
    if(create){std::ofstream out(path+".verify");out<<std::hex<<core<<' '<<pixels<<'\n';if(!out)return 1;}
    else{
        uint64_t expected_core=0,expected_pixels=0;std::ifstream in(path+".verify");in>>std::hex>>expected_core>>expected_pixels;
        if(!in||expected_core!=core||expected_pixels!=pixels){std::fprintf(stderr,"Checkpoint mismatch: core %016llx/%016llx pixels %016llx/%016llx\n",(unsigned long long)core,(unsigned long long)expected_core,(unsigned long long)pixels,(unsigned long long)expected_pixels);return 1;}
    }
    std::fprintf(stderr,"%s: core %016llx pixels %016llx\n",create?"Reference saved":"Checkpoint verified in fresh process",(unsigned long long)core,(unsigned long long)pixels);return 0;
}
