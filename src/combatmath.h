// The engine's Q7 logarithm/exponential recurrence, widened before every operation.
// If the native exponential recurrence overflows, evaluate its exponent with rigorous
// integer intervals. Ordinary non-overflowing recurrence results remain bit-identical.
#pragma once
#include "numericbig.h"
namespace combatmath {
using numeric::Integer;
inline bool Fits(const Integer& value){return Integer(value.Clamp())==value;}
inline Integer Down(const Integer& value,int64_t divisor){return value.FloorDivide(Integer(divisor));}
inline Integer Ceil(const Integer& value,const Integer& positive){return (value+positive-Integer(1))/positive;}
inline Integer Pow2(size_t bits){Integer value(1);for(size_t i=0;i<bits;++i)value=value*Integer(2);return value;}
inline Integer Log(Integer value){
    if(!value.Positive() || value==Integer(128))return Integer();
    Integer offset;
    while(!(value<Integer(256))){value=value*Integer(128)/Integer(348);offset+=Integer(128);}
    value-=Integer(128);Integer term=value,total=value,denominator(128);
    for(;;){term=Down(term*value,128);auto next=term*Integer(-128)/(denominator+Integer(128));
        total+=next;term=-term;denominator+=Integer(128);if(next.Zero())return total+offset;}
}
// floor(128*exp(exponent/128)), or cap+1 when the exact result exceeds cap.
// Taylor terms have outward-rounded bounds; after n>2*x the remaining tail is
// bounded by twice the next term. Precision increases until both bounds agree.
inline Integer Exp(const Integer& exponent,const Integer& cap){
    if(exponent.Zero())return cap<Integer(128)?cap+Integer(1):Integer(128);
    bool inverse=exponent.Negative();auto magnitude=exponent.Abs();
    if(inverse && Integer(896)<magnitude)return Integer(); // exp(-7)*128 < 1
    if(!inverse && Integer(static_cast<int64_t>(cap.Bits()+1)*128)<magnitude)return cap+Integer(1);
    int64_t a=magnitude.Clamp();
    for(size_t precision=cap.Bits()+128;;precision*=2){
        auto scale=Pow2(precision);Integer lowTerm=scale,highTerm=scale,low=scale,high=scale;
        size_t limit=size_t(a/128)*2+precision*2+32;
        for(size_t n=1;n<=limit;++n){
            Integer denominator(static_cast<int64_t>(n)*128);
            lowTerm=lowTerm*Integer(a)/denominator;highTerm=Ceil(highTerm*Integer(a),denominator);
            low+=lowTerm;high+=highTerm;
            if(static_cast<int64_t>(n)*128<=a*2)continue;
            auto tail=Ceil(highTerm*Integer(a),Integer(static_cast<int64_t>(n+1)*128))*Integer(2);
            auto upper=high+tail;
            Integer lowerAnswer,upperAnswer;
            if(inverse){lowerAnswer=scale*Integer(128)/upper;upperAnswer=scale*Integer(128)/low;}
            else {lowerAnswer=low*Integer(128)/scale;upperAnswer=upper*Integer(128)/scale;}
            if(cap<lowerAnswer)return cap+Integer(1);
            if(lowerAnswer==upperAnswer)return lowerAnswer;
        }
    }
}
inline Integer Power(Integer base,const Integer& exponent,const Integer& cap){
    bool negative=base.Negative() && !(exponent%Integer(256)).Zero();
    auto logarithm=Log(base.Abs());auto product=exponent*logarithm;
    auto argument=Down(product,128);auto numerator=argument*Integer(128);
    Integer sum(128),term(128),denominator(128);
    bool overflow=!Fits(base) || !Fits(product);
    while(!overflow){
        auto factor=numerator/denominator;auto next=term*factor;
        if(!Fits(next)){overflow=true;break;}
        term=Down(next,128);sum+=term;denominator+=Integer(128);
        if(!Fits(sum) || !Fits(sum*Integer(256))){overflow=true;break;}
        if(term.Zero())break;
    }
    if(overflow)sum=Exp(argument,cap);
    return negative?-sum:sum;
}
struct Defines {int64_t armor,shield,damage,health,exponent,scale;};
inline int64_t Formula(const int64_t* values,bool allowZeroDamage,const Defines& d){
    auto bonus=Integer(values[0])*Integer(256);auto scale=Down(Integer(d.scale),256);
    if(scale.Zero())return bonus.Clamp();
    auto denominator=Integer(128)-Integer(values[4]);if(denominator<Integer(1))denominator=Integer(1);
    auto damage=Down(Down(Integer(d.damage),256)*Integer(values[5]),128);
    if(!allowZeroDamage && damage<Integer(128))damage=Integer(128);
    auto weighted=Down(Down(Integer(d.armor),256)*Integer(values[2]),128)*Integer(128)+
        Down(Down(Integer(d.shield),256)*Integer(values[3]),128)*Integer(128)+Integer(values[1])*Integer(128);
    weighted=weighted/denominator;
    weighted=Down(weighted*Down(Integer(d.health),256),128);
    auto base=Down(damage*weighted,128);
    // Larger powers cannot cancel the fixed bonus back into the destination range.
    auto cap=bonus.Abs()+Integer(INT64_MAX)+Integer(256);
    auto power=Power(base,Down(Integer(d.exponent),256),cap);
    return (Down(power*Integer(2)*scale,256)*Integer(256)+bonus).Clamp();
}
} // namespace combatmath
