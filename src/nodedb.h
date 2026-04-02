
struct queueddbentry {
    queueddbentry *next;
    SYSTEMTIME st;
    DWORD idletime;
    DWORD awaysecs;
    char elements[MAXTMPSTR * 3 + 16];
};

void addtodatabase(char *elements, SYSTEMTIME &st, DWORD idletime, DWORD awaysecs);

CRITICAL_SECTION databaselock;
CRITICAL_SECTION queuedblock;
HANDLE queuedbworkevent = NULL;
HANDLE queuedbidleevent = NULL;
HANDLE queuedbthread = NULL;
queueddbentry *queuedbhead = NULL;
queueddbentry *queuedbtail = NULL;
bool queuedbshutdown = false;
LONG queuedbdepth = 0;

DWORD WINAPI queueddatabaseproc(LPVOID) {
    for (;;) {
        WaitForSingleObject(queuedbworkevent, INFINITE);
        for (;;) {
            queueddbentry *entry = NULL;
            bool shutdown = false;
            LONG pendingdepth = 0;
            EnterCriticalSection(&queuedblock);
            if (queuedbhead) {
                entry = queuedbhead;
                queuedbhead = entry->next;
                if (!queuedbhead) queuedbtail = NULL;
                queuedbdepth--;
                pendingdepth = queuedbdepth;
            } else {
                shutdown = queuedbshutdown;
                if (!shutdown) ResetEvent(queuedbworkevent);
                SetEvent(queuedbidleevent);
            }
            LeaveCriticalSection(&queuedblock);
            if (!entry) {
                if (shutdown) return 0;
                break;
            }
            LONGLONG start = hookdiagqpc();
            addtodatabase(entry->elements, entry->st, entry->idletime, entry->awaysecs);
            hookdiagrecorddbwork(hookdiagus(start, hookdiagqpc()), pendingdepth, entry->elements);
            delete entry;
        }
    }
}

void initdatabasequeue() {
    InitializeCriticalSection(&databaselock);
    InitializeCriticalSection(&queuedblock);
    queuedbworkevent = CreateEvent(NULL, TRUE, FALSE, NULL);
    queuedbidleevent = CreateEvent(NULL, TRUE, TRUE, NULL);
    queuedbthread = CreateThread(NULL, 0, queueddatabaseproc, NULL, 0, NULL);
    if (!queuedbworkevent || !queuedbidleevent || !queuedbthread) {
        panic("PT: can't launch database queue thread");
    }
}

void flushdatabasequeue() {
    if (queuedbidleevent) WaitForSingleObject(queuedbidleevent, INFINITE);
}

void killdatabasequeue() {
    if (!queuedbthread) return;
    EnterCriticalSection(&queuedblock);
    queuedbshutdown = true;
    SetEvent(queuedbworkevent);
    LeaveCriticalSection(&queuedblock);
    WaitForSingleObject(queuedbthread, INFINITE);
    CloseHandle(queuedbthread);
    CloseHandle(queuedbworkevent);
    CloseHandle(queuedbidleevent);
    queuedbthread = NULL;
    queuedbworkevent = NULL;
    queuedbidleevent = NULL;
    queuedbdepth = 0;
    DeleteCriticalSection(&queuedblock);
    DeleteCriticalSection(&databaselock);
}

void queueaddtodatabase(char *elements, SYSTEMTIME &st, DWORD idletime, DWORD awaysecs) {
    LONGLONG start = hookdiagqpc();
    LONG pendingdepth = 0;
    if (!*elements) return;
    queueddbentry *entry = new queueddbentry;
    char diagelements[sizeof(entry->elements)];
    entry->next = NULL;
    entry->st = st;
    entry->idletime = idletime;
    entry->awaysecs = awaysecs;
    strncpy(entry->elements, elements, sizeof(entry->elements));
    entry->elements[sizeof(entry->elements) - 1] = 0;
    memcpy(diagelements, entry->elements, sizeof(diagelements));
    EnterCriticalSection(&queuedblock);
    if (queuedbtail) {
        queuedbtail->next = entry;
    } else {
        queuedbhead = entry;
    }
    queuedbtail = entry;
    queuedbdepth++;
    pendingdepth = queuedbdepth;
    ResetEvent(queuedbidleevent);
    SetEvent(queuedbworkevent);
    LeaveCriticalSection(&queuedblock);
    hookdiagrecorddbqueue(hookdiagus(start, hookdiagqpc()), pendingdepth, diagelements);
}

void save(bool filtered = false, char *givenfilename = NULL) {
    flushdatabasequeue();
    EnterCriticalSection(&databaselock);
    static int firstsave = TRUE;
    gzFile f = gzopen(givenfilename ? givenfilename : databasetemp, "wb1h");
    if (!f) panic("PT: could not open database file for writing");
    if (filtered) recompaccum();
    wint(f, FILE_FORMAT_VERSION);
    wint(f, 'PTFF');
    wint(f, MAXTAGS);
    loop(i, MAXTAGS) {
        gzwrite_s(f, tags[i].name, sizeof(tags[i].name));
        wint(f, tags[i].color);
    }
    wint(f, minfilter.ival * 60);
    wint(f, foldlevel);
    loop(i, NUM_PREFS) wint(f, prefs[i].ival);
    wint(f, browserhookpersistenabled);
    root->save(f, filtered);
    if (gzclose(f) != Z_OK) panic("PT: could not finish writing database file");
    if (!givenfilename) {
        lastsavetime = GetTickCount();
        // only delete previous backups if we have a database
        if (GetFileAttributesA(databasemain) != INVALID_FILE_ATTRIBUTES) {
            if (firstsave) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                char lrun[MAX_PATH];
                sprintf_s(lrun, MAX_PATH, "%sdb_BACKUP_%d_%02d_%02d.PT", databaseroot, st.wYear,
                          st.wMonth, st.wDay);
                DeleteFileA(lrun);
                CopyFileA(databasemain, lrun, FALSE);  // backup last saved of last run once per day
                firstsave = FALSE;
            }
            DeleteFileA(databaseback);
            MoveFileA(databasemain, databaseback);  // backup last saved
        }
        MoveFileA(databasetemp, databasemain);
    }
    LeaveCriticalSection(&databaselock);
}

// NOTE: this function should only overwrite globals it gets by argument, see callers.
void loadglobals(gzFile f, int version, numeditbox *prefs, tag *tags, numeditbox &minfilter,
                 DWORD &foldlevel, DWORD &browserhookpersistenabled) {
    // FF: int numtags
    int ntags = rint(f);
    if (ntags > MAXTAGS) panic("PT: wrong number of tags in file");
    loop(i, ntags) {
        // FF: struct { char name[32]; int color; } tags[numtags]
        gzread_s(f, tags[i].name, sizeof(tags[i].name));
        int col = rint(f);
        tags[i].color = col;
    }
    // FF: int minfilter
    if (version < 6) {
        loop(i, 10) gzgetc_s(f);
    } else
        minfilter.ival = rint(f) / 60;
    // FF: int foldlevel
    foldlevel = rint(f);
    ASSERT(NUM_PREFS == 11);
    if (version >= 6) {
        // FF: int prefs[6] (see advanced prefs window)
        loop(i, 5) prefs[i].ival = rint(f);
        if (version >= 10) prefs[5].ival = rint(f);
        if (version >= 11) {
            if (version >= 12) {
                prefs[PREF_XINPUTACTIVITYFREQUENCY].ival = rint(f);
                prefs[PREF_XINPUTACTIVITY].ival = rint(f);
            } else {
                rint(f);  // throw away the "Xbox controller connected" preference if set in version
                          // 11
            }
            prefs[PREF_FOREGROUNDFULLSCREEN].ival = rint(f);
        }
        if (version >= 13) { prefs[PREF_AWAYAUTO].ival = rint(f); }
        if (version >= 14) { prefs[PREF_DAYSTART].ival = rint(f); }
    }
    if (version >= 16) browserhookpersistenabled = rint(f);
}


void load(node *root, char *fn, bool merge) {
    // FF: struct DATABASE {
    gzFile f = gzopen(fn, "rb");
    if (!f) return;  // inform the user if its not his first run?
    // FF: int version (== 10, as of v1.6, format may be different if other version)
    int version = rint(f);
    if (version > FILE_FORMAT_VERSION) panic("PT: trying to load db from newer version");
    // FF: int magic (== 'PTFF' on x86)
    if (version >= 6 && rint(f) != 'PTFF') panic("PT: not a PT database file");
    if (version >= 4) {
        if (merge) {
            // don't overwrite global data
            numeditbox _prefs[NUM_PREFS];
            tag _tags[MAXTAGS];
            numeditbox _minfilter;
            DWORD _foldlevel;
            DWORD _browserhookpersistenabled = 1;
            loadglobals(f, version, _prefs, _tags, _minfilter, _foldlevel,
                        _browserhookpersistenabled);
        } else {
            loadglobals(f, version, prefs, tags, minfilter, foldlevel,
                        browserhookpersistenabled);
        }
        if (version == 8) loop(i, 24) gzgetc_s(f);
    }
    // FF: NODE root
    root->nname = "";
    root->load(f, version, merge);
    gzclose(f);
    // FF: }
}

void exporthtml(char *fn) {
    flushdatabasequeue();
    EnterCriticalSection(&databaselock);
    recompaccum();
    FILE *f = fopen(fn, "w");
    if (f) {
        fprintf(f, "<!DOCTYPE html><HTML><HEAD><meta charset=\"utf-8\"/>"
                   "<TITLE>Procrastitracker export</TITLE></HEAD><BODY>");
        root->print(f, true);
        fprintf(f, "</BODY></HTML>");
        fclose(f);
    }
    LeaveCriticalSection(&databaselock);
}

void mergedb(char *fn) {
    node *dbr = new node("(root)", NULL);
    load(dbr, fn, true);
    flushdatabasequeue();
    EnterCriticalSection(&databaselock);
    root->merge(*dbr);
    LeaveCriticalSection(&databaselock);
    delete dbr;
}

static char *seperators[] = {" - ", " | ", " : ", " > ", "\\\\", "\\", "//", "/", NULL};

void dumpst(SYSTEMTIME st) {
    char msg[256];
    sprintf_s(msg, 256, "time: %04d-%02d-%02d %02d:%02d\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    OutputDebugStringA(msg);
}

void addtodatabase(char *elements, SYSTEMTIME &st, DWORD idletime, DWORD awaysecs) {
    EnterCriticalSection(&databaselock);
    node *n = root;
    for (char *rest = elements, *head; *rest;) {
        if (n->flatten) break;
        head = rest;
        char *sep = NULL;
        int sepl = 0;
        for (char **seps = seperators; *seps; seps++) {
            char *sepn = strstr(rest, *seps);
            if (sepn && (!sep || sepn < sep)) {
                sep = sepn;
                sepl = strlen(*seps);
            }
        }
        if (sep) {
            rest = sep + sepl;
            *sep = 0;
        } else {
            rest = "";
        }
        if (*head) {
            if (head[0] == '[') head++;
            for (;;) {
                size_t l = strlen(head);
                if (l && (head[l - 1] == '*' || head[l - 1] == ']'))
                    head[l - 1] = 0;
                else
                    break;
            }
            if (*head) n = n->add(head);
        }
    }
    if (prefs[PREF_DAYSTART].ival) {
        // Convert to FILETIME and back, as that is the only way to get
        // consequtive time, since our own dayordering() has gaps, which
        // means substracting time wouldn't work.
        // This produces a time where if your DAYSTART is at 4am, your
        // day lasts from 4am to 4am, and thus any minute markers recorded
        // are relative to that, and not the real day. That seems acceptable
        // for this purpose.
        //dumpst(st);
        unsigned long long ft;
        SystemTimeToFileTime(&st, (FILETIME *)&ft);
        auto daystart = 10000000ULL * 60ULL * 60ULL * prefs[PREF_DAYSTART].ival;
        ft -= daystart;
        FileTimeToSystemTime((FILETIME *)&ft, &st);
        //dumpst(st);
    }
    n->hit(st, idletime, awaysecs);
    LeaveCriticalSection(&databaselock);
}
