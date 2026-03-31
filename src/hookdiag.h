#pragma once

#include <windows.h>

extern LARGE_INTEGER hookdiagfreq;

inline LONGLONG hookdiagqpc() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

inline LONGLONG hookdiagus(LONGLONG start, LONGLONG end) {
    return hookdiagfreq.QuadPart ? ((end - start) * 1000000LL) / hookdiagfreq.QuadPart : 0;
}

void hookdiaginit();
void hookdiagcleanup();
void openhookdiagwindow(HWND parent);
bool hookdiagisdialogmessage(MSG *msg);
void hookdiagrecordkeyboard(LONGLONG us, WPARAM message);
void hookdiagrecordmouse(LONGLONG us, WPARAM message);
void hookdiagrecorddbqueue(LONGLONG us, LONG depth, const char *elements);
void hookdiagrecorddbwork(LONGLONG us, LONG depth, const char *elements);
void hookdiagrecordbrowser(LONGLONG total_us, LONGLONG accessible_us, LONGLONG value_us,
                           LONGLONG name_us, HWND hwnd, DWORD event, LONG idObject, LONG idChild,
                           const char *classname, HRESULT hr, bool matched, bool probe);
