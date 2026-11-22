#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

//LAB4 function3 2413238
void
schedule(void) {
    bool intr_flag;
    list_entry_t *le;
    struct proc_struct *next = NULL;

    local_intr_save(intr_flag);
    {
        current->need_resched = 0;
        list_entry_t *start = (current == idleproc) ? &proc_list : &(current->list_link);
        le = start;
        while ((le = list_next(le)) != start) {
            if (le != &proc_list) {
                struct proc_struct *proc = le2proc(le, list_link);
                if (proc->state == PROC_RUNNABLE) {
                    next = proc;
                    break;
                }
            }
        }
        if (next == NULL) {
            next = idleproc;
        }
        if (next != current) {
            proc_run(next);
        }
    }
    local_intr_restore(intr_flag);
}


