*[English](README.md)*

# win-focus-hook

一個教學用的小專案，示範 Windows 上「保持視窗作用中（keep active）」的焦點
hook 是如何運作的 —— 也就是 DisplayFusion 那個「防止視窗停用」功能背後的同一類機制。

它示範兩件事：

1. **乾淨的進入點** —— 用 `SetWindowsHookEx(WH_CALLWNDPROC)` 讓系統把 DLL 載入到
   目標程序，而不是用遠端執行緒注入（remote-thread injection）。
2. **兩種可互換的改寫手法**（編譯期選擇）：
   - **預設**：子類化（subclass）目標視窗的 `WndProc`，改寫焦點相關訊息
     （`WM_KILLFOCUS`、`WM_ACTIVATE`、`WM_ACTIVATEAPP`、`WM_NCACTIVATE`）。
   - **`USE_DR_HOOK`**：用 debug register（DR0）+ VEH 攔截 `GetForegroundWindow`，
     在不更動任何程式碼位元組（byte）的情況下改寫該 API 的回傳值。

> **僅供教育／研究用途。** 本專案用於理解 Windows 內部機制，並且是針對你自己撰寫的
> 小型測試程式（`target.exe`）做示範。請**勿**將其用於受反作弊或其他完整性
> 保護機制保護的軟體 —— 將程式碼載入這類程序（無論用什麼方式，包括
> `SetWindowsHookEx`）都是可被偵測的，可能導致帳號被封，或違反該軟體的使用條款。

## 運作原理

```
installer.exe
   1. 找到目標視窗，取得它的 thread id
   2. SetPropW(target, "__KeepWindowActive__", 1)   <- 旗標：這個視窗要保持作用中
   3. InstallHook(targetThreadId)
        -> SetWindowsHookEx(WH_CALLWNDPROC, ..., threadId)   [乾淨的進入點]
           系統因此把 hookproc.dll 載入到「目標程序內」

進入目標程序後，hookproc.dll 接著套用下列其中一種：
   - [預設]         子類化視窗的 WndProc，改寫焦點訊息
   - [USE_DR_HOOK]  對 GetForegroundWindow 設 DR0 + VEH，偽造回傳值
```

`WH_CALLWNDPROC` 這個 hook 本身是唯讀的，**無法**改寫訊息 —— 它只被用來當作
「進入點」。真正的改寫是透過子類化（或 DR hook）在目標程序內完成的。

## 編譯

開啟 **「x64 Native Tools Command Prompt for VS」**（MSVC），然後：

```bat
:: 預設模式（子類化，改寫視窗訊息）
build.bat

:: debug register hook 模式（攔 GetForegroundWindow，不改任何 byte）
build.bat dr
```

或手動編譯：

```bat
cl /nologo /EHsc /LD /Fo"obj/" /Fe"bin/" hookproc.cpp user32.lib
cl /nologo /EHsc /Fo"obj/" /Fe"bin/" installer.cpp user32.lib
```

DR hook 版本，請在 `hookproc.cpp` 的編譯指令上加 `/DUSE_DR_HOOK`。

> 三者（hookproc.dll、installer.exe、以及你的測試 target）必須**全部都是 x64**。

## 使用方式

1. 編譯並執行你自己的測試 `target.exe`（一個會回報自身焦點狀態的簡單視窗 ——
   本專案未附，請自行寫一個最小範例，以便在安全環境下實驗）。
2. 把 `hookproc.dll` 放在 `installer.exe` 找得到的位置（同一個資料夾）。
3. 執行：
   ```bat
   bin\installer.exe TARGET
   ```
   （`TARGET` = 目標視窗標題裡的關鍵字。）
4. 用 [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview)
   觀察 `[SUBCLASS]` / `[DRHOOK]` / `[HOOKPROC]` 的訊息。
5. 在 installer 視窗按 Enter 即可乾淨卸載。

## 檔案

| 檔案            | 用途                                                  |
|-----------------|-------------------------------------------------------|
| `hookproc.cpp`  | hook DLL（進入點 + 改寫手法）。                        |
| `installer.cpp` | 安裝 hook、設定視窗旗標、處理乾淨卸載。                |
| `build.bat`     | 一鍵編譯，可加 `dr` 參數切換模式。                     |

## 說明與限制

- `WH_CALLWNDPROC` 本身無法攔截／改寫訊息，它只是載入 DLL 的進入點。
- 這裡用 `SetWindowLongPtr` 做子類化是為了清楚易懂；正式環境應改用
  `SetWindowSubclass` / `RemoveWindowSubclass`，以正確處理子類化鏈與 DLL 卸載。
- DR hook 版本只會對「安裝當下已存在的執行緒」設定中斷點，並使用 CPU 的
  debug register（只有 4 個，且與偵錯器共用）。
- 乾淨卸載很重要：若 DLL 卸載時視窗的 WndProc 仍指向 DLL 內部，目標程序會 crash。
  本專案在 `DLL_PROCESS_DETACH` 還原狀態，並在卸載時推動目標處理訊息以觸發還原。

## 授權

MIT —— 見 [LICENSE](LICENSE)。
