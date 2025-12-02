#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
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

static void check_valid_user_pointer(const void* uaddr);
static void check_writable_pointer(void* uaddr);
static void check_valid_buffer(const void* uaddr, size_t size, bool check_write);
static void check_valid_string(const char* str);

void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame* f UNUSED)
{
    // TODO: Your implementation goes here.
    printf("system call!\n");
    thread_exit();
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
}