#include <stdint.h>
#include <string>
#include <sylvan_tls.h>
#ifdef TAINT_PROFILE
extern uint64_t move_time;
#endif

#ifdef TAINT_COUNT
extern  uint64_t move_count;
#endif

class ssa
{
public:
    uint64_t ref_count;
    uint64_t bdd;
    ssa()
    {
        ref_count = 0;
        bdd = 0; // mtbdd_false
    }
};

class ssa_tag
{
public:
    ssa *ssa_ref;
    
    ssa_tag() : ssa_ref(NULL){};

    ssa_tag(ssa *ref) : ssa_ref(ref){};

    ssa_tag(const ssa_tag &rhs)
    {
        if (rhs.ssa_ref != NULL)
            rhs.ssa_ref->ref_count++;//__sync_add_and_fetch(&(*rhs.ssa_ref).ref_count, 1);
        ssa_ref = rhs.ssa_ref;
    }

    ssa_tag(ssa_tag &&rhs)
    {
        ssa_ref = rhs.ssa_ref;
        rhs.ssa_ref = NULL;
    }

    ~ssa_tag()
    {
        if (this->ssa_ref != NULL)
            ssa_ref->ref_count--;//__sync_sub_and_fetch(&(*ssa_ref).ref_count, 1);
        this->ssa_ref = NULL;
    }

    inline ssa_tag &operator=(const ssa_tag &rhs)
    {
#ifdef TAINT_PROFILE
        uint64_t pre = __rdtsc();
#endif
        if (rhs.ssa_ref != NULL)
            rhs.ssa_ref->ref_count++;//__sync_add_and_fetch(&(*rhs.ssa_ref).ref_count, 1);
        if (this->ssa_ref != NULL)
            ssa_ref->ref_count--;//__sync_sub_and_fetch(&(*ssa_ref).ref_count, 1);
        ssa_ref = rhs.ssa_ref;
#ifdef TAINT_PROFILE
        move_time += __rdtsc() - pre;
#endif
#ifdef TAINT_COUNT
        move_count++;
#endif
        return *this;
    }

    inline ssa_tag &operator=(ssa_tag &&rhs)
    {
#ifdef TAINT_PROFILE
        uint64_t pre = __rdtsc();
#endif
        if (this->ssa_ref != NULL)
            ssa_ref->ref_count--;//__sync_sub_and_fetch(&(*ssa_ref).ref_count, 1);
        ssa_ref = rhs.ssa_ref;
        rhs.ssa_ref = NULL;
#ifdef TAINT_PROFILE
        move_time += __rdtsc() - pre;
#endif
#ifdef TAINT_COUNT
        move_count++;
#endif
        return *this;
    }

    inline bool operator==(const ssa_tag &rhs) const
    {
        if (ssa_ref == rhs.ssa_ref)
            return true;
        if (ssa_ref == NULL || rhs.ssa_ref == NULL)
            return false;
        if (ssa_ref->bdd == rhs.ssa_ref->bdd)
            return true;
        return false;
    }
};