// Exact scheduler time on both 64-bit hosts and ARMv7. Attoseconds cannot be
// narrowed to uint64_t: that would wrap after only about 18 seconds.
#pragma once
#if defined(__SIZEOF_INT128__) && !defined(VF2_FORCE_PORTABLE_U128)
namespace vf2 { using uint128_t = unsigned __int128; }
#elif defined(VF2_USE_BOOST_U128)
#include <boost/multiprecision/cpp_int.hpp>
namespace vf2 { using uint128_t = boost::multiprecision::uint128_t; }
#else
#include <bit>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
namespace vf2 {
// Fixed-width modular arithmetic, with no sign/limb-count bookkeeping. This
// keeps the hot ARMv7 scheduler operations as two-word additions/comparisons.
struct uint128_t {
    uint64_t lo=0,hi=0;
    constexpr uint128_t()=default;
    constexpr uint128_t(uint64_t high,uint64_t low):lo(low),hi(high){}
    template<class I,std::enable_if_t<std::is_integral_v<I>,int> =0>
    constexpr uint128_t(I value):lo(uint64_t(value)){
        if constexpr(std::is_signed_v<I>)if(value<0)hi=~uint64_t(0);
    }
    template<class I,std::enable_if_t<std::is_integral_v<I>,int> =0>
    explicit constexpr operator I()const {
        if constexpr(std::is_same_v<I,bool>)return hi||lo;
        else return I(lo);
    }
    constexpr bool operator==(const uint128_t&)const=default;
    constexpr std::strong_ordering operator<=>(const uint128_t &b)const {
        if(auto order=hi<=>b.hi;order!=0)return order;
        return lo<=>b.lo;
    }
    constexpr uint128_t &operator+=(uint128_t b){uint64_t before=lo;lo+=b.lo;hi+=b.hi+(lo<before);return *this;}
    constexpr uint128_t &operator-=(uint128_t b){uint64_t borrow=lo<b.lo;lo-=b.lo;hi-=b.hi+borrow;return *this;}
    constexpr uint128_t &operator|=(uint128_t b){lo|=b.lo;hi|=b.hi;return *this;}
    constexpr uint128_t &operator&=(uint128_t b){lo&=b.lo;hi&=b.hi;return *this;}
    constexpr uint128_t &operator^=(uint128_t b){lo^=b.lo;hi^=b.hi;return *this;}
    friend constexpr uint128_t operator+(uint128_t a,uint128_t b){return a+=b;}
    friend constexpr uint128_t operator-(uint128_t a,uint128_t b){return a-=b;}
    friend constexpr uint128_t operator|(uint128_t a,uint128_t b){return a|=b;}
    friend constexpr uint128_t operator&(uint128_t a,uint128_t b){return a&=b;}
    friend constexpr uint128_t operator^(uint128_t a,uint128_t b){return a^=b;}
    friend constexpr uint128_t operator~(uint128_t a){return {~a.hi,~a.lo};}
    friend constexpr uint128_t operator-(uint128_t a){return uint128_t(0)-a;}
    friend constexpr uint128_t operator<<(uint128_t a,unsigned shift){
        if(!shift)return a;
        if(shift<64)return {(a.hi<<shift)|(a.lo>>(64-shift)),a.lo<<shift};
        if(shift<128)return {a.lo<<(shift-64),0};
        return 0;
    }
    friend constexpr uint128_t operator>>(uint128_t a,unsigned shift){
        if(!shift)return a;
        if(shift<64)return {a.hi>>shift,(a.lo>>shift)|(a.hi<<(64-shift))};
        if(shift<128)return {0,a.hi>>(shift-64)};
        return 0;
    }
    constexpr uint128_t &operator<<=(unsigned shift){return *this=*this<<shift;}
    constexpr uint128_t &operator>>=(unsigned shift){return *this=*this>>shift;}
    static constexpr uint64_t multiply_high(uint64_t a,uint64_t b){
        uint64_t a0=uint32_t(a),a1=a>>32,b0=uint32_t(b),b1=b>>32;
        uint64_t bottom=a0*b0,t=a1*b0+(bottom>>32);
        uint64_t middle=uint32_t(t)+a0*b1;
        return a1*b1+(t>>32)+(middle>>32);
    }
    friend constexpr uint128_t operator*(uint128_t a,uint128_t b){
        return {multiply_high(a.lo,b.lo)+a.lo*b.hi+a.hi*b.lo,a.lo*b.lo};
    }
    constexpr uint128_t &operator*=(uint128_t b){return *this=*this*b;}
    constexpr unsigned bits()const{return hi?64+std::bit_width(hi):std::bit_width(lo);}
    static constexpr uint128_t divide(uint128_t a,uint128_t b,uint128_t &remainder){
        if(!b)std::abort();
        if(!a.hi&&!b.hi){remainder=a.lo%b.lo;return a.lo/b.lo;}
        remainder=a;if(a<b)return 0;
        int shift=int(a.bits())-int(b.bits());uint128_t divisor=b<<shift,quotient=0;
        for(int i=shift;i>=0;i--){if(remainder>=divisor){remainder-=divisor;quotient|=uint128_t(1)<<i;}divisor>>=1;}
        return quotient;
    }
    friend constexpr uint128_t operator/(uint128_t a,uint128_t b){uint128_t r;return divide(a,b,r);}
    friend constexpr uint128_t operator%(uint128_t a,uint128_t b){uint128_t r;divide(a,b,r);return r;}
    constexpr uint128_t &operator/=(uint128_t b){return *this=*this/b;}
    constexpr uint128_t &operator%=(uint128_t b){return *this=*this%b;}
    constexpr uint128_t &operator++(){return *this+=1;}
    constexpr uint128_t &operator--(){return *this-=1;}
    constexpr uint128_t operator++(int){auto old=*this;++*this;return old;}
    constexpr uint128_t operator--(int){auto old=*this;--*this;return old;}
    template<class F,unsigned digits> F floating()const {
        unsigned width=bits();if(width<=digits)return F(lo);
        unsigned shift=width-digits;uint64_t top=uint64_t(*this>>shift);
        uint128_t rest=*this-(uint128_t(top)<<shift),half=uint128_t(1)<<(shift-1);
        if(rest>half||(rest==half&&(top&1)))top++;
        return std::ldexp(F(top),int(shift));
    }
    explicit operator double()const{return floating<double,53>();}
    explicit operator float()const{return floating<float,24>();}
};
static_assert(sizeof(uint128_t)==16&&std::is_trivially_copyable_v<uint128_t>);
}
#endif
