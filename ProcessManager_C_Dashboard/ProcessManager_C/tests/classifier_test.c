#include <stdio.h>
#include <wchar.h>
#include "classifier.h"

int main(void) {
    wchar_t result[32];
    classify_process(0.5, 20.0, result, 32);
    if (wcscmp(result, L"Idle") != 0) return 1;
    classify_process(35.0, 10.0, result, 32);
    if (wcscmp(result, L"CPU-Bound") != 0) return 2;
    classify_process(5.0, 1500.0, result, 32);
    if (wcscmp(result, L"I/O-Bound") != 0) return 3;
    classify_process(5.0, 200.0, result, 32);
    if (wcscmp(result, L"Balanced") != 0) return 4;
    classify_process(-1.0, 0.0, result, 32);
    if (wcscmp(result, L"Warming Up") != 0) return 5;
    puts("All classifier tests passed.");
    return 0;
}
