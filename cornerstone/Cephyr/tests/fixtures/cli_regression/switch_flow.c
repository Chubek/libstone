static int dispatch(unsigned char key) {
    int result = 0;
    switch (key) {
    case -1:
        return 100;
    case 1:
        result = 3;
    case 2:
        return result + 5;
    default:
        return 9;
    }
}

static int loops(int limit) {
    int result = 0;
    for (int i = 0; i < limit; ++i) {
        switch (i) {
        case 0:
            continue;
        case 1:
            switch (limit) {
            case 4:
                result += 10;
                break;
            }
            break;
        case 2:
            if (0) {
            case 3:
                result += 100;
            }
            result += 1;
            break;
        }
        result += 2;
    }
    return result;
}

int main(void) {
    return !(dispatch(1) == 8 && dispatch(2) == 5 && dispatch(255) == 9 &&
             loops(4) == 118);
}
