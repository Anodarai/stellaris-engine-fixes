#include <array>
#include <cstdio>
#include <thread>
#include "../src/incomeaccumulator.h"
int failures=0;
void Check(bool ok,const char* name){if(!ok && ++failures<=20)printf("FAIL %s\n",name);}
int main(){
    incomeaccumulator::Pass pass;std::array<int64_t,4> a{},b{};int64_t old=0;numeric::Sum sum;
    for(int round=0;round<40;++round){
        a={INT64_MAX,INT64_MIN,0,17};b={0,0,0,0};pass.Begin({{b.data(),b.size()},{a.data(),a.size()}});
        std::vector<std::thread> workers;
        for(int thread=0;thread<8;++thread)workers.emplace_back([&,thread]{for(int i=0;i<10000;++i){int64_t previous;
            int64_t delta=(thread%2?INT64_MAX:-INT64_MAX);pass.Add(a.data(),delta,previous);pass.Add(a.data()+1,delta,previous);pass.Add(b.data(),delta,previous);}});
        for(auto& worker:workers)worker.join();
        Check(a[0]==INT64_MAX && a[1]==INT64_MIN && b[0]==0,"mixed-sign workers have order-independent exact cancellation");
        pass.Add(a.data(),1,old);Check(old==INT64_MAX && a[0]==INT64_MAX,"published view saturates without losing carry");
        pass.Add(a.data(),-1,old);Check(a[0]==INT64_MAX,"carry cancels exactly");
        Check(pass.End(),"joined representable pass ends cleanly");
        Check(!pass.Add(a.data(),1,old),"unregistered lifetime falls through");
    }
    a={INT64_MAX,0,0,0};pass.Begin({{a.data(),a.size()}});pass.Add(a.data(),INT64_MAX,old);
    Check(pass.Read(a.data(),sum) && sum.Clamp()==INT64_MAX,"wide read retains oversized sum");
    bool exchanged=false;pass.CompareSubtract(a.data(),INT64_MAX,INT64_MAX,old,exchanged);
    Check(exchanged && a[0]==INT64_MAX,"compare-subtract removes exact amount from wide total");
    pass.CompareSubtract(a.data(),0,1,old,exchanged);Check(!exchanged && old==INT64_MAX,"compare-subtract rejects stale expected value");
    pass.Add(a.data(),INT64_MAX,old);pass.Take(a.data(),sum);sum.Subtract(INT64_MAX);
    Check(sum.Clamp()==INT64_MAX && a[0]==0,"drain transfers carry before destination narrowing");
    pass.Add(a.data(),INT64_MAX,old);pass.Set(a.data(),-19);Check(pass.End() && a[0]==-19,"explicit reset discards preceding carry");
    a[0]=INT64_MIN;pass.Begin({{a.data(),1}});pass.Add(a.data(),-1,old);Check(!pass.End(),"undrained unrepresentable residual is reported");
    a[0]=12;pass.Begin({{a.data(),1}});pass.Add(a.data(),3,old);Check(a[0]==15 && pass.End(),"reused address begins from new contents");
    bool rejected=false;try{pass.Begin({{a.data(),2},{a.data()+1,2}});}catch(...){rejected=true;}Check(rejected,"overlap rejected before publication");
    printf("income accumulator: 3.2 million mixed-sign worker iterations, drains, resets, CAS and address reuse; %d failures\n",failures);return failures?1:0;
}
