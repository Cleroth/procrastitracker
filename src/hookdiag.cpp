#include "stdafx.h"
#include "..\procrastitracker\resource.h"
#include "IdleTracker.h"
#include "hookdiag.h"

#include "slaballoc.h"
extern SlabAlloc pool;
#include "tools.h"

#ifdef __MINGW32__
#define sprintf_s snprintf
#endif

extern HINSTANCE hInst;

bool browserhookenabled();
void setbrowserhookenabled(bool enabled);
bool runbrowserhookprobe(int iterations, char *status, int statuslen);

enum {
    HOOKDIAG_LOG_CAPACITY = 64,
    HOOKDIAG_REFRESH_MS = 500,
    HOOKDIAG_GLOBAL_CAPTURE_MS = 3000,
    HOOKDIAG_BROWSER_BURST_COUNT = 25,
};

enum HookDiagGlobalCaptureMode {
    HOOKDIAG_CAPTURE_LIVE = 0,
    HOOKDIAG_CAPTURE_RUNNING = 1,
    HOOKDIAG_CAPTURE_FROZEN = 2,
};

struct __declspec(align(8)) HookDiagStat {
    volatile LONG count;
    volatile LONG padding;
    volatile LONGLONG totalus;
    volatile LONGLONG maxus;
    volatile LONG over100us;
    volatile LONG over500us;
    volatile LONG over1000us;
    volatile LONG over5000us;
};

struct HookDiagLogEntry {
    DWORD tick;
    char source[32];
    char detail[160];
    LONGLONG totalus;
    LONGLONG accessus;
    LONGLONG valueus;
    LONGLONG nameus;
};

LARGE_INTEGER hookdiagfreq = {0};
static HookDiagStat hookdiagkeyboard = {0};
static HookDiagStat hookdiagmouse = {0};
static HookDiagStat hookdiagbrowser = {0};
static HookDiagStat hookdiagbrowserprobe = {0};
static HookDiagStat hookdiagaccessible = {0};
static HookDiagStat hookdiagvalue = {0};
static HookDiagStat hookdiagname = {0};
static HookDiagStat hookdiagdbqueue = {0};
static HookDiagStat hookdiagdbwork = {0};
static HookDiagLogEntry hookdiaglog[HOOKDIAG_LOG_CAPACITY] = {{0}};
static CRITICAL_SECTION hookdiagloglock;
static bool hookdiagready = false;
static HWND hookdiagwindow = NULL;
static int hookdiaglognext = 0;
static int hookdiaglogcount = 0;
static DWORD hookdiagglobalcaptureend = 0;
static volatile LONG hookdiagglobalcapturemode = HOOKDIAG_CAPTURE_LIVE;
static char hookdiagstatus[256] = "Live diagnostics running.";
static SIZE hookdiagminwindow = {0};
static volatile LONG hookdiagdbqueuedepth = 0;
static volatile LONG hookdiagdbmaxqueuedepth = 0;

static LONGLONG hookdiagread64(volatile LONGLONG *v) {
    return InterlockedCompareExchange64((LONGLONG *)v, 0, 0);
}

static void hookdiagsetstatus(const char *fmt, ...) {
    varargs(v, fmt, _vsnprintf(hookdiagstatus, sizeof(hookdiagstatus), fmt, v));
    hookdiagstatus[sizeof(hookdiagstatus) - 1] = 0;
}

static void hookdiagclearstat(HookDiagStat &stat) {
    InterlockedExchange(&stat.count, 0);
    InterlockedExchange64((LONGLONG *)&stat.totalus, 0);
    InterlockedExchange64((LONGLONG *)&stat.maxus, 0);
    InterlockedExchange(&stat.over100us, 0);
    InterlockedExchange(&stat.over500us, 0);
    InterlockedExchange(&stat.over1000us, 0);
    InterlockedExchange(&stat.over5000us, 0);
}

static void hookdiagupdatemax(volatile LONGLONG *target, LONGLONG us) {
    LONGLONG old = hookdiagread64(target);
    while (us > old) {
        LONGLONG prev = InterlockedCompareExchange64((LONGLONG *)target, us, old);
        if (prev == old) break;
        old = prev;
    }
}

static void hookdiagupdatemax32(volatile LONG *target, LONG value) {
    LONG old = InterlockedCompareExchange((LONG *)target, 0, 0);
    while (value > old) {
        LONG prev = InterlockedCompareExchange((LONG *)target, value, old);
        if (prev == old) break;
        old = prev;
    }
}

static void hookdiagrecordstat(HookDiagStat &stat, LONGLONG us) {
    InterlockedIncrement(&stat.count);
    InterlockedExchangeAdd64((LONGLONG *)&stat.totalus, us);
    hookdiagupdatemax(&stat.maxus, us);
    if (us >= 100) InterlockedIncrement(&stat.over100us);
    if (us >= 500) InterlockedIncrement(&stat.over500us);
    if (us >= 1000) InterlockedIncrement(&stat.over1000us);
    if (us >= 5000) InterlockedIncrement(&stat.over5000us);
}

static void hookdiagaddlog(const char *source, LONGLONG totalus, LONGLONG accessus,
                           LONGLONG valueus, LONGLONG nameus, const char *detail) {
    if (!hookdiagready) return;
    EnterCriticalSection(&hookdiagloglock);
    HookDiagLogEntry &entry = hookdiaglog[hookdiaglognext];
    entry.tick = GetTickCount();
    strncpy(entry.source, source, sizeof(entry.source));
    entry.source[sizeof(entry.source) - 1] = 0;
    strncpy(entry.detail, detail, sizeof(entry.detail));
    entry.detail[sizeof(entry.detail) - 1] = 0;
    entry.totalus = totalus;
    entry.accessus = accessus;
    entry.valueus = valueus;
    entry.nameus = nameus;
    hookdiaglognext = (hookdiaglognext + 1) % HOOKDIAG_LOG_CAPACITY;
    if (hookdiaglogcount < HOOKDIAG_LOG_CAPACITY) hookdiaglogcount++;
    LeaveCriticalSection(&hookdiagloglock);
}

static const char *hookdiagmsgname(WPARAM message) {
    switch (message) {
        case WM_KEYDOWN: return "WM_KEYDOWN";
        case WM_KEYUP: return "WM_KEYUP";
        case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
        case WM_SYSKEYUP: return "WM_SYSKEYUP";
        case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
        case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
        case WM_RBUTTONDOWN: return "WM_RBUTTONDOWN";
        case WM_MOUSEWHEEL: return "WM_MOUSEWHEEL";
        default: return "other";
    }
}

void hookdiagrecordkeyboard(LONGLONG us, WPARAM message) {
    LONG capturemode = InterlockedCompareExchange(&hookdiagglobalcapturemode, 0, 0);
    if (capturemode != HOOKDIAG_CAPTURE_LIVE) {
        if (capturemode == HOOKDIAG_CAPTURE_FROZEN) return;
        if (GetTickCount() >= hookdiagglobalcaptureend) {
            InterlockedCompareExchange(&hookdiagglobalcapturemode, HOOKDIAG_CAPTURE_FROZEN,
                                       HOOKDIAG_CAPTURE_RUNNING);
            return;
        }
    }
    hookdiagrecordstat(hookdiagkeyboard, us);
    if (us >= 1000) {
        char detail[160];
        sprintf_s(detail, sizeof(detail), "message=%s (0x%X)", hookdiagmsgname(message),
                  (unsigned int)message);
        hookdiagaddlog("global-keyboard", us, 0, 0, 0, detail);
    }
}

void hookdiagrecordmouse(LONGLONG us, WPARAM message) {
    LONG capturemode = InterlockedCompareExchange(&hookdiagglobalcapturemode, 0, 0);
    if (capturemode != HOOKDIAG_CAPTURE_LIVE) {
        if (capturemode == HOOKDIAG_CAPTURE_FROZEN) return;
        if (GetTickCount() >= hookdiagglobalcaptureend) {
            InterlockedCompareExchange(&hookdiagglobalcapturemode, HOOKDIAG_CAPTURE_FROZEN,
                                       HOOKDIAG_CAPTURE_RUNNING);
            return;
        }
    }
    hookdiagrecordstat(hookdiagmouse, us);
    if (us >= 1000) {
        char detail[160];
        sprintf_s(detail, sizeof(detail), "message=%s (0x%X)", hookdiagmsgname(message),
                  (unsigned int)message);
        hookdiagaddlog("global-mouse", us, 0, 0, 0, detail);
    }
}

void hookdiagrecorddbqueue(LONGLONG us, LONG depth, const char *elements) {
    hookdiagrecordstat(hookdiagdbqueue, us);
    InterlockedExchange(&hookdiagdbqueuedepth, depth);
    hookdiagupdatemax32(&hookdiagdbmaxqueuedepth, depth);
    if (us >= 1000 || depth >= 10) {
        char detail[160];
        sprintf_s(detail, sizeof(detail), "pending=%ld enqueue=%s", depth,
                  elements ? elements : "");
        hookdiagaddlog("db-queue", us, 0, 0, 0, detail);
    }
}

void hookdiagrecorddbwork(LONGLONG us, LONG depth, const char *elements) {
    hookdiagrecordstat(hookdiagdbwork, us);
    InterlockedExchange(&hookdiagdbqueuedepth, depth);
    if (us >= 5000) {
        char detail[160];
        sprintf_s(detail, sizeof(detail), "pending=%ld work=%s", depth, elements ? elements : "");
        hookdiagaddlog("addtodatabase", us, 0, 0, 0, detail);
    }
}

void hookdiagrecordbrowser(LONGLONG total_us, LONGLONG accessible_us, LONGLONG value_us,
                           LONGLONG name_us, HWND hwnd, DWORD event, LONG idObject, LONG idChild,
                           const char *classname, HRESULT hr, bool matched, bool probe) {
    hookdiagrecordstat(probe ? hookdiagbrowserprobe : hookdiagbrowser, total_us);
    hookdiagrecordstat(hookdiagaccessible, accessible_us);
    if (value_us) hookdiagrecordstat(hookdiagvalue, value_us);
    if (name_us) hookdiagrecordstat(hookdiagname, name_us);
    if (probe || total_us >= 5000) {
        char detail[160];
        sprintf_s(detail, sizeof(detail),
                  "%s class=%s event=0x%X hwnd=0x%p obj=%ld child=%ld hr=0x%08X matched=%s",
                  probe ? "probe" : "callback", classname, (unsigned int)event, hwnd, idObject,
                  idChild, (unsigned int)hr, matched ? "yes" : "no");
        hookdiagaddlog(probe ? "browser-probe" : "browser-hook", total_us, accessible_us,
                       value_us, name_us, detail);
    }
}

static void hookdiagresetall() {
    hookdiagclearstat(hookdiagkeyboard);
    hookdiagclearstat(hookdiagmouse);
    hookdiagclearstat(hookdiagbrowser);
    hookdiagclearstat(hookdiagbrowserprobe);
    hookdiagclearstat(hookdiagaccessible);
    hookdiagclearstat(hookdiagvalue);
    hookdiagclearstat(hookdiagname);
    hookdiagclearstat(hookdiagdbqueue);
    hookdiagclearstat(hookdiagdbwork);
    InterlockedExchange(&hookdiagdbqueuedepth, 0);
    InterlockedExchange(&hookdiagdbmaxqueuedepth, 0);
    EnterCriticalSection(&hookdiagloglock);
    hookdiaglognext = 0;
    hookdiaglogcount = 0;
    LeaveCriticalSection(&hookdiagloglock);
    hookdiagglobalcaptureend = 0;
    InterlockedExchange(&hookdiagglobalcapturemode, HOOKDIAG_CAPTURE_LIVE);
    hookdiagsetstatus("Live diagnostics running.");
}

static void hookdiagresetglobal() {
    hookdiagclearstat(hookdiagkeyboard);
    hookdiagclearstat(hookdiagmouse);
    hookdiagglobalcaptureend = 0;
    InterlockedExchange(&hookdiagglobalcapturemode, HOOKDIAG_CAPTURE_LIVE);
}

static void hookdiagappendstat(String &s, const char *label, HookDiagStat &stat) {
    LONG count = stat.count;
    LONGLONG totalus = hookdiagread64(&stat.totalus);
    LONGLONG maxus = hookdiagread64(&stat.maxus);
    s.FormatCat("%s\r\n", label);
    s.FormatCat("  count=%ld avg=%.1f us max=%lld us", count,
                count ? totalus / (double)count : 0.0, maxus);
    s.FormatCat("  >=100us=%ld >=500us=%ld >=1ms=%ld >=5ms=%ld\r\n", stat.over100us,
                stat.over500us, stat.over1000us, stat.over5000us);
}

static void hookdiagbuildsummary(String &s) {
    s.Format("Global hooks: %s\r\n", globalhooksenabled() ? "enabled" : "disabled");
    hookdiagappendstat(s, "Keyboard hook", hookdiagkeyboard);
    hookdiagappendstat(s, "Mouse hook", hookdiagmouse);
    s.Cat("\r\n");
    s.Cat("Database updates\r\n");
    s.FormatCat("  queue pending=%ld max=%ld\r\n",
                InterlockedCompareExchange((LONG *)&hookdiagdbqueuedepth, 0, 0),
                InterlockedCompareExchange((LONG *)&hookdiagdbmaxqueuedepth, 0, 0));
    hookdiagappendstat(s, "Database queue enqueue", hookdiagdbqueue);
    hookdiagappendstat(s, "addtodatabase worker", hookdiagdbwork);
    s.Cat("\r\n");
    s.FormatCat("Browser hook: %s\r\n", browserhookenabled() ? "enabled" : "disabled");
    hookdiagappendstat(s, "Browser live callback total", hookdiagbrowser);
    hookdiagappendstat(s, "Browser manual probe total", hookdiagbrowserprobe);
    hookdiagappendstat(s, "AccessibleObjectFromEvent", hookdiagaccessible);
    hookdiagappendstat(s, "get_accValue", hookdiagvalue);
    hookdiagappendstat(s, "get_accName", hookdiagname);
}

static void hookdiagbuildlog(String &s) {
    EnterCriticalSection(&hookdiagloglock);
    int start = hookdiaglogcount == HOOKDIAG_LOG_CAPACITY ? hookdiaglognext : 0;
    loop(i, hookdiaglogcount) {
        HookDiagLogEntry &entry = hookdiaglog[(start + i) % HOOKDIAG_LOG_CAPACITY];
        s.FormatCat("[%10lu] %s total=%lld us", entry.tick, entry.source, entry.totalus);
        if (entry.accessus || entry.valueus || entry.nameus)
            s.FormatCat(" AOFE=%lld value=%lld name=%lld", entry.accessus, entry.valueus,
                        entry.nameus);
        s.FormatCat(" %s\r\n", entry.detail);
    }
    LeaveCriticalSection(&hookdiagloglock);
    if (!hookdiaglogcount) s.Cat("No slow events logged yet.\r\n");
}

static void hookdiagbuildreport(String &s) {
    s.Cat("ProcrastiTracker hook diagnostics\r\n\r\n");
    s.FormatCat("Status: %s\r\n", hookdiagstatus);
    LONG capturemode = InterlockedCompareExchange(&hookdiagglobalcapturemode, 0, 0);
    DWORD nowtick = GetTickCount();
    if (capturemode == HOOKDIAG_CAPTURE_RUNNING && nowtick >= hookdiagglobalcaptureend) {
        capturemode = HOOKDIAG_CAPTURE_FROZEN;
    }
    if (capturemode == HOOKDIAG_CAPTURE_RUNNING) {
        s.FormatCat("Global capture: running (%lu ms remaining)\r\n",
                    hookdiagglobalcaptureend - nowtick);
    } else if (capturemode == HOOKDIAG_CAPTURE_FROZEN) {
        s.Cat("Global capture: finished\r\n");
    }
    s.Cat("\r\n");
    hookdiagbuildsummary(s);
    s.Cat("\r\nRecent slow events\r\n");
    hookdiagbuildlog(s);
}

static int hookdiagduw(HWND hDlg, int x) {
    RECT r = {0, 0, x, 0};
    MapDialogRect(hDlg, &r);
    return r.right;
}

static int hookdiagduh(HWND hDlg, int y) {
    RECT r = {0, 0, 0, y};
    MapDialogRect(hDlg, &r);
    return r.bottom;
}

static void hookdiaglayout(HWND hDlg) {
    RECT client;
    GetClientRect(hDlg, &client);

    int marginx = hookdiagduw(hDlg, 7);
    int marginy = hookdiagduh(hDlg, 7);
    int gapx = hookdiagduw(hDlg, 7);
    int buttongapx = hookdiagduw(hDlg, 4);
    int buttongapy = hookdiagduh(hDlg, 4);
    int labely = hookdiagduh(hDlg, 5);
    int labelh = hookdiagduh(hDlg, 8);
    int edity = hookdiagduh(hDlg, 15);
    int buttonh = hookdiagduh(hDlg, 14);
    int statush = hookdiagduh(hDlg, 12);
    int sectiongap = hookdiagduh(hDlg, 7);

    int row2y = client.bottom - marginy - statush - buttongapy - buttonh;
    int row1y = row2y - buttongapy - buttonh;
    int editheight = row1y - sectiongap - edity;

    int summaryx = marginx;
    int summaryw = (client.right - marginx * 2 - gapx) / 3;
    int logx = summaryx + summaryw + gapx;
    int logw = client.right - marginx - logx;

    int resetw = hookdiagduw(hDlg, 40);
    int capturew = hookdiagduw(hDlg, 61);
    int singlew = hookdiagduw(hDlg, 79);
    int burstw = hookdiagduw(hDlg, 75);
    int copyw = hookdiagduw(hDlg, 60);
    int savew = hookdiagduw(hDlg, 60);
    int togglew = hookdiagduw(hDlg, 95);

    int resetx = marginx;
    int capturex = resetx + resetw + buttongapx;
    int singlex = capturex + capturew + buttongapx;
    int burstx = singlex + singlew + buttongapx;
    int savex = client.right - marginx - savew;
    int copyx = savex - buttongapx - copyw;
    int globalx = marginx;
    int browserx = globalx + togglew + buttongapx;

    HDWP hdwp = BeginDeferWindowPos(13);
    if (!hdwp) return;

    auto place = [&](int id, int x, int y, int w, int h) {
        HWND child = GetDlgItem(hDlg, id);
        if (child)
            hdwp = DeferWindowPos(hdwp, child, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    place(IDC_STATIC_HOOKDIAG_SUMMARY, summaryx, labely, summaryw, labelh);
    place(IDC_EDIT_HOOKDIAG_SUMMARY, summaryx, edity, summaryw, editheight);
    place(IDC_STATIC_HOOKDIAG_LOG, logx, labely, logw, labelh);
    place(IDC_EDIT_HOOKDIAG_LOG, logx, edity, logw, editheight);
    place(IDC_BUTTON_HOOKDIAG_RESET, resetx, row1y, resetw, buttonh);
    place(IDC_BUTTON_HOOKDIAG_CAPTURE, capturex, row1y, capturew, buttonh);
    place(IDC_BUTTON_HOOKDIAG_PROBE, singlex, row1y, singlew, buttonh);
    place(IDC_BUTTON_HOOKDIAG_BURST, burstx, row1y, burstw, buttonh);
    place(IDC_BUTTON_HOOKDIAG_COPY, copyx, row1y, copyw, buttonh);
    place(IDC_BUTTON_HOOKDIAG_SAVE, savex, row1y, savew, buttonh);
    place(IDC_BUTTON_HOOKDIAG_GLOBALTOGGLE, globalx, row2y, togglew, buttonh);
    place(IDC_BUTTON_HOOKDIAG_BROWSERTOGGLE, browserx, row2y, togglew, buttonh);
    place(IDC_STATIC_HOOKDIAG_STATUS, marginx, client.bottom - marginy - statush,
          client.right - marginx * 2, statush);

    EndDeferWindowPos(hdwp);
}

static void hookdiagsyncwindow(HWND hDlg) {
    if (!hDlg) return;
    LONG capturemode = InterlockedCompareExchange(&hookdiagglobalcapturemode, 0, 0);
    if (capturemode == HOOKDIAG_CAPTURE_RUNNING) {
        DWORD nowtick = GetTickCount();
        if (nowtick >= hookdiagglobalcaptureend) {
            if (InterlockedCompareExchange(&hookdiagglobalcapturemode, HOOKDIAG_CAPTURE_FROZEN,
                                           HOOKDIAG_CAPTURE_RUNNING) ==
                HOOKDIAG_CAPTURE_RUNNING) {
                hookdiagsetstatus("Global capture finished. Inspect keyboard/mouse hook stats.");
            }
            capturemode = HOOKDIAG_CAPTURE_FROZEN;
        }
    }
    String summary;
    hookdiagbuildsummary(summary);
    String log;
    hookdiagbuildlog(log);
    auto setedittext = [&](int id, String &text) {
        HWND edit = GetDlgItem(hDlg, id);
        if (!edit) return;
        int topline = (int)SendMessage(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        bool keepbottom = false;
        SCROLLINFO si = {sizeof(si), SIF_ALL};
        if (GetScrollInfo(edit, SB_VERT, &si)) {
            int page = (int)si.nPage;
            if (page > 0) page--;
            int maxpos = si.nMax - page;
            keepbottom = si.nPos >= maxpos - 1;
        }
        SetWindowTextA(edit, text);
        if (keepbottom) {
            SendMessage(edit, WM_VSCROLL, SB_BOTTOM, 0);
        } else {
            int newtop = (int)SendMessage(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
            int delta = topline - newtop;
            if (delta) SendMessage(edit, EM_LINESCROLL, 0, delta);
        }
    };
    setedittext(IDC_EDIT_HOOKDIAG_SUMMARY, summary);
    setedittext(IDC_EDIT_HOOKDIAG_LOG, log);
    char status[256];
    strncpy(status, hookdiagstatus, sizeof(status));
    status[sizeof(status) - 1] = 0;
    if (capturemode == HOOKDIAG_CAPTURE_RUNNING) {
        DWORD nowtick = GetTickCount();
        if (nowtick < hookdiagglobalcaptureend) {
            size_t used = strlen(status);
            sprintf_s(status + used, sizeof(status) - used, "  Capture: %lu ms remaining",
                      hookdiagglobalcaptureend - nowtick);
        }
    }
    SetWindowTextA(GetDlgItem(hDlg, IDC_STATIC_HOOKDIAG_STATUS), status);
    SetWindowTextA(GetDlgItem(hDlg, IDC_BUTTON_HOOKDIAG_GLOBALTOGGLE),
                   globalhooksenabled() ? "Disable Global Hooks" : "Enable Global Hooks");
    SetWindowTextA(GetDlgItem(hDlg, IDC_BUTTON_HOOKDIAG_BROWSERTOGGLE),
                   browserhookenabled() ? "Disable Browser Hook" : "Enable Browser Hook");
}

static bool hookdiagcopyreport(HWND hDlg) {
    String report;
    hookdiagbuildreport(report);
    if (!OpenClipboard(hDlg)) return false;
    EmptyClipboard();
    SIZE_T len = report.Len() + 1;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    void *ptr = GlobalLock(mem);
    memcpy(ptr, report.c_str(), len);
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_TEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

static bool hookdiagsavereport(HWND hDlg) {
    char filename[MAX_PATH] = "hook_diagnostics.txt";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hDlg;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = sizeof(filename);
    ofn.lpstrFilter = "Text Files\0*.txt\0All Files\0*.*\0\0";
    ofn.lpstrTitle = "Save Hook Diagnostics Report As...";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_ENABLESIZING | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameA(&ofn)) return false;
    String report;
    hookdiagbuildreport(report);
    FILE *f = fopen(filename, "wb");
    if (!f) return false;
    fwrite(report.c_str(), 1, report.Len(), f);
    fclose(f);
    return true;
}

static INT_PTR CALLBACK HookDiag(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
        case WM_INITDIALOG:
            hookdiagwindow = hDlg;
            {
                RECT r;
                GetWindowRect(hDlg, &r);
                hookdiagminwindow.cx = r.right - r.left;
                hookdiagminwindow.cy = r.bottom - r.top;
            }
            SendMessageA(hDlg, WM_SETICON, 0,
                         (LPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_PROCRASTITRACKER)));
            SetTimer(hDlg, 1, HOOKDIAG_REFRESH_MS, NULL);
            hookdiaglayout(hDlg);
            hookdiagsyncwindow(hDlg);
            return (INT_PTR)TRUE;
        case WM_GETMINMAXINFO:
            if (hookdiagminwindow.cx && hookdiagminwindow.cy) {
                MINMAXINFO *mmi = (MINMAXINFO *)lParam;
                mmi->ptMinTrackSize.x = hookdiagminwindow.cx;
                mmi->ptMinTrackSize.y = hookdiagminwindow.cy;
            }
            return (INT_PTR)TRUE;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                hookdiaglayout(hDlg);
                hookdiagsyncwindow(hDlg);
            }
            return (INT_PTR)TRUE;
        case WM_TIMER:
            hookdiagsyncwindow(hDlg);
            return (INT_PTR)TRUE;
        case WM_CLOSE:
            DestroyWindow(hDlg);
            return (INT_PTR)TRUE;
        case WM_DESTROY:
            KillTimer(hDlg, 1);
            hookdiagwindow = NULL;
            return (INT_PTR)TRUE;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDCANCEL:
                    DestroyWindow(hDlg);
                    return (INT_PTR)TRUE;
                case IDC_BUTTON_HOOKDIAG_RESET:
                    hookdiagresetall();
                    hookdiagsyncwindow(hDlg);
                    return (INT_PTR)TRUE;
                case IDC_BUTTON_HOOKDIAG_CAPTURE:
                    hookdiagresetglobal();
                    hookdiagglobalcaptureend = GetTickCount() + HOOKDIAG_GLOBAL_CAPTURE_MS;
                    InterlockedExchange(&hookdiagglobalcapturemode, HOOKDIAG_CAPTURE_RUNNING);
                    hookdiagsetstatus("Global capture running. Move the mouse and press a few keys.");
                    hookdiagsyncwindow(hDlg);
                    return (INT_PTR)TRUE;
                case IDC_BUTTON_HOOKDIAG_GLOBALTOGGLE:
                    {
                        bool enable = !globalhooksenabled();
                        setglobalhooksenabled(enable);
                        hookdiagsetstatus("Requested global hooks %s.",
                                          enable ? "enable" : "disable");
                    }
                    hookdiagsyncwindow(hDlg);
                    return (INT_PTR)TRUE;
                case IDC_BUTTON_HOOKDIAG_BROWSERTOGGLE:
                    {
                        bool enable = !browserhookenabled();
                        setbrowserhookenabled(enable);
                        hookdiagsetstatus("Browser hook %s.", enable ? "enabled" : "disabled");
                    }
                    hookdiagsyncwindow(hDlg);
                    return (INT_PTR)TRUE;
                case IDC_BUTTON_HOOKDIAG_PROBE: {
                    char status[256] = "";
                    if (runbrowserhookprobe(1, status, sizeof(status)))
                        hookdiagsetstatus("%s", status);
                    else
                        hookdiagsetstatus("%s", status[0] ? status : "Browser probe failed.");
                    hookdiagsyncwindow(hDlg);
                    return (INT_PTR)TRUE;
                }
                case IDC_BUTTON_HOOKDIAG_BURST: {
                    char status[256] = "";
                    if (runbrowserhookprobe(HOOKDIAG_BROWSER_BURST_COUNT, status,
                                            sizeof(status)))
                        hookdiagsetstatus("%s", status);
                    else
                        hookdiagsetstatus("%s",
                                          status[0] ? status : "Browser burst probe failed.");
                    hookdiagsyncwindow(hDlg);
                    return (INT_PTR)TRUE;
                }
                case IDC_BUTTON_HOOKDIAG_COPY:
                    hookdiagsetstatus(hookdiagcopyreport(hDlg) ? "Copied diagnostics report to clipboard."
                                                          : "Failed to copy diagnostics report.");
                    hookdiagsyncwindow(hDlg);
                    return (INT_PTR)TRUE;
                case IDC_BUTTON_HOOKDIAG_SAVE:
                    hookdiagsetstatus(hookdiagsavereport(hDlg) ? "Saved diagnostics report."
                                                             : "Diagnostics report was not saved.");
                    hookdiagsyncwindow(hDlg);
                    return (INT_PTR)TRUE;
            }
            break;
    }
    return (INT_PTR)FALSE;
}

void openhookdiagwindow(HWND parent) {
    if (hookdiagwindow) {
        ShowWindow(hookdiagwindow, SW_SHOW);
        SetForegroundWindow(hookdiagwindow);
        hookdiagsyncwindow(hookdiagwindow);
        return;
    }
    hookdiagwindow = CreateDialog(hInst, MAKEINTRESOURCE(IDD_HOOKDIAG), parent, HookDiag);
    if (hookdiagwindow) {
        ShowWindow(hookdiagwindow, SW_SHOW);
        SetForegroundWindow(hookdiagwindow);
    }
}

bool hookdiagisdialogmessage(MSG *msg) {
    return hookdiagwindow && IsDialogMessage(hookdiagwindow, msg);
}

void hookdiaginit() {
    if (hookdiagready) return;
    QueryPerformanceFrequency(&hookdiagfreq);
    InitializeCriticalSection(&hookdiagloglock);
    hookdiagready = true;
    hookdiagresetall();
}

void hookdiagcleanup() {
    if (hookdiagwindow) DestroyWindow(hookdiagwindow);
    if (!hookdiagready) return;
    DeleteCriticalSection(&hookdiagloglock);
    hookdiagready = false;
}
