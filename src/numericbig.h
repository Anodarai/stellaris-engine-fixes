// Exact signed integers for calculations whose intermediate range exceeds 128 bits.
// Base 2^32 limbs; no floating point, platform-dependent signed overflow, or external runtime.
#pragma once
#include <stdint.h>
#include <vector>
#include <algorithm>
#include <stdexcept>
namespace numeric {
// An Integer's limbs. The game's terms and their products fit 256 bits, so those stay inline:
// the hooks on per-ship and per-design paths do their arithmetic without heap allocation. Only
// the rare wider value (the exponential fallback's precision) goes to the heap.
class Words {
    static constexpr size_t kInline=8;
    uint32_t local[kInline];
    uint32_t* heap=nullptr;
    size_t count=0,capacity=kInline;
    uint32_t* data(){return heap?heap:local;}
    const uint32_t* data()const{return heap?heap:local;}
    void reserve(size_t n){
        if(n<=capacity)return;
        size_t grown=(std::max)(n,capacity*2);
        auto* p=new uint32_t[grown];
        std::copy(data(),data()+count,p);
        delete[] heap;heap=p;capacity=grown;
    }
    void release(){delete[] heap;heap=nullptr;count=0;capacity=kInline;}
    void copyFrom(const Words& other){reserve(other.count);std::copy(other.data(),other.data()+other.count,data());count=other.count;}
    void takeFrom(Words& other){
        if(other.heap){heap=other.heap;capacity=other.capacity;count=other.count;other.heap=nullptr;other.capacity=kInline;}
        else{std::copy(other.local,other.local+other.count,local);count=other.count;}
        other.count=0;
    }
public:
    Words()=default;
    Words(const Words& other){copyFrom(other);}
    Words(Words&& other)noexcept{takeFrom(other);}
    Words& operator=(const Words& other){if(this!=&other){count=0;copyFrom(other);}return *this;}
    Words& operator=(Words&& other)noexcept{if(this!=&other){release();takeFrom(other);}return *this;}
    ~Words(){delete[] heap;}
    size_t size()const{return count;}
    bool empty()const{return count==0;}
    uint32_t& operator[](size_t i){return data()[i];}
    uint32_t operator[](size_t i)const{return data()[i];}
    uint32_t back()const{return data()[count-1];}
    void pop_back(){--count;}
    void push_back(uint32_t value){reserve(count+1);data()[count++]=value;}
    void resize(size_t n,uint32_t value){reserve(n);for(size_t i=count;i<n;++i)data()[i]=value;count=n;}
    uint32_t* begin(){return data();}
    uint32_t* end(){return data()+count;}
    bool operator==(const Words& other)const{return count==other.count && std::equal(data(),data()+count,other.data());}
};
class Integer {
    Words words;
    bool negative=false;
    void Normalize(){while(!words.empty() && !words.back())words.pop_back();if(words.empty())negative=false;}
    static int AbsCompare(const Integer& a,const Integer& b){
        if(a.words.size()!=b.words.size())return a.words.size()<b.words.size()?-1:1;
        for(size_t i=a.words.size();i>0;--i)if(a.words[i-1]!=b.words[i-1])return a.words[i-1]<b.words[i-1]?-1:1;
        return 0;
    }
    void AddMagnitude(const Integer& b){
        size_t size=(std::max)(words.size(),b.words.size());words.resize(size,0);uint64_t carry=0;
        for(size_t i=0;i<size;++i){uint64_t sum=uint64_t(words[i])+(i<b.words.size()?b.words[i]:0)+carry;words[i]=static_cast<uint32_t>(sum);carry=sum>>32;}
        if(carry)words.push_back(static_cast<uint32_t>(carry));
    }
    // Requires |*this| >= |b|.
    void SubMagnitude(const Integer& b){
        uint64_t borrow=0;
        for(size_t i=0;i<words.size();++i){uint64_t sub=(i<b.words.size()?uint64_t(b.words[i]):0)+borrow;
            uint64_t prior=words[i];words[i]=static_cast<uint32_t>(prior-sub);borrow=prior<sub;}
        Normalize();
    }
    void ShiftBit(bool bit){uint32_t carry=bit?1:0;for(auto& w:words){uint32_t next=w>>31;w=(w<<1)|carry;carry=next;}if(carry)words.push_back(carry);}
public:
    Integer()=default;
    explicit Integer(int64_t value){negative=value<0;uint64_t v=negative?uint64_t(0)-static_cast<uint64_t>(value):static_cast<uint64_t>(value);
        if(v){words.push_back(static_cast<uint32_t>(v));if(v>>32)words.push_back(static_cast<uint32_t>(v>>32));}}
    bool Zero()const{return words.empty();}
    bool Negative()const{return negative;}
    bool Positive()const{return !negative && !Zero();}
    size_t Bits()const{if(words.empty())return 0;size_t bits=(words.size()-1)*32;uint32_t top=words.back();while(top){++bits;top>>=1;}return bits;}
    Integer Abs()const{Integer v=*this;v.negative=false;return v;}
    Integer operator-()const{Integer v=*this;if(!Zero())v.negative=!negative;return v;}
    int Compare(const Integer& b)const{if(negative!=b.negative)return negative?-1:1;int c=AbsCompare(*this,b);return negative?-c:c;}
    bool operator==(const Integer& b)const{return negative==b.negative && words==b.words;}
    bool operator<(const Integer& b)const{return Compare(b)<0;}
    Integer& operator+=(const Integer& b){
        if(negative==b.negative)AddMagnitude(b);
        else {int cmp=AbsCompare(*this,b);if(cmp>=0)SubMagnitude(b);else {Integer result=b;result.SubMagnitude(*this);*this=std::move(result);}}
        Normalize();return *this;
    }
    Integer& operator-=(const Integer& b){return *this+=-b;}
    friend Integer operator+(Integer a,const Integer& b){return a+=b;}
    friend Integer operator-(Integer a,const Integer& b){return a-=b;}
    friend Integer operator*(const Integer& a,const Integer& b){
        Integer result;if(a.Zero() || b.Zero())return result;result.negative=a.negative!=b.negative;
        result.words.resize(a.words.size()+b.words.size(),0);
        for(size_t i=0;i<a.words.size();++i){uint64_t carry=0;
            for(size_t j=0;j<b.words.size();++j){uint64_t product=uint64_t(a.words[i])*b.words[j]+result.words[i+j]+carry;
                result.words[i+j]=static_cast<uint32_t>(product);carry=product>>32;}
            result.words[i+b.words.size()]=static_cast<uint32_t>(carry);
        }
        result.Normalize();return result;
    }
    static void DivMod(const Integer& numerator,const Integer& denominator,Integer& quotient,Integer& remainder){
        if(denominator.Zero())throw std::domain_error("integer division by zero");
        // Local outputs permit aliases with either input.
        Integer q,r;auto divisor=denominator.Abs();q.words.resize(numerator.words.size(),0);
        for(size_t limb=numerator.words.size();limb>0;--limb)for(int bit=31;bit>=0;--bit){
            r.ShiftBit(((numerator.words[limb-1]>>bit)&1)!=0);
            if(AbsCompare(r,divisor)>=0){r.SubMagnitude(divisor);q.words[limb-1]|=uint32_t(1)<<bit;}
        }
        q.negative=numerator.negative!=denominator.negative;r.negative=numerator.negative;q.Normalize();r.Normalize();
        quotient=std::move(q);remainder=std::move(r);
    }
    friend Integer operator/(const Integer& a,const Integer& b){Integer q,r;DivMod(a,b,q,r);return q;}
    friend Integer operator%(const Integer& a,const Integer& b){Integer q,r;DivMod(a,b,q,r);return r;}
    // Arithmetic right shifts and cleared low bits round down, unlike signed IDIV.
    Integer FloorDivide(const Integer& positiveDivisor)const{
        Integer q,r;DivMod(*this,positiveDivisor,q,r);
        if(Negative() && !r.Zero())q-=Integer(1);
        return q;
    }
    int64_t Clamp()const{
        if(words.empty())return 0;if(words.size()>2)return negative?INT64_MIN:INT64_MAX;
        uint64_t v=words[0];if(words.size()==2)v|=uint64_t(words[1])<<32;
        uint64_t limit=negative?uint64_t(1)<<63:static_cast<uint64_t>(INT64_MAX);
        if(v>=limit)return negative?INT64_MIN:INT64_MAX;
        return negative?-static_cast<int64_t>(v):static_cast<int64_t>(v);
    }
};
} // namespace numeric
