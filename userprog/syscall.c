#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/synch.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame*);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

// syscall 함수 ========
static void sys_halt(void);
static void sys_exit(int status);
static bool sys_create(const char* file, unsigned initial_size);
static bool sys_remove(const char* file);
static int sys_open(const char* file);
static int sys_filesize(int fd);
static int sys_read(int fd, void* buffer, unsigned length);
static int sys_write(int fd, const void* buffer, unsigned length);
static void sys_seek(int fd, unsigned position);
static unsigned sys_tell(int fd);
static void sys_close(int fd);

// helper 함수 ==========
static struct open_file_list_elem* get_list_elem_from_fd(int fd);
static void check_valid_addr(void* addr);
static void check_valid_string(const char* str);
static void check_valid_buffer(const void* uaddr, size_t size, bool check_write);
static void check_writable_pointer(void* uaddr);
static bool fd_less(const struct list_elem* a, const struct list_elem* b, void* aux UNUSED);

struct open_file_list_elem {
    int fd;
    struct file* file;
    struct list_elem elem;
};

struct lock file_lock;

void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
    lock_init(&file_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame* f)
{
    switch (f->R.rax) {
    case SYS_HALT:
        sys_halt();
        break;

    case SYS_EXIT:
        sys_exit((int)f->R.rdi);
        break;

    case SYS_CREATE:
        f->R.rax = sys_create((const char*)f->R.rdi, (unsigned int)f->R.rsi);
        break;

    case SYS_REMOVE:
        f->R.rax = sys_remove((const char*)f->R.rdi);
        break;

    case SYS_OPEN:
        f->R.rax = sys_open((const char*)f->R.rdi);
        break;
    case SYS_FILESIZE:
        f->R.rax = sys_filesize((int)f->R.rdi);
        break;

    case SYS_READ:
        f->R.rax = sys_read((int)f->R.rdi, (void*)f->R.rsi, (unsigned int)f->R.rdx);
        break;

    case SYS_WRITE:
        f->R.rax = sys_write((int)f->R.rdi, (void*)f->R.rsi, (unsigned int)f->R.rdx);
        break;

    case SYS_SEEK:
        sys_seek((int)f->R.rdi, (unsigned int)f->R.rsi);
        break;

    case SYS_TELL:
        f->R.rax = sys_tell((int)f->R.rdi);
        break;

    case SYS_CLOSE:
        sys_close((int)f->R.rdi);
        break;
    }
}

void sys_halt()
{
    power_off();
}

void sys_exit(int status)
{
    thread_current()->exit_code = status;
    thread_exit();
}

static bool sys_create(const char* file, unsigned initial_size)
{
    check_valid_string(file);

    lock_acquire(&file_lock);
    bool is_created = filesys_create(file, initial_size);
    lock_release(&file_lock);
    return is_created;
}

static bool sys_remove(const char* file)
{
    check_valid_string(file);

    lock_acquire(&file_lock);
    bool is_removed = filesys_remove(file);
    lock_release(&file_lock);
    return is_removed;
}

static int sys_open(const char* file)
{
    check_valid_string(file);

    lock_acquire(&file_lock);
    struct file* open_file = filesys_open(file);
    lock_release(&file_lock);

    if (open_file == NULL) {
        return -1;
    }
    struct thread* curr = thread_current();

    // fd list 순회하며 비어있는 부분 체크해 fd값 부여
    struct list* open_file_list = &thread_current()->open_file_list;
    struct list_elem* e;

    int fd = 2;
    for (e = list_begin(open_file_list); e != list_end(open_file_list); e = list_next(e)) {
        struct open_file_list_elem* fd_entry = list_entry(e, struct open_file_list_elem, elem);
        if (fd_entry->fd != fd++) {
            break;
        }
    }

    struct open_file_list_elem* fd_entry = malloc(sizeof *fd_entry);
    fd_entry->fd = fd;
    fd_entry->file = open_file;
    list_insert_ordered(&curr->open_file_list, &fd_entry->elem, fd_less, NULL);
    return fd;
}

static int sys_filesize(int fd)
{
    struct file* file = get_list_elem_from_fd(fd)->file;

    lock_acquire(&file_lock);
    int file_len = file_length(file);
    lock_release(&file_lock);
    return file_len;
}

static int sys_read(int fd, void* buffer, unsigned size)
{
    if (size == 0) {
        return 0;
    }
    check_valid_buffer(buffer, size, false);

    uint8_t* buf = (uint8_t*)buffer;

    if (fd == 0) {
        for (int i = 0; i < size; i++) {
            uint8_t key = input_getc();
            buf[i] = key;
        }
        return size;
    } else if (fd == 1) {
        return -1;
    } else {
        struct file* file = get_list_elem_from_fd(fd)->file;
        if (file == NULL) {
            return -1;
        }
        lock_acquire(&file_lock);
        int read_byte = file_read(file, buffer, (off_t)size);
        lock_release(&file_lock);
        return read_byte;
    }
}

static int sys_write(int fd, const void* buffer, unsigned size)
{
    if (size == 0) {
        return 0;
    }

    check_valid_buffer(buffer, size, true);

    if (fd == 0 || fd == NULL) {
        return -1;
    } else if (fd == 1) {
        putbuf((const char*)buffer, (size_t)size);
        return size;
    } else {
        struct file* write_file = get_list_elem_from_fd(fd)->file;
        if (write_file != NULL) {
            lock_acquire(&file_lock);
            int bytes_written = file_write(write_file, buffer, (off_t)size);
            lock_release(&file_lock);

            return bytes_written;
        } else {
            return -1;
        }
    }
}

static void sys_seek(int fd, unsigned position)
{
    struct file* file = get_list_elem_from_fd(fd)->file;
    lock_acquire(&file_lock);
    file_seek(file, (off_t)position);
    lock_release(&file_lock);
}

static unsigned sys_tell(int fd)
{
    struct file* file = get_list_elem_from_fd(fd)->file;
    lock_acquire(&file_lock);
    unsigned tell_byte = file_tell(file);
    lock_release(&file_lock);
    return tell_byte;
}

static void sys_close(int fd)
{

    // open_file_list 에서 제거
    struct open_file_list_elem* fd_entry = get_list_elem_from_fd(fd);
    if (fd_entry == NULL) {
        return;
    }
    list_remove(&fd_entry->elem);

    lock_acquire(&file_lock);
    file_close(fd_entry->file);
    lock_release(&file_lock);
}

// fd 인자로 받아 open_file_ilst_elem 리턴하는 함수
static struct open_file_list_elem* get_list_elem_from_fd(int fd)
{
    struct list* open_file_list = &thread_current()->open_file_list;
    struct list_elem* e;

    for (e = list_begin(open_file_list); e != list_end(open_file_list); e = list_next(e)) {
        struct open_file_list_elem* fd_entry = list_entry(e, struct open_file_list_elem, elem);
        if (fd_entry->fd == fd) {
            return fd_entry;
        }
    }
    return NULL;
}

static void check_valid_addr(void* addr)
{
    if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
        sys_exit(-1);
    }
}

static void check_valid_string(const char* str)
{
    if (str == NULL) {
        sys_exit(-1);
    }

    const char* page_entry = (const char*)pg_round_down(str);
    const char* page_boundary = page_entry + PGSIZE;

    for (const char* p = str;; p++) {
        if (p >= page_boundary) {
            check_valid_addr(p);
            page_entry = (const char*)pg_round_down(p);
            page_boundary = page_entry + PGSIZE;
        } else if (p == str || (p - page_entry) == 0) {
            check_valid_addr(p);
        }
        if (*p == '\0') {
            return;
        }
    }
}

static void check_valid_buffer(const void* uaddr, size_t size, bool check_write)
{
    if (size == 0)
        return;
    uintptr_t start_addr = (uintptr_t)uaddr;
    uintptr_t end_addr = start_addr + size - 1;
    if (end_addr < start_addr) // 덧셈 오버플로우 방지
        sys_exit(-1);
    const uint8_t* start = (const uint8_t*)start_addr;
    const uint8_t* end = (const uint8_t*)end_addr; // 마지막 바이트
    /* 시작/끝이 걸쳐 있는 모든 페이지를 순회하며, 각 페이지 내의 실제 접근 바이트를 검증 */
    uintptr_t current_page = (uintptr_t)pg_round_down(start);
    uintptr_t end_page = (uintptr_t)pg_round_down(end);
    for (uintptr_t page = current_page; page <= end_page; page += PGSIZE) {
        const uint8_t* check_addr = (page < (uintptr_t)start) ? start : (const uint8_t*)page;
        if (check_addr > end)
            check_addr = end;
        check_valid_addr(check_addr);
        /* 쓰기 권한 검사가 필요한 경우 각 페이지마다 검사 수행 */
        if (check_write)
            check_writable_pointer((void*)check_addr);
    }
}

static void check_writable_pointer(void* uaddr)
{
    uint64_t* pte = pml4e_walk(thread_current()->pml4, (uint64_t)uaddr, false);
    if (pte == NULL || !is_writable(pte))
        sys_exit(-1);
}

static bool fd_less(const struct list_elem* a, const struct list_elem* b, void* aux UNUSED)
{
    struct open_file_list_elem* fd_entry_a = list_entry(a, struct open_file_list_elem, elem);
    struct open_file_list_elem* fd_entry_b = list_entry(b, struct open_file_list_elem, elem);
    return fd_entry_a->fd < fd_entry_b->fd;
}
