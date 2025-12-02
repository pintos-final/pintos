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
static void check_valid_string(const char* str, size_t max_len);

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
        exit(-1);
    if (!is_user_vaddr(uaddr))
        exit(-1);
    if (!pml4_get_page(thread_current()->pml4, uaddr))
        exit(-1);
}

/* 주어진 유저 포인터가 쓰기 가능한 페이지를 가리키는지 검사
 * 코드/읽기 전용 페이지에 대한 쓰기 시도를 탐지하면 프로세스를 종료 */
static void check_writable_pointer(void* uaddr)
{
    uint64_t* pte = pml4e_walk(thread_current()->pml4, (uint64_t)uaddr, false);
    if (pte == NULL || !is_writable(pte))
        exit(-1);
}

static void check_valid_buffer(const void* uaddr, size_t size, bool check_write)
{
    if (size == 0)
        return;
    if (uaddr == NULL)
        exit(-1);

    uintptr_t start_addr = (uintptr_t)uaddr;
    uintptr_t end_addr = start_addr + size - 1;

    if (end_addr < start_addr) // 오버플로우
        exit(-1);

    for (uintptr_t addr = start_addr; addr <= end_addr;) {
        check_valid_user_pointer((void*)addr);
        if (check_write)
            check_writable_pointer((void*)addr);

        uintptr_t next_page = pg_round_down(addr) + PGSIZE;
        if (next_page <= addr) // 오버플로우 감지
            break;
        addr = next_page;
    }
}

/* 유저 문자열이 유효한지 검사
 * - 최대 max_len 길이 이내에 널 종료 문자가 존재하는지 확인
 * - 문자열의 각 바이트가 유효한 유저 메모리인지 확인
 * - 널 종료 문자가 max_len 바이트 내에 없으면 false 반환 */
static void check_valid_string(const char* str, size_t max_len)
{
    if (str == NULL)
        exit(-1);

    const char* page_boundary = (const char*)pg_round_down(str) + PGSIZE;

    for (size_t i = 0; i <= max_len; i++) {
        const char* current = str + i;

        if (current >= page_boundary) { // i == 0일 때는 아래서 무조건 검사
            check_valid_user_pointer(current);
            page_boundary = (const char*)pg_round_down(current) + PGSIZE;
        } else if (i == 0) {
            check_valid_user_pointer(current); // 첫 바이트는 반드시 검사
        }

        if (*current == '\0')
            return;
    }

    exit(-1);
}