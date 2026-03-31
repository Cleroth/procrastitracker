
DWORD idInst = 0;
HDDEDATA CALLBACK DdeCallback(UINT uType, UINT uFmt, HCONV hconv, HSZ hsz1, HSZ hsz2,
                              HDDEDATA hdata, DWORD dwData1, DWORD dwData2) {
    return 0;
}
bool ddeinit() {
    return DdeInitialize(&idInst, (PFNCALLBACK)DdeCallback, APPCLASS_STANDARD | APPCMD_CLIENTONLY,
                         0) == DMLERR_NO_ERROR;
}
void ddeclean() {
    if (idInst) DdeUninitialize(idInst);
}

void ddereq(char *server, char *topic, char *item, char *buf, int len) {
    buf[0] = 0;
    HSZ hszApp = DdeCreateStringHandleA(idInst, server, 0);
    HSZ hszTopic = DdeCreateStringHandleA(idInst, topic, 0);
    HCONV hConv = DdeConnect(idInst, hszApp, hszTopic, NULL);
    DdeFreeStringHandle(idInst, hszApp);
    DdeFreeStringHandle(idInst, hszTopic);
    if (hConv == NULL) {
        // OutputDebugF("dde error: %x\n", DdeGetLastError(idInst));
        // DMLERR_NO_CONV_ESTABLISHED on chrome, see
        // https://bugs.chromium.org/p/chromium/issues/detail?id=70184
        return;
    }

    HSZ hszItem = DdeCreateStringHandleA(idInst, item, 0);
    HDDEDATA hData =
        DdeClientTransaction(NULL, 0, hConv, hszItem, CF_TEXT, XTYP_REQUEST, 5000, NULL);
    if (hData != NULL) {
        DdeGetData(hData, (unsigned char *)buf, len, 0);
        buf[len - 1] = 0;
        DdeFreeDataHandle(hData);
    } else {
        // OutputDebugF("dde error: %x\n", DdeGetLastError(idInst));
    }

    DdeFreeStringHandle(idInst, hszItem);

    DdeDisconnect(hConv);
}

// Chrome still doesn't support DDE, so we use this kludgey code instead to get the current URL.
// https://bugs.chromium.org/p/chromium/issues/detail?id=70184
// http://stackoverflow.com/questions/21010017/how-to-get-current-url-for-chrome-current-version
// Newer: https://stackoverflow.com/questions/48504300/get-active-tab-url-in-chrome-with-c

// Firefox:
// https://wiki.mozilla.org/Accessibility/AT-Windows-API

HWINEVENTHOOK LHook = 0;
char current_chrome_url[MAXTMPSTR] = {0};
HWND last_chrome_hwnd = NULL;
DWORD last_chrome_event = 0;
LONG last_chrome_idobject = 0;
LONG last_chrome_idchild = 0;
bool have_last_chrome_event = false;

#define DEBUG_URL 0

void DebugURL(const char* msg) {
    #if DEBUG_URL
    OutputDebugStringA(msg);
    #endif
}

void DebugURL(LPCWSTR msg) {
    #if DEBUG_URL
        OutputDebugStringW(msg);
    #endif
}

bool BrowserHookInvoke(HWND hwnd, DWORD event, LONG idObject, LONG idChild, bool probe,
                       char *status = NULL, int statuslen = 0) {
    if (status && statuslen) status[0] = 0;
    if (!hwnd) {
        DebugURL("NULL HWND\n");
        if (status && statuslen) {
            strncpy(status, "No browser window handle", statuslen);
            status[statuslen - 1] = 0;
        }
        return false;
    }
    char classname[MAXTMPSTR];
    if (!GetClassName(hwnd, classname, MAXTMPSTR)) {
        DebugURL("GetClassName FAIL\n");
        if (status && statuslen) {
            strncpy(status, "GetClassName failed", statuslen);
            status[statuslen - 1] = 0;
        }
        return false;
    }
    classname[MAXTMPSTR - 1] = 0;
    DebugURL("GetClassName: ");
    DebugURL(classname);
    DebugURL("\n");

    auto is_chrome = strcmp(classname, "Chrome_WidgetWin_1") == 0;
    if (!is_chrome) {
        DebugURL("NOT CHROME/EDGE\n");
        if (status && statuslen) {
            strncpy(status, "Last event was not Chrome/Edge", statuslen);
            status[statuslen - 1] = 0;
        }
        return false;
    }

    last_chrome_hwnd = hwnd;
    last_chrome_event = event;
    last_chrome_idobject = idObject;
    last_chrome_idchild = idChild;
    have_last_chrome_event = true;

    if (IsDebuggerPresent()) {
        if (status && statuslen) {
            strncpy(status, "Browser probe skipped while debugger is attached", statuslen);
            status[statuslen - 1] = 0;
        }
        return false;
    }

    LONGLONG totalstart = hookdiagqpc();
    IAccessible *pAcc = NULL;
    VARIANT varChild;
    LONGLONG accessstart = hookdiagqpc();
    HRESULT hr = AccessibleObjectFromEvent(hwnd, idObject, idChild, &pAcc, &varChild);
    LONGLONG accessus = hookdiagus(accessstart, hookdiagqpc());
    LONGLONG valueus = 0;
    LONGLONG nameus = 0;
    bool matched = false;
    if ((hr == S_OK) && (pAcc != NULL)) {
        BSTR bstrName, bstrValue;
        LONGLONG valuestart = hookdiagqpc();
        pAcc->get_accValue(varChild, &bstrValue);
        valueus = hookdiagus(valuestart, hookdiagqpc());
        LONGLONG namestart = hookdiagqpc();
        pAcc->get_accName(varChild, &bstrName);
        nameus = hookdiagus(namestart, hookdiagqpc());
        DebugURL(bstrName);
        DebugURL(" = ");
        DebugURL(bstrValue);
        DebugURL("\n");
        if (bstrName && bstrValue && !wcscmp(bstrName, L"Address and search bar")) {
            WideCharToMultiByte(CP_UTF8, 0, bstrValue, -1, current_chrome_url, MAXTMPSTR, NULL,
                NULL);
            current_chrome_url[MAXTMPSTR - 1] = 0;
            matched = true;
            DebugURL("Got URL:");
            DebugURL(current_chrome_url);
            DebugURL("\n");
        } else {
            DebugURL("Address and search bar fail\n");
        }
        pAcc->Release();
    } else {
        DebugURL("AccessibleObjectFromEvent fail\n");
    }
    LONGLONG totalus = hookdiagus(totalstart, hookdiagqpc());
    hookdiagrecordbrowser(totalus, accessus, valueus, nameus, hwnd, event, idObject, idChild,
                          classname, hr, matched, probe);
    if (status && statuslen) {
        sprintf_s(status, statuslen,
                  probe ? "Probe %s: total=%lld us, AOFE=%lld us, value=%lld us, name=%lld us"
                        : "Callback %s: total=%lld us, AOFE=%lld us, value=%lld us, name=%lld us",
                  matched ? "matched" : "no match", totalus, accessus, valueus, nameus);
    }
    return true;
}

#ifdef MINGW32_BUG
void WinEventProc
#else
void CALLBACK WinEventProc
#endif
    (HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
     DWORD dwEventThread, DWORD dwmsEventTime) {
    BrowserHookInvoke(hwnd, event, idObject, idChild, false);
}

void eventhookinit() {
    if (LHook != 0) return;
    CoInitialize(NULL);
    LHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_VALUECHANGE, 0, WinEventProc, 0, 0,
                            WINEVENT_SKIPOWNPROCESS);
}

void eventhookclean() {
    if (LHook == 0) return;
    UnhookWinEvent(LHook);
    LHook = 0;
    CoUninitialize();
}

bool browserhookenabled() { return LHook != 0; }

void setbrowserhookenabled(bool enabled) {
    if (enabled)
        eventhookinit();
    else
        eventhookclean();
}

bool runbrowserhookprobe(int iterations, char *status, int statuslen) {
    if (status && statuslen) status[0] = 0;
    if (!have_last_chrome_event) {
        if (status && statuslen) {
            strncpy(status, "No Chrome/Edge event captured yet. Focus a browser window first.",
                    statuslen);
            status[statuslen - 1] = 0;
        }
        return false;
    }
    if (iterations < 1) iterations = 1;
    char laststatus[256] = "";
    int successes = 0;
    loop(i, iterations) {
        if (BrowserHookInvoke(last_chrome_hwnd, last_chrome_event, last_chrome_idobject,
                              last_chrome_idchild, true, laststatus, sizeof(laststatus)))
            successes++;
    }
    if (status && statuslen) {
        if (iterations == 1) {
            strncpy(status, laststatus, statuslen);
            status[statuslen - 1] = 0;
        } else if (!successes) {
            sprintf_s(status, statuslen, "All %d browser probes failed. Last result: %s",
                      iterations, laststatus[0] ? laststatus : "Browser probe failed.");
        } else if (successes == iterations) {
            sprintf_s(status, statuslen,
                      "Ran %d browser probes against the last Chrome/Edge event", iterations);
        } else {
            sprintf_s(status, statuslen,
                      "Ran %d browser probes: %d succeeded, %d failed. Last result: %s",
                      iterations, successes, iterations - successes,
                      laststatus[0] ? laststatus : "n/a");
        }
    }
    return successes > 0;
}
