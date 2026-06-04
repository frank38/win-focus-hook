// installer.cpp  -> 編譯成 installer.exe
// 安裝 hookproc.dll 的 WH_CALLWNDPROC hook 到 target，並在 target 視窗上
// 設定 prop 旗標啟用「保持作用中」行為。維持訊息迴圈讓 hook 持續有效，
// 關閉時移除 hook 並清除旗標。
//
// 編譯（x64 Native Tools）:
//   cl /nologo /EHsc /Fo"obj/" /Fe"bin/" installer.cpp user32.lib
//
// 用法:
//   installer.exe <視窗標題關鍵字>
//   例如 target 標題開頭是 "TARGET" -> installer.exe TARGET

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <conio.h>   // _kbhit / _getch

typedef BOOL (*InstallHook_t)(DWORD);
typedef BOOL (*UninstallHook_t)();

static const wchar_t* FLAG_KEEP_ACTIVE = L"__KeepWindowActive__";

static HWND   g_targetWnd = nullptr;
static char   g_titleKey[128] = {0};

// 依標題關鍵字找視窗
static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    char title[256] = {0};
    GetWindowTextA(h, title, sizeof(title));
    if (title[0] && strstr(title, g_titleKey) && IsWindowVisible(h)) {
        g_targetWnd = h;
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <window-title-keyword>\n", argv[0]);
        return 1;
    }
    strncpy_s(g_titleKey, argv[1], _TRUNCATE);

    EnumWindows(EnumProc, 0);
    if (!g_targetWnd) {
        printf("No visible window matching '%s'.\n", g_titleKey);
        return 1;
    }
    printf("Target window: 0x%p\n", (void*)g_targetWnd);

    // 取得 target 視窗所屬的執行緒 ID（用於 thread-specific hook，影響面最小）
    DWORD targetTid = GetWindowThreadProcessId(g_targetWnd, nullptr);
    printf("Target thread id: %lu\n", targetTid);

    HMODULE dll = LoadLibraryA("hookproc.dll");
    if (!dll) { printf("LoadLibrary(hookproc.dll) failed: %lu\n", GetLastError()); return 1; }

    auto Install   = (InstallHook_t)GetProcAddress(dll, "InstallHook");
    auto Uninstall = (UninstallHook_t)GetProcAddress(dll, "UninstallHook");
    if (!Install || !Uninstall) { printf("GetProcAddress failed\n"); return 1; }

    // 在 target 視窗掛旗標：啟用「保持作用中」行為（仿 DisplayFusion 的 SetProp）
    SetPropW(g_targetWnd, FLAG_KEEP_ACTIVE, (HANDLE)1);

    // 裝 thread-specific WH_CALLWNDPROC hook。
    // 系統會把 hookproc.dll 載入到 target 程序，hook procedure 在 target 內執行。
    if (!Install(targetTid)) {
        printf("InstallHook failed: %lu\n", GetLastError());
        RemovePropW(g_targetWnd, FLAG_KEEP_ACTIVE);
        return 1;
    }
    printf("Hook installed. The hook DLL is now loaded into the target by the system.\n");
    printf("Press Enter to uninstall and exit...\n");

    // 維持訊息迴圈：thread-specific hook 需要安裝端有訊息泵維持。
    // 這裡用一條等 Enter 的迴圈，同時 pump 訊息。
    while (true) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (_kbhit() && _getch() == '\r') break;   // 按 Enter 結束
        Sleep(20);
    }

    // ---- 卸載順序很重要 ----
    // 1. 先移除 KEEP_ACTIVE 旗標：SubclassProc 立刻停止改寫，恢復原樣放行，
    //    即使子類化還在，行為也已無害。
    if (IsWindow(g_targetWnd)) RemovePropW(g_targetWnd, FLAG_KEEP_ACTIVE);

    // 2. 停止 hook。注意：這不保證系統「立刻」把 hookproc.dll 從 target 卸載。
    Uninstall();

    // 3. 推 target 跑幾輪訊息處理，促使系統在 target 內卸載 hookproc.dll，
    //    進而觸發其 DLL_PROCESS_DETACH —— 那裡會把 WndProc 還原回原始的。
    //    送無害訊息（WM_NULL）即可逼它處理一次訊息。
    if (IsWindow(g_targetWnd)) {
        for (int i = 0; i < 20; ++i) {
            SendMessageTimeoutW(g_targetWnd, WM_NULL, 0, 0,
                                SMTO_ABORTIFHUNG, 100, nullptr);
            Sleep(30);
        }
    }

    FreeLibrary(dll);   // 釋放 installer 自己這端的 DLL 參照
    printf("Hook removed. Bye.\n");
    return 0;
}
