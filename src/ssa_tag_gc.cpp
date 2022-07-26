#include "ssa_tag.h"
#include "sylvan_int.h"
#include "list"
#include "map"
#include "algorithm"
#include "libdft_api.h"

#ifdef TAG_SSA
#ifdef TAINT_COUNT
extern  uint64_t combine_count;
#endif

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
#ifdef SSA_PROFILE
    uint64_t ss[ss_max];
    uint64_t pading2[6];
#endif
} ssa_tls_t;

struct
{
    volatile uint64_t __attribute__((aligned(LINE_SIZE))) _h;
    volatile uint64_t __attribute__((aligned(LINE_SIZE))) _t;
    ssa **__attribute__((aligned(LINE_SIZE))) _q;
    uint64_t __attribute__((aligned(LINE_SIZE))) gc_lock;
} free_ssa;
std::list<ssa *> ssa_blk_list;

ssa_tls_t *ssa_tls;

BDD var_set;

/*
分配新的ssa_blk，并填充free_ssa_stack。可能在初始化时被调用，或者一次gc后
free_ssa_stack中可用元素仍少于阈值（75%）时被调用
*/
void add_ssa_blk()
{
    uint64_t empty_count =
        free_ssa._t >= free_ssa._h ? SSA_BLK - 1 - (free_ssa._t - free_ssa._h) : free_ssa._h - free_ssa._t - 1;
    ssa *sp = (ssa *)malloc(sizeof(ssa) * SSA_BLK);
    memset((void *)sp, 0xff, sizeof(ssa) * SSA_BLK);
    ssa_blk_list.push_back(sp);
    uint64_t temp_t = free_ssa._t;
    /*
    当empty_count小于SSA_BLK时，剩余的ssa在之后的gc时被使用
    */
    for (size_t i = 0; i < empty_count; i++)
    {
        free_ssa._q[temp_t] = &sp[i];
        temp_t = (temp_t + 1) % SSA_BLK;
    }
    free_ssa._t = temp_t;
}
/*
关于free_ssa的gc，ssa会在alloc和combine时被消耗，因此需要回收没有被引用的ssa
gc发生时，free_ssa队列为空，我们将遍历ssa_blk中的ssa直到填满free_ssa队列，如果
遍历完仍然不能填满，就分配新的ssa_blk，遍历过程中,ref_count代表该ssa的状态：
0x0000000000000000->ssa曾经被使用过,可以使用，将其加入free_ssa队列需要unprotect对应bbd指针
0xffffffffffffffff->ssa从未被使用过,可以使用，将其加入free_ssa队列不需要unprotect对应bbd指针
其他->正在被使用，不能加入free_ssa队列
*/
void ssa_gc()
{
    uint64_t empty_count =
        free_ssa._t >= free_ssa._h ? SSA_BLK - 1 - (free_ssa._t - free_ssa._h) : free_ssa._h - free_ssa._t - 1;
    uint64_t temp_t = free_ssa._t;
#ifdef SSA_PROFILE_GC
    LOGD("empty_count %lu\n", empty_count);
    uint64_t unused_ssa = 0;
    uint64_t unalloced_ssa = 0;
    bool alloc_newblk = false;
#endif
    auto it = ssa_blk_list.begin();
    while (it != ssa_blk_list.end())
    {
        for (uint64_t i = 0; i < SSA_BLK; i++)
        {
            if ((*it)[i].ref_count == 0) //
            {
                free_ssa._q[temp_t] = &(*it)[i];
#ifdef SSA_PROFILE_GC
                unused_ssa++;
#endif
                /*
                为了防止重复加入free_ssa queue，设置ref_count为0xffffffffffffffff,
                并且unprotect对应bbd指针,sylvan gc即刻就被允许释放该bdd，而不必
                等到新的bdd值替换掉旧的之后。
                */
                //(*it)[i].ref_count = 0xffffffffffffffff;
                if ((*it)[i].bdd != 0)
                    mtbdd_unprotect(&(*it)[i].bdd);
                temp_t = (temp_t + 1) % SSA_BLK;
                if (--empty_count == 0)
                {
                    free_ssa._t = temp_t;
                    goto exit_gc;
                }
                continue;
            }
            if ((*it)[i].ref_count == 0xffffffffffffffff)
            {
#ifdef SSA_PROFILE_GC
                unalloced_ssa++;
#endif
                free_ssa._q[temp_t] = &(*it)[i];
                temp_t = (temp_t + 1) % SSA_BLK;
                if (--empty_count == 0)
                {
                    free_ssa._t = temp_t;
                    goto exit_gc;
                }
                continue;
            }
        }
        it++;
    }
    free_ssa._t = temp_t;
    if (empty_count > SSA_GC_THRESHOLD)
    {
        add_ssa_blk();
#ifdef SSA_PROFILE_GC
        alloc_newblk = true;
#endif
    }

exit_gc:
#ifdef SSA_PROFILE_GC
    if (alloc_newblk)
        LOGD("gc: unused_ssa ssa %lu,unalloced_ssa %lu,ssa blk: %lu new blk\n", unused_ssa, unalloced_ssa, ssa_blk_list.size());
    else
        LOGD("gc: unused_ssa ssa %lu,unalloced_ssa %lu,ssa blk: %lu\n", unused_ssa, unalloced_ssa, ssa_blk_list.size());
#endif
    free_ssa.gc_lock = 0;
    return;
}

ssa_tag ssa_tag_alloc(unsigned int offset, uint64_t tid)
{
    ssa_tls_t *tls = &ssa_tls[tid];
    SS_ADD(ss_alloc, 1);
    //申请ssa存放新产生的tag
    uint64_t idx;
    do
    {
        idx = free_ssa._h;
        SS_EVENT_PRE
        while (unlikely(idx == free_ssa._t))
        {
            if (__sync_bool_compare_and_swap(&free_ssa.gc_lock, 0, 1))
            {
                ssa_gc();
            }
            SS_EVENT_SET_FACTOR
        }
        SS_EVENT_END(ss_alloc_wait);
    } while (!__sync_bool_compare_and_swap(&free_ssa._h, idx, (idx + 1) % SSA_BLK));

    ssa *victim = free_ssa._q[idx];
    victim->ref_count = 1;

    //设置参数，发送分配tag指令给BDD后端
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
    while (t->task_type != 0)
        ;
    victim->bdd = t->res;
    sylvan_protect(&victim->bdd);
    t->res = 0;
    return ssa_tag(victim);
}

ssa_tag ssa_tag_combine(ssa_tag const &lhs, ssa_tag const &rhs, uint64_t tid)
{
    //边界条件
    if (lhs.ssa_ref == NULL)
        return rhs;
    if (rhs.ssa_ref == NULL || lhs == rhs)
        return lhs;

#ifdef TAINT_COUNT
    combine_count++;
#endif
    //检查cache是否命中
    ssa_tls_t *tls = &ssa_tls[tid];
    SS_ADD(ss_cache_access, 1);
    if (tls->cb_cache_l == lhs && tls->cb_cache_r == rhs)
    {
        SS_ADD(ss_cache_hit, 1);
        return tls->cb_cache_v;
    }

    //设置参数，发送合并tag指令给BDD后端
    SS_ADD(ss_combine, 1);
    SSA_Task *t = tls->t;
    t->arg1 = lhs.ssa_ref->bdd;
    t->arg2 = rhs.ssa_ref->bdd;
    mfence();
    t->task_type = 2;
    while (t->task_type != 0)
        ;

#ifdef SSA_PROFILE_COMBINE
    /*     if (t->l_count2 < PARALLEL_COMBINE_THRESHOLD && t->r_count2 < PARALLEL_COMBINE_THRESHOLD)
        {
            SS_ADD(ss_cb_ll, 1);
        }
        else if (t->l_count2 > PARALLEL_COMBINE_THRESHOLD && t->r_count2 > PARALLEL_COMBINE_THRESHOLD)
        {
            SS_ADD(ss_cb_hh, 1);
        }
        else
        {
            SS_ADD(ss_cb_lh, 1);
        } */
    SS_ADD(bdd_cb_count, t->cb_count);
/*     LOGD("combine\n");
    LOGD("\tl_guess:%lu,l_actual:%lu\n",t->l_count2,t->l_count1);
    LOGD("\tr_guess:%lu,r_actual:%lu\n",t->r_count2,t->r_count1);
    LOGD("\tcombine_count:%lu\n",t->cb_count); */
#endif

    //检查返回结果是否为参与合并的两个tag之一（一个tag为另一个tag的子集）
    if (t->res == lhs.ssa_ref->bdd)
    {
        tls->cb_cache_l = lhs;
        tls->cb_cache_r = rhs;
        tls->cb_cache_v = lhs;
        return lhs;
    }
    if (t->res == rhs.ssa_ref->bdd)
    {
        tls->cb_cache_l = lhs;
        tls->cb_cache_r = rhs;
        tls->cb_cache_v = rhs;
        return rhs;
    }

    //申请ssa存放新产生的tag
    uint64_t ssa_idx;
    do
    {
        ssa_idx = free_ssa._h;
        SS_EVENT_PRE
        while (unlikely(ssa_idx == free_ssa._t))
        {
            if (__sync_bool_compare_and_swap(&free_ssa.gc_lock, 0, 1))
            {
                ssa_gc();
            }
            SS_EVENT_SET_FACTOR
        }
        SS_EVENT_END(ss_alloc_wait);
    } while (!__sync_bool_compare_and_swap(&free_ssa._h, ssa_idx, (ssa_idx + 1) % SSA_BLK));

    ssa *victim = free_ssa._q[ssa_idx];
    victim->ref_count = 1;
    victim->bdd = t->res;
    sylvan_protect(&victim->bdd);
    t->res = 0;

    ssa_tag res(victim);
    tls->cb_cache_l = lhs;
    tls->cb_cache_r = rhs;
    tls->cb_cache_v = res;
    return res;
}

std::string ssa_tag_print(ssa_tag const &tag)
{
    std::string ss = "";
    ss += "{";
    std::vector<uint32_t> offset_buf;

    if (tag.ssa_ref != NULL)
    {
        uint8_t res[TAG_WIDTH];
        MTBDD leaf = mtbdd_enum_all_first(tag.ssa_ref->bdd, var_set, res, NULL);
        while (leaf != mtbdd_false)
        {
            uint32_t offset = 0;
            for (size_t i = 0; i < TAG_WIDTH; i++)
                if (res[VAR_ORDER])
                    offset += 1 << i;
            offset_buf.push_back(offset);
            leaf = mtbdd_enum_all_next(tag.ssa_ref->bdd, var_set, res, NULL);
        }
    }
    std::sort(offset_buf.begin(), offset_buf.end());
    auto it = offset_buf.begin();
    while (it != offset_buf.end())
    {
        uint32_t offset = *it;
        char buf[100];
        sprintf(buf, "(%d, %d) ", offset, offset + 1);
        // sprintf(buf, "%d ", offset);
        std::string s(buf);
        ss += s;
        it++;
    }
    ss += "}";
    return ss;
}

#ifdef SSA_PROFILE

extern thread_ctx_t *threads_ctx;
extern size_t tctx_ct;
extern tag_dir_t tag_dir;
std::map<ssa *, uint64_t> c_map;
bool volatile profile_exit = false;
void ssa_lastcheck()
{
    // 1.保存所有依然被引用的ssa到c_map
    c_map.clear();
    auto it = ssa_blk_list.begin();
    while (it != ssa_blk_list.end())
    {
        for (size_t i = 0; i < SSA_BLK; i++)
        {
            if ((*it)[i].ref_count != 0 && (*it)[i].ref_count != 0xffffffffffffffff)
            {
                c_map.insert(std::pair<ssa *, uint64_t>(&(*it)[i], (*it)[i].ref_count));
            }
        }
        it++;
    }

    // 2.检查页表中所有非零的ssa_tag都指向了合法的ssa，并在c_map中去除这个引用
    for (size_t tab_i = 0; tab_i < TOP_DIR_SZ; tab_i++)
    {
        if (tag_dir.table[tab_i])
        {
            tag_table_t *table = tag_dir.table[tab_i];
            for (size_t pag_i = 0; pag_i < PAGETABLE_SZ; pag_i++)
            {
                if ((*table).page[pag_i])
                {
                    tag_page_t *page = (*table).page[pag_i];
                    for (size_t tag_i = 0; tag_i < PAGE_SIZE; tag_i++)
                    {
                        if ((*page).tag[tag_i].ssa_ref != NULL)
                        {
                            auto it = c_map.find((*page).tag[tag_i].ssa_ref);
                            if (it == c_map.end())
                            {
                                LOGD("error: page walk find ssa_tag point to empty ssa\n");
                            }
                            if (--(*it).first->ref_count == 0)
                            {
                                // LOGD("log: find ref at page ,content:%s\n", ssa_tag_print((*page).tag[tag_i].ssa_ref).c_str());
                                c_map.erase(it);
                            }
                        }
                    }
                }
            }
        }
    }

    // 2.检查虚拟CPU寄存器中所有非零的ssa_tag都指向了合法的ssa，并在c_map中去除这个引用
    for (size_t tid_i = 0; tid_i < tctx_ct; tid_i++)
    {
        for (size_t reg_i = 0; reg_i < GRP_NUM; reg_i++)
        {
            for (size_t tag_i = 0; tag_i < TAGS_PER_GPR; tag_i++)
            {
                if (threads_ctx[tid_i].vcpu.gpr[reg_i][tag_i].ssa_ref != NULL)
                {
                    auto it = c_map.find(threads_ctx[tid_i].vcpu.gpr[reg_i][tag_i].ssa_ref);
                    if (it == c_map.end())
                    {
                        LOGD("error: threads_ctx walk find ssa_tag point to empty ssa\n");
                    }
                    if (--(*it).first->ref_count == 0)
                    {
                        // LOGD("log: find ref at reg ,content:%s\n", ssa_tag_print(threads_ctx[tid_i].vcpu.gpr[reg_i][tag_i].ssa_ref).c_str());
                        c_map.erase(it);
                    }
                }
            }
        }
    }

    // 3.检查c_map是否为空来判断一致性
    if (c_map.size() != 0)
    {
        LOGD("error: c_map is not empty after check, %lu remain\n", c_map.size());
    }

    //打印统计信息
    uint64_t total_combine_wait = 0, total_alloc_wait = 0;
    uint64_t total_cache_access = 0, total_cache_hit = 0;
    uint64_t total_tag_combine = 0, total_tag_alloc = 0;
    uint64_t total_cb_ll = 0, total_cb_lh = 0, total_cb_hh = 0;
    uint64_t total_bdd_cb = 0;
    for (size_t tid_i = 0; tid_i < tctx_ct; tid_i++) // 128
    {
        LOGD("thread %lu:\n", tid_i);
        LOGD("\tcombine wait %lu,alloc wait %lu\n", ssa_tls[tid_i].ss[ss_combine_wait], ssa_tls[tid_i].ss[ss_alloc_wait]);
        LOGD("\tcache access %lu,cache hit %lu,hit rate %f\n", ssa_tls[tid_i].ss[ss_cache_access], ssa_tls[tid_i].ss[ss_cache_hit],
             (double)ssa_tls[tid_i].ss[ss_cache_hit] / (double)ssa_tls[tid_i].ss[ss_cache_access]);
        LOGD("\ttag combine %lu,tag alloc %lu\n", ssa_tls[tid_i].ss[ss_combine], ssa_tls[tid_i].ss[ss_alloc]);
        LOGD("\tcb_ll %lu,cb_lh %lu,cb_hh %lu\n", ssa_tls[tid_i].ss[ss_cb_ll], ssa_tls[tid_i].ss[ss_cb_lh], ssa_tls[tid_i].ss[ss_cb_hh]);
        LOGD("\tbdd_cb %lu\n", ssa_tls[tid_i].ss[bdd_cb_count]);
        total_combine_wait += ssa_tls[tid_i].ss[ss_combine_wait];
        total_alloc_wait += ssa_tls[tid_i].ss[ss_alloc_wait];
        total_cache_access += ssa_tls[tid_i].ss[ss_cache_access];
        total_cache_hit += ssa_tls[tid_i].ss[ss_cache_hit];
        total_tag_combine += ssa_tls[tid_i].ss[ss_combine];
        total_tag_alloc += ssa_tls[tid_i].ss[ss_alloc];
        total_cb_ll += ssa_tls[tid_i].ss[ss_cb_ll];
        total_cb_lh += ssa_tls[tid_i].ss[ss_cb_lh];
        total_cb_hh += ssa_tls[tid_i].ss[ss_cb_hh];
        total_bdd_cb += ssa_tls[tid_i].ss[bdd_cb_count];
    }
    LOGD("total\n");
    LOGD("\tcache statis: access %lu,hit %lu,hit rate %f\n", total_cache_access, total_cache_hit, (double)total_cache_hit / (double)total_cache_access);
    //LOGD("\ttaint op statis: combine %lu,alloc %lu,transfer %lu\n", total_tag_combine, total_tag_alloc, ss_transfer);
    //LOGD("\tcombine type statis: cb_ll %lu,cb_lh %lu,cb_hh %lu\n", total_cb_ll, total_cb_lh, total_cb_hh);
    LOGD("\tbdd_cb_count:%lu\n", total_bdd_cb);
}
#endif

#ifdef SSA_PROFILE_GC
static void profile_thread(void *arg)
{
    while (!profile_exit)
    {
        PIN_Sleep(1000);
        uint64_t total_ssa = 0;
        uint64_t inuse_ssa = 0;
        uint64_t unused_ssa = 0;
        uint64_t unalloced_ssa = 0;
        auto it = ssa_blk_list.begin();
        while (it != ssa_blk_list.end())
        {
            total_ssa += SSA_BLK;
            for (size_t i = 0; i < SSA_BLK; i++)
            {
                if ((*it)[i].ref_count == 0)
                    unused_ssa++;
                else if ((*it)[i].ref_count == 0xffffffffffffffff)
                    unalloced_ssa++;
                else
                    inuse_ssa++;
            }
            it++;
        }
        LOGD("ssa profile: total ssa %lu,insuse_ssa %lu,unused ssa %lu,unalloced_ssa %lu,ssa blk: %lu\n", total_ssa, inuse_ssa, unused_ssa, unalloced_ssa, ssa_blk_list.size());
    }
    profile_exit = false;
}
#endif

void ssa_init()
{
    // 1.init lace and sylvan
    lace_start(THREAD_CTX_BLK, LACE_DQ_SIZE, HELPER_THREAD_NUM);

    // use at most SYLVAN_MEMORY_LIMIT, nodes:cache ratio 2:1, initial size 1/32 of maximum
    sylvan_set_limits(SYLVAN_MEMORY_LIMIT, 1, 5);
    sylvan_init_package();
    sylvan_init_mtbdd();

    // 2 var_set,use tmp tls block
    sylvan_tcb_t tmp_tls;
    tmp_tls.slots[my_region_slot] = -1;
    tmp_tls.slots[my_worker_id_slot] = 0;
    lace_n_workers_id = 1;
    uint64_t old_fs = _readfsbase_u64();
    _writefsbase_u64((uint64_t)&tmp_tls);

    sylvan_protect(&var_set);
    uint32_t array[TAG_WIDTH];
    for (size_t i = 0; i < TAG_WIDTH; i++)
    {
        array[i] = i;
    }
    //该操作应该不会导致gc，因此此时没有worker thread也没关系
    var_set = mtbdd_set_from_array((uint32_t *)&array, TAG_WIDTH);
    _writefsbase_u64(old_fs);
    lace_n_workers_id = 0;
    mfence();
    // 3 ssa_tls
    ssa_tls = (ssa_tls_t *)memalign(LINE_SIZE, sizeof(ssa_tls_t) * THREAD_CTX_BLK);

    // 2.2 free_ssa
    free_ssa._q = (ssa **)malloc(sizeof(ssa *) * SSA_BLK);
    free_ssa._h = 0;
    free_ssa._t = 0;
    free_ssa.gc_lock = 0;
    add_ssa_blk();

#ifdef SSA_PROFILE_GC
    PIN_SpawnInternalThread(profile_thread, NULL, 0, NULL);
#endif
}

void ssa_exit()
{
    lace_stop();
#ifdef SSA_PROFILE

#ifdef SSA_PROFILE_GC
    profile_exit = true;
    while (profile_exit)
        ;

#endif
    ssa_lastcheck();
#endif
    sylvan_quit();
    free(ssa_tls);

    auto it = ssa_blk_list.begin();
    while (it != ssa_blk_list.end())
    {
        free(*it);
        it++;
    }
    free(free_ssa._q);
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
    // ssa_tag除了存在于page table和reg中，还临时存在于cb_caches中
    t->cb_cache_l.~ssa_tag();
    t->cb_cache_r.~ssa_tag();
    t->cb_cache_v.~ssa_tag();
}

#else
extern BDDTag bdd_tag;
extern FILE *log_fd;
void ssa_init() { return; }
void ssa_exit() { return; }
void ssa_thread_start(uint64_t tid) { return; }
void ssa_thread_fini(uint64_t tid) { return; }
ssa_tag ssa_tag_alloc(unsigned int offset, uint64_t tid) { return ssa_tag(); }
ssa_tag ssa_tag_combine(ssa_tag const &lhs, ssa_tag const &rhs, uint64_t tid) { return ssa_tag(); }
std::string ssa_tag_print(ssa_tag const &tag) { return nullptr; }
#endif