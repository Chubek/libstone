#include "Scheduler.h"

#include <stdio.h>


int main(void) {
    char diagnostic[256];
    enum archon_scheduler_status status;

    status = archon_scheduler_emit_embedded(
        stdout, NULL, diagnostic, sizeof diagnostic);
    if (status != ARCHON_SCHEDULER_OK) {
        (void)fprintf(
            stderr,
            "scheduler-demo: %s: %s\n",
            archon_scheduler_status_name(status),
            diagnostic);
        return 1;
    }
    return 0;
}
