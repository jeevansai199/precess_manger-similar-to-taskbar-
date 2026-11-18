#include "system_info.h"

static ULONGLONG ft_to_u64(FILETIME ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

void collect_system_info(double *cpu_percent, double *memory_used_percent) {
    static ULONGLONG previous_idle = 0, previous_kernel = 0, previous_user = 0;
    FILETIME idle_ft, kernel_ft, user_ft;
    MEMORYSTATUSEX mem;
    if (cpu_percent) *cpu_percent = 0.0;
    if (memory_used_percent) *memory_used_percent = 0.0;

    if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        ULONGLONG idle = ft_to_u64(idle_ft);
        ULONGLONG kernel = ft_to_u64(kernel_ft);
        ULONGLONG user = ft_to_u64(user_ft);
        if (previous_kernel || previous_user) {
            ULONGLONG total = (kernel - previous_kernel) + (user - previous_user);
            ULONGLONG idle_delta = idle - previous_idle;
            if (total > 0 && cpu_percent) {
                double value = 100.0 * (double)(total - idle_delta) / (double)total;
                *cpu_percent = value < 0 ? 0 : (value > 100 ? 100 : value);
            }
        }
        previous_idle = idle; previous_kernel = kernel; previous_user = user;
    }
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem) && memory_used_percent)
        *memory_used_percent = (double)mem.dwMemoryLoad;
}
