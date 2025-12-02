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
static struct file* get_file_from_fd(int fd);
static void check_valid_addr(void* addr);

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
    // 인자 순서 : rdi, rsi, rdx
    // 시스템콜 번호 / 리턴 : rax

    uint64_t syscall_no = f->R.rax;

    int status;
    char* file;
    unsigned initial_size;
    int fd;
    void* buffer;
    unsigned size;
    unsigned position;

    switch (syscall_no) {
    case SYS_HALT:
        sys_halt();
        break;

    case SYS_EXIT:
        int status = f->R.rdi;
        sys_exit(status);
        break;

    case SYS_CREATE:
        file = f->R.rdi;
        initial_size = f->R.rsi;
        f->R.rax = sys_create(file, initial_size);
        break;

    case SYS_REMOVE:
        file = f->R.rdi;
        f->R.rax = sys_remove(file);
        break;

    case SYS_OPEN:
        file = f->R.rdi;
        f->R.rax = sys_open(file);
        break;
    case SYS_FILESIZE:
        fd = f->R.rdi;
        f->R.rax = sys_filesize(fd);
        break;

    case SYS_READ:
        fd = f->R.rdi;
        buffer = f->R.rsi;
        size = f->R.rdx;

        f->R.rax = sys_read(fd, buffer, size);
        break;

    case SYS_WRITE:
        fd = f->R.rdi;
        buffer = f->R.rsi;
        size = f->R.rdx;

        f->R.rax = sys_write(fd, buffer, size);
        break;

    case SYS_SEEK:
        fd = f->R.rdi;
        position = f->R.rsi;

        sys_seek(fd, position);
        break;

    case SYS_TELL:
        fd = f->R.rdi;

        f->R.rax = sys_tell(fd);
        break;

    case SYS_CLOSE:
        fd = f->R.rdi;

        sys_close(fd);
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
    check_valid_addr(file);

    lock_acquire(&file_lock);
    bool is_created = filesys_create(file, initial_size);
    lock_release(&file_lock);
    return is_created;
}

static bool sys_remove(const char* file)
{
    check_valid_addr(file);

    lock_acquire(&file_lock);
    bool is_removed = filesys_remove(file);
    lock_release(&file_lock);
    return is_removed;
}

static int sys_open(const char* file)
{
    check_valid_addr(file);

    lock_acquire(&file_lock);
    struct file* open_file = filesys_open(file);
    lock_release(&file_lock);

    if (open_file == NULL) {
        return -1;
    }
    struct thread* curr = thread_current();
    int fd = list_size(&curr->open_file_list) + 2; // 0과 1은 표준입,출력

    struct open_file_list_elem* fd_entry = malloc(sizeof *fd_entry);
    fd_entry->fd = fd;
    fd_entry->file = open_file;
    list_push_back(&curr->open_file_list, &fd_entry->elem);
    return fd;
}

static int sys_filesize(int fd)
{
    lock_acquire(&file_lock);

    struct file* file = get_file_from_fd(fd);
    int file_len = file_length(file);
    lock_release(&file_lock);
    return file_len;
}

static int sys_read(int fd, void* buffer, unsigned size)
{
    check_valid_addr(buffer);
    check_valid_addr(buffer + size - 1);

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
        struct file* file = get_file_from_fd(fd);
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
    check_valid_addr(buffer);
    check_valid_addr(buffer + size - 1);

    if (fd == 0 || fd == NULL) {
        return -1;
    } else if (fd == 1) {
        putbuf((const char*)buffer, (size_t)size);
        return size;
    } else {
        struct file* write_file = get_file_from_fd(fd);
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
    struct file* file = get_file_from_fd(fd);
    lock_acquire(&file_lock);
    file_seek(file, (off_t)position);
    lock_release(&file_lock);
}

static unsigned sys_tell(int fd)
{
    struct file* file = get_file_from_fd(fd);
    lock_acquire(&file_lock);
    unsigned tell_byte = file_tell(file);
    lock_release(&file_lock);
    return tell_byte;
}

static void sys_close(int fd)
{

    // open_file_list 에서 제거
    struct list* open_file_list = &thread_current()->open_file_list;
    struct list_elem* e;

    for (e = list_begin(open_file_list); e != list_end(open_file_list); e = list_next(e)) {
        struct open_file_list_elem* fd_entry = list_entry(e, struct open_file_list_elem, elem);
        if (fd_entry->fd == fd) {
            list_remove(&fd_entry->elem);

            lock_acquire(&file_lock);
            file_close(fd_entry->file);
            lock_release(&file_lock);
            break;
        }
    }
}

// fd로 file 찾는 함수
static struct file* get_file_from_fd(int fd)
{
    struct list* open_file_list = &thread_current()->open_file_list;
    struct list_elem* e;

    for (e = list_begin(open_file_list); e != list_end(open_file_list); e = list_next(e)) {
        struct open_file_list_elem* fd_entry = list_entry(e, struct open_file_list_elem, elem);
        if (fd_entry->fd == fd) {
            return fd_entry->file;
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