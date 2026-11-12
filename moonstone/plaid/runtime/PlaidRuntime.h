#ifndef MOONSTONE_PLAID_RUNTIME_H
#define MOONSTONE_PLAID_RUNTIME_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PLAID_ABI_VERSION 1
typedef struct plaid_runtime plaid_runtime;
typedef void (*plaid_callback)(void* user_data);
typedef enum plaid_status {
    PLAID_OK=0, PLAID_INVALID=1, PLAID_ALLOCATION_FAILED=2,
    PLAID_CALLBACK_FAILED=3, PLAID_BUDGET_EXHAUSTED=4
} plaid_status;
plaid_status plaid_runtime_create(plaid_runtime** out);
void plaid_runtime_destroy(plaid_runtime* runtime);
plaid_status plaid_runtime_post(plaid_runtime* runtime, plaid_callback callback, void* user_data);
plaid_status plaid_runtime_run(plaid_runtime* runtime, size_t budget, size_t* executed);
size_t plaid_runtime_pending(const plaid_runtime* runtime);
#ifdef __cplusplus
}
#endif
#endif
