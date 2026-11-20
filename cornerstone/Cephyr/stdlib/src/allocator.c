/*
 * Simple mmap-backed allocator for the freestanding static runtime.
 *
 * Every live allocation owns one anonymous mapping.  This favors correctness
 * and independence over performance and does not attempt arenas, coalescing,
 * or allocator-wide concurrency support.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct cephyr_allocation {
    void *mapping;
    size_t mapping_size;
    size_t requested_size;
};

static void *allocate(size_t alignment, size_t size)
{
    struct cephyr_allocation *allocation;
    unsigned char *mapping;
    unsigned char *data;
    size_t mapping_size;
    size_t overhead;

    /* C permits malloc(0) to return either null or a freeable unique pointer. */
    if (size == 0)
        size = 1;
    if (alignment < _Alignof(max_align_t))
        alignment = _Alignof(max_align_t);
    /* Leave space for metadata immediately before the aligned address. */
    overhead = sizeof(*allocation) + (alignment - 1);
    if (size > SIZE_MAX - overhead) {
        errno = ENOMEM;
        return NULL;
    }

    mapping_size = size + overhead;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        return NULL;

    data = mapping + sizeof(*allocation);
    data += (-(uintptr_t)data) & (alignment - 1);
    allocation = (struct cephyr_allocation *)(data - sizeof(*allocation));
    allocation->mapping = mapping;
    allocation->mapping_size = mapping_size;
    allocation->requested_size = size;
    return data;
}

void *malloc(size_t size)
{
    return allocate(_Alignof(max_align_t), size);
}

void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        (size & (alignment - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }
    return allocate(alignment, size);
}

void free(void *pointer)
{
    struct cephyr_allocation *allocation;
    int saved_errno;

    if (pointer == NULL)
        return;

    allocation = ((struct cephyr_allocation *)pointer) - 1;
    saved_errno = errno;
    (void)munmap(allocation->mapping, allocation->mapping_size);
    errno = saved_errno;
}

void *calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }

    /*
     * Linux guarantees that fresh anonymous mappings are zero-filled, so
     * malloc already supplies calloc's required representation.
     */
    return malloc(count * size);
}

void *realloc(void *pointer, size_t size)
{
    struct cephyr_allocation *old_allocation;
    size_t copy_size;
    void *new_pointer;

    if (pointer == NULL)
        return malloc(size);
    if (size == 0) {
        free(pointer);
        return NULL;
    }

    old_allocation = ((struct cephyr_allocation *)pointer) - 1;
    new_pointer = malloc(size);
    if (new_pointer == NULL)
        return NULL;

    copy_size = old_allocation->requested_size;
    if (copy_size > size)
        copy_size = size;
    memcpy(new_pointer, pointer, copy_size);

    free(pointer);
    return new_pointer;
}
