#include "../runtime/PlaidRuntime.h"
#include <assert.h>
static void increment(void* value){ ++*(int*)value; }
int main(void){
    plaid_runtime* runtime=0;size_t steps=99;int counter=0;
    assert(plaid_runtime_create(&runtime)==PLAID_OK);
    assert(plaid_runtime_post(runtime,increment,&counter)==PLAID_OK);
    assert(plaid_runtime_post(runtime,increment,&counter)==PLAID_OK);
    assert(plaid_runtime_run(runtime,1,&steps)==PLAID_BUDGET_EXHAUSTED);
    assert(steps==1 && counter==1 && plaid_runtime_pending(runtime)==1);
    assert(plaid_runtime_run(runtime,5,&steps)==PLAID_OK);
    assert(steps==1 && counter==2);
    plaid_runtime_destroy(runtime);
}
