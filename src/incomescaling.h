// Complete income-target semantics with exact arithmetic and a finite scaling calculation.
#pragma once
#include "numericbig.h"
namespace incomescaling {
using Values=std::vector<numeric::Integer>;
inline bool Reached(const Values& total,const std::vector<int64_t>& target){
    for(size_t i=0;i<total.size();++i)
        if((target[i]>=0 || total[i].Positive()) && numeric::Integer(target[i])<total[i])return true;
    return false;
}
// Called only when the initial threshold check did not stop the loop. An empty
// scaling round still counts once, as in the native loop. If every net increment
// is <= 0 the threshold is unreachable; one evaluation is the finite fixed policy.
inline numeric::Integer Scale(Values& total,const Values& delta,const std::vector<int64_t>& target){
    numeric::Integer rounds;bool reachable=false;
    for(size_t i=0;i<delta.size();++i)if(delta[i].Positive()){
        auto distance=numeric::Integer((std::max)(int64_t(0),target[i]))-total[i];
        auto candidate=distance/delta[i]+numeric::Integer(1);
        if(!reachable || candidate<rounds){rounds=std::move(candidate);reachable=true;}
    }
    if(!reachable)rounds=numeric::Integer(1);
    for(size_t i=0;i<total.size();++i)total[i]+=delta[i]*rounds;
    return rounds;
}
} // namespace incomescaling
