// Build on a host with __int128 and -DVF2_FORCE_PORTABLE_U128 to check the
// ARMv7 fallback against native arithmetic, including clock wrap/serialization.
#include "../../src/core/wide_int.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
using U = vf2::uint128_t;
using N = unsigned __int128;
static void same(U a, N b) {
    if (uint64_t(a) != uint64_t(b) || uint64_t(a >> 64) != uint64_t(b >> 64)) {
        std::fputs("uint128 arithmetic differs\n", stderr);
        std::exit(1);
    }
}
int main() {
    std::mt19937_64 rng(0xAC0128);
    constexpr U second = 1000000000000000000ull;
    same(second * 3600, N(1000000000000000000ull) * 3600);
    same(U(-1),N(-1));same(U(-1234567),N(-1234567));
    const N edges[]={0,1,2,3,(N(1)<<53)+1,(N(1)<<64)-1,N(1)<<64,(N(1)<<127),~N(0)};
    for(N x:edges){
        U a=(U(uint64_t(x>>64))<<64)|uint64_t(x);
        for(unsigned s=0;s<128;s++){same(a<<s,x<<s);same(a>>s,x>>s);}
        for(N y:edges){
            U b=(U(uint64_t(y>>64))<<64)|uint64_t(y);
            same(a+b,x+y);same(a-b,x-y);same(a*b,x*y);
            if(y){same(a/b,x/y);same(a%b,x%y);}
        }
        if(double(a)!=double(x)||float(a)!=float(x))return 2;
    }
    for (int i = 0; i < 10000; ++i) {
        uint64_t ah = rng(), al = rng(), bh = rng(), bl = rng() | 1;
        U a = (U(ah) << 64) | al, b = (U(bh) << 64) | bl;
        N x = (N(ah) << 64) | al, y = (N(bh) << 64) | bl;
        same(a + b, x + y); same(a - b, x - y); same(a * b, x * y);
        same(a / b, x / y); same(a % b, x % y);
        same(a / bl, x / bl); same(a % bl, x % bl);
        int shift = rng() % 128;
        same(a << shift, x << shift); same(a >> shift, x >> shift);
        same((U(uint64_t(a >> 64)) << 64) | uint64_t(a), x);
        if ((a < b) != (x < y) || (a == b) != (x == y)) return 1;
        if(double(a)!=double(x)||float(a)!=float(x))return 2;
    }
    std::puts("portable uint128: 10000 exact arithmetic and serialization comparisons passed");
}
