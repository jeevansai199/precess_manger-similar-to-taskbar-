#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "gui.h"
#include "process_monitor.h"

#define ID_LIST       1001
#define ID_SEARCH     1002
#define ID_PAUSE      1003
#define ID_FILTER     1004
#define ID_SORT       1005
#define ID_DETAILS    1006
#define ID_CPU_CHART  1007
#define ID_MEM_CHART  1008
#define WM_MONITOR_UPDATE (WM_APP + 1)
#define HISTORY_POINTS 60
#define TIMER_REFRESH  200

typedef enum { SORT_PID = 0, SORT_NAME, SORT_CPU, SORT_MEMORY } SortMode;
typedef enum { CHART_CPU = 1, CHART_MEMORY = 2 } ChartKind;

static HWND g_list, g_search, g_status, g_pause, g_filter, g_sort, g_title;
static HWND g_details, g_cpu_chart, g_mem_chart;
static HANDLE g_thread = NULL, g_stop = NULL;
static volatile LONG g_paused = 0;
static MonitorSnapshot g_shared_data;
static MonitorSnapshot g_ui_data;
static CRITICAL_SECTION g_data_lock;
static BOOL g_lock_ready = FALSE;
static SortMode g_sort_mode = SORT_CPU;
static double g_cpu_history[HISTORY_POINTS];
static double g_mem_history[HISTORY_POINTS];
static unsigned g_history_count = 0;
static unsigned g_history_next = 0;
static const char* g_headers[] = { "PID", "Process", "CPU %", "Memory MB", "Read KB/s", "Write KB/s", "Class" };
static HFONT g_font = NULL, g_title_font = NULL;
static HBRUSH g_navy_brush = NULL;
static HBRUSH g_pale_blue_brush = NULL;
static HBRUSH g_edit_brush = NULL;
static COLORREF COLOR_NAVY = RGB(23, 54, 93);
static COLORREF COLOR_BLUE = RGB(40, 120, 208);
static COLORREF COLOR_PALE_BLUE = RGB(234, 242, 251);
static COLORREF COLOR_TEXT = RGB(31, 41, 55);
static COLORREF COLOR_MUTED = RGB(100, 116, 139);

static void push_history(double cpu, double memory_percent)
{
    g_cpu_history[g_history_next] = cpu;
    g_mem_history[g_history_next] = memory_percent;
    g_history_next = (g_history_next + 1) % HISTORY_POINTS;
    if (g_history_count < HISTORY_POINTS) ++g_history_count;
}

static int process_matches_filter(const ProcessInfo* p, int filter)
{
    switch (filter)
    {
    case 1: return p->classification == CLASS_IDLE;
    case 2: return p->classification == CLASS_CPU;
    case 3: return p->classification == CLASS_IO;
    case 4: return p->classification == CLASS_BALANCED;
    case 5: return p->classification == CLASS_WARMING;
    default: return 1;
    }
}

static int compare_process_indices(size_t a, size_t b)
{
    const ProcessInfo* pa = &g_ui_data.items[a];
    const ProcessInfo* pb = &g_ui_data.items[b];
    switch (g_sort_mode)
    {
    case SORT_NAME: return _stricmp(pa->name, pb->name);
    case SORT_CPU:
        if (pa->cpu_percent < pb->cpu_percent) return 1;
        if (pa->cpu_percent > pb->cpu_percent) return -1;
        return 0;
    case SORT_MEMORY:
        if (pa->memory_mb < pb->memory_mb) return 1;
        if (pa->memory_mb > pb->memory_mb) return -1;
        return 0;
    default:
        if (pa->pid < pb->pid) return -1;
        if (pa->pid > pb->pid) return 1;
        return 0;
    }
}

static void set_list_columns(void)
{
    int widths[] = { 72, 205, 72, 92, 92, 92, 105 };
    int i;
    for (i = 0; i < 7; ++i)
    {
        LVCOLUMNA col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = (LPSTR)g_headers[i];
        col.cx = widths[i];
        col.iSubItem = i;
        ListView_InsertColumn(g_list, i, &col);
    }
}

static void update_details_from_selection(void)
{
    int selected = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    char text[512];
    if (selected < 0)
    {
        SetWindowTextA(g_details,
            "Select a process in the table to view its details.\r\n\r\n"
            "Metrics may be unavailable for protected Windows processes.");
        return;
    }
    else
    {
        LVITEMA item;
        LPARAM index;
        ProcessInfo* p;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_PARAM;
        item.iItem = selected;
        if (!ListView_GetItem(g_list, &item)) return;
        index = item.lParam;
        if (index < 0 || (size_t)index >= g_ui_data.count) return;
        p = &g_ui_data.items[(size_t)index];
        snprintf(text, sizeof(text),
            "Process: %s\r\nPID: %lu\r\nCPU usage: %.2f%%\r\n"
            "Memory: %.2f MB\r\nRead rate: %.2f KB/s\r\n"
            "Write rate: %.2f KB/s\r\nClassification: %s\r\n"
            "Valid samples: %u\r\n\r\n"
            "Classification is based on the project's rolling-window heuristic.",
            p->name, (unsigned long)p->pid, p->cpu_percent, p->memory_mb,
            p->read_kbps, p->write_kbps,
            classification_name(p->classification), p->samples);
        SetWindowTextA(g_details, text);
    }
}

static void refresh_list(void)
{
    char query[NAME_LEN] = { 0 };
    int filter = (int)SendMessage(g_filter, CB_GETCURSEL, 0, 0);
    size_t indices[MAX_PROCESSES];
    size_t count = 0, i, j;
    char status[256];
    double used_memory = g_ui_data.total_memory_mb - g_ui_data.available_memory_mb;

    GetWindowTextA(g_search, query, sizeof(query));
    if (filter < 0) filter = 0;

    for (i = 0; i < g_ui_data.count; ++i)
    {
        ProcessInfo* p = &g_ui_data.items[i];
        if (query[0] && strstr(p->name, query) == NULL) continue;
        if (!process_matches_filter(p, filter)) continue;
        indices[count++] = i;
    }

    /* Insertion sort is sufficient for the few hundred processes typically shown. */
    for (i = 1; i < count; ++i)
    {
        size_t key = indices[i];
        j = i;
        while (j > 0 && compare_process_indices(indices[j - 1], key) > 0)
        {
            indices[j] = indices[j - 1];
            --j;
        }
        indices[j] = key;
    }

    ListView_DeleteAllItems(g_list);
    for (i = 0; i < count; ++i)
    {
        ProcessInfo* p = &g_ui_data.items[indices[i]];
        char cell[128];
        LVITEMA item;
        int row;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.lParam = (LPARAM)indices[i];
        snprintf(cell, sizeof(cell), "%lu", (unsigned long)p->pid);
        item.pszText = cell;
        row = ListView_InsertItem(g_list, &item);
        if (row < 0) continue;
        ListView_SetItemText(g_list, row, 1, p->name);
        snprintf(cell, sizeof(cell), "%.1f", p->cpu_percent);
        ListView_SetItemText(g_list, row, 2, cell);
        snprintf(cell, sizeof(cell), "%.1f", p->memory_mb);
        ListView_SetItemText(g_list, row, 3, cell);
        snprintf(cell, sizeof(cell), "%.1f", p->read_kbps);
        ListView_SetItemText(g_list, row, 4, cell);
        snprintf(cell, sizeof(cell), "%.1f", p->write_kbps);
        ListView_SetItemText(g_list, row, 5, cell);
        ListView_SetItemText(g_list, row, 6, (LPSTR)classification_name(p->classification));
    }

    snprintf(status, sizeof(status),
        "PROCESSES  %zu     SYSTEM CPU  %.1f%%     MEMORY  %.0f / %.0f MB     SHOWING  %zu",
        g_ui_data.count, g_ui_data.total_cpu, used_memory,
        g_ui_data.total_memory_mb, count);
    SetWindowTextA(g_status, status);
    update_details_from_selection();
}

static void draw_chart(HWND hwnd, HDC dc, ChartKind kind)
{
    RECT r;
    HPEN grid_pen, line_pen, old_pen;
    HBRUSH bg;
    int left = 38, right, top = 28, bottom;
    unsigned n, k;
    double max_value = 100.0;
    double* values = kind == CHART_CPU ? g_cpu_history : g_mem_history;
    char label[80];
    GetClientRect(hwnd, &r);
    right = r.right - 12;
    bottom = r.bottom - 24;
    bg = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COLOR_NAVY);
    {
        const char* chart_title = kind == CHART_CPU ? "CPU HISTORY (%)" : "MEMORY HISTORY (%)";
        TextOutA(dc, 12, 7, chart_title, (int)strlen(chart_title));
    }
    if (kind == CHART_MEMORY)
    {
        double current = g_ui_data.total_memory_mb > 0.0
            ? 100.0 * (g_ui_data.total_memory_mb - g_ui_data.available_memory_mb) / g_ui_data.total_memory_mb : 0.0;
        (void)current;
    }
    grid_pen = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
    old_pen = (HPEN)SelectObject(dc, grid_pen);
    for (k = 0; k <= 4; ++k)
    {
        int y = top + (bottom - top) * (int)k / 4;
        MoveToEx(dc, left, y, NULL);
        LineTo(dc, right, y);
    }
    SelectObject(dc, old_pen);
    DeleteObject(grid_pen);
    if (g_history_count > 1)
    {
        POINT pts[HISTORY_POINTS];
        n = g_history_count;
        for (k = 0; k < n; ++k)
        {
            unsigned idx = (g_history_next + HISTORY_POINTS - n + k) % HISTORY_POINTS;
            double v = values[idx];
            if (v < 0.0) v = 0.0;
            if (v > max_value) v = max_value;
            pts[k].x = left + (right - left) * (int)k / (int)(n - 1);
            pts[k].y = bottom - (int)((bottom - top) * v / max_value);
        }
        line_pen = CreatePen(PS_SOLID, 3, kind == CHART_CPU ? COLOR_BLUE : RGB(16, 150, 120));
        old_pen = (HPEN)SelectObject(dc, line_pen);
        Polyline(dc, pts, (int)n);
        SelectObject(dc, old_pen);
        DeleteObject(line_pen);
    }
    snprintf(label, sizeof(label), "0%%                                      100%%");
    SetTextColor(dc, COLOR_MUTED);
    TextOutA(dc, left, bottom + 5, label, (int)strlen(label));
}

static LRESULT CALLBACK chart_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ChartKind kind = (ChartKind)(LONG_PTR)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        draw_chart(hwnd, dc, kind);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static unsigned __stdcall monitor_worker(void* arg)
{
    HWND hwnd = (HWND)arg;
    while (WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT)
    {
        if (!InterlockedCompareExchange(&g_paused, 0, 0))
        {
            MonitorSnapshot sample;
            monitor_collect(&sample);
            if (g_lock_ready)
            {
                EnterCriticalSection(&g_data_lock);
                g_shared_data = sample;
                LeaveCriticalSection(&g_data_lock);
            }
            PostMessage(hwnd, WM_MONITOR_UPDATE, 0, 0);
        }
        if (WaitForSingleObject(g_stop, 2000) == WAIT_OBJECT_0) break;
    }
    return 0;
}

static void start_worker(HWND hwnd)
{
    uintptr_t thread_value;
    g_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_stop) return;
    thread_value = _beginthreadex(NULL, 0, monitor_worker, hwnd, 0, NULL);
    if (thread_value) g_thread = (HANDLE)thread_value;
}

static void stop_worker(void)
{
    if (g_stop) SetEvent(g_stop);
    if (g_thread)
    {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stop)
    {
        CloseHandle(g_stop);
        g_stop = NULL;
    }
}

static void layout_controls(HWND hwnd)
{
    RECT r;
    int width, height, content_y = 150, bottom_h = 185;
    int right_w, left_w, list_h;
    GetClientRect(hwnd, &r);
    width = r.right;
    height = r.bottom;
    right_w = width > 1050 ? 330 : 285;
    left_w = width - right_w - 50;
    if (left_w < 400) left_w = 400;
    list_h = height - content_y - bottom_h - 45;
    if (list_h < 170) list_h = 170;
    MoveWindow(g_status, 20, 105, width - 40, 30, TRUE);
    MoveWindow(g_list, 20, content_y, left_w, list_h, TRUE);
    MoveWindow(g_details, left_w + 32, content_y, right_w, 145, TRUE);
    MoveWindow(g_cpu_chart, 20, content_y + list_h + 12, (width - 60) / 2, bottom_h, TRUE);
    MoveWindow(g_mem_chart, 40 + (width - 60) / 2, content_y + list_h + 12, (width - 60) / 2, bottom_h, TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT font;
        HINSTANCE instance = ((LPCREATESTRUCT)lp)->hInstance;
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_font = font;
        g_navy_brush = CreateSolidBrush(COLOR_NAVY);
        g_pale_blue_brush = CreateSolidBrush(COLOR_PALE_BLUE);
        g_edit_brush = CreateSolidBrush(RGB(255, 255, 255));
        g_title_font = CreateFontA(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        InitializeCriticalSection(&g_data_lock);
        g_lock_ready = TRUE;

        g_title = CreateWindowA("STATIC", "PROCESS MANAGER  /  REAL-TIME WINDOWS MONITOR",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 20, 16, 720, 34, hwnd, NULL, instance, NULL);
        g_search = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 58, 235, 27,
            hwnd, (HMENU)ID_SEARCH, instance, NULL);
        CreateWindowA("STATIC", "Search", WS_CHILD | WS_VISIBLE,
            263, 63, 48, 20, hwnd, NULL, instance, NULL);
        g_filter = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            325, 58, 145, 180, hwnd, (HMENU)ID_FILTER, instance, NULL);
        SendMessageA(g_filter, CB_ADDSTRING, 0, (LPARAM)"All classifications");
        SendMessageA(g_filter, CB_ADDSTRING, 0, (LPARAM)"Idle");
        SendMessageA(g_filter, CB_ADDSTRING, 0, (LPARAM)"CPU-Bound");
        SendMessageA(g_filter, CB_ADDSTRING, 0, (LPARAM)"I/O-Bound");
        SendMessageA(g_filter, CB_ADDSTRING, 0, (LPARAM)"Balanced");
        SendMessageA(g_filter, CB_ADDSTRING, 0, (LPARAM)"Warming Up");
        SendMessageA(g_filter, CB_SETCURSEL, 0, 0);
        g_sort = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            485, 58, 145, 180, hwnd, (HMENU)ID_SORT, instance, NULL);
        SendMessageA(g_sort, CB_ADDSTRING, 0, (LPARAM)"Sort: CPU highest");
        SendMessageA(g_sort, CB_ADDSTRING, 0, (LPARAM)"Sort: Memory highest");
        SendMessageA(g_sort, CB_ADDSTRING, 0, (LPARAM)"Sort: Name A-Z");
        SendMessageA(g_sort, CB_ADDSTRING, 0, (LPARAM)"Sort: PID");
        SendMessageA(g_sort, CB_SETCURSEL, 0, 0);
        g_pause = CreateWindowA("BUTTON", "Pause Monitoring",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 660, 56, 155, 31,
            hwnd, (HMENU)ID_PAUSE, instance, NULL);
        g_status = CreateWindowA("STATIC", "Collecting process information...",
            WS_CHILD | WS_VISIBLE, 20, 105, 850, 30, hwnd, NULL, instance, NULL);
        g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            20, 150, 650, 400, hwnd, (HMENU)ID_LIST, instance, NULL);
        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        set_list_columns();
        g_details = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
            "Select a process to view details.",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            700, 150, 280, 145, hwnd, (HMENU)ID_DETAILS, instance, NULL);
        g_cpu_chart = CreateWindowExA(WS_EX_CLIENTEDGE, "PMChart", "",
            WS_CHILD | WS_VISIBLE, 20, 570, 400, 170, hwnd,
            (HMENU)ID_CPU_CHART, instance, (LPVOID)(INT_PTR)CHART_CPU);
        g_mem_chart = CreateWindowExA(WS_EX_CLIENTEDGE, "PMChart", "",
            WS_CHILD | WS_VISIBLE, 440, 570, 400, 170, hwnd,
            (HMENU)ID_MEM_CHART, instance, (LPVOID)(INT_PTR)CHART_MEMORY);
        SendMessage(g_search, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessage(g_filter, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessage(g_sort, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessage(g_pause, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessage(g_status, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessage(g_details, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessage(g_list, WM_SETFONT, (WPARAM)font, TRUE);
        if (g_title_font && g_title) SendMessage(g_title, WM_SETFONT, (WPARAM)g_title_font, TRUE);
        start_worker(hwnd);
        return 0;
    }
    case WM_MONITOR_UPDATE:
        if (g_lock_ready)
        {
            EnterCriticalSection(&g_data_lock);
            g_ui_data = g_shared_data;
            LeaveCriticalSection(&g_data_lock);
        }
        {
            double mem_percent = g_ui_data.total_memory_mb > 0.0
                ? 100.0 * (g_ui_data.total_memory_mb - g_ui_data.available_memory_mb) / g_ui_data.total_memory_mb
                : 0.0;
            push_history(g_ui_data.total_cpu, mem_percent);
        }
        refresh_list();
        InvalidateRect(g_cpu_chart, NULL, TRUE);
        InvalidateRect(g_mem_chart, NULL, TRUE);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_PAUSE && HIWORD(wp) == BN_CLICKED)
        {
            LONG paused = InterlockedXor(&g_paused, 1);
            SetWindowTextA(g_pause, paused ? "Resume Monitoring" : "Pause Monitoring");
        }
        else if ((LOWORD(wp) == ID_SEARCH && HIWORD(wp) == EN_CHANGE) ||
            (LOWORD(wp) == ID_FILTER && HIWORD(wp) == CBN_SELCHANGE))
        {
            refresh_list();
        }
        else if (LOWORD(wp) == ID_SORT && HIWORD(wp) == CBN_SELCHANGE)
        {
            int choice = (int)SendMessage(g_sort, CB_GETCURSEL, 0, 0);
            g_sort_mode = choice == 1 ? SORT_MEMORY : choice == 2 ? SORT_NAME : choice == 3 ? SORT_PID : SORT_CPU;
            refresh_list();
        }
        return 0;

    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)wp;
        HWND control = (HWND)lp;
        if (control == g_title)
        {
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkColor(dc, COLOR_NAVY);
            return (LRESULT)g_navy_brush;
        }
        if (control == g_status)
        {
            SetTextColor(dc, COLOR_NAVY);
            SetBkColor(dc, COLOR_PALE_BLUE);
            return (LRESULT)g_pale_blue_brush;
        }
        SetTextColor(dc, COLOR_TEXT);
        SetBkColor(dc, RGB(255, 255, 255));
        return (LRESULT)g_edit_brush;
    }

    case WM_CTLCOLOREDIT:
    {
        HDC dc = (HDC)wp;
        SetTextColor(dc, COLOR_TEXT);
        SetBkColor(dc, RGB(255, 255, 255));
        return (LRESULT)g_edit_brush;
    }

    case WM_NOTIFY:
        if (((LPNMHDR)lp)->idFrom == ID_LIST &&
            ((LPNMHDR)lp)->code == LVN_ITEMCHANGED)
        {
            update_details_from_selection();
            return 0;
        }
        if (((LPNMHDR)lp)->idFrom == ID_LIST &&
            ((LPNMHDR)lp)->code == NM_CUSTOMDRAW)
        {
            NMLVCUSTOMDRAW* draw = (NMLVCUSTOMDRAW*)lp;
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                return CDRF_NOTIFYSUBITEMDRAW;
            if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
            {
                int row = (int)draw->nmcd.dwItemSpec;
                if (draw->iSubItem == 6)
                {
                    char class_text[64] = { 0 };
                    ListView_GetItemText(g_list, row, 6, class_text, (int)sizeof(class_text));
                    if (strcmp(class_text, "CPU-Bound") == 0)
                        draw->clrText = RGB(190, 45, 45);
                    else if (strcmp(class_text, "I/O-Bound") == 0)
                        draw->clrText = RGB(37, 99, 190);
                    else if (strcmp(class_text, "Idle") == 0)
                        draw->clrText = RGB(22, 128, 75);
                    else if (strcmp(class_text, "Balanced") == 0)
                        draw->clrText = RGB(180, 105, 20);
                    else
                        draw->clrText = COLOR_MUTED;
                    draw->clrTextBk = (row % 2) ? RGB(247, 250, 253) : RGB(255, 255, 255);
                }
                else
                {
                    draw->clrText = COLOR_TEXT;
                    draw->clrTextBk = (row % 2) ? RGB(247, 250, 253) : RGB(255, 255, 255);
                }
                return CDRF_NEWFONT;
            }
        }
        return 0;

    case WM_SIZE:
        layout_controls(hwnd);
        return 0;

    case WM_DESTROY:
        stop_worker();
        if (g_lock_ready)
        {
            DeleteCriticalSection(&g_data_lock);
            g_lock_ready = FALSE;
        }
        if (g_title_font) DeleteObject(g_title_font);
        if (g_navy_brush) DeleteObject(g_navy_brush);
        if (g_pale_blue_brush) DeleteObject(g_pale_blue_brush);
        if (g_edit_brush) DeleteObject(g_edit_brush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int gui_run(HINSTANCE instance, int show_cmd)
{
    INITCOMMONCONTROLSEX controls;
    WNDCLASSEXA chart_class;
    WNDCLASSEXA window_class;
    HWND hwnd;
    MSG msg;

    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);

    memset(&chart_class, 0, sizeof(chart_class));
    chart_class.cbSize = sizeof(chart_class);
    chart_class.lpfnWndProc = chart_proc;
    chart_class.hInstance = instance;
    chart_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    chart_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    chart_class.lpszClassName = "PMChart";
    RegisterClassExA(&chart_class);

    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = wndproc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = "ProcessManagerCWindow";
    window_class.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassExA(&window_class)) return 1;

    hwnd = CreateWindowExA(0, window_class.lpszClassName,
        "Process Manager - Operating Systems Project",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1180, 850, NULL, NULL, instance, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}