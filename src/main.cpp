// Doppler's File Duplicates Remover - pure C++ / Win32, single standalone exe.
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

enum { ID_OPEN = 101, ID_PATH, ID_SELALL, ID_LIST, ID_ANALYZE, ID_DELETE, ID_MOVE,
       ID_STATUS, IDT_PATH = 1, WM_ANALYZE_DONE = WM_APP + 1 };

struct Entry {
    std::wstring name;
    unsigned long long size = 0;
    FILETIME time{};
    int group = 0;            // 0 = unique, >0 duplicate set id
};

HWND g_wnd, g_open, g_path, g_selAll, g_list, g_analyze, g_delete, g_move, g_status;
HFONT g_font;
std::wstring g_folder;
std::vector<Entry> g_entries;     // lParam of list items = index here
int g_sortCol = 0; bool g_sortAsc = true;   // g_sortCol -1 = by duplicate group
bool g_busy = false, g_loading = false;
std::vector<int> g_result;        // worker output: group id per entry (negative = keeper)
int g_dupGroups = 0;

void SetStatus(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }

std::wstring FmtBytes(unsigned long long v) {
    wchar_t b[64]; StrFormatByteSizeW((LONGLONG)v, b, 64); return b;
}
std::wstring FmtSize(unsigned long long v) {
    std::wstring d = std::to_wstring(v);
    for (int i = (int)d.size() - 3; i > 0; i -= 3) d.insert(i, L",");
    return FmtBytes(v) + L"  (" + d + L" B)";
}
std::wstring FmtDate(const FILETIME& ft) {
    FILETIME l; SYSTEMTIME st; FileTimeToLocalFileTime(&ft, &l); FileTimeToSystemTime(&l, &st);
    wchar_t b[64]; wsprintfW(b, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}
std::wstring FullPath(const std::wstring& n) {
    return g_folder + (g_folder.back() == L'\\' ? L"" : L"\\") + n;
}

int CALLBACK CmpProc(LPARAM a, LPARAM b, LPARAM) {
    const Entry& x = g_entries[a]; const Entry& y = g_entries[b];
    int r = 0;
    switch (g_sortCol) {
    case 0: r = lstrcmpiW(x.name.c_str(), y.name.c_str()); break;
    case 1: r = x.size < y.size ? -1 : x.size > y.size ? 1 : 0; break;
    case 2: r = CompareFileTime(&x.time, &y.time); break;
    case -1: // duplicate sets first (by set id), unique files last
        if (!x.group != !y.group) return x.group ? -1 : 1;
        r = x.group - y.group; if (!r) r = lstrcmpiW(x.name.c_str(), y.name.c_str()); return r;
    }
    if (r == 0 && g_sortCol != 0) r = lstrcmpiW(x.name.c_str(), y.name.c_str());
    return g_sortAsc ? r : -r;
}
void UpdateHeaderArrows() {
    HWND h = ListView_GetHeader(g_list);
    for (int i = 0; i < 3; i++) {
        HDITEMW it{}; it.mask = HDI_FORMAT; Header_GetItem(h, i, &it);
        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_sortCol) it.fmt |= g_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(h, i, &it);
    }
}
void Resort() { ListView_SortItems(g_list, CmpProc, 0); UpdateHeaderArrows(); }

int CountChecked() {
    int n = 0, c = ListView_GetItemCount(g_list);
    for (int i = 0; i < c; i++) if (ListView_GetCheckState(g_list, i)) n++;
    return n;
}
void UpdateStatus() {
    std::wstring s = std::to_wstring(g_entries.size()) + L" files, " + std::to_wstring(CountChecked()) + L" selected";
    if (g_dupGroups) s += L"  |  duplicate sets: " + std::to_wstring(g_dupGroups);
    SetStatus(s);
}
void SetAllChecks(bool on) {
    g_loading = true;
    int c = ListView_GetItemCount(g_list);
    for (int i = 0; i < c; i++) ListView_SetCheckState(g_list, i, on);
    g_loading = false;
    UpdateStatus();
}
void SyncSelAll() {
    int c = ListView_GetItemCount(g_list), n = CountChecked();
    SendMessageW(g_selAll, BM_SETCHECK, (c && n == c) ? BST_CHECKED : BST_UNCHECKED, 0);
}
void EnableButtons() {
    BOOL e = !g_busy;
    EnableWindow(g_analyze, e); EnableWindow(g_delete, e); EnableWindow(g_move, e); EnableWindow(g_open, e);
}

void Populate() {
    g_loading = true;
    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_entries.size(); i++) {
        const Entry& e = g_entries[i];
        LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = (int)i; it.lParam = (LPARAM)i;
        it.pszText = (LPWSTR)e.name.c_str();
        int idx = ListView_InsertItem(g_list, &it);
        std::wstring s = FmtSize(e.size), d = FmtDate(e.time);
        ListView_SetItemText(g_list, idx, 1, (LPWSTR)s.c_str());
        ListView_SetItemText(g_list, idx, 2, (LPWSTR)d.c_str());
    }
    g_loading = false;
    Resort();
    SendMessageW(g_selAll, BM_SETCHECK, BST_UNCHECKED, 0);
    UpdateStatus();
}

void LoadFolder(const std::wstring& dir) {
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { SetStatus(L"Folder not found."); return; }
    g_folder = dir;
    g_entries.clear(); g_dupGroups = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(FullPath(L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Entry e; e.name = fd.cFileName;
            e.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.time = fd.ftLastWriteTime;
            g_entries.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (g_sortCol == -1) g_sortCol = 0;
    Populate();
}

bool PickFolder(HWND owner, std::wstring& out) {
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD o; dlg->GetOptions(&o); dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(dlg->GetResult(&si))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); ok = true; }
            si->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ---------------------------------------------------------------- analysis
// Method: 1) group by exact size (different size => cannot be equal, no I/O)
//         2) within a size group hash the first 64 KB (cheap filter)
//         3) files with the same size + head hash are compared byte-for-byte
//            against each cluster's representative - no hash-collision risk.
// File names and dates are ignored. Empty files are skipped.
struct Item { std::wstring path; unsigned long long size; int idx; FILETIME time; };

HANDLE OpenRead(const std::wstring& p) {
    return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}
bool HeadHash(const std::wstring& p, unsigned long long& out) {
    HANDLE h = OpenRead(p);
    if (h == INVALID_HANDLE_VALUE) return false;
    static thread_local std::vector<unsigned char> buf(65536);
    DWORD n = 0; BOOL ok = ReadFile(h, buf.data(), 65536, &n, nullptr); CloseHandle(h);
    if (!ok) return false;
    unsigned long long x = 1469598103934665603ULL;
    for (DWORD i = 0; i < n; i++) { x ^= buf[i]; x *= 1099511628211ULL; }
    out = x; return true;
}
bool SameContent(const std::wstring& a, const std::wstring& b) {
    HANDLE ha = OpenRead(a);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = OpenRead(b);
    if (hb == INVALID_HANDLE_VALUE) { CloseHandle(ha); return false; }
    std::vector<unsigned char> x(1 << 20), y(1 << 20);
    bool same = true;
    for (;;) {
        DWORD na = 0, nb = 0;
        if (!ReadFile(ha, x.data(), (DWORD)x.size(), &na, nullptr) || !ReadFile(hb, y.data(), (DWORD)y.size(), &nb, nullptr)) { same = false; break; }
        if (na != nb || memcmp(x.data(), y.data(), na) != 0) { same = false; break; }
        if (na == 0) break;
    }
    CloseHandle(ha); CloseHandle(hb);
    return same;
}

void Worker(std::vector<Item> items, size_t total) {
    std::vector<int> groups(total, 0);
    std::map<unsigned long long, std::vector<Item>> bySize;
    for (auto& it : items) if (it.size > 0) bySize[it.size].push_back(it);
    int nextGroup = 1;
    for (auto& kv : bySize) {
        if (kv.second.size() < 2) continue;
        std::map<unsigned long long, std::vector<Item>> byHead;
        for (auto& it : kv.second) { unsigned long long h; if (HeadHash(it.path, h)) byHead[h].push_back(it); }
        for (auto& hv : byHead) {
            if (hv.second.size() < 2) continue;
            std::vector<std::vector<Item>> clusters;
            for (auto& it : hv.second) {
                bool placed = false;
                for (auto& c : clusters) if (SameContent(c[0].path, it.path)) { c.push_back(it); placed = true; break; }
                if (!placed) clusters.push_back({it});
            }
            for (auto& c : clusters) {
                if (c.size() < 2) continue;
                // keep the oldest file (ties: first by path); the rest get selected. Keeper stored as negative id.
                size_t keep = 0;
                for (size_t i = 1; i < c.size(); i++) {
                    int r = CompareFileTime(&c[i].time, &c[keep].time);
                    if (r < 0 || (r == 0 && lstrcmpiW(c[i].path.c_str(), c[keep].path.c_str()) < 0)) keep = i;
                }
                for (size_t i = 0; i < c.size(); i++) groups[c[i].idx] = (i == keep) ? -nextGroup : nextGroup;
                nextGroup++;
            }
        }
    }
    g_result = std::move(groups);
    PostMessageW(g_wnd, WM_ANALYZE_DONE, 0, 0);
}

void StartAnalyze() {
    if (g_entries.empty()) { SetStatus(L"Nothing to analyze - open a folder first."); return; }
    std::vector<Item> items;
    for (size_t i = 0; i < g_entries.size(); i++)
        items.push_back({FullPath(g_entries[i].name), g_entries[i].size, (int)i, g_entries[i].time});
    g_busy = true; EnableButtons(); SetStatus(L"Analyzing...");
    std::thread(Worker, std::move(items), g_entries.size()).detach();
}

void FinishAnalyze() {
    int maxg = 0;
    std::vector<bool> check(g_entries.size(), false);
    for (size_t i = 0; i < g_entries.size(); i++) {
        int g = g_result[i];
        g_entries[i].group = g < 0 ? -g : g;
        check[i] = g > 0;
        maxg = std::max(maxg, g_entries[i].group);
    }
    g_dupGroups = maxg;
    g_busy = false; EnableButtons();
    g_sortCol = -1; g_sortAsc = true;
    Resort();
    g_loading = true;
    int c = ListView_GetItemCount(g_list), dups = 0;
    unsigned long long bytes = 0;
    for (int i = 0; i < c; i++) {
        LVITEMW it{}; it.mask = LVIF_PARAM; it.iItem = i; ListView_GetItem(g_list, &it);
        bool on = check[it.lParam];
        ListView_SetCheckState(g_list, i, on);
        if (on) { dups++; bytes += g_entries[it.lParam].size; }
    }
    g_loading = false;
    InvalidateRect(g_list, nullptr, TRUE);
    SyncSelAll();
    SetStatus(L"Analysis done: " + std::to_wstring(maxg) + L" duplicate set(s), " + std::to_wstring(dups) +
              L" redundant file(s) selected (oldest copy of each is kept), " + FmtBytes(bytes) + L" can be freed.");
}

// ---------------------------------------------------------------- actions
std::vector<int> CheckedEntries() {
    std::vector<int> r; int c = ListView_GetItemCount(g_list);
    for (int i = 0; i < c; i++) if (ListView_GetCheckState(g_list, i)) {
        LVITEMW it{}; it.mask = LVIF_PARAM; it.iItem = i; ListView_GetItem(g_list, &it); r.push_back((int)it.lParam);
    }
    return r;
}

void DoDelete() {
    auto sel = CheckedEntries();
    if (sel.empty()) { SetStatus(L"No files selected."); return; }
    std::wstring m = L"Delete " + std::to_wstring(sel.size()) + L" selected file(s)?\n\nFiles are sent to the Recycle Bin.";
    if (MessageBoxW(g_wnd, m.c_str(), L"Confirm delete", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    std::wstring list;
    for (int i : sel) { list += FullPath(g_entries[i].name); list.push_back(L'\0'); }
    list.push_back(L'\0');
    SHFILEOPSTRUCTW op{}; op.hwnd = g_wnd; op.wFunc = FO_DELETE; op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    SHFileOperationW(&op);
    std::wstring f = g_folder; LoadFolder(f);
    SetStatus(L"Delete finished. " + std::to_wstring(g_entries.size()) + L" files remain.");
}

void DoMove() {
    auto sel = CheckedEntries();
    if (sel.empty()) { SetStatus(L"No files selected."); return; }
    std::wstring dest;
    if (!PickFolder(g_wnd, dest)) return;
    if (_wcsicmp(dest.c_str(), g_folder.c_str()) == 0) { SetStatus(L"Destination is the same folder."); return; }
    int ok = 0, fail = 0;
    for (int i : sel) {
        std::wstring src = FullPath(g_entries[i].name);
        std::wstring name = g_entries[i].name, dst = dest + L"\\" + name;
        for (int n = 2; GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES; n++) { // never overwrite
            std::wstring stem = name, ext;
            size_t dot = name.rfind(L'.');
            if (dot != std::wstring::npos && dot > 0) { stem = name.substr(0, dot); ext = name.substr(dot); }
            dst = dest + L"\\" + stem + L" (" + std::to_wstring(n) + L")" + ext;
        }
        if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED)) ok++; else fail++;
    }
    std::wstring f = g_folder; LoadFolder(f);
    SetStatus(L"Moved " + std::to_wstring(ok) + L" file(s)" + (fail ? L", failed: " + std::to_wstring(fail) : L"") + L".");
}

// ---------------------------------------------------------------- UI
void Layout() {
    RECT rc; GetClientRect(g_wnd, &rc);
    UINT dpi = 96; if (HDC dc = GetDC(g_wnd)) { dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(g_wnd, dc); }
    auto S = [&](int v) { return MulDiv(v, dpi, 96); };
    int m = S(10), w = rc.right, h = rc.bottom, rowH = S(26), y = m;
    MoveWindow(g_open, m, y, S(110), rowH, TRUE);
    MoveWindow(g_path, m + S(118), y, w - 2 * m - S(118), rowH, TRUE);
    y += rowH + S(8);
    MoveWindow(g_selAll, m, y, S(200), S(22), TRUE);
    y += S(26);
    int statusH = S(20), btnH = S(30);
    int listH = h - y - m - btnH - S(8) - statusH - S(4);
    MoveWindow(g_list, m, y, w - 2 * m, listH, TRUE);
    y += listH + S(8);
    int bw = S(110);
    MoveWindow(g_analyze, m, y, bw, btnH, TRUE);
    MoveWindow(g_delete, m + bw + S(8), y, bw, btnH, TRUE);
    MoveWindow(g_move, m + 2 * (bw + S(8)), y, bw, btnH, TRUE);
    MoveWindow(g_status, m, y + btnH + S(4), w - 2 * m, statusH, TRUE);
    int lw = w - 2 * m - GetSystemMetrics(SM_CXVSCROLL) - S(8);
    ListView_SetColumnWidth(g_list, 1, S(170)); ListView_SetColumnWidth(g_list, 2, S(130));
    ListView_SetColumnWidth(g_list, 0, std::max(S(150), lw - S(170) - S(130)));
}

HWND Mk(const wchar_t* cls, const wchar_t* txt, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = hwnd;
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm); SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        g_open = Mk(L"BUTTON", L"Open Folder...", BS_PUSHBUTTON | WS_TABSTOP, ID_OPEN);
        g_path = Mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_PATH, WS_EX_CLIENTEDGE);
        g_selAll = Mk(L"BUTTON", L"Select all", BS_AUTOCHECKBOX | WS_TABSTOP, ID_SELALL);
        g_list = Mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        const wchar_t* cols[] = {L"File name", L"Size", L"Date modified"};
        for (int i = 0; i < 3; i++) {
            LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)cols[i]; c.cx = 100;
            ListView_InsertColumn(g_list, i, &c);
        }
        g_analyze = Mk(L"BUTTON", L"Analyze", BS_PUSHBUTTON | WS_TABSTOP, ID_ANALYZE);
        g_delete = Mk(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, ID_DELETE);
        g_move = Mk(L"BUTTON", L"Move...", BS_PUSHBUTTON | WS_TABSTOP, ID_MOVE);
        g_status = Mk(L"STATIC", L"Open a folder to begin.", SS_LEFT | SS_ENDELLIPSIS, ID_STATUS);
        UpdateHeaderArrows();
        return 0;
    }
    case WM_SIZE: if (wp != SIZE_MINIMIZED) Layout(); return 0;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp; SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: { MINMAXINFO* mi = (MINMAXINFO*)lp; mi->ptMinTrackSize.x = 520; mi->ptMinTrackSize.y = 380; return 0; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_OPEN: { std::wstring d; if (PickFolder(hwnd, d)) { SetWindowTextW(g_path, d.c_str()); KillTimer(hwnd, IDT_PATH); LoadFolder(d); } break; }
        case ID_PATH: if (HIWORD(wp) == EN_CHANGE) SetTimer(hwnd, IDT_PATH, 600, nullptr); break;
        case ID_SELALL: SetAllChecks(SendMessageW(g_selAll, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case ID_ANALYZE: if (!g_busy) StartAnalyze(); break;
        case ID_DELETE: if (!g_busy) DoDelete(); break;
        case ID_MOVE: if (!g_busy) DoMove(); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == IDT_PATH) {
            KillTimer(hwnd, IDT_PATH);
            wchar_t b[MAX_PATH * 4]; GetWindowTextW(g_path, b, MAX_PATH * 4);
            std::wstring p = b; while (p.size() > 3 && (p.back() == L'\\' || p.back() == L' ')) p.pop_back();
            if (!p.empty() && p != g_folder && !g_busy) LoadFolder(p);
        }
        return 0;
    case WM_ANALYZE_DONE: FinishAnalyze(); return 0;
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->hwndFrom != g_list) break;
        if (nh->code == LVN_COLUMNCLICK) {
            int c = ((NMLISTVIEW*)lp)->iSubItem;
            if (c == g_sortCol) g_sortAsc = !g_sortAsc; else { g_sortCol = c; g_sortAsc = true; }
            Resort(); InvalidateRect(g_list, nullptr, TRUE);
        } else if (nh->code == LVN_ITEMCHANGED && !g_loading) {
            NMLISTVIEW* n = (NMLISTVIEW*)lp;
            if ((n->uChanged & LVIF_STATE) && ((n->uOldState ^ n->uNewState) & LVIS_STATEIMAGEMASK)) { SyncSelAll(); UpdateStatus(); }
        } else if (nh->code == NM_CUSTOMDRAW) { // tint alternate duplicate sets so groups are visible
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int idx = (int)cd->nmcd.lItemlParam;
                if (idx >= 0 && idx < (int)g_entries.size() && g_entries[idx].group > 0) {
                    cd->clrTextBk = (g_entries[idx].group & 1) ? RGB(255, 244, 214) : RGB(222, 238, 255);
                    cd->clrText = RGB(0, 0, 0);
                }
            }
            return CDRF_DODEFAULT;
        }
        break;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"DopplerDupRemover";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(hi, MAKEINTRESOURCE(1));    RegisterClassW(&wc);
    UINT dpi = 96; if (HDC dc = GetDC(nullptr)) { dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"Doppler's File Duplicates Remover", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(760, dpi, 96), MulDiv(560, dpi, 96), nullptr, nullptr, hi, nullptr);
    ShowWindow(w, show); UpdateWindow(w);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    return 0;
}
