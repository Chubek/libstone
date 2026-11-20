static int initializer_calls;
typedef int complete;

static int initialize(void) {
    ++initializer_calls;
    return -42;
}

static int enter_body(int choose) {
    int sum = 0;
    if (choose)
        goto inside;
    goto first;
    if (0) {
        int value = initialize();
inside:
        value = 3;
        sum += value;
    }
first:
    if (sum == 0)
        goto again;
    return sum;
again:
    sum = 5;
    goto first;
}

static int enter_for(void) {
    int result = 0;
    goto start;
    for (int i = initialize(); i < 4; ++i) {
        if (0) {
start:
            i = 1;
            continue;
        }
        result += i;
    }
    return result;
}

static int enter_switch(int input) {
    int result = 0;
    if (input)
        goto shared;
    switch (0) {
    case 0:
shared:
        result = 9;
        break;
    default:
        result = -1;
    }
    return result;
}

static int two_entries(int entry) {
    int x = 0;
    if (entry)
        goto right;
left:
    x += 3;
    if (x < 7)
        goto right;
    return x;
right:
    x += 5;
    if (x < 7)
        goto left;
    return x;
}

static int unused_loop(void) {
again:
    goto again;
}

int main(void) {
    int again = 0;
    goto again;
    return 1;
again:
    if (enter_body(1) != 3 || enter_body(0) != 5) return 2;
    if (enter_for() != 5 || initializer_calls) return 3;
    if (enter_switch(0) != 9 || enter_switch(1) != 9) return 4;
    if (two_entries(0) != 8 || two_entries(1) != 8) return 5;
    goto complete;
complete:
    return again;
}
