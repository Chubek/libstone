#include <pthread.h>

static int work(int n) { return n ? work(n - 1) + 1 : 0; }
static void *worker(void *argument) {
    int *value = (int *)argument;
    *value = work(*value);
    return NULL;
}
int main(void) {
    pthread_t threads[4];
    int values[4] = {4, 4, 4, 4};
    for (int i = 0; i < 4; ++i)
        if (pthread_create(&threads[i], NULL, worker, &values[i]) != 0)
            return 1;
    for (int i = 0; i < 4; ++i)
        if (pthread_join(threads[i], NULL) != 0 || values[i] != 4)
            return 2;
    return work(4) == 4 ? 0 : 3;
}
