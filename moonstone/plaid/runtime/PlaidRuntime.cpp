#include "PlaidRuntime.h"
#include "../loom/Scheduler.hpp"
#include <new>
#include <stdexcept>
#include <utility>
struct plaid_runtime { moonstone::plaid::loom::CooperativeScheduler queue; };
extern "C" plaid_status plaid_runtime_create(plaid_runtime** out) {
    if (!out) return PLAID_INVALID;
    *out=nullptr;
    try { *out=new plaid_runtime; return PLAID_OK; }
    catch (...) { return PLAID_ALLOCATION_FAILED; }
}
extern "C" void plaid_runtime_destroy(plaid_runtime* p) { delete p; }
extern "C" plaid_status plaid_runtime_post(plaid_runtime* p, plaid_callback cb, void* data) {
    if (!p || !cb) return PLAID_INVALID;
    try { p->queue.post([=]{ cb(data); }); return PLAID_OK; }
    catch (...) { return PLAID_ALLOCATION_FAILED; }
}
extern "C" plaid_status plaid_runtime_run(plaid_runtime* p, size_t budget, size_t* executed) {
    if (!p || !executed) return PLAID_INVALID;
    *executed=0;
    try {
        *executed=p->queue.run(budget);
        return p->queue.pending() ? PLAID_BUDGET_EXHAUSTED : PLAID_OK;
    } catch (...) { return PLAID_CALLBACK_FAILED; }
}
extern "C" size_t plaid_runtime_pending(const plaid_runtime* p) {
    return p ? p->queue.pending() : 0;
}
