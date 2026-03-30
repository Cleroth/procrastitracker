
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
        // Some browsers do not expose this DDE conversation at all.
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

void eventhookinit() {}

void eventhookclean() {}
