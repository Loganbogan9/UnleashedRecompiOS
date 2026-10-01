#pragma once
// Minimal PPC ABI stand-in so the production memory/heap sources can be tested
// without copyrighted game input or generated PowerPC functions.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>
#include <o1heap.h>
#include <xbox.h>
union PPCRegister { uint64_t u64; uint32_t u32; double f64; };
struct PPCContext
{
    PPCRegister r1{}, r3{}, r4{}, r5{}, r6{}, r7{}, r8{}, r9{}, r10{}, r13{};
    PPCRegister f1{}, f2{}, f3{}, f4{}, f5{}, f6{}, f7{}, f8{}, f9{}, f10{}, f11{}, f12{}, f13{};
    uint32_t fpscr{};
};
using PPCFunc = void(PPCContext&, uint8_t*);
struct PPCFuncMapping { uint32_t guest; PPCFunc* host; };
inline PPCFuncMapping PPCFuncMappings[] = { { 0, nullptr } };
#define PPC_MEMORY_SIZE 0x100000000ull
#define PPC_LOOKUP_FUNC(base, address) (*reinterpret_cast<PPCFunc**>((base) + 0x18000 + (address)))
#define PPC_FUNC(name) void name(PPCContext& ctx, uint8_t* base)
