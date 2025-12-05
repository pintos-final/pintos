#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char* file_name, struct intr_frame* if_);
static void initd(void* f_name);
static void __do_fork(void*);

/* 부모의 자식 목록에서 child_tid를 가진 자식 찾는 함수 */
static struct child* find_child(struct thread* parent, int child_tid);

/* open_file_list 복사 함수 */
static void fd_list_clone(struct list* parent_file_list, struct list* child_file_list);

struct child_args_aux {
    char* fn_copy;
    struct child* child;
};

struct fork_args {
    struct intr_frame if_;
    struct thread* parent;
    struct child* child;
};

/* General process initializer for initd and other process. */
static void process_init(void)
{
    struct thread* current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char* file_name)
{
    char* fn_copy;
    tid_t tid;
    struct child_args_aux* child_args_aux;

    /* Make a copy of FILE_NAME.
     * Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page(0);
    if (fn_copy == NULL)
        return TID_ERROR;
    strlcpy(fn_copy, file_name, PGSIZE);

    // 프로세스명 잘라서 사용
    char* name_copy = palloc_get_page(0);
    if (name_copy == NULL) {
        palloc_free_page(fn_copy);
        return TID_ERROR;
    }
    strlcpy(name_copy, file_name, PGSIZE);

    char* save_ptr;
    char* name = strtok_r(name_copy, " ", &save_ptr);
    if (name == NULL) {
        palloc_free_page(fn_copy);
        palloc_free_page(name_copy);
        return TID_ERROR;
    }

    // child 구조체 초기화 및 list에 추가
    struct child* child = malloc(sizeof *child);
    child->status_code = 0;
    child->is_exited = false;
    sema_init(&child->child_sema, 0);
    list_push_back(&thread_current()->child_list, &child->elem);

    // aux 구조체 초기화
    child_args_aux = malloc(sizeof *child_args_aux);
    child_args_aux->fn_copy = fn_copy;
    child_args_aux->child = child;

    /* Create a new thread to execute FILE_NAME. */
    tid = thread_create(name, PRI_DEFAULT, initd, child_args_aux);
    if (tid == TID_ERROR)
        palloc_free_page(fn_copy);
    palloc_free_page(name_copy);
    child->tid = tid;
    return tid;
}

/* A thread function that launches first user process. */
static void initd(void* child_args_aux)
{
#ifdef VM
    supplemental_page_table_init(&thread_current()->spt);
#endif

    process_init();

    struct child_args_aux* param = (struct child_args_aux*)child_args_aux;
    char* f_name = param->fn_copy;
    thread_current()->child_struct_pointer = param->child;

    if (process_exec(f_name) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char* name, struct intr_frame* if_)
{
    // child 구조체 초기화 및 list에 추가
    struct child* child = malloc(sizeof *child);
    child->status_code = 0;
    child->is_exited = false;
    sema_init(&child->child_sema, 0);
    list_push_back(&thread_current()->child_list, &child->elem);

    struct fork_args* fork_args = malloc(sizeof *fork_args);
    fork_args->if_ = *if_;
    fork_args->parent = thread_current();
    fork_args->child = child;

    tid_t result_tid = thread_create(name, PRI_DEFAULT, __do_fork, fork_args);
    child->tid = result_tid;
    sema_down(&thread_current()->fork_sema);
    if (result_tid == TID_ERROR) {
        return TID_ERROR;
    }
    return result_tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool duplicate_pte(uint64_t* pte, void* va, void* aux)
{
    struct thread* current = thread_current();
    struct thread* parent = (struct thread*)aux;
    void* parent_page;
    void* newpage;
    bool writable;

    /* 1. 부모 페이지가 커널 페이지면 바로 retrun true */
    if (is_kernel_vaddr(va)) {
        return true;
    }

    /* 2. 부모의 pml4(4단계 페이지 맵)에서 VA(가상 주소)에 해당하는 페이지 찾기 */
    parent_page = pml4_get_page(parent->pml4, va);

    /* 3. 자식용으로 새로운 PAL_USER 페이지를 할당하고, 그 결과 주소를 NEWPAGE에 저장 */
    newpage = palloc_get_page(PAL_USER);
    if (newpage == NULL) {
        return false;
    }

    /* 4. 부모 페이지가 writable인지 확인한 뒤 부모 페이지의 내용을 새 페이지로 복사 */
    writable = is_writable(pte);
    memcpy(newpage, parent_page, PGSIZE);

    /* 5.자식의 페이지 테이블에 VA 위치에 새 페이지를 WRITABLE 권한과 함께 매핑 */
    if (!pml4_set_page(current->pml4, va, newpage, writable)) {
        /* 6. 페이지 삽입 실패시 false로 작업 중단 */
        palloc_free_page(newpage);
        return false;
    }
    return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void __do_fork(void* aux)
{
    struct fork_args* fork_args = (struct fork_args*)aux;
    struct intr_frame if_;
    struct thread* parent = fork_args->parent;
    struct thread* current = thread_current();
    struct intr_frame* parent_if = &fork_args->if_;
    bool succ = true;

    /* 1. Read the cpu context to local stack. */
    memcpy(&if_, parent_if, sizeof(struct intr_frame));
    if_.R.rax = 0;

    current->child_struct_pointer = fork_args->child;

    /* 2. Duplicate PT */
    current->pml4 = pml4_create();
    if (current->pml4 == NULL)
        goto error;

    process_activate(current);
#ifdef VM
    supplemental_page_table_init(&current->spt);
    if (!supplemental_page_table_copy(&current->spt, &parent->spt))
        goto error;
#else
    if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
        goto error;
#endif
    fd_list_clone(&parent->open_file_list, &current->open_file_list);

    process_init();
    sema_up(&parent->fork_sema);

    /* Finally, switch to the newly created process. */
    if (succ) {
        do_iret(&if_);
    }
error:
    thread_exit();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void* fn_copy)
{
    char* file_name = fn_copy;
    bool success;

    /* We cannot use the intr_frame in the thread structure.
     * This is because when current thread rescheduled,
     * it stores the execution information to the member. */
    struct intr_frame _if;
    _if.ds = _if.es = _if.ss = SEL_UDSEG;
    _if.cs = SEL_UCSEG;
    _if.eflags = FLAG_IF | FLAG_MBS;

    /* We first kill the current context */
    process_cleanup();

    /* And then load the binary */
    success = load(file_name, &_if);
    /* If load failed, quit. */
    palloc_free_page(file_name);
    if (!success)
        return -1;

    /* Start switched process. */
    do_iret(&_if);
    NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid)
{
    struct thread* curr = thread_current();
    struct child* target_child = find_child(curr, child_tid);

    // child_tid를 가진 자식이 없으면 -1 리턴
    if (target_child == NULL) {
        return -1;
    }

    // 자식이 살아있으면 대기 -> 자식은 죽을 떄 sema_up
    // 자식이 죽었더라도 list에는 남아있음. 죽은지 여부는 is_exited로만
    if (!target_child->is_exited) {
        sema_down(&target_child->child_sema);
    }
    list_remove(&target_child->elem);
    return target_child->status_code;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
    struct thread* curr = thread_current();
    // 1. child_struct의 status를 exit_code로 설정
    if (curr->child_struct_pointer != NULL) {
        curr->child_struct_pointer->status_code = curr->exit_code;

        // 2. child_struct의 is_exited = true
        curr->child_struct_pointer->is_exited = true;

        // 3. sema_up(&child_sema)
        sema_up(&curr->child_struct_pointer->child_sema);
    }

    // process termination message
    if (curr->pml4 != NULL) {
        printf("%s: exit(%d)\n", curr->name, curr->exit_code);
    }

    process_cleanup();
}

/* Free the current process's resources. */
static void process_cleanup(void)
{
    struct thread* curr = thread_current();

#ifdef VM
    supplemental_page_table_kill(&curr->spt);
#endif

    uint64_t* pml4;
    /* Destroy the current process's page directory and switch back
     * to the kernel-only page directory. */
    pml4 = curr->pml4;
    if (pml4 != NULL) {
        /* Correct ordering here is crucial.  We must set
         * cur->pagedir to NULL before switching page directories,
         * so that a timer interrupt can't switch back to the
         * process page directory.  We must activate the base page
         * directory before destroying the process's page
         * directory, or our active page directory will be one
         * that's been freed (and cleared). */
        curr->pml4 = NULL;
        pml4_activate(NULL);
        pml4_destroy(pml4);
    }
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread* next)
{
    /* Activate thread's page tables. */
    pml4_activate(next->pml4);

    /* Set thread's kernel stack for use in processing interrupts. */
    tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct ELF64_PHDR {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool parse_arguments(const char* cmd_line, char** fn_copy_ptr, char*** argv_ptr, int* argc_ptr);
static void setup_argument_stack(struct intr_frame* if_, char** argv, int argc);
static bool setup_stack(struct intr_frame* if_);
static bool validate_segment(const struct Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool load(const char* file_name, struct intr_frame* if_)
{
    struct thread* t = thread_current();
    struct ELF ehdr;
    struct file* file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    char* fn_copy = NULL;
    char** argv = NULL;
    int argc = 0;

    if (!parse_arguments(file_name, &fn_copy, &argv, &argc))
        goto done;

    /* Allocate and activate page directory. */
    t->pml4 = pml4_create();
    if (t->pml4 == NULL)
        goto done;
    process_activate(thread_current());

    /* Open executable file. */
    file = filesys_open(argv[0]);
    if (file == NULL) {
        printf("load: %s: open failed\n", file_name);
        goto done;
    }

    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) ||
        ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
        || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024) {
        printf("load: %s: error loading executable\n", file_name);
        goto done;
    }

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file))
            goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type) {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
            /* Ignore this segment. */
            break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
            goto done;
        case PT_LOAD:
            if (validate_segment(&phdr, file)) {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint64_t file_page = phdr.p_offset & ~PGMASK;
                uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint64_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0) {
                    /* Normal segment.
                     * Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
                } else {
                    /* Entirely zero.
                     * Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                }
                if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
                    goto done;
            } else
                goto done;
            break;
        }
    }

    /* Set up stack. */
    if (!setup_stack(if_))
        goto done;

    /* Start address. */
    if_->rip = ehdr.e_entry;
    setup_argument_stack(if_, argv, argc);

    success = true;

done:
    /* We arrive here whether the load is successful or not. */
    if (fn_copy != NULL)
        palloc_free_page(fn_copy);
    if (argv != NULL)
        palloc_free_page(argv);

    file_close(file);
    return success;
}

/* 명령줄을 파싱하여 argv/argc 구성
   성공 시 true 반환, 실패 시 false 반환 */
static bool parse_arguments(const char* cmd_line, char** fn_copy_ptr, char*** argv_ptr, int* argc_ptr)
{
    size_t cmd_len = strlen(cmd_line) + 1;
    if (cmd_len > PGSIZE)
        return false;

    char* fn_copy = palloc_get_page(PAL_ZERO);
    if (fn_copy == NULL)
        return false;

    char** argv = palloc_get_page(PAL_ZERO);
    if (argv == NULL) {
        palloc_free_page(fn_copy);
        return false;
    }

    strlcpy(fn_copy, cmd_line, PGSIZE);

    int argc = 0;
    int max_argc = PGSIZE / sizeof(char*);
    char* token;
    char* save_ptr;

    for (token = strtok_r(fn_copy, " ", &save_ptr); token != NULL && argc < max_argc;
         token = strtok_r(NULL, " ", &save_ptr)) {
        argv[argc++] = token;
    }

    if (argc == 0) {
        palloc_free_page(fn_copy);
        palloc_free_page(argv);
        return false;
    }

    *fn_copy_ptr = fn_copy;
    *argv_ptr = argv;
    *argc_ptr = argc;
    return true;
}

/* 사용자 스택에 인자를 적재 (x86-64 SysV ABI) */
static void setup_argument_stack(struct intr_frame* if_, char** argv, int argc)
{
    /* 1. 인자 문자열을 역순으로 스택에 복사 */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        if_->rsp -= len;
        memcpy((void*)if_->rsp, argv[i], len);
        argv[i] = (char*)if_->rsp; // 스택 주소로 갱신
    }

    /* 2. 8바이트 정렬 */
    if_->rsp &= ~(uintptr_t)0x7;

    /* 3. argv[argc] = NULL 및 argv 포인터 배열 */
    if_->rsp -= sizeof(uint64_t) * (argc + 1);
    uint64_t* argv_base = (uint64_t*)if_->rsp;
    for (int i = 0; i < argc; i++)
        argv_base[i] = (uint64_t)argv[i];
    argv_base[argc] = 0; // NULL terminator

    /* 4. fake return address */
    if_->rsp -= sizeof(uint64_t);
    *(uint64_t*)if_->rsp = 0;

    /* 5. 레지스터 설정 */
    if_->R.rdi = argc;
    if_->R.rsi = (uint64_t)argv_base;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Phdr* phdr, struct file* file)
{
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
        return false;

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (uint64_t)file_length(file))
        return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false;

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
        return false;

    /* The virtual memory region must both start and end within the
       user address space range. */
    if (!is_user_vaddr((void*)phdr->p_vaddr))
        return false;
    if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
        return false;

    /* The region cannot "wrap around" across the kernel virtual
       address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
        return false;

    /* Disallow mapping page 0.
       Not only is it a bad idea to map page 0, but if we allowed
       it then user code that passed a null pointer to system calls
       could quite likely panic the kernel by way of null pointer
       assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE)
        return false;

    /* It's okay. */
    return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void* upage, void* kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    file_seek(file, ofs);
    while (read_bytes > 0 || zero_bytes > 0) {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* Get a page of memory. */
        uint8_t* kpage = palloc_get_page(PAL_USER);
        if (kpage == NULL)
            return false;

        /* Load this page. */
        if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
            palloc_free_page(kpage);
            return false;
        }
        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page(upage, kpage, writable)) {
            printf("fail\n");
            palloc_free_page(kpage);
            return false;
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool setup_stack(struct intr_frame* if_)
{
    uint8_t* kpage;
    bool success = false;

    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL) {
        success = install_page(((uint8_t*)USER_STACK) - PGSIZE, kpage, true);
        if (success)
            if_->rsp = USER_STACK;
        else
            palloc_free_page(kpage);
    }
    return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable)
{
    struct thread* t = thread_current();

    /* Verify that there's not already a page at that virtual
     * address, then map our page there. */
    return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool lazy_load_segment(struct page* page, void* aux)
{
    /* TODO: Load the segment from the file */
    /* TODO: This called when the first page fault occurs on address VA. */
    /* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable)
{
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    while (read_bytes > 0 || zero_bytes > 0) {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* TODO: Set up aux to pass information to the lazy_load_segment. */
        void* aux = NULL;
        if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable, lazy_load_segment, aux))
            return false;

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool setup_stack(struct intr_frame* if_)
{
    bool success = false;
    void* stack_bottom = (void*)(((uint8_t*)USER_STACK) - PGSIZE);

    /* TODO: Map the stack on stack_bottom and claim the page immediately.
     * TODO: If success, set the rsp accordingly.
     * TODO: You should mark the page is stack. */
    /* TODO: Your code goes here */

    return success;
}
#endif /* VM */

static struct child* find_child(struct thread* parent, int child_tid)
{
    struct child* target_child;
    struct list_elem* e;
    for (e = list_begin(&parent->child_list); e != list_end(&parent->child_list); e = list_next(e)) {
        if (list_entry(e, struct child, elem)->tid == child_tid) {
            target_child = list_entry(e, struct child, elem);
            return target_child;
        }
    }
    return NULL;
}

static void fd_list_clone(struct list* parent_file_list, struct list* child_file_list)
{
    struct list_elem* e;
    for (e = list_begin(parent_file_list); e != list_end(parent_file_list); e = list_next(e)) {
        struct open_file_list_elem* p = list_entry(e, struct open_file_list_elem, elem);
        struct open_file_list_elem* c = malloc(sizeof *c);

        c->fd = p->fd;
        c->file = file_duplicate(p->file);

        if (c->file == NULL) {
            free(c);
            break;
        }
        list_push_back(child_file_list, &c->elem);
    }
}