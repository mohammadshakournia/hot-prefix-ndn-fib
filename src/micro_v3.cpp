// micro_v3.cpp -- cost breakdown: parse only, parse+hash, hash FIB, hot hit-only replay
#include "fib_v3.hpp"
#include <fstream>
#include <iostream>
using namespace fibv3;
int main(int argc,char**argv){
  std::ifstream f(argv[1]),g(argv[2]); std::string line; std::vector<std::pair<std::string,int>> E;
  std::getline(f,line); while(std::getline(f,line)){auto c=line.rfind(','); E.emplace_back(canonicalize(line.substr(0,c)),std::stoi(line.substr(c+1)));}
  std::string buf; std::vector<std::pair<size_t,size_t>> sp; std::getline(g,line);
  while(std::getline(g,line)){auto n=canonicalize(line); sp.emplace_back(buf.size(),n.size()); buf+=n;}
  std::vector<std::string_view> Q; for(auto[o,n]:sp) Q.emplace_back(buf.data()+o,n);
  TrieFib3 trie; HashFib3 hash; hash.reserve(E.size()); for(auto&e:E){trie.insert(e.first,e.second);hash.insert(e.first,e.second);}
  auto T=[&](const char* name, auto&& fn){ uint64_t s=0; auto a=std::chrono::steady_clock::now(); for(int rep=0;rep<3;++rep) for(auto q:Q) s+=fn(q); auto b=std::chrono::steady_clock::now();
    std::cout<<name<<" "<<std::chrono::duration<double,std::nano>(b-a).count()/(3.0*Q.size())<<" ns (chk "<<(s&0xff)<<")\n";};
  T("parse_offsets",[&](std::string_view q){NameView v; parseName<false>(q,v); return (uint64_t)v.m;});
  T("parse_hashes",[&](std::string_view q){NameView v; parseName<true>(q,v); return v.ph[v.m];});
  T("hash_fib",[&](std::string_view q){auto r=hash.lookup(q); return (uint64_t)r.face;});
  hash.resetProbes(); for(auto q:Q) hash.lookup(q); std::cout<<"hash probes/lookup "<<double(hash.probes())/Q.size()<<"\n";
  T("trie",[&](std::string_view q){auto r=trie.lookup(q); return (uint64_t)r.face;});
  // hot: warm a B1 cache over the stream with no cutoff, then replay hit-only names
  HotConfig hc; hc.capacity=std::stoul(argv[3]); hc.learningLimit=0;
  HotPrefix3<fibv2::BoundedExactPolicy,HashFib3> hot(trie,hash,fibv2::BoundedExactPolicy(4*hc.capacity,1.0),hc);
  for(int r=0;r<2;++r) for(auto q:Q) hot.lookup(q);
  std::vector<std::string_view> H; uint64_t h0=hot.hits(); for(auto q:Q){uint64_t a=hot.hits(); hot.lookup(q); if(hot.hits()!=a) H.push_back(q);} (void)h0;
  std::cout<<"hit-only names "<<H.size()<<"\n";
  auto Th=[&](const char* name, auto&& fn){ uint64_t s=0; auto a=std::chrono::steady_clock::now(); for(int rep=0;rep<3;++rep) for(auto q:H) s+=fn(q); auto b=std::chrono::steady_clock::now();
    std::cout<<name<<" "<<std::chrono::duration<double,std::nano>(b-a).count()/(3.0*H.size())<<" ns\n";};
  Th("hot_on_hits",[&](std::string_view q){return (uint64_t)hot.lookup(q).face;});
  Th("hash_on_same_names",[&](std::string_view q){return (uint64_t)hash.lookup(q).face;});
  Th("parse_hashes_same",[&](std::string_view q){NameView v; parseName<true>(q,v); return v.ph[v.m];});
}
