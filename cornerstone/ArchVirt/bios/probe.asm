; Independent BIOS consumer: disk-loaded second stage, not a host-injected PC.
bits 16
section .boot start=0 vstart=0x7c00
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    cld
    cmp dl, 0x80
    jne fail1
%ifdef ROM_WRITE
    mov ax, 0xf000
    mov es, ax
    mov byte [es:0], 0
    cli
    hlt
%else
    mov ax, 0x0204
    mov cx, 2
    mov dx, 0x80
    mov bx, 0x8000
    int 0x13
    jc fail1
    jmp 0:stage2
%endif
fail1:
    mov al, '!'
    mov ah, 0x0e
    int 0x10
    cli
    hlt
times 510-($-$$) db 0
dw 0xaa55

section .stage start=512 vstart=0x8000
stage2:
    int 0x12
    cmp ax, 636
    jne fail
    mov ax, 0x4100
    mov bx, 0x55aa
    mov dl, 0x80
    int 0x13
    jc fail
    cmp bx, 0xaa55
    jne fail
    test cx, 1
    jz fail
    xor ebx, ebx
    mov di, 0x6000
.map:
    mov eax, 0xe820
    mov edx, 0x534d4150
    mov ecx, 24
    int 0x15
    jc fail
    cmp eax, 0x534d4150
    jne fail
    cmp ecx, 24
    jne fail
    add di, 24
    test ebx, ebx
    jnz .map
    cmp di, 0x6060
    jne fail
    mov si, dap
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc fail
    cmp dword [0x7000], 0x12345678
    jne fail
    ; Write/read the private disk, then fail a request without touching the buffer.
    mov dword [0x7000], 0x87654321
    xor al, al
    mov ah, 0x43
    int 0x13
    jc fail
    mov dword [0x7000], 0
    mov ah, 0x42
    int 0x13
    jc fail
    cmp dword [0x7000], 0x87654321
    jne fail
    mov word [dap+2], 0
    mov ah, 0x42
    int 0x13
    jnc fail
    cmp dword [0x7000], 0x87654321
    jne fail
    mov word [dap+2], 2
    mov dword [dap+8], 1007
    mov ah, 0x42
    int 0x13
    jnc fail
    mov word [dap+2], 1
    mov dword [dap+8], 8
    mov word [dap+4], 0xff00
    mov ah, 0x42
    int 0x13
    jnc fail
    mov word [dap+4], 0x7000
    mov ah, 0x44
    int 0x13
    jc fail
    mov ah, 0x47
    int 0x13
    jc fail
    mov byte [dap], 15
    mov ah, 0x42
    int 0x13
    jnc fail
    mov byte [dap], 16
    mov dword [dap+12], 1
    mov ah, 0x42
    int 0x13
    jnc fail
    cmp dword [0x7000], 0x87654321
    jne fail
    ; Service failures must survive IRET in the caller's saved FLAGS.
    mov ah, 0x99
    int 0x13
    jnc fail
    mov ah, 8
    int 0x13
    jc fail
    cmp cx, 63
    jne fail
    cmp dx, 0x0f01
    jne fail
    mov si, ready
    call puts
.key:
    mov ah, 1
    int 0x16
    jz .key
    cmp al, 'Z'
    jne fail
    ; Repeated peek and get see the same byte.
    mov ah, 1
    int 0x16
    jz fail
    cmp al, 'Z'
    jne fail
    xor ah, ah
    int 0x16
    cmp al, 'Z'
    jne fail
    mov si, passed
    call puts
    mov dword [0x6100], 0x50415353
    cli
    hlt
fail:
    mov si, failed
    call puts
    cli
    hlt
puts:
    lodsb
    test al, al
    jz .ret
    mov ah, 0x0e
    int 0x10
    jmp puts
.ret: ret
ready: db 'BIOS PROBE INPUT',13,10,0
passed: db 'BIOS PROBE PASS',13,10,0
failed: db 'BIOS PROBE FAIL',13,10,0
dap: db 16,0
    dw 1,0x7000,0
    dq 8
times 2048-($-$$) db 0
section .data start=4096
dd 0x12345678
times 1008*512-4096-4 db 0
