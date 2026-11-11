// Signed fixed-point arithmetic with wide intermediates and saturation at the output boundary.
#pragma once
#include <intrin.h>
#include <stdint.h>

namespace numeric {

inline uint64_t Magnitude(int64_t value) {
    return value < 0 ? uint64_t(0) - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
}
enum class Rounding { TowardZero, Floor, NearestAway };

// divisor must be nonzero. Check the high word before _udiv128, which faults if its
// unsigned quotient does not fit. No signed negation of INT64_MIN and no narrow product.
inline int64_t MulDiv(int64_t a, int64_t b, int64_t divisor, Rounding rounding = Rounding::TowardZero) {
    bool negative = (a < 0) != (b < 0);
    negative = negative != (divisor < 0);
    uint64_t high, low = _umul128(Magnitude(a), Magnitude(b), &high);
    uint64_t denominator = Magnitude(divisor);
    int64_t cap = negative ? INT64_MIN : INT64_MAX;
    if (high >= denominator) return cap;
    uint64_t remainder;
    uint64_t quotient = _udiv128(high, low, denominator, &remainder);
    bool increment = (rounding == Rounding::Floor && negative && remainder != 0) ||
        (rounding == Rounding::NearestAway && remainder >= (denominator + 1) / 2);
    if (increment) {
        if (quotient == UINT64_MAX) return cap;
        ++quotient;
    }
    uint64_t limit = negative ? uint64_t(1) << 63 : static_cast<uint64_t>(INT64_MAX);
    if (quotient >= limit) return cap;
    return negative ? -static_cast<int64_t>(quotient) : static_cast<int64_t>(quotient);
}

// A sum of at most INT32_MAX int64 values fits comfortably in signed 128 bits. Keep
// cancellation exact; saturating each addition would make the result order-dependent.
struct Sum {
    uint64_t low = 0;
    int64_t high = 0;
    void Add(int64_t value) {
        unsigned char carry = _addcarry_u64(0, low, static_cast<uint64_t>(value), &low);
        high += (value < 0 ? -1 : 0) + static_cast<int64_t>(carry);
    }
    void Subtract(int64_t value) {
        unsigned char borrow = _subborrow_u64(0, low, static_cast<uint64_t>(value), &low);
        high -= (value < 0 ? -1 : 0) + static_cast<int64_t>(borrow);
    }
    void Merge(const Sum& other) {
        unsigned char carry=_addcarry_u64(0,low,other.low,&low);
        high+=other.high+static_cast<int64_t>(carry);
    }
    int64_t Clamp() const {
        if (high == 0 && low <= static_cast<uint64_t>(INT64_MAX)) return static_cast<int64_t>(low);
        if (high == -1 && low >= (uint64_t(1) << 63)) {
            // This conversion is defined by the MSVC x64 ABI (two's complement).
            return static_cast<int64_t>(low);
        }
        return high < 0 ? INT64_MIN : INT64_MAX;
    }
    bool Fits() const {
        return (high==0 && low<=static_cast<uint64_t>(INT64_MAX)) ||
               (high==-1 && low>=(uint64_t(1)<<63));
    }
};

struct Unsigned128 {uint64_t low,high;};
inline Unsigned128 Abs(const Sum& value) {
    Unsigned128 out{value.low,static_cast<uint64_t>(value.high)};
    if(value.high<0) {out.low=~out.low+1;out.high=~out.high+(out.low==0);}
    return out;
}
inline bool AtLeast(Unsigned128 a,Unsigned128 b) {return a.high>b.high || (a.high==b.high && a.low>=b.low);}
inline Unsigned128 Difference(Unsigned128 a,Unsigned128 b) {
    unsigned char borrow=_subborrow_u64(0,a.low,b.low,&a.low);a.high-=b.high+borrow;return a;
}
// A 128-bit sum times a 64-bit scale needs 192 bits. Divide the full product before
// narrowing; this also handles coupled fleet totals without losing health ratios.
inline int64_t Ratio(const Sum& numerator,const Sum& denominator,uint64_t scale,Rounding rounding=Rounding::TowardZero) {
    auto a=Abs(numerator),d=Abs(denominator);
    if(!(d.low|d.high))return 0;
    uint64_t product[3],carry,top;
    product[0]=_umul128(a.low,scale,&carry);
    product[1]=_umul128(a.high,scale,&top);
    unsigned char overflow=_addcarry_u64(0,product[1],carry,&product[1]);product[2]=top+overflow;
    bool negative=(numerator.high<0)!=(denominator.high<0),tooLarge=false;
    uint64_t quotient=0;Unsigned128 remainder{0,0};
    for(int bit=191;bit>=0;--bit) {
        bool shiftedOut=(remainder.high>>63)!=0;
        remainder.high=(remainder.high<<1)|(remainder.low>>63);
        remainder.low=(remainder.low<<1)|((product[bit/64]>>(bit%64))&1);
        if(shiftedOut || AtLeast(remainder,d)) {
            remainder=Difference(remainder,d);
            if(bit>=64)tooLarge=true;else quotient|=uint64_t(1)<<bit;
        }
    }
    bool increment=(rounding==Rounding::Floor && negative && (remainder.low|remainder.high)) ||
        (rounding==Rounding::NearestAway && AtLeast(remainder,Difference(d,remainder)));
    if(increment){if(quotient==UINT64_MAX)tooLarge=true;else ++quotient;}
    uint64_t limit=negative?uint64_t(1)<<63:static_cast<uint64_t>(INT64_MAX);
    if(tooLarge || quotient>=limit)return negative?INT64_MIN:INT64_MAX;
    return negative?-static_cast<int64_t>(quotient):static_cast<int64_t>(quotient);
}
inline void Coupled(const Sum& current,const Sum& maximum,int64_t& outCurrent,int64_t& outMaximum) {
    if(current.Fits() && maximum.Fits()){outCurrent=current.Clamp();outMaximum=maximum.Clamp();return;}
    auto a=Abs(current),b=Abs(maximum);auto bound=AtLeast(a,b)?a:b;
    Sum denominator{bound.low,static_cast<int64_t>(bound.high)};
    outCurrent=Ratio(current,denominator,INT64_MAX);
    outMaximum=Ratio(maximum,denominator,INT64_MAX);
}

inline int64_t Add(int64_t a, int64_t b) { Sum s; s.Add(a); s.Add(b); return s.Clamp(); }
inline int64_t Subtract(int64_t a, int64_t b) { Sum s; s.Add(a); s.Subtract(b); return s.Clamp(); }
inline int64_t Negate(int64_t value) { return Subtract(0, value); }

inline int64_t BinaryToDecimal(int64_t raw) {
    return MulDiv(raw, 100000, 32768, Rounding::NearestAway);
}
inline int64_t DecimalToBinary(int64_t raw) {
    return MulDiv(raw, 32768, 100000);
}

} // namespace numeric
