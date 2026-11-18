#ifndef PROCESS_MONITOR_H
#define PROCESS_MONITOR_H
#include <windows.h>
#include <stdint.h>
#define MAX_PROCESSES 2048
#define NAME_LEN 260
#define HISTORY_LEN 5

typedef enum { CLASS_WARMING=0, CLASS_IDLE, CLASS_CPU, CLASS_IO, CLASS_BALANCED } ProcessClass;
typedef struct {
    DWORD pid;
    char name[NAME_LEN];
    double cpu_percent;
    double memory_mb;
    double read_kbps;
    double write_kbps;
    ProcessClass classification;
    unsigned samples;
    ULONGLONG creation_time;
    ULONGLONG prev_cpu_100ns;
    ULONGLONG prev_read;
    ULONGLONG prev_write;
    ULONGLONG prev_tick;
    double cpu_hist[HISTORY_LEN];
    double io_hist[HISTORY_LEN];
    unsigned hist_count;
    unsigned hist_pos;
} ProcessInfo;

typedef struct {
    ProcessInfo items[MAX_PROCESSES];
    size_t count;
    double total_cpu;
    double total_memory_mb;
    double available_memory_mb;
    DWORD last_error;
} MonitorSnapshot;

void monitor_collect(MonitorSnapshot *out);
const char *classification_name(ProcessClass c);
#endif
