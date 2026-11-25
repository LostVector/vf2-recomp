#include "../../src/machine/word_fifo.h"
#include "../../src/machine/state_io.h"
#include <deque>
#include <cstdio>
int main(){
 m2::word_fifo f;std::deque<uint32_t> d;uint32_t rng=12345;
 for(unsigned i=0;i<1000000;++i){
  rng=rng*1664525+1013904223;
  if((rng&255)==0){f.clear();d.clear();}
  else if(d.empty()||(d.size()<8&&(rng&256))){f.push_back(rng);d.push_back(rng);}
  else{if(f.front()!=d.front())return 1;f.pop_front();d.pop_front();}
  if(f.size()!=d.size()||f.empty()!=d.empty())return 2;
  if((i%101)==0){
   std::vector<uint8_t>a,b;m2::StateWriter w(a),v(b);f.state(w);v.deq(d);if(a!=b)return 3;
   m2::word_fifo g;m2::StateReader r(a.data(),a.size());g.state(r);if(!r.done())return 4;
   for(auto x:d){if(g.front()!=x)return 5;g.pop_front();}if(!g.empty())return 6;
  }
 }
 for(uint64_t n:{uint64_t(9),~uint64_t(0)}){m2::StateReader r(reinterpret_cast<uint8_t*>(&n),8);f.state(r);if(r.ok)return 7;}
 puts("1,000,000 FIFO operations match deque, including wraparound, serialization, restore and invalid lengths");
}