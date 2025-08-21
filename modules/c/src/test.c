#include "fip.h"

#include <stdio.h>

int FIP_FN foo() {
    return 0;
}

int FIP_FN bar(int x, int y) {
    return x + y;
}

int main() {
    printf("foo() = %i\n", foo());
    printf("bar(1, 2) = %i\n", bar(1, 2));
    return 0;
}
