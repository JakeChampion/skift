
#include <libfile/elf.h>
#include <libmath/math.h>
#include <libsystem/Assert.h>
#include <libsystem/Atomic.h>
#include <libsystem/CString.h>
#include <libsystem/Debug.h>
#include <libsystem/Logger.h>
#include <libsystem/Result.h>

#include "arch/Arch.h"
#include "arch/x86/Interrupts.h" /* XXX */

#include "kernel/memory/Physical.h"
#include "kernel/memory/Virtual.h"
#include "kernel/scheduling/Scheduler.h"
#include "kernel/system/System.h"
#include "kernel/tasking.h"
#include "kernel/tasking/Handles.h"

/* -------------------------------------------------------------------------- */
/*   TASKING                                                                  */
/* -------------------------------------------------------------------------- */

static int TID = 0;

static List *all_tasks;

void tasking_initialize(void)
{
    all_tasks = list_create();

    task_shared_memory_setup();

    Task *idle_task = task_spawn(NULL, "Idle", system_hang, NULL, false);
    task_go(idle_task);
    task_set_state(idle_task, TASK_STATE_HANG);

    scheduler_did_create_idle_task(idle_task);

    Task *kernel_task = task_spawn(NULL, "System", NULL, NULL, false);
    task_go(kernel_task);

    scheduler_did_create_running_task(kernel_task);

    Task *garbage_task = task_spawn(NULL, "GarbageCollector", garbage_collector, NULL, false);
    task_go(garbage_task);
}

/* -------------------------------------------------------------------------- */
/*   TASKS                                                                    */
/* -------------------------------------------------------------------------- */

Task *task_create(Task *parent, const char *name, bool user)
{
    ASSERT_ATOMIC;

    Task *task = __create(Task);

    task->id = TID++;
    strlcpy(task->name, name, PROCESS_NAME_SIZE);
    task->state = TASK_STATE_NONE;

    // Setup memory space
    if (user)
    {
        task->pdir = memory_pdir_create();
    }
    else
    {
        task->pdir = memory_kpdir();
    }

    // Setup shms
    task->memory_mapping = list_create();

    // Setup current working directory.
    lock_init(task->cwd_lock);

    if (parent != NULL)
    {
        task->cwd_path = path_clone(parent->cwd_path);
    }
    else
    {
        Path *path = path_create("/");
        task->cwd_path = path;
        assert(task->cwd_path);
    }

    // Setup fildes
    lock_init(task->handles_lock);
    for (int i = 0; i < PROCESS_HANDLE_COUNT; i++)
    {
        task->handles[i] = NULL;
    }

    memory_alloc(task->pdir, PROCESS_STACK_SIZE, MEMORY_CLEAR, (uintptr_t *)&task->stack);
    task->stack_pointer = ((uintptr_t)task->stack + PROCESS_STACK_SIZE - 1);

    arch_save_context(task);

    list_pushback(all_tasks, task);

    return task;
}

void task_destroy(Task *task)
{
    atomic_begin();

    if (task->state != TASK_STATE_NONE)
        task_set_state(task, TASK_STATE_NONE);

    list_remove(all_tasks, task);
    atomic_end();

    list_foreach(MemoryMapping, memory_mapping, task->memory_mapping)
    {
        task_memory_mapping_destroy(task, memory_mapping);
    }

    list_destroy(task->memory_mapping);

    task_fshandle_close_all(task);

    lock_acquire(task->cwd_lock);
    path_destroy(task->cwd_path);

    memory_free(task->pdir, (MemoryRange){(uintptr_t)task->stack, PROCESS_STACK_SIZE});

    if (task->pdir != memory_kpdir())
    {
        memory_pdir_destroy(task->pdir);
    }

    free(task);
}

List *task_all(void)
{
    return all_tasks;
}

Task *task_by_id(int id)
{
    list_foreach(Task, task, all_tasks)
    {
        if (task->id == id)
            return task;
    }

    return NULL;
}

int task_count(void)
{
    atomic_begin();
    int result = list_count(all_tasks);
    atomic_end();

    return result;
}

Task *task_spawn(Task *parent, const char *name, TaskEntry entry, void *arg, bool user)
{
    ASSERT_ATOMIC;

    Task *task = task_create(parent, name, user);

    task_set_entry(task, entry, user);
    task_stack_push(task, &arg, sizeof(arg));

    return task;
}

Task *task_spawn_with_argv(Task *parent, const char *name, TaskEntry entry, const char **argv, bool user)
{
    atomic_begin();

    Task *task = task_create(parent, name, user);

    task_set_entry(task, entry, true);

    uintptr_t argv_list[PROCESS_ARG_COUNT] = {};

    int argc;
    for (argc = 0; argv[argc] && argc < PROCESS_ARG_COUNT; argc++)
    {
        argv_list[argc] = task_stack_push(task, argv[argc], strlen(argv[argc]) + 1);
    }

    uintptr_t argv_list_ref = task_stack_push(task, &argv_list, sizeof(argv_list));

    task_stack_push(task, &argv_list_ref, sizeof(argv_list_ref));
    task_stack_push(task, &argc, sizeof(argc));

    atomic_end();

    return task;
}

/* --- Tasks methodes ------------------------------------------------------- */

void task_set_state(Task *task, TaskState state)
{
    ASSERT_ATOMIC;

    scheduler_did_change_task_state(task, task->state, state);
    task->state = state;
}

void task_set_entry(Task *task, TaskEntry entry, bool user)
{
    task->entry = entry;
    task->user = user;
}

uintptr_t task_stack_push(Task *task, const void *value, uint size)
{
    task->stack_pointer -= size;
    memcpy((void *)task->stack_pointer, value, size);

    return task->stack_pointer;
}

void task_go(Task *task)
{
    InterruptStackFrame stackframe;

    stackframe.eflags = 0x202;
    stackframe.eip = (uintptr_t)task->entry;
    stackframe.ebp = ((uintptr_t)task->stack + PROCESS_STACK_SIZE);

    // TODO: userspace task
    stackframe.cs = 0x08;
    stackframe.ds = 0x10;
    stackframe.es = 0x10;
    stackframe.fs = 0x10;
    stackframe.gs = 0x10;

    task_stack_push(task, &stackframe, sizeof(InterruptStackFrame));

    atomic_begin();
    task_set_state(task, TASK_STATE_RUNNING);
    atomic_end();
}

/* --- Task wait state ------------------------------------------------------ */

Result task_sleep(Task *task, int timeout)
{
    task_block(task, blocker_time_create(system_get_tick() + timeout), -1);

    return TIMEOUT;
}

Result task_wait(int task_id, int *exit_value)
{
    atomic_begin();
    Task *task = task_by_id(task_id);

    if (!task)
    {
        atomic_end();

        return ERR_NO_SUCH_TASK;
    }

    atomic_end();

    task_block(
        scheduler_running(),
        blocker_wait_create(task, exit_value), -1);

    return SUCCESS;
}

BlockerResult task_block(Task *task, Blocker *blocker, Timeout timeout)
{
    assert(!task->blocker);

    atomic_begin();

    task->blocker = blocker;
    if (blocker->can_unblock(blocker, task))
    {

        if (blocker->on_unblock)
        {
            blocker->on_unblock(blocker, task);
        }

        task->blocker = NULL;
        free(blocker);
        atomic_end();

        return BLOCKER_UNBLOCKED;
    }

    if (timeout == (Timeout)-1)
    {
        blocker->timeout = (Timeout)-1;
    }
    else
    {
        blocker->timeout = system_get_tick() + timeout;
    }

    task_set_state(task, TASK_STATE_BLOCKED);
    atomic_end();

    scheduler_yield();

    BlockerResult result = blocker->result;

    free(blocker);
    task->blocker = NULL;

    return result;
}

/* --- Task stopping and canceling ------------------------------------------ */

Result task_cancel(Task *task, int exit_value)
{
    assert(task);

    atomic_begin();

    task->exit_value = exit_value;
    task_set_state(task, TASK_STATE_CANCELED);

    atomic_end();

    return SUCCESS;
}

void task_exit(int exit_value)
{
    task_cancel(scheduler_running(), exit_value);

    scheduler_yield();

    ASSERT_NOT_REACHED();
}

/* --- Task Memory management ----------------------------------------------- */

PageDirectory *task_switch_pdir(Task *task, PageDirectory *pdir)
{
    PageDirectory *oldpdir = task->pdir;

    task->pdir = pdir;

    memory_pdir_switch(pdir);

    return oldpdir;
}

Result task_memory_map(Task *task, MemoryRange range)
{
    return memory_map(task->pdir, range, MEMORY_USER | MEMORY_CLEAR);
}

Result task_memory_alloc(Task *task, size_t size, uintptr_t *out_address)
{
    return memory_alloc(task->pdir, size, MEMORY_USER | MEMORY_CLEAR, out_address);
}

Result task_memory_free(Task *task, MemoryRange range)
{
    return memory_free(task->pdir, range);
}

/* --- Task dump ------------------------------------------------------------ */

void task_dump(Task *task)
{
    atomic_begin();
    printf("\n\t - Task %d %s", task->id, task->name);
    printf("\n\t   State: %s", task_state_string(task->state));
    printf("\n\t   User memory: ");
    memory_pdir_dump(task->pdir, false);

    if (task->pdir == memory_kpdir())
    {
        printf("\n\t   Page directory: %08x (kpdir)", task->pdir);
    }
    else
    {
        printf("\n\t   Page directory: %08x", task->pdir);
    }

    printf("\n");
    atomic_end();
}

void task_panic_dump(void)
{
    if (scheduler_running() == NULL)
        return;

    atomic_begin();

    printf("\n\tRunning task %d: '%s'", scheduler_running_id(), scheduler_running()->name);
    task_dump(scheduler_running());

    atomic_end();
}

/* --- Current working directory -------------------------------------------- */

Path *task_cwd_resolve(Task *task, const char *buffer)
{
    Path *path = path_create(buffer);

    if (path_is_relative(path))
    {
        lock_acquire(task->cwd_lock);

        Path *combined = path_combine(task->cwd_path, path);
        path_destroy(path);
        path = combined;

        lock_release(task->cwd_lock);
    }

    path_normalize(path);

    return path;
}

Result task_set_cwd(Task *task, const char *buffer)
{
    Result result = SUCCESS;

    Path *path = task_cwd_resolve(task, buffer);
    FsNode *node = filesystem_find_and_ref(path);

    if (node == NULL)
    {
        result = ERR_NO_SUCH_FILE_OR_DIRECTORY;
        goto cleanup_and_return;
    }

    if (node->type != FILE_TYPE_DIRECTORY)
    {
        result = ERR_NOT_A_DIRECTORY;
        goto cleanup_and_return;
    }

    lock_acquire(task->cwd_lock);

    path_destroy(task->cwd_path);
    task->cwd_path = path;
    path = NULL;

    lock_release(task->cwd_lock);

cleanup_and_return:
    if (node)
        fsnode_deref(node);

    if (path)
        path_destroy(path);

    return result;
}

Result task_get_cwd(Task *task, char *buffer, uint size)
{
    lock_acquire(task->cwd_lock);

    path_to_cstring(task->cwd_path, buffer, size);

    lock_release(task->cwd_lock);

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/*   SHARED MEMORY                                                            */
/* -------------------------------------------------------------------------- */

static int _memory_object_id = 0;
static List *_memory_objects;
static Lock _memory_objects_lock;

void task_shared_memory_setup(void)
{
    lock_init(_memory_objects_lock);
    _memory_objects = list_create();
}

/* --- Memory object -------------------------------------------------------- */

MemoryObject *memory_object_create(size_t size)
{
    size = PAGE_ALIGN_UP(size);

    MemoryObject *memory_object = __create(MemoryObject);

    memory_object->id = _memory_object_id++;
    memory_object->refcount = 1;
    memory_object->address = physical_alloc(size / PAGE_SIZE);
    memory_object->size = size;

    lock_acquire(_memory_objects_lock);
    list_pushback(_memory_objects, memory_object);
    lock_release(_memory_objects_lock);

    return memory_object;
}

void memory_object_destroy(MemoryObject *memory_object)
{
    list_remove(_memory_objects, memory_object);

    physical_free(memory_object->address, PAGE_ALIGN_UP(memory_object->size) / PAGE_SIZE);
    free(memory_object);
}

MemoryObject *memory_object_ref(MemoryObject *memory_object)
{
    __atomic_add_fetch(&memory_object->refcount, 1, __ATOMIC_SEQ_CST);

    return memory_object;
}

void memory_object_deref(MemoryObject *memory_object)
{
    lock_acquire(_memory_objects_lock);

    if (__atomic_sub_fetch(&memory_object->refcount, 1, __ATOMIC_SEQ_CST) == 0)
    {
        memory_object_destroy(memory_object);
    }

    lock_release(_memory_objects_lock);
}

MemoryObject *memory_object_by_id(int id)
{
    lock_acquire(_memory_objects_lock);

    list_foreach(MemoryObject, memory_object, _memory_objects)
    {
        if (memory_object->id == id)
        {
            memory_object_ref(memory_object);

            lock_release(_memory_objects_lock);

            return memory_object;
        }
    }

    lock_release(_memory_objects_lock);
    return NULL;
}

/* --- Memory Mapping ------------------------------------------------------- */

MemoryMapping *task_memory_mapping_create(Task *task, MemoryObject *memory_object)
{
    MemoryMapping *memory_mapping = __create(MemoryMapping);

    memory_mapping->object = memory_object_ref(memory_object);
    memory_mapping->address = virtual_alloc(task->pdir, (MemoryRange){memory_object->address, memory_object->size}, MEMORY_USER).base;
    memory_mapping->size = memory_object->size;

    list_pushback(task->memory_mapping, memory_mapping);

    return memory_mapping;
}

void task_memory_mapping_destroy(Task *task, MemoryMapping *memory_mapping)
{
    virtual_free(task->pdir, (MemoryRange){memory_mapping->address, memory_mapping->size});
    memory_object_deref(memory_mapping->object);

    list_remove(task->memory_mapping, memory_mapping);
    free(memory_mapping);
}

MemoryMapping *task_memory_mapping_by_address(Task *task, uintptr_t address)
{
    list_foreach(MemoryMapping, memory_mapping, task->memory_mapping)
    {
        if (memory_mapping->address == address)
        {
            return memory_mapping;
        }
    }

    return NULL;
}

/* --- User facing API ------------------------------------------------------ */

Result task_shared_memory_alloc(Task *task, size_t size, uintptr_t *out_address)
{
    MemoryObject *memory_object = memory_object_create(size);

    MemoryMapping *memory_mapping = task_memory_mapping_create(task, memory_object);

    memory_object_deref(memory_object);

    *out_address = memory_mapping->address;

    return SUCCESS;
}

Result task_shared_memory_free(Task *task, uintptr_t address)
{
    MemoryMapping *memory_mapping = task_memory_mapping_by_address(task, address);

    if (!memory_mapping)
    {
        return ERR_BAD_ADDRESS;
    }

    task_memory_mapping_destroy(task, memory_mapping);

    return SUCCESS;
}

Result task_shared_memory_include(Task *task, int handle, uintptr_t *out_address, size_t *out_size)
{
    MemoryObject *memory_object = memory_object_by_id(handle);

    if (!memory_object)
    {
        return ERR_BAD_ADDRESS;
    }

    MemoryMapping *memory_mapping = task_memory_mapping_create(task, memory_object);

    memory_object_deref(memory_object);

    *out_address = memory_mapping->address;
    *out_size = memory_mapping->size;

    return SUCCESS;
}

Result task_shared_memory_get_handle(Task *task, uintptr_t address, int *out_handle)
{
    MemoryMapping *memory_mapping = task_memory_mapping_by_address(task, address);

    if (!memory_mapping)
    {
        return ERR_BAD_ADDRESS;
    }

    *out_handle = memory_mapping->object->id;
    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/*   GARBAGE COLLECTOR                                                        */
/* -------------------------------------------------------------------------- */

static IterationDecision destroy_task_if_canceled(void *target, Task *task)
{
    __unused(target);

    if (task->state == TASK_STATE_CANCELED)
    {
        task_destroy(task);
    }

    return ITERATION_CONTINUE;
}

void garbage_collector(void)
{
    while (true)
    {
        task_sleep(scheduler_running(), 100); // We don't do this really often.

        atomic_begin();
        list_iterate(all_tasks, NULL, (ListIterationCallback)destroy_task_if_canceled);
        atomic_end();
    }
}
