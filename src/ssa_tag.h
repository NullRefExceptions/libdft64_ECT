#ifndef SSA_TAG_H
#define SSA_TAG_H
#include "stdint.h"
#include <sylvan_config.h>
typedef enum
{
    ss_combine_wait,
    ss_alloc_wait,

    ss_cache_access,
    ss_cache_hit,

    ss_combine,
    ss_alloc,

    ss_cb_ll,
    ss_cb_lh,
    ss_cb_hh,

    bdd_cb_count,

    ss_max
} ss_index;

extern __inline unsigned long long
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
__rdtsc (void) { return __builtin_ia32_rdtsc ();}

#define TAG_WIDTH 24   //set max input size to 16MB

#define VAR_ORDER_RE i
#define VAR_ORDER_ER TAG_WIDTH -1 -i
#define VAR_ORDER VAR_ORDER_RE

#define LACE_DQ_SIZE 1000000 //
#define SYLVAN_MEMORY_LIMIT 512 * 1024 * 1024 //512mb
#define HELPER_THREAD_NUM 0  //no helper thread until we figure out how to identify heavy work

#define SSA_BLK 0x10000 // 0x100000
#define SSA_GC_THRESHOLD SSA_BLK / 2 //申请新SSA块的阈值

#ifdef SSA_PROFILE
#define SS_ADD(idx, count) tls->ss[idx] += count
#define SS_EVENT_PRE        \
    uint64_t p = __rdtsc(); \
    uint64_t f = 0;
#define SS_EVENT_SET_FACTOR f = 1;
#define SS_EVENT_END(idx) SS_ADD(idx, (__rdtsc() - p) * f)
#else
#define SS_ADD(idx, count)
#define SS_EVENT_PRE 
#define SS_EVENT_SET_FACTOR
#define SS_EVENT_END(idx)
#endif

#ifdef SSA_PROFILE
//#define SSA_PROFILE_GC
#define SSA_PROFILE_COMBINE
#endif

#ifdef SSA_NOGC
#include "ssa_tag_nogc.h"
#else
#include "ssa_tag_gc.h"
#endif

void ssa_init();
void ssa_exit();
void ssa_thread_start(uint64_t tid);
void ssa_thread_fini(uint64_t tid);
ssa_tag ssa_tag_alloc(unsigned int offset, uint64_t tid);
ssa_tag ssa_tag_combine(ssa_tag const &lhs, ssa_tag const &rhs, uint64_t tid);
std::string ssa_tag_print(ssa_tag const &tag);
#endif