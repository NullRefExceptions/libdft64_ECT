/*
 * Written by Josh Dybnis and released to the public domain, as explained at
 * http://creativecommons.org/licenses/publicdomain
 *
 * A platform independant wrapper around thread-local storage. On platforms that don't support
 * __thread variables (e.g. Mac OS X), we have to use the pthreads library for thread-local storage
 */

#ifndef TLS_H
#define TLS_H

#ifdef __ELF__ // use gcc thread-local storage (i.e. __thread variables)
#define current_worker_slot 0
/* 
在执行mtbdd_refs_init TASK时，通过TOGETHER(mtbdd_refs_init_task);在所有
的work线程的tls中设置mtbdd_refs_key 
*/
#define mtbdd_refs_key_slot 1
/* 
同上面类似
*/
#define lddmc_refs_key_slot 2
/* 
主线程调用llmsset_create，然后通过TOGETHER(llmsset_reset_region);在所有
的work线程的tls中设置my_region
*/
#define my_region_slot 6
#define my_worker_id_slot 7
#define SLOT_SIZE 8
typedef struct __attribute__ ((aligned (64))) sylvan_tcb
{
    uintptr_t slots[SLOT_SIZE];
}sylvan_tcb_t;

extern __inline unsigned int
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_readfsbase_u32 (void)
{
  return __builtin_ia32_rdfsbase32 ();
}

extern __inline unsigned long long
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_readfsbase_u64 (void)
{
  return __builtin_ia32_rdfsbase64 ();
}

extern __inline void
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_writefsbase_u32 (unsigned int __B)
{
  __builtin_ia32_wrfsbase32 (__B);
}

extern __inline void
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_writefsbase_u64 (unsigned long long __B)
{
  __builtin_ia32_wrfsbase64 (__B);
}

#define DECLARE_THREAD_LOCAL(name, type) //__thread type name
#define INIT_THREAD_LOCAL(name)
#define SET_THREAD_LOCAL(name, value) tls_slot_set(name##_slot,(uintptr_t)value)//name = value
#define LOCALIZE_THREAD_LOCAL(name, type) type name = (type)tls_slot_get(name##_slot)
#define DELETE_THREAD_LOCAL(name)

static inline uintptr_t tls_slot_get(size_t slot) {
  uintptr_t res;
  const size_t ofs = (slot*sizeof(uintptr_t));
#if defined(__i386__)
  __asm__("movl %%gs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // 32-bit always uses GS
#elif defined(__x86_64__)
  __asm__("movq %%fs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // x86_64 Linux, BSD uses FS
#endif
  return res;
}

static inline void tls_slot_set(size_t slot, uintptr_t value) {
  const size_t ofs = (slot*sizeof(uintptr_t));
#if defined(__i386__)
  __asm__("movl %1,%%gs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // 32-bit always uses GS
#elif defined(__x86_64__)
  __asm__("movq %1,%%fs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // x86_64 Linux, BSD uses FS
#endif
}

#else//!__ELF__

#define DECLARE_THREAD_LOCAL(name, type) pthread_key_t name##_KEY

#define INIT_THREAD_LOCAL(name) \
    do { \
        if (pthread_key_create(&name##_KEY, NULL) != 0) { assert(0); } \
    } while (0)

#define SET_THREAD_LOCAL(name, value) pthread_setspecific(name##_KEY, (void *)(size_t)value);

#define LOCALIZE_THREAD_LOCAL(name, type) type name = (type)(size_t)pthread_getspecific(name##_KEY)

#define DELETE_THREAD_LOCAL(name) pthread_key_delete(name##_KEY)
#endif//__ELF__
#endif//TLS_H