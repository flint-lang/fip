#include <stdio.h>

int foo() {
    return 0;
}

int bar(int x, int y) {
    return x + y;
}

int main() {
    printf("foo() = %i\n", foo());
    printf("bar(1, 2) = %i\n", bar(1, 2));
    return 0;
}
