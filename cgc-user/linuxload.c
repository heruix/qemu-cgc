/* Code for loading Linux executables.  Mostly linux kernel code.  */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "qemu.h"

#define NGROUPS 32

/* ??? This should really be somewhere else.  */
abi_long memcpy_to_target(abi_ulong dest, const void *src,
                          unsigned long len)
{
    void *host_ptr;

    host_ptr = lock_user(VERIFY_WRITE, dest, len, 0);
    if (!host_ptr)
        return -TARGET_EFAULT;
    memcpy(host_ptr, src, len);
    unlock_user(host_ptr, dest, 1);
    return 0;
}

static int count(char ** vec)
{
    int		i;

    for(i = 0; *vec; i++) {
        vec++;
    }

    return(i);
}

static int prepare_binprm(struct linux_binprm *bprm)
{
    struct stat		st;
    int mode;
    int retval;

    if(fstat(bprm->fd, &st) < 0) {
	return(-errno);
    }

    mode = st.st_mode;
    if(!S_ISREG(mode)) {	/* Must be regular file */
	return(-EACCES);
    }
    if(!(mode & 0111)) {	/* Must have at least one execute bit set */
	return(-EACCES);
    }

    bprm->e_uid = geteuid();
    bprm->e_gid = getegid();

    /* Set-uid? */
    if(mode & S_ISUID) {
    	bprm->e_uid = st.st_uid;
    }

    /* Set-gid? */
    /*
     * If setgid is set but no group execute bit then this
     * is a candidate for mandatory locking, not a setgid
     * executable.
     */
    if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) {
	bprm->e_gid = st.st_gid;
    }

    retval = read(bprm->fd, bprm->buf, BPRM_BUF_SIZE);
    if (retval < 0) {
	perror("prepare_binprm");
	exit(-1);
    }
    if (retval < BPRM_BUF_SIZE) {
        /* Make sure the rest of the loader won't read garbage.  */
        memset(bprm->buf + retval, 0, BPRM_BUF_SIZE - retval);
    }
    return retval;
}

/* Construct the envp and argv tables on the target stack.  */
abi_ulong loader_build_argptr(int envc, int argc, abi_ulong sp,
                              abi_ulong stringp, int push_ptr)
{
    TaskState *ts = (TaskState *)thread_cpu->opaque;
    int n = sizeof(abi_ulong);
    abi_ulong envp;
    abi_ulong argv;

    sp -= (envc + 1) * n;
    envp = sp;
    sp -= (argc + 1) * n;
    argv = sp;
    if (push_ptr) {
        /* FIXME - handle put_user() failures */
        sp -= n;
        put_user_ual(envp, sp);
        sp -= n;
        put_user_ual(argv, sp);
    }
    sp -= n;
    /* FIXME - handle put_user() failures */
    put_user_ual(argc, sp);
    ts->info->arg_start = stringp;
    while (argc-- > 0) {
        /* FIXME - handle put_user() failures */
        put_user_ual(stringp, argv);
        argv += n;
        stringp += target_strlen(stringp) + 1;
    }
    ts->info->arg_end = stringp;
    /* FIXME - handle put_user() failures */
    put_user_ual(0, argv);
    while (envc-- > 0) {
        /* FIXME - handle put_user() failures */
        put_user_ual(stringp, envp);
        envp += n;
        stringp += target_strlen(stringp) + 1;
    }
    /* FIXME - handle put_user() failures */
    put_user_ual(0, envp);

    return sp;
}

int loader_exec(int fdexec, const char *filename, char **argv, char **envp,
             struct target_pt_regs * regs, struct image_info *infop,
             struct linux_binprm *bprm)
{
    int retval;
    int i;

    bprm->p = TARGET_PAGE_SIZE*MAX_ARG_PAGES-sizeof(unsigned int);
    memset(bprm->page, 0, sizeof(bprm->page));
    bprm->fd = fdexec;
    bprm->filename = (char *)filename;
    bprm->argc = count(argv);
    bprm->argv = argv;
    bprm->envc = count(envp);
    bprm->envp = envp;

    retval = prepare_binprm(bprm);

    if(retval>=0) {
        if (bprm->buf[0] == 0x7f
                && bprm->buf[1] == 'C'
                && bprm->buf[2] == 'G'
                && bprm->buf[3] == 'C') {
            retval = load_elf_binary(bprm, infop);
#if defined(TARGET_HAS_BFLT)
        } else if (bprm->buf[0] == 'b'
                && bprm->buf[1] == 'F'
                && bprm->buf[2] == 'L'
                && bprm->buf[3] == 'T') {
            retval = load_flt_binary(bprm, infop);
#endif
        } else {
            return -ENOEXEC;
        }
    }
#ifdef CONFIG_CGC_USER
    abi_long flag_page = 0;

    #define FLAG_PAGE 0x4347C000
    #define FLAG_PAGE_SIZE 4096

    // abi_long stack_page = 0;

    #define STACK_INIT_VALUE 0xbaaaaffc
    #define STACK_PAGE_LIMIT ((STACK_INIT_VALUE & 0xFFFFFFFFFFFFF000) + 0x1000)
    #define STACK_PAGE_SIZE 4096
    #define STACK_PAGES_NUM ((infop->start_stack - infop->stack_limit) / STACK_PAGE_SIZE + 1)
#endif

#ifdef CONFIG_CGC_USER
    flag_page = target_mmap(FLAG_PAGE, FLAG_PAGE_SIZE,
                            PROT_READ|PROT_WRITE,
                            MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    uint32_t *ptr = lock_user(VERIFY_READ, flag_page, FLAG_PAGE_SIZE, 0);
    int fi;
    for (fi = 0; fi < FLAG_PAGE_SIZE / 4; fi += 1) {
        ptr[fi] = 0x4342434C; // LCBC
    }
    unlock_user(ptr, flag_page, FLAG_PAGE_SIZE);

    abi_long stack_start = STACK_PAGE_LIMIT - STACK_PAGES_NUM * STACK_PAGE_SIZE;
    target_mmap(stack_start, STACK_PAGE_SIZE * STACK_PAGES_NUM,
                            PROT_READ|PROT_WRITE|PROT_EXEC,
                            MAP_ANONYMOUS|MAP_PRIVATE|MAP_STACK, -1, 0);
#endif

#ifdef CONFIG_CGC_USER
    regs->ecx = flag_page;
    infop->start_stack = STACK_INIT_VALUE;
    infop->stack_limit = STACK_PAGE_LIMIT;
#endif


    if(retval>=0) {
        /* success.  Initialize important registers */
        do_init_thread(regs, infop);
        return retval;
    }

    /* Something went wrong, return the inode and free the argument pages*/
    for (i=0 ; i<MAX_ARG_PAGES ; i++) {
        g_free(bprm->page[i]);
    }
    return(retval);
}
