// Allocation-free storage for the board's eight-word hardware FIFOs.
#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <cstddef>
namespace m2 {
class word_fifo {
    std::array<uint32_t,8> words{};
    unsigned head=0,count=0;
public:
    bool empty()const{return count==0;}
    size_t size()const{return count;}
    uint32_t front()const{assert(count);return words[head];}
    void clear(){head=count=0;}
    void pop_front(){assert(count);head=(head+1)&7;--count;}
    void push_back(uint32_t word){assert(count<8);words[(head+count)&7]=word;++count;}
    template<class IO> void state(IO &io){
        // Preserve the previous deque's serialized length and logical order.
        uint64_t n=count;io.pod(n);
        if(!io.check(n<=8))return;
        if constexpr(IO::loading){
            clear();for(uint64_t i=0;i<n;++i){uint32_t word=0;io.pod(word);push_back(word);}
        }else for(unsigned i=0;i<count;++i){uint32_t word=words[(head+i)&7];io.pod(word);}
    }
};
}
