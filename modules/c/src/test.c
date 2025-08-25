#include <stddef.h>
#include <stdio.h>

typedef struct {
    float x;
    float y;
    float z;
} Vector3;

Vector3 add_vectors(const Vector3 v1, const Vector3 v2) {
    Vector3 result = v1;
    result.x += v2.x;
    result.y += v2.y;
    result.z += v2.z;
    return result;
}

int foo(const int x, const int y) {
    int *z = NULL;
    printf("z = %p\n", z);
    printf("*z = %i\n", *z);
    return x + y;
}

int bar() {
    return 69;
}
