/* Publish observations in guest RAM instead of making Linux system calls. */
extern int main(void);

#define MAILBOX ((volatile unsigned long *)ARCHVIRT_MAILBOX)
#define PAYLOAD_LIMIT 65536UL

long cfg_write_stdout(const void *data, unsigned long size) {
    if (size > PAYLOAD_LIMIT)
        return -1;
    const unsigned char *source = data;
    volatile unsigned char *destination = (volatile unsigned char *)(MAILBOX + 3);
    for (unsigned long i = 0; i < size; ++i)
        destination[i] = source[i];
    MAILBOX[2] = size;
    return (long)size;
}

void archvirt_main(void) {
    MAILBOX[2] = 0;
    MAILBOX[1] = (unsigned int)main();
    MAILBOX[0] = 0x4345504859525631UL;
}
