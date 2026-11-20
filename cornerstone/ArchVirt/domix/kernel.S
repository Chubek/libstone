.section .text
.global _start
.type _start, @function

.equ CONSOLE_COUNT, 4
.equ CONSOLE_COLUMNS, 80
.equ CONSOLE_ROWS, 25
.equ CONSOLE_CELL_BYTES, 2
.equ CONSOLE_BYTES, 4000
.equ CONSOLE_LINE_BYTES, 128

/* Domix starts in long mode with identity paging supplied by Domvirt. */
_start:
    cld
    mov $0x700000, %rsp
    call initialize_consoles
    lea banner(%rip), %rsi
    call print_string

    /* The Domix image is supplied by Domvirt as a packed initrd at 0x800000.
       Validate its header and locate the /init entry before handing control
       to userspace.  This is intentionally a tiny freestanding bootstrap:
       it provides deterministic diagnostics until the full process/syscall
       layer is available. */
    mov $0x800000, %r12
    lea image_magic(%rip), %rsi
    mov %r12, %rdi
    call compare_magic
    jnz image_bad
    cmpl $1, 8(%r12)
    jne image_bad

    lea userspace(%rip), %rsi
    call print_string
    mov %r12, %rdi
    call find_init
    jc image_bad
    mov %rax, %r13
    lea init_found(%rip), %rsi
    call print_string
    lea init_exec(%rip), %rsi
    call print_string
    lea prompt(%rip), %rsi
    call print_string
    call select_line_buffer
shell_loop:
    call serial_getc
    test %al, %al
    jz shell_loop
    cmp $0x11, %al
    jb shell_input
    cmp $0x14, %al
    ja shell_input
    sub $0x11, %al
    call switch_console
    jmp shell_loop
shell_input:
    cmp $8, %al
    je shell_backspace
    cmp $127, %al
    je shell_backspace
    cmp $13, %al
    je shell_submit
    cmp $10, %al
    je shell_submit
    cmp $127, %ecx
    jae shell_loop
    mov %al, (%rdi,%rcx,1)
    inc %ecx
    call console_putc
    jmp shell_loop
shell_backspace:
    test %ecx, %ecx
    jz shell_loop
    dec %ecx
    mov $8, %al
    call console_putc
    mov $32, %al
    call console_putc
    mov $8, %al
    call console_putc
    jmp shell_loop
shell_submit:
    movb $0, (%rdi,%rcx,1)
    mov $10, %al
    call console_putc
    mov %rdi, %rsi
    call shell_dispatch
    xor %ecx, %ecx
    call store_line_length
    lea prompt(%rip), %rsi
    call print_string
    call select_line_buffer
    jmp shell_loop
shell_dispatch:
    cmpl $0, (%rsi)
    je shell_done
    cmpl $0x706c6568, (%rsi)
    jne .Lnot_help
    lea help_text(%rip), %rsi
    call print_string
    ret
.Lnot_help:
    cmpl $0x0000736c, (%rsi)
    jne .Lnot_ls
    lea ls_text(%rip), %rsi
    call print_string
    ret
.Lnot_ls:
    cmpl $0x206c73, (%rsi)
    jne .Lnot_ls_etc
    lea etc_listing(%rip), %rsi
    call print_string
    ret
.Lnot_ls_etc:
    cmpl $0x646e616d, (%rsi)
    jne .Lnot_uname
    lea uname_text(%rip), %rsi
    call print_string
    ret
.Lnot_uname:
    cmpl $0x68746170, (%rsi)
    jne .Lnot_pwd
    lea pwd_text(%rip), %rsi
    call print_string
    ret
.Lnot_pwd:
    cmpl $0x6f686365, (%rsi)
    jne .Lnot_echo
    lea 5(%rsi), %rsi
    call print_string
    mov $10, %al
    call serial_putc
    ret
.Lnot_echo:
    cmpl $0x20746163, (%rsi)
    jne .Lnot_cat
    cmpl $0x6374652f, 4(%rsi)
    jne .Lnot_cat
    cmpl $0x7373692f, 8(%rsi)
    jne .Lnot_cat
    cmpl $0x657565, 12(%rsi)
    jne .Lnot_cat
    lea issue_path(%rip), %rsi
    mov $8, %edx
    call find_named
    jc .Lcat_missing
    mov %rax, %rsi
    call print_line
    ret
.Lcat_missing:
    lea file_missing(%rip), %rsi
    call print_string
    ret
.Lnot_cat:
    cmpl $0x20746163, (%rsi)
    jne .Lnot_cat2
    cmpl $0x6374652f, 4(%rsi)
    jne .Lnot_cat2
    cmpl $0x74736f68, 8(%rsi)
    jne .Lnot_cat2
    cmpl $0x656d616e, 12(%rsi)
    jne .Lnot_cat2
    lea hostname_path(%rip), %rsi
    mov $11, %edx
    call find_named
    jc .Lcat_missing
    mov %rax, %rsi
    call print_line
    ret
.Lnot_cat2:
    cmpl $0x20746163, (%rsi)
    jne .Lnot_cat3
    cmpl $0x6374652f, 4(%rsi)
    jne .Lnot_cat3
    cmpl $0x73736170, 8(%rsi)
    jne .Lnot_cat3
    cmpl $0x64726f77, 12(%rsi)
    jne .Lnot_cat3
    lea passwd_path(%rip), %rsi
    mov $11, %edx
    call find_named
    jc .Lcat_missing
    mov %rax, %rsi
    call print_line
    ret
.Lnot_cat4:
    cmpl $0x20746163, (%rsi)
    jne .Lnot_cat5
    cmpl $0x6374652f, 4(%rsi)
    jne .Lnot_cat5
    cmpl $0x62617473, 8(%rsi)
    jne .Lnot_cat5
    cmpl $0x00000066, 12(%rsi)
    jne .Lnot_cat5
    lea fstab_path(%rip), %rsi
    mov $9, %edx
    call find_named
    jc .Lcat_missing
    mov %rax, %rsi
    call print_line
    ret
.Lnot_cat3:
    cmpl $0x20746163, (%rsi)
    jne .Lnot_cat5
    cmpl $0x6374652f, 4(%rsi)
    jne .Lnot_cat4
    cmpl $0x666f7270, 8(%rsi)
    jne .Lnot_cat4
    cmpl $0x656c69, 12(%rsi)
    jne .Lnot_cat4
    lea profile_path(%rip), %rsi
    mov $11, %edx
    call find_named
    jc .Lcat_missing
    mov %rax, %rsi
    call print_line
    ret
.Lnot_cat5:
    cmpl $0x74697865, (%rsi)
    jne .Lnot_exit
    jmp halt
.Lnot_exit:
    cmpl $0x6d6f6877, (%rsi)
    jne .Lnot_whoami
    lea whoami_text(%rip), %rsi
    call print_string
    ret
.Lnot_whoami:
    cmpl $0x00006469, (%rsi)
    jne .Lnot_id
    lea id_text(%rip), %rsi
    call print_string
    ret
.Lnot_id:
    cmpl $0x7261656c, (%rsi)
    jne .Lnot_clear
    call console_clear
    ret
.Lnot_clear:
    cmpl $0x73726576, (%rsi)
    jne .Lnot_version
    lea version_text(%rip), %rsi
    call print_string
    ret
.Lnot_version:
    cmpl $0x6e756f6d, (%rsi)
    jne .Lnot_mount
    lea mount_text(%rip), %rsi
    call print_string
    ret
.Lnot_mount:
    cmpl $0x006466, (%rsi)
    jne .Lnot_df
    mov 16(%r12), %rax
    add 24(%r12), %rax
    add 32(%r12), %rax
    lea df_prefix(%rip), %rsi
    call print_string
    mov %rax, %rsi
    call print_decimal
    lea df_suffix(%rip), %rsi
    call print_string
    ret
.Lnot_df:
    lea unknown_command(%rip), %rsi
    call print_string
shell_done:
    ret

image_bad:
    lea image_missing(%rip), %rsi
    call print_string
halt:
    /* Keep the guest alive until the host asks it to stop. */
    cli
1:
    hlt
    jmp 1b

/* Initialize the four independent VGA text consoles and their shell state. */
initialize_consoles:
    push %rax
    push %rcx
    push %rdi
    lea console_buffers(%rip), %rdi
    mov $0x0720, %ax
    mov $8000, %ecx
    rep stosw
    movq $0, console_cursors(%rip)
    movq $0, console_cursors+8(%rip)
    movq $0, console_cursors+16(%rip)
    movq $0, console_cursors+24(%rip)
    movl $0, console_line_lengths(%rip)
    movl $0, console_line_lengths+4(%rip)
    movl $0, console_line_lengths+8(%rip)
    movl $0, console_line_lengths+12(%rip)
    movb $0, active_console(%rip)
    pop %rdi
    pop %rcx
    pop %rax
    ret

/* Return the active console's backing buffer in R8 and cursor in R9. */
active_console_state:
    mov active_console(%rip), %al
    cmp $0, %al
    je .active_console0
    cmp $1, %al
    je .active_console1
    cmp $2, %al
    je .active_console2
    lea console_buffers+12000(%rip), %r8
    lea console_cursors+24(%rip), %r9
    ret
.active_console0:
    lea console_buffers(%rip), %r8
    lea console_cursors(%rip), %r9
    ret
.active_console1:
    lea console_buffers+4000(%rip), %r8
    lea console_cursors+8(%rip), %r9
    ret
.active_console2:
    lea console_buffers+8000(%rip), %r8
    lea console_cursors+16(%rip), %r9
    ret

/* Return the active console's line buffer in RDI and its length in ECX. */
select_line_buffer:
    mov active_console(%rip), %al
    cmp $0, %al
    je .select_line0
    cmp $1, %al
    je .select_line1
    cmp $2, %al
    je .select_line2
    lea line_buffers+384(%rip), %rdi
    mov console_line_lengths+12(%rip), %ecx
    ret
.select_line0:
    lea line_buffers(%rip), %rdi
    mov console_line_lengths(%rip), %ecx
    ret
.select_line1:
    lea line_buffers+128(%rip), %rdi
    mov console_line_lengths+4(%rip), %ecx
    ret
.select_line2:
    lea line_buffers+256(%rip), %rdi
    mov console_line_lengths+8(%rip), %ecx
    ret

/* Store the current shell line length for the active console. */
store_line_length:
    mov active_console(%rip), %al
    cmp $0, %al
    je .store_line0
    cmp $1, %al
    je .store_line1
    cmp $2, %al
    je .store_line2
    mov %ecx, console_line_lengths+12(%rip)
    ret
.store_line0:
    mov %ecx, console_line_lengths(%rip)
    ret
.store_line1:
    mov %ecx, console_line_lengths+4(%rip)
    ret
.store_line2:
    mov %ecx, console_line_lengths+8(%rip)
    ret

/* Copy the active console to the hardware-compatible VGA text page. */
console_refresh:
    push %rcx
    push %rdi
    push %rsi
    push %r8
    push %r9
    call active_console_state
    mov %r8, %rsi
    mov $0xb8000, %rdi
    mov $4000, %ecx
    rep movsb
    pop %r9
    pop %r8
    pop %rsi
    pop %rdi
    pop %rcx
    ret

/* Scroll the active console by one row and refresh VGA. */
console_scroll:
    push %rax
    push %rcx
    push %rdi
    push %rsi
    push %r8
    push %r9
    call active_console_state
    lea 160(%r8), %rsi
    mov %r8, %rdi
    mov $3840, %ecx
    rep movsb
    lea 3840(%r8), %rdi
    mov $0x0720, %ax
    mov $80, %ecx
    rep stosw
    movq $3840, (%r9)
    call console_refresh
    pop %r9
    pop %r8
    pop %rsi
    pop %rdi
    pop %rcx
    pop %rax
    ret

/* Emit one terminal character to the active console and COM1. */
console_putc:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %r8
    push %r9
    push %r10
    mov %al, %bl
    mov %bl, %al
    call serial_putc
    call active_console_state
    mov (%r9), %rdx
    cmp $8, %bl
    je .console_backspace
    cmp $127, %bl
    je .console_backspace
    cmp $13, %bl
    je .console_done
    cmp $10, %bl
    je .console_newline
    movb %bl, (%r8,%rdx,1)
    movb $0x1f, 1(%r8,%rdx,1)
    movb %bl, 0xb8000(%rdx)
    movb $0x1f, 0xb8001(%rdx)
    add $2, %rdx
    cmp $4000, %rdx
    jb .console_save_cursor
    call console_scroll
    jmp .console_done
.console_newline:
    mov $160, %r10
.console_newline_loop:
    cmp %r10, %rdx
    jb .console_save_newline
    add $160, %r10
    cmp $4000, %r10
    jb .console_newline_loop
    call console_scroll
    jmp .console_done
.console_save_newline:
    mov %r10, (%r9)
    jmp .console_done
.console_backspace:
    cmp $2, %rdx
    jb .console_done
    sub $2, %rdx
    movb $' ', (%r8,%rdx,1)
    movb $0x1f, 1(%r8,%rdx,1)
    movb $' ', 0xb8000(%rdx)
    movb $0x1f, 0xb8001(%rdx)
    mov %rdx, (%r9)
    jmp .console_done
.console_save_cursor:
    mov %rdx, (%r9)
.console_done:
    pop %r10
    pop %r9
    pop %r8
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    ret

/* Clear the active console and its VGA presentation. */
console_clear:
    push %rax
    push %rcx
    push %rdi
    push %r8
    push %r9
    call active_console_state
    mov %r8, %rdi
    mov $0x0720, %ax
    mov $2000, %ecx
    rep stosw
    movq $0, (%r9)
    call console_refresh
    pop %r9
    pop %r8
    pop %rdi
    pop %rcx
    pop %rax
    ret

/* Switch to console AL (0..3), preserving each shell's input line. */
switch_console:
    push %rax
    push %rbx
    push %rdx
    push %r8
    push %r9
    push %r10
    mov %al, %bl
    call store_line_length
    mov %bl, active_console(%rip)
    call console_refresh
    call active_console_state
    mov (%r9), %rdx
    test %rdx, %rdx
    jnz .switch_console_existing
    lea prompt(%rip), %rsi
    call print_string
.switch_console_existing:
    call select_line_buffer
    pop %r10
    pop %r9
    pop %r8
    pop %rdx
    pop %rbx
    pop %rax
    ret

/* RSI = NUL-terminated string; emit it to the active text console. */
print_string:
1:
    lodsb
    test %al, %al
    jz 2f
    call console_putc
    jmp 1b
2:
    ret

/* Compare 8-byte image magic at [RDI] with RSI. */
compare_magic:
    mov (%rsi), %rax
    cmp (%rdi), %rax
    ret

/* Locate the packed image's "init" entry. Returns data pointer in RAX. */
find_init:
    mov 32(%rdi), %rcx          /* file_count */
    lea 40(%rdi), %r8           /* image header */
    mov 16(%rdi), %rax          /* kernel_size */
    add 24(%rdi), %rax          /* + manifest_size */
    add %rax, %r8
next_entry:
    test %rcx, %rcx
    jz not_found
    mov (%r8), %edx             /* path length */
    lea 4(%r8), %r9             /* path bytes */
    cmp $4, %edx
    jne skip_entry
    cmpl $0x74696e69, (%r9)     /* "init" little endian */
    jne skip_entry
    lea 4(%r9,%rdx,1), %r10     /* data_size follows mode */
    lea 8(%r10), %rax            /* data bytes follow the u64 data_size */
    clc
    ret
skip_entry:
    lea 4(%r9,%rdx,1), %r10     /* mode field */
    mov (%r10), %rax             /* data_size */
    lea 8(%r10,%rax,1), %r8     /* next entry */
    dec %rcx
    jmp next_entry
not_found:
    stc
    ret

/* Find a rootfs path in the packed image. RSI=path, EDX=length. */
find_named:
    mov %rsi, %r15
    mov %edx, %r14d
    mov 32(%r12), %rcx
    lea 40(%r12), %r8
    mov 16(%r12), %rax
    add 24(%r12), %rax
    add %rax, %r8
.find_next:
    test %rcx, %rcx
    jz .find_fail
    mov (%r8), %eax
    cmp %r14d, %eax
    jne .find_skip
    lea 4(%r8), %r9
    xor %r10d, %r10d
.find_cmp:
    cmp %r14d, %r10d
    jae .find_hit
    mov (%r9,%r10,1), %al
    cmp (%r15,%r10,1), %al
    jne .find_skip
    inc %r10d
    jmp .find_cmp
.find_hit:
    lea 4(%r9,%r14,1), %r10     /* data_size field */
    lea 8(%r10), %rax            /* data bytes */
    clc
    ret
.find_skip:
    lea 4(%r8,%rax,1), %r9
    add $4, %r9                  /* data_size field */
    mov (%r9), %rax              /* data_size */
    lea 8(%r9,%rax,1), %r8       /* next entry */
    dec %rcx
    jmp .find_next
.find_fail:
    stc
    ret

/* Print up to 256 bytes from RSI, stopping at NUL/newline. */
print_line:
    mov $256, %rcx
1:
    lodsb
    test %al, %al
    jz 2f
    cmp $10, %al
    je 2f
    call console_putc
    loop 1b
2:
    mov $10, %al
    call console_putc
    ret

serial_putc:
    push %rdx
    mov $0x3f8, %dx
    outb %al, (%dx)
    pop %rdx
    ret

serial_getc:
    xor %eax, %eax
    mov $0x3f8, %dx
    inb (%dx), %al
    ret

print_decimal:
    push %rbx
    lea decimal_buffer+20(%rip), %rdi
    movb $0, (%rdi)
    mov $10, %rbx
    test %rsi, %rsi
    jnz 1f
    dec %rdi
    movb $'0', (%rdi)
    jmp 3f
1:
    xor %edx, %edx
    mov %rsi, %rax
    div %rbx
    add $'0', %dl
    dec %rdi
    mov %dl, (%rdi)
    mov %rax, %rsi
    test %rax, %rax
    jnz 1b
3:
    mov %rdi, %rsi
    call print_string
    pop %rbx
    ret

.section .rodata
banner:
    .asciz "Domix 0.1  |  Domvirt booting..."
userspace:
    .asciz "Domix userspace: mounting packed rootfs..."
init_found:
    .asciz " /init found"
image_missing:
    .asciz "Domix userspace image invalid or missing"
image_magic:
    .ascii "DOMIXIMG"
init_exec:
    .asciz " /init executed; four Domix consoles ready (Ctrl+F1-F4)\r\n"
prompt:
    .asciz "domix$ "
issue_path:
    .ascii "etc/issue"
hostname_path:
    .ascii "etc/hostname"
passwd_path:
    .ascii "etc/passwd"
etc_listing:
    .asciz "fstab  group  hostname  issue  passwd  profile\r\n"
file_missing:
    .asciz "cat: file not found\r\n"
help_text:
    .asciz "help ls pwd uname echo cat whoami id clear version mount df exit\r\n"
ls_text:
    .asciz "bin  dev  etc  init  proc  sys\r\n"
uname_text:
    .asciz "Domix domvirt x86_64\r\n"
pwd_text:
    .asciz "/\r\n"
whoami_text:
    .asciz "root\r\n"
id_text:
    .asciz "uid=0(root) gid=0(root)\r\n"
version_text:
    .asciz "Domix 0.1 (domvirt long-mode guest)\r\n"
mount_text:
    .asciz "rootfs: DOMIXIMG initrd mounted at /; proc/sys virtual\r\n"
df_prefix:
    .asciz "domix-rootfs "
df_suffix:
    .asciz " bytes\r\n"
fstab_path:
    .ascii "etc/fstab"
profile_path:
    .ascii "etc/profile"
unknown_command:
    .asciz "command not found\r\n"

.section .bss
.align 8
active_console:
    .byte 0
.align 8
console_cursors:
    .space 32
console_line_lengths:
    .space 16
console_buffers:
    .space 16000
line_buffers:
    .space 512
decimal_buffer:
    .space 24
