/*
 * Simple mmap-backed allocator for the freestanding static runtime.
 *
 * Every live allocation owns one anonymous mapping.  This favors correctness
 * and independence over performance and does not attempt arenas, coalescing,
 * or allocator-wide concurrency support.
 */

typedef unsigned long cephyr_size_t;

extern int errno;
extern void *mmap(void *address, cephyr_size_t length, int protection,
                  int flags, int descriptor, long offset);
extern int munmap(void *address, cephyr_size_t length);

#define CEPHYR_ENOMEM 12
#define CEPHYR_PROT_READ 0x1
#define CEPHYR_PROT_WRITE 0x2
#define CEPHYR_MAP_PRIVATE 0x02
#define CEPHYR_MAP_ANONYMOUS 0x20
#define CEPHYR_MAP_FAILED ((void *)-1)

struct cephyr_allocation {
    cephyr_size_t mapping_size;
    cephyr_size_t requested_size;
};

#define CEPHYR_HEADER_SIZE ((cephyr_size_t)sizeof(struct cephyr_allocation))
#define CEPHYR_SIZE_MAX ((cephyr_size_t)-1)

void *malloc(cephyr_size_t size) {
    struct cephyr_allocation *allocation;
    cephyr_size_t mapping_size;

    /* C permits malloc(0) to return either null or a freeable unique pointer. */
    if (size == 0) {
        size = 1;
    }
    if (size > CEPHYR_SIZE_MAX - CEPHYR_HEADER_SIZE) {
        errno = CEPHYR_ENOMEM;
        return (void *)0;
    }

    mapping_size = CEPHYR_HEADER_SIZE + size;
    allocation = (struct cephyr_allocation *)mmap(
        (void *)0, mapping_size, CEPHYR_PROT_READ | CEPHYR_PROT_WRITE,
        CEPHYR_MAP_PRIVATE | CEPHYR_MAP_ANONYMOUS, -1, 0);
    if ((void *)allocation == CEPHYR_MAP_FAILED) {
        return (void *)0;
    }

    allocation->mapping_size = mapping_size;
    allocation->requested_size = size;
    return (void *)(allocation + 1);
}

void free(void *pointer) {
    struct cephyr_allocation *allocation;
    int saved_errno;

    if (pointer == (void *)0) {
        return;
    }

    allocation = ((struct cephyr_allocation *)pointer) - 1;
    saved_errno = errno;
    (void)munmap((void *)allocation, allocation->mapping_size);
    errno = saved_errno;
}

void *calloc(cephyr_size_t count, cephyr_size_t size) {
    cephyr_size_t total;

    if (size != 0 && count > CEPHYR_SIZE_MAX / size) {
        errno = CEPHYR_ENOMEM;
        return (void *)0;
    }
    total = count * size;

    /*
     * Linux guarantees that fresh anonymous mappings are zero-filled, so
     * malloc already supplies calloc's required representation.
     */
    return malloc(total);
}

void *realloc(void *pointer, cephyr_size_t size) {
    struct cephyr_allocation *old_allocation;
    cephyr_size_t copy_size;
    void *new_pointer;
    volatile unsigned char *destination;
    volatile const unsigned char *source;
    cephyr_size_t index;

    if (pointer == (void *)0) {
        return malloc(size);
    }
    if (size == 0) {
        free(pointer);
        return (void *)0;
    }

    old_allocation = ((struct cephyr_allocation *)pointer) - 1;
    new_pointer = malloc(size);
    if (new_pointer == (void *)0) {
        return (void *)0;
    }

    copy_size = old_allocation->requested_size;
    if (copy_size > size) {
        copy_size = size;
    }
    destination = (volatile unsigned char *)new_pointer;
    source = (volatile const unsigned char *)pointer;
    for (index = 0; index < copy_size; ++index) {
        destination[index] = source[index];
    }

    free(pointer);
    return new_pointer;
}
