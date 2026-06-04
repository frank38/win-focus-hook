// hookproc.cpp  -> 編譯成 hookproc.dll
// =====================================================================
//  完整 DisplayFusion 模式：兩個機制各司其職
//
//  [機制 A] SetWindowsHookEx(WH_CALLWNDPROC) = 「車票」
//      目的不是改訊息（它改不了），而是讓【系統自動把這顆 DLL 載入到
//      目標程序】。這是免遠端注入、全 documented 的進入點。
//
//  [機制 B] 子類化目標視窗的 WndProc = 「真正改寫的地方」
//      DLL 被系統載入 target 後，hook procedure 第一次跑到時，就對目標
//      視窗做 SetWindowLongPtr 子類化。之後所有訊息流經我們的 WndProc，
//      這裡才能像第一版 hookdll 那樣吞掉/改寫 WM_KILLFOCUS 等。
//
//  [設定傳遞] SetPropW/GetPropW 旗標
//      跨「安裝端」與「被載入端」傳遞「哪個視窗要套用、套什麼行為」。
// =====================================================================
//
// 編譯（x64 Native Tools）:
//   [預設] 子類化改訊息（對原始 target 有效）:
//     cl /nologo /EHsc /LD /Fo"obj/" /Fe"bin/" hookproc.cpp user32.lib
//
//   [開啟 DR hook] 改用 debug register hook GetForegroundWindow
//                  （對 defended target 有效、且躲過 prologue byte 偵測）:
//     cl /nologo /EHsc /DUSE_DR_HOOK /LD /Fo"obj/" /Fe"bin/" hookproc.cpp user32.lib
//
//   兩種模式的「進入點」都是 SetWindowsHookEx，差別只在進去後的「改寫手法」。
//
// 匯出: InstallHook(DWORD threadId) / UninstallHook()

#include <windows.h>
#ifdef USE_DR_HOOK
#include <tlhelp32.h>   // 列舉執行緒以對全部設定 DR0
#endif

// ---- 跨程序共用 hook handle（同名 section 在各程序間共用）----
#pragma data_seg(".shared")
HHOOK g_hHook = nullptr;
HWND  g_subclassedWnd = nullptr;   // 被子類化的目標視窗（供 DETACH 還原）
#pragma data_seg()
#pragma comment(linker, "/SECTION:.shared,RWS")

static HINSTANCE g_hInst = nullptr;

// 視窗旗標：仿 DisplayFusion 的 prop 命名風格
static const wchar_t* FLAG_KEEP_ACTIVE = L"__KeepWindowActive__";   // 啟用「保持作用中」
static const wchar_t* PROP_OLDPROC     = L"__OrigWndProc__";        // 存原始 WndProc
static const wchar_t* PROP_SUBCLASSED  = L"__AlreadySubclassed__";  // 防重複子類化

// =====================================================================
//  [機制 B] 改寫手法 —— 依 USE_DR_HOOK 開關二選一
// =====================================================================

#ifndef USE_DR_HOOK
// ---------------------------------------------------------------------
//  預設：子類化目標視窗的 WndProc，改寫視窗訊息。
//  對「被動接訊息判斷焦點」的原始 target 有效；
//  會被 defended target 的 WndProc 竄改偵測抓到，且對其主動查詢無效。
// ---------------------------------------------------------------------
LRESULT CALLBACK SubclassProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    WNDPROC oldProc = (WNDPROC)GetPropW(h, PROP_OLDPROC);
    if (!oldProc) return DefWindowProc(h, msg, w, l); // 理論上不會發生

    // 只有掛了 KEEP_ACTIVE 旗標的視窗才改寫；否則完全原樣放行
    if (GetPropW(h, FLAG_KEEP_ACTIVE)) {
        switch (msg) {
        case WM_KILLFOCUS:
            // 吞掉：不讓程式知道失去焦點
            OutputDebugStringA("[SUBCLASS] swallowed WM_KILLFOCUS\n");
            return 0;

        case WM_ACTIVATE:
            // 改寫成永遠 active（把 LOWORD(wParam) 變成 WA_ACTIVE）
            OutputDebugStringA("[SUBCLASS] forcing WM_ACTIVATE -> ACTIVE\n");
            w = MAKEWPARAM(WA_ACTIVE, HIWORD(w));
            break;

        case WM_ACTIVATEAPP:
            // 永遠回報 app 為 active
            OutputDebugStringA("[SUBCLASS] forcing WM_ACTIVATEAPP -> TRUE\n");
            w = TRUE;
            break;

        case WM_NCACTIVATE:
            // 標題列維持 active 外觀
            w = TRUE;
            break;
        }
    }

    return CallWindowProc(oldProc, h, msg, w, l);
}

// 對單一視窗做子類化（只做一次）
static void EnsureSubclassed(HWND hwnd) {
    if (GetPropW(hwnd, PROP_SUBCLASSED)) return;          // 已處理過
    WNDPROC oldProc = (WNDPROC)SetWindowLongPtr(
        hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
    if (oldProc) {
        SetPropW(hwnd, PROP_OLDPROC, (HANDLE)oldProc);    // 存原始 WndProc 供還原/轉呼叫
        SetPropW(hwnd, PROP_SUBCLASSED, (HANDLE)1);
        g_subclassedWnd = hwnd;                           // 記住，供 DETACH 還原
        OutputDebugStringA("[HOOKPROC] target window subclassed\n");
    }
}

// 還原子類化（卸載時用）
static void RemoveSubclass(HWND hwnd) {
    WNDPROC oldProc = (WNDPROC)GetPropW(hwnd, PROP_OLDPROC);
    if (oldProc) {
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)oldProc);
        RemovePropW(hwnd, PROP_OLDPROC);
        RemovePropW(hwnd, PROP_SUBCLASSED);
        if (g_subclassedWnd == hwnd) g_subclassedWnd = nullptr;
        OutputDebugStringA("[HOOKPROC] subclass removed (WndProc restored)\n");
    }
}

#else  // USE_DR_HOOK
// ---------------------------------------------------------------------
//  DR hook：用 debug register (DR0) + VEH 攔 GetForegroundWindow，
//  讓它對 target 永遠回傳 target 自己的視窗。不改任何 byte。
//  對「主動呼叫 GetForegroundWindow 判斷焦點」的 defended target 有效，
//  且躲過 prologue byte 偵測（但仍可被 debug register 偵測抓到）。
// ---------------------------------------------------------------------
static void*  g_targetFunc = nullptr;   // GetForegroundWindow 位址
static PVOID  g_vehHandle  = nullptr;
static bool   g_drArmed    = false;

LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP &&
        ep->ExceptionRecord->ExceptionAddress == g_targetFunc) {

        // 偽造回傳值：RAX = 被保持作用中的視窗；再模擬一次 ret
        CONTEXT* ctx = ep->ContextRecord;
        ctx->Rax = (DWORD64)g_subclassedWnd;   // 沿用共用變數存「目標視窗」
        DWORD64* sp = (DWORD64*)ctx->Rsp;
        ctx->Rip = *sp;
        ctx->Rsp += 8;
        ctx->Dr6 = 0;                          // 清狀態位，保留 DR7 致能
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void SetHwBp(HANDLE hThread, void* addr) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(hThread, &ctx)) return;
    ctx.Dr0 = (DWORD64)addr;
    ctx.Dr7 |= 0x1;                 // L0 致能
    ctx.Dr7 &= ~(0xF << 16);        // RW0=00 執行、LEN0=00 長度1
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    SetThreadContext(hThread, &ctx);
}

static void ClearHwBp(HANDLE hThread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(hThread, &ctx)) return;
    ctx.Dr0 = 0;
    ctx.Dr7 &= ~0x1;
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    SetThreadContext(hThread, &ctx);
}

// 對本程序所有執行緒設定/清除 DR0
static void ForEachThread(void (*fn)(HANDLE)) {
    DWORD myPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == myPid) {
                HANDLE hT = OpenThread(
                    THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                    FALSE, te.th32ThreadID);
                if (hT) {
                    SuspendThread(hT);
                    fn(hT);
                    ResumeThread(hT);
                    CloseHandle(hT);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// 啟用 DR hook（在 target 程序內呼叫）
static void EnsureDrHook(HWND hwnd) {
    if (g_drArmed) return;
    g_subclassedWnd = hwnd;   // 借用此變數記「要偽裝成前景的視窗」

    g_targetFunc = (void*)GetProcAddress(
        GetModuleHandleA("user32.dll"), "GetForegroundWindow");
    if (!g_targetFunc) return;

    g_vehHandle = AddVectoredExceptionHandler(1, VehHandler);
    if (!g_vehHandle) return;

    ForEachThread(SetHwBp);
    g_drArmed = true;
    OutputDebugStringA("[DRHOOK] hardware breakpoint armed on GetForegroundWindow\n");
}

// 移除 DR hook
static void RemoveDrHook() {
    if (!g_drArmed) return;
    ForEachThread(ClearHwBp);
    if (g_vehHandle) { RemoveVectoredExceptionHandler(g_vehHandle); g_vehHandle = nullptr; }
    g_drArmed = false;
    g_subclassedWnd = nullptr;
    OutputDebugStringA("[DRHOOK] hardware breakpoint cleared\n");
}

#endif // USE_DR_HOOK

// =====================================================================
//  [機制 A] WH_CALLWNDPROC hook procedure —— 只當「進入點 + 觸發子類化」
//  它本身改不了訊息（唯讀），但它跑在 target 程序內，正好用來
//  在第一次見到目標視窗時觸發子類化（機制 B）。
// =====================================================================
LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        CWPSTRUCT* p = (CWPSTRUCT*)lParam;
        if (p->hwnd && GetPropW(p->hwnd, FLAG_KEEP_ACTIVE)) {
#ifndef USE_DR_HOOK
            EnsureSubclassed(p->hwnd);   // 預設：子類化改訊息
#else
            EnsureDrHook(p->hwnd);       // DR 模式：debug register hook GetForegroundWindow
#endif
        }
    }
    SetLastError(ERROR_SUCCESS);   // 仿 log：清乾淨 last error
    return CallNextHookEx(g_hHook, code, wParam, lParam);
}

// =====================================================================
//  匯出函式：由 installer 呼叫
// =====================================================================
extern "C" __declspec(dllexport)
BOOL InstallHook(DWORD targetThreadId) {
    if (g_hHook) return TRUE;
    // thread-specific hook：只掛在目標視窗的執行緒，影響面最小。
    g_hHook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, g_hInst, targetThreadId);
    return g_hHook != nullptr;
}

extern "C" __declspec(dllexport)
BOOL UninstallHook() {
    if (g_hHook && UnhookWindowsHookEx(g_hHook)) {
        g_hHook = nullptr;
        return TRUE;
    }
    return FALSE;
}

// 提供給 installer 在卸載前還原（可選）
extern "C" __declspec(dllexport)
void RestoreWindow(HWND hwnd) {
#ifndef USE_DR_HOOK
    RemoveSubclass(hwnd);
#else
    (void)hwnd;
    RemoveDrHook();
#endif
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = (HINSTANCE)hMod;
        DisableThreadLibraryCalls(hMod);
    } else if (reason == DLL_PROCESS_DETACH) {
        // 關鍵：DLL 即將從本程序卸載，必須在消失前撤掉自己留下的 hook，
        // 否則殘留的 WndProc 指標 / VEH / DR0 會指向已卸載的程式碼而 crash。
#ifndef USE_DR_HOOK
        if (g_subclassedWnd && IsWindow(g_subclassedWnd)) {
            RemoveSubclass(g_subclassedWnd);
        }
#else
        RemoveDrHook();
#endif
    }
    return TRUE;
}
