#include "branch_pred.h"
#include "libdft_api.h"
#include "pin.H"
#include "syscall_desc.h"
#include "tagmap.h"
#include "debug.h"
#include <iostream>
#include <set>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include "syscall_struct.h"
#include "ssa_tag.h"
#define FUZZING_INPUT_FILE "input"
extern FILE *log_fd;
static std::set<int> fdset;
extern syscall_desc_t syscall_desc[SYSCALL_MAX];
static unsigned int stdin_read_off = 0;

static void post_open_hook(THREADID tid, syscall_ctx_t *ctx)
{
    const int fd = ctx->ret;
    if (unlikely(fd < 0))
        return;
    const char *file_name = (char *)ctx->arg[SYSCALL_ARG0];
    if (strstr(file_name, FUZZING_INPUT_FILE) != NULL)
    {
        fdset.insert(fd);
        //LOGD("[open] fd: %d : %s \n", fd, file_name);
    }
}

static void post_openat_hook(THREADID tid, syscall_ctx_t *ctx)
{
    const int fd = ctx->ret;
    const char *file_name = (char *)ctx->arg[SYSCALL_ARG1];
    if (strstr(file_name, FUZZING_INPUT_FILE) != NULL)
    {
        fdset.insert(fd);
        //LOGD("[openat] fd: %d : %s \n", fd, file_name);
    }
}

static void post_close_hook(THREADID tid, syscall_ctx_t *ctx)
{
    /* not successful; optimized branch */
    if (unlikely((long)ctx->ret < 0))
        return;
    auto it = fdset.find((int)ctx->arg[SYSCALL_ARG0]);
    if (likely(it != fdset.end()))
        fdset.erase(it);
}
/* 
这里有两点可以调整的地方：
1。保存返回值的rax是否应该设置tag
2。read的count参数比实际读取字节数大时，没有读到的部分是否应该设置tag（这部分
区域可能潜在的属于用户可以控制的区域）
 */
static void post_read_hook(THREADID tid, syscall_ctx_t *ctx)
{
    const size_t nr = ctx->ret;
    if (unlikely((long)nr <= 0))
        return;
    const int fd = ctx->arg[SYSCALL_ARG0];
    const ADDRINT buf = ctx->arg[SYSCALL_ARG1];
    if (fdset.find(fd) != fdset.end())
    {
        unsigned int read_off = 0;
        if (fd == STDIN_FILENO)
        {
            // maintain it by ourself
            read_off = stdin_read_off;
            stdin_read_off += nr;
        }
        else
        {
            read_off = lseek(fd, 0, SEEK_CUR);
            read_off -= nr; // post
        }
        
        for (unsigned int i = 0; i < nr; i++)
        {
            tag_t t = tag_alloc<tag_t>(read_off + i,tid);
            tagmap_setb(buf + i, t);
        }
    }
    else
    {
        tagmap_clrn(buf, nr);
    }
}

static void post_pread64_hook(THREADID tid, syscall_ctx_t *ctx)
{
    const size_t nr = ctx->ret;
    if (unlikely((long)nr <= 0))
        return;
    const int fd = ctx->arg[SYSCALL_ARG0];
    const ADDRINT buf = ctx->arg[SYSCALL_ARG1];
    if (fdset.find(fd) != fdset.end())
    {
        unsigned int read_off = ctx->arg[SYSCALL_ARG3];        
        for (unsigned int i = 0; i < nr; i++)
        {
            tag_t t = tag_alloc<tag_t>(read_off + i,tid);
            tagmap_setb(buf + i, t);
        }
    }
    else
    {
        tagmap_clrn(buf, nr);
    }
}

static void post_readv_hook(THREADID tid, syscall_ctx_t *ctx)
{
    /* iterators */
    int i;
    struct iovec *iov;
    std::set<int>::iterator it;
    /* bytes copied in a iovec structure */
    size_t iov_tot;
    /* total bytes copied */
    size_t bytes_readed = (size_t)ctx->ret;
    /* readv() was not successful; optimized branch */
    if (unlikely((long)ctx->ret <= 0))
        return;
    uint32_t read_off = 0;
    /* get the descriptor */
    it = fdset.find((int)ctx->arg[SYSCALL_ARG0]);
    /* iterate the iovec structures */
    for (i = 0; i < (int)ctx->arg[SYSCALL_ARG2] && bytes_readed > 0; i++)
    {
        /* get an iovec  */
        iov = ((struct iovec *)ctx->arg[SYSCALL_ARG1]) + i;
        /* get the length of the iovec */
        iov_tot = (bytes_readed >= (size_t)iov->iov_len) ? (size_t)iov->iov_len : bytes_readed;

        /* taint interesting data and zero everything else */
        if (it != fdset.end())
        {
            //fprintf(log_fd,"readbuf at %p\n",iov->iov_base);
            for (size_t i = 0; i < iov_tot; i++)
            {
                tag_t t = tag_alloc<tag_t>(read_off,tid);
                tagmap_setb(((ADDRINT)iov->iov_base) + i, t);
                read_off++;
            }
        }
        else
        {
            tagmap_clrn((size_t)iov->iov_base, iov_tot);
        }
        /* housekeeping */
        bytes_readed -= iov_tot;
    }
}

static void post_mmap_hook(THREADID tid, syscall_ctx_t *ctx)
{
    if (unlikely((void *)ctx->ret == MAP_FAILED))
        return;
    size_t offset = (size_t)ctx->arg[SYSCALL_ARG1];
    const int fd = ctx->arg[SYSCALL_ARG4];
    const ADDRINT addr = ctx->ret;

    /* estimate offset; optimized branch */
    if (unlikely(offset < PAGE_SZ))
        offset = PAGE_SZ;
    else
        offset = offset + PAGE_SZ - (offset % PAGE_SZ);

    /* grow downwards; optimized branch */
    if (unlikely((int)ctx->arg[SYSCALL_ARG3] & MAP_GROWSDOWN))
        /* fix starting address */
        ctx->ret = ctx->ret - offset;

    if (fdset.find(fd) != fdset.end())
    {
        for (unsigned int i = 0; i < offset; i++)
        {
            tagmap_setb(addr + i, tag_alloc<tag_t>(i,tid));
        }
    }
    else
    {
        tagmap_clrn((size_t)ctx->ret, offset);
    }
}



int main(int argc, char **argv)
{
    /* initialize symbol processing */
    PIN_InitSymbols();

    if (unlikely(PIN_Init(argc, argv)))
        goto err;

    if (unlikely(libdft_init() != 0))
        /* failed */
        goto err;

    syscall_set_post(&syscall_desc[__NR_open], post_open_hook);
    syscall_set_post(&syscall_desc[__NR_openat], post_openat_hook);
    syscall_set_post(&syscall_desc[__NR_close], post_close_hook);

    syscall_set_post(&syscall_desc[__NR_read], post_read_hook);
    syscall_set_post(&syscall_desc[__NR_pread64], post_pread64_hook);
    syscall_set_post(&syscall_desc[__NR_readv], post_readv_hook);
    syscall_set_post(&syscall_desc[__NR_mmap], post_mmap_hook);

    PIN_StartProgram();

    /* typically not reached; make the compiler happy */
    return EXIT_SUCCESS;

err:
    return EXIT_FAILURE;
}