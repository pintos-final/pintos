#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/synch.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

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

#define EXIT_FAILURE -1    /* 프로세스 종료 코드 */
#define SYSCALL_FAILURE -1 /* 시스템콜 반환값 */

#define STDIN_FILENO 0
#define STDOUT_FILENO 1

struct lock filesys_lock;

static void check_valid_user_pointer(const void* uaddr);
static void check_writable_pointer(void* uaddr);
static void check_valid_buffer(const void* uaddr, size_t size, bool check_write);
static void check_valid_string(const char* str);

static void halt(void) NO_RETURN;
static void exit(int status) NO_RETURN;
static bool create(const char* file, unsigned initial_size);
static bool remove(const char* file);
static int open(const char* file);
static int filesize(int fd);
static int read(int fd, void* buffer, unsigned length);
static int write(int fd, const void* buffer, unsigned length);
static void seek(int fd, unsigned position);
static unsigned tell(int fd);
static void close(int fd);

void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

    lock_init(&filesys_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame* f)
{
    switch (f->R.rax) {
    case SYS_HALT:
        halt();
        break;
    case SYS_EXIT:
        exit((int)f->R.rdi);
        break;
    case SYS_CREATE:
        f->R.rax = create((const char*)f->R.rdi, (unsigned)f->R.rsi);
        break;
    case SYS_REMOVE:
        f->R.rax = remove((const char*)f->R.rdi);
        break;
    case SYS_OPEN:
        f->R.rax = open((const char*)f->R.rdi);
        break;
    case SYS_FILESIZE:
        f->R.rax = filesize((int)f->R.rdi);
        break;
    case SYS_READ:
        f->R.rax = read((int)f->R.rdi, (void*)f->R.rsi, (unsigned)f->R.rdx);
        break;
    case SYS_WRITE:
        f->R.rax = write((int)f->R.rdi, (const void*)f->R.rsi, (unsigned)f->R.rdx);
        break;
    case SYS_SEEK:
        seek((int)f->R.rdi, (unsigned)f->R.rsi);
        break;
    case SYS_TELL:
        f->R.rax = tell((int)f->R.rdi);
        break;
    case SYS_CLOSE:
        close((int)f->R.rdi);
        break;
    default:
        exit(EXIT_FAILURE);
    }
}

/* 유저 포인터가 유효한지 검사하고, 잘못된 경우 프로세스를 종료
 * - NULL 포인터인지, 유저 영역 가상 주소인지, 현재 스레드의 pml4에 매핑된 페이지인지 확인 */
static void check_valid_user_pointer(const void* uaddr)
{
    if (uaddr == NULL)
        exit(EXIT_FAILURE);
    if (!is_user_vaddr(uaddr))
        exit(EXIT_FAILURE);
    if (!pml4_get_page(thread_current()->pml4, uaddr))
        exit(EXIT_FAILURE);
}

/* 주어진 유저 포인터가 쓰기 가능한 페이지를 가리키는지 검사
 * 코드/읽기 전용 페이지에 대한 쓰기 시도를 탐지하면 프로세스를 종료 */
static void check_writable_pointer(void* uaddr)
{
    uint64_t* pte = pml4e_walk(thread_current()->pml4, (uint64_t)uaddr, false);
    if (pte == NULL || !is_writable(pte))
        exit(EXIT_FAILURE);
}

static void check_valid_buffer(const void* uaddr, size_t size, bool check_write)
{
    if (size == 0)
        return;

    uintptr_t start_addr = (uintptr_t)uaddr;
    uintptr_t end_addr = start_addr + size - 1;
    if (end_addr < start_addr) // 덧셈 오버플로우 방지
        exit(EXIT_FAILURE);

    const uint8_t* start = (const uint8_t*)start_addr;
    const uint8_t* end = (const uint8_t*)end_addr; // 마지막 바이트

    /* 시작/끝이 걸쳐 있는 모든 페이지를 순회하며, 각 페이지 내의 실제 접근 바이트를 검증 */
    uintptr_t current_page = (uintptr_t)pg_round_down(start);
    uintptr_t end_page = (uintptr_t)pg_round_down(end);
    for (uintptr_t page = current_page; page <= end_page; page += PGSIZE) {
        const uint8_t* check_addr = (page < (uintptr_t)start) ? start : (const uint8_t*)page;
        if (check_addr > end)
            check_addr = end;

        check_valid_user_pointer(check_addr);

        /* 쓰기 권한 검사가 필요한 경우 각 페이지마다 검사 수행 */
        if (check_write)
            check_writable_pointer((void*)check_addr);
    }
}

static void check_valid_string(const char* str)
{
    if (str == NULL)
        exit(EXIT_FAILURE);

    const char* page_start = (const char*)pg_round_down(str);
    const char* page_boundary = page_start + PGSIZE;

    for (const char* p = str;; p++) {
        // 페이지 경계 도달 시마다 검증
        if (p >= page_boundary) {
            check_valid_user_pointer(p);
            page_start = (const char*)pg_round_down(p);
            page_boundary = page_start + PGSIZE;
        } else if (p == str || (p - page_start) == 0) {
            // 첫 문자 또는 새 페이지 첫 바이트 검증
            check_valid_user_pointer(p);
        }

        if (*p == '\0')
            return;
    }
}

static struct file* get_file_from_fd(int fd)
{
    if (fd < FD_MIN || fd >= FD_MAX)
        return NULL;
    return thread_current()->fd_table[fd];
}

static void halt(void)
{
    power_off();
    NOT_REACHED();
}

static void exit(int status)
{
    thread_current()->exit_status = status;
    thread_exit();
    NOT_REACHED();
}

static bool create(const char* file, unsigned initial_size)
{
    check_valid_string(file);

    lock_acquire(&filesys_lock);
    bool is_success = filesys_create(file, initial_size);
    lock_release(&filesys_lock);

    return is_success;
}

static bool remove(const char* file)
{
    check_valid_string(file);

    lock_acquire(&filesys_lock);
    bool is_success = filesys_remove(file);
    lock_release(&filesys_lock);

    return is_success;
}

static int open(const char* file)
{
    check_valid_string(file);

    lock_acquire(&filesys_lock);
    struct file* opened_file = filesys_open(file);
    lock_release(&filesys_lock);

    if (opened_file == NULL) {
        return SYSCALL_FAILURE;
    }

    struct thread* curr = thread_current();
    for (int fd = FD_MIN; fd < FD_MAX; fd++) {
        if (curr->fd_table[fd] == NULL) {
            curr->fd_table[fd] = opened_file;
            return fd;
        }
    }

    // FD 테이블이 가득 찬 경우
    lock_acquire(&filesys_lock);
    file_close(opened_file);
    lock_release(&filesys_lock);
    return SYSCALL_FAILURE;
}

static int filesize(int fd)
{

    struct file* f = get_file_from_fd(fd);
    if (f == NULL) {
        return SYSCALL_FAILURE;
    }

    lock_acquire(&filesys_lock);
    int size = file_length(f);
    lock_release(&filesys_lock);
    return size;
}

static int read(int fd, void* buffer, unsigned length)
{
    if (length == 0)
        return 0;

    check_valid_buffer(buffer, length, true);

    if (fd == STDIN_FILENO) {
        char* buf = buffer;
        for (unsigned i = 0; i < length; i++)
            buf[i] = input_getc();
        return length;
    }

    if (fd == STDOUT_FILENO)
        return SYSCALL_FAILURE;

    struct file* f = get_file_from_fd(fd);
    if (f == NULL)
        return SYSCALL_FAILURE;

    lock_acquire(&filesys_lock);
    int read_bytes = file_read(f, buffer, length);
    lock_release(&filesys_lock);

    return read_bytes;
}

static int write(int fd, const void* buffer, unsigned length)
{
    if (length == 0)
        return 0;

    check_valid_buffer(buffer, length, false);

    if (fd == STDIN_FILENO)
        return SYSCALL_FAILURE;

    if (fd == STDOUT_FILENO) {
        putbuf(buffer, length);
        return length;
    }

    struct file* f = get_file_from_fd(fd);
    if (f == NULL)
        return SYSCALL_FAILURE;

    lock_acquire(&filesys_lock);
    int written_bytes = file_write(f, buffer, length);
    lock_release(&filesys_lock);

    return written_bytes;
}

static void seek(int fd, unsigned position)
{
    struct file* f = get_file_from_fd(fd);
    if (f == NULL) {
        return;
    }

    lock_acquire(&filesys_lock);
    file_seek(f, position);
    lock_release(&filesys_lock);
}

static unsigned tell(int fd)
{
    struct file* f = get_file_from_fd(fd);
    if (f == NULL) {
        return SYSCALL_FAILURE;
    }

    lock_acquire(&filesys_lock);
    unsigned pos = file_tell(f);
    lock_release(&filesys_lock);
    return pos;
}

static void close(int fd)
{
    struct file* f = get_file_from_fd(fd);
    if (f == NULL) {
        return;
    }

    lock_acquire(&filesys_lock);
    file_close(f);
    lock_release(&filesys_lock);

    thread_current()->fd_table[fd] = NULL;
}