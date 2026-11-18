
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "process_monitor.h"
#include "classifier.h"

/* Convert FILETIME into a 64-bit integer */
static ULONGLONG filetime_value(FILETIME ft)
{
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

/* Find the previous sample for the same process instance */
static ProcessInfo* find_previous(
    MonitorSnapshot* old,
    DWORD pid,
    ULONGLONG created)
{
    size_t i;

    for (i = 0; i < old->count; ++i)
    {
        if (old->items[i].pid == pid &&
            old->items[i].creation_time == created)
        {
            return &old->items[i];
        }
    }

    return NULL;
}

/* Previous monitoring snapshot */
static MonitorSnapshot previous_snapshot;
static int have_previous_snapshot = 0;

/* Set a fallback process name */
static void set_fallback_process_name(ProcessInfo* p)
{
    if (p->pid == 0)
    {
        strncpy_s(
            p->name,
            sizeof(p->name),
            "System Idle Process",
            _TRUNCATE
        );
    }
    else if (p->pid == 4)
    {
        strncpy_s(
            p->name,
            sizeof(p->name),
            "System",
            _TRUNCATE
        );
    }
    else
    {
        snprintf(
            p->name,
            sizeof(p->name),
            "PID %lu",
            (unsigned long)p->pid
        );
    }
}

/* Retrieve the executable process name */
static void read_process_name(
    HANDLE process,
    ProcessInfo* p)
{
    char image_path[MAX_PATH] = { 0 };
    DWORD path_size = MAX_PATH;
    const char* base_name;

    if (!QueryFullProcessImageNameA(
        process,
        0,
        image_path,
        &path_size))
    {
        return;
    }

    base_name = strrchr(image_path, '\\');

    if (base_name != NULL)
        base_name++;
    else
        base_name = image_path;

    if (base_name[0] != '\0')
    {
        strncpy_s(
            p->name,
            sizeof(p->name),
            base_name,
            _TRUNCATE
        );
    }
}

/* Main process monitoring function */
void monitor_collect(MonitorSnapshot* out)
{
    DWORD process_ids[MAX_PROCESSES];
    DWORD bytes_needed = 0;
    DWORD process_count;
    DWORD i;

    ULONGLONG now;

    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));

    /* =====================================
       1. SYSTEM MEMORY INFORMATION
       ===================================== */

    {
        MEMORYSTATUSEX mem;

        memset(&mem, 0, sizeof(mem));
        mem.dwLength = sizeof(mem);

        if (GlobalMemoryStatusEx(&mem))
        {
            out->total_memory_mb =
                (double)mem.ullTotalPhys / 1048576.0;

            out->available_memory_mb =
                (double)mem.ullAvailPhys / 1048576.0;
        }
    }

    /* =====================================
       2. TOTAL SYSTEM CPU UTILIZATION
       ===================================== */

    {
        static ULONGLONG prev_kernel = 0;
        static ULONGLONG prev_user = 0;
        static ULONGLONG prev_idle = 0;

        FILETIME idle_ft;
        FILETIME kernel_ft;
        FILETIME user_ft;

        if (GetSystemTimes(
            &idle_ft,
            &kernel_ft,
            &user_ft))
        {
            ULONGLONG idle =
                filetime_value(idle_ft);

            ULONGLONG kernel =
                filetime_value(kernel_ft);

            ULONGLONG user =
                filetime_value(user_ft);

            if (prev_kernel != 0)
            {
                ULONGLONG total =
                    (kernel - prev_kernel) +
                    (user - prev_user);

                ULONGLONG idle_delta =
                    idle - prev_idle;

                if (total > 0)
                {
                    ULONGLONG busy =
                        total > idle_delta
                        ? total - idle_delta
                        : 0;

                    out->total_cpu =
                        100.0 * (double)busy /
                        (double)total;
                }
            }

            prev_kernel = kernel;
            prev_user = user;
            prev_idle = idle;
        }
    }

    /* =====================================
       3. ENUMERATE RUNNING PROCESSES
       ===================================== */

    if (!EnumProcesses(
        process_ids,
        sizeof(process_ids),
        &bytes_needed))
    {
        out->last_error = GetLastError();
        return;
    }

    process_count =
        bytes_needed / sizeof(DWORD);

    if (process_count > MAX_PROCESSES)
        process_count = MAX_PROCESSES;

    now = GetTickCount64();

    /* =====================================
       4. COLLECT INDIVIDUAL PROCESS DATA
       ===================================== */

    for (i = 0;
        i < process_count && out->count < MAX_PROCESSES;
        ++i)
    {
        DWORD pid = process_ids[i];

        ProcessInfo p;

        HANDLE query_handle = NULL;
        HANDLE io_handle = NULL;
        HANDLE memory_handle = NULL;

        FILETIME creation;
        FILETIME exit_time;
        FILETIME kernel_time;
        FILETIME user_time;

        ULONGLONG cpu_now = 0;

        IO_COUNTERS io;
        BOOL have_io = FALSE;

        ProcessInfo* old = NULL;

        memset(&p, 0, sizeof(p));
        memset(&io, 0, sizeof(io));

        p.pid = pid;

        set_fallback_process_name(&p);

        /* ---------------------------------
           A. Open basic process query handle
           --------------------------------- */

        query_handle = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            pid
        );

        if (query_handle != NULL)
        {
            /* Retrieve process executable name */
            read_process_name(query_handle, &p);

            /* ---------------------------------
               B. Retrieve CPU time information
               --------------------------------- */

            if (GetProcessTimes(
                query_handle,
                &creation,
                &exit_time,
                &kernel_time,
                &user_time))
            {
                p.creation_time =
                    filetime_value(creation);

                cpu_now =
                    filetime_value(kernel_time) +
                    filetime_value(user_time);

                /* Find previous process sample */
                if (have_previous_snapshot)
                {
                    old = find_previous(
                        &previous_snapshot,
                        pid,
                        p.creation_time
                    );
                }

                /* ---------------------------------
                   C. Open separate I/O handle
                   --------------------------------- */

                io_handle = OpenProcess(
                    PROCESS_QUERY_INFORMATION,
                    FALSE,
                    pid
                );

                if (io_handle != NULL)
                {
                    if (GetProcessIoCounters(
                        io_handle,
                        &io))
                    {
                        have_io = TRUE;
                    }
                }

                /* ---------------------------------
                   D. Open separate memory handle
                   --------------------------------- */

                memory_handle = OpenProcess(
                    PROCESS_QUERY_INFORMATION |
                    PROCESS_VM_READ,
                    FALSE,
                    pid
                );

                if (memory_handle != NULL)
                {
                    PROCESS_MEMORY_COUNTERS_EX pmc;

                    memset(&pmc, 0, sizeof(pmc));
                    pmc.cb = sizeof(pmc);

                    if (GetProcessMemoryInfo(
                        memory_handle,
                        (PROCESS_MEMORY_COUNTERS*)&pmc,
                        sizeof(pmc)))
                    {
                        p.memory_mb =
                            (double)pmc.WorkingSetSize /
                            1048576.0;
                    }
                }

                /* ---------------------------------
                   E. Calculate CPU and I/O rates
                   --------------------------------- */

                if (old != NULL && now > old->prev_tick)
                {
                    double seconds =
                        (double)(now - old->prev_tick) /
                        1000.0;

                    ULONGLONG elapsed_100ns =
                        (now - old->prev_tick) * 10000ULL;

                    SYSTEM_INFO si;
                    DWORD cores;

                    GetSystemInfo(&si);

                    cores = si.dwNumberOfProcessors;

                    if (cores == 0)
                        cores = 1;

                    /* CPU percentage */
                    if (elapsed_100ns > 0 &&
                        cpu_now >= old->prev_cpu_100ns)
                    {
                        ULONGLONG cpu_delta =
                            cpu_now - old->prev_cpu_100ns;

                        p.cpu_percent =
                            100.0 *
                            (double)cpu_delta /
                            (double)elapsed_100ns /
                            (double)cores;
                    }

                    if (p.cpu_percent < 0.0)
                        p.cpu_percent = 0.0;

                    if (p.cpu_percent > 100.0)
                        p.cpu_percent = 100.0;

                    /* I/O throughput */
                    if (have_io &&
                        seconds > 0.0 &&
                        io.ReadTransferCount >= old->prev_read &&
                        io.WriteTransferCount >= old->prev_write)
                    {
                        p.read_kbps =
                            (double)(
                                io.ReadTransferCount -
                                old->prev_read
                                ) / 1024.0 / seconds;

                        p.write_kbps =
                            (double)(
                                io.WriteTransferCount -
                                old->prev_write
                                ) / 1024.0 / seconds;
                    }

                    /* Copy classification history */
                    p.samples = old->samples + 1;

                    p.hist_count = old->hist_count;
                    p.hist_pos = old->hist_pos;

                    memcpy(
                        p.cpu_hist,
                        old->cpu_hist,
                        sizeof(p.cpu_hist)
                    );

                    memcpy(
                        p.io_hist,
                        old->io_hist,
                        sizeof(p.io_hist)
                    );
                }
                else
                {
                    /* First monitoring sample */
                    p.samples = 1;
                    p.hist_count = 0;
                    p.hist_pos = 0;
                }

                /* Save current counters */
                p.prev_cpu_100ns = cpu_now;
                p.prev_tick = now;

                if (have_io)
                {
                    p.prev_read =
                        io.ReadTransferCount;

                    p.prev_write =
                        io.WriteTransferCount;
                }
                else if (old != NULL)
                {
                    p.prev_read = old->prev_read;
                    p.prev_write = old->prev_write;
                }
            }
        }

        /* =====================================
           5. PROCESS CLASSIFICATION
           ===================================== */

        if (p.creation_time == 0)
        {
            p.samples = 0;
            p.classification = CLASS_WARMING;
        }
        else
        {
            if (p.hist_count < HISTORY_LEN)
                p.hist_count++;

            p.cpu_hist[p.hist_pos] =
                p.cpu_percent;

            p.io_hist[p.hist_pos] =
                p.read_kbps + p.write_kbps;

            p.hist_pos =
                (p.hist_pos + 1) % HISTORY_LEN;

            p.classification =
                classify_process(&p);
        }

        /* =====================================
           6. CLOSE ALL PROCESS HANDLES
           ===================================== */

        if (memory_handle != NULL)
            CloseHandle(memory_handle);

        if (io_handle != NULL)
            CloseHandle(io_handle);

        if (query_handle != NULL)
            CloseHandle(query_handle);

        /* Add process to output snapshot */
        out->items[out->count++] = p;
    }

    /* =====================================
       7. SAVE SNAPSHOT FOR NEXT SAMPLE
       ===================================== */

    previous_snapshot = *out;
    have_previous_snapshot = 1;
}

/* Return readable classification name */
const char* classification_name(ProcessClass c)
{
    switch (c)
    {
    case CLASS_IDLE:
        return "Idle";

    case CLASS_CPU:
        return "CPU-Bound";

    case CLASS_IO:
        return "I/O-Bound";

    case CLASS_BALANCED:
        return "Balanced";

    default:
        return "Warming Up";
    }
}
