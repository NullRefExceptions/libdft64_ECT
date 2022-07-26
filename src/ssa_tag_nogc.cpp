#include "ssa_tag.h"
#include "sylvan_int.h"
#include "list"
#include "map"
#include "algorithm"
#include "libdft_api.h"

#ifdef TAG_SSA

using namespace sylvan;
extern FILE *log_fd;
#define LOGD(...)                     \
    do                                \
    {                                 \
        fprintf(log_fd, __VA_ARGS__); \
    } while (0)

typedef struct
{
    ssa_tag cb_cache_l;
    ssa_tag cb_cache_r;
    ssa_tag cb_cache_v;
    int volatile quit;
    SSA_Task *t;
    uint64_t pading[3];
} ssa_tls_t;

ssa_tls_t *ssa_tls;
BDD var_set;

ssa_tag ssa_tag_alloc(unsigned int offset, uint64_t tid)
{
    ssa_tls_t *tls = &ssa_tls[tid];
    SSA_Task *t = tls->t;
    uint8_t array[TAG_WIDTH];
    for (size_t i = 0; i < TAG_WIDTH; i++)
    {
        if (0x1 << i & offset)
            array[VAR_ORDER] = 1;
        else
            array[VAR_ORDER] = 0;
    }
    t->arg1 = var_set;
    t->arg2 = (uint64_t)array;
    mfence();
    t->task_type = 1;
    while(t->task_type!=0)
    ;
    return t->res;
}

ssa_tag ssa_tag_combine(ssa_tag const &lhs, ssa_tag const &rhs, uint64_t tid)
{
    if (lhs == 0)
        return rhs;
    if (rhs == 0 || lhs == rhs)
        return lhs;

    ssa_tls_t *tls = &ssa_tls[tid];
    if (tls->cb_cache_l == lhs && tls->cb_cache_r == rhs)
    {
        return tls->cb_cache_v;
    }

    SSA_Task *t = tls->t;
    t->arg1 = lhs;
    t->arg2 = rhs;
    mfence();
    t->task_type = 2;
    while(t->task_type!=0)
        ;
        
    BDD res = t->res;

    ssa_tls[tid].cb_cache_l = lhs;
    ssa_tls[tid].cb_cache_r = rhs;
    ssa_tls[tid].cb_cache_v = res;
    return res;
}

std::string ssa_tag_print(ssa_tag const &tag)
{
    std::string ss = "";
    ss += "{";
    std::vector<uint32_t> offset_buf;

    if (tag != 0)
    {
        uint8_t res[TAG_WIDTH];
        MTBDD leaf = mtbdd_enum_all_first(tag, var_set, res, NULL);
        while (leaf != mtbdd_false)
        {
            uint32_t offset = 0;
            for (size_t i = 0; i < TAG_WIDTH; i++)
                if (res[VAR_ORDER])
                    offset += 1 << i;
            offset_buf.push_back(offset);
            leaf = mtbdd_enum_all_next(tag, var_set, res, NULL);
        }
    }
    std::sort(offset_buf.begin(), offset_buf.end());
    auto it = offset_buf.begin();
    while (it != offset_buf.end())
    {
        uint32_t offset = *it;
        char buf[100];
        //sprintf(buf, "(%d, %d) ", offset, offset + 1);
        sprintf(buf, "%d ", offset);
        std::string s(buf);
        ss += s;
        it++;
    }
    ss += "}";
    return ss;
}

void ssa_init()
{
    //1.init sylvan
    lace_start(THREAD_CTX_BLK, LACE_DQ_SIZE,HELPER_THREAD_NUM);

    // use at most SYLVAN_MEMORY_LIMIT, nodes:cache ratio 2:1, initial size 1/32 of maximum
    sylvan_set_limits(SYLVAN_MEMORY_LIMIT, 1, 5);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_gc_disable();

    //2 var_set,use tmp tls block
    sylvan_tcb_t tmp_tls;
    tmp_tls.slots[my_region_slot] = -1;
    tmp_tls.slots[my_worker_id_slot] = 0;
    lace_n_workers_id = 1;
    uint64_t old_fs = _readfsbase_u64();
    _writefsbase_u64((uint64_t)&tmp_tls);

    uint32_t array[TAG_WIDTH];
    for (size_t i = 0; i < TAG_WIDTH; i++)
    {
        array[i] = i;
    }
    var_set = mtbdd_set_from_array((uint32_t *)&array, TAG_WIDTH);
    _writefsbase_u64(old_fs);
    lace_n_workers_id = 0;
    mfence();
    //3 ssa_tls
    ssa_tls = (ssa_tls_t *)memalign(LINE_SIZE, sizeof(ssa_tls_t) * THREAD_CTX_BLK);
}

void ssa_exit()
{
    lace_stop();
    sylvan_quit();
    free(ssa_tls);
}

void ssa_thread_start(uint64_t tid)
{
    ssa_tls_t *tls = &ssa_tls[tid];
    memset((void *)tls, 0, sizeof(ssa_tls_t));
    tls->t = lace_spawn_worker((void *)&tls->quit);
}

void ssa_thread_fini(uint64_t tid)
{
    ssa_tls_t *t = &ssa_tls[tid];
    t->quit = true;
    while (t->quit)
        ;
}

#else
void ssa_init() { return; }
void ssa_exit() { return; }
void ssa_thread_start(uint64_t tid) { return; }
void ssa_thread_fini(uint64_t tid) { return; }
ssa_tag ssa_tag_alloc(unsigned int offset, uint64_t tid) { return ssa_tag(); }
ssa_tag ssa_tag_combine(ssa_tag const &lhs, ssa_tag const &rhs, uint64_t tid) { return ssa_tag(); }
std::string ssa_tag_print(ssa_tag const &tag) { return nullptr; }
#endif