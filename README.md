# win-focus-hook

A small educational project demonstrating how a window-focus "keep active" hook
works on Windows — the same kind of mechanism used by tools like DisplayFusion's
"prevent window deactivation" feature.

It shows two things:

1. **A clean entry point** — using `SetWindowsHookEx(WH_CALLWNDPROC)` to have the
   system load a DLL into a target process, instead of remote-thread injection.
2. **Two interchangeable modification techniques** (selected at compile time):
   - **Default**: subclass the target window's `WndProc` and rewrite focus-related
     messages (`WM_KILLFOCUS`, `WM_ACTIVATE`, `WM_ACTIVATEAPP`, `WM_NCACTIVATE`).
   - **`USE_DR_HOOK`**: a debug-register (DR0) + VEH hook on `GetForegroundWindow`,
     which modifies the API's return value without changing any code bytes.

> **Educational / research use only.** This project is meant for understanding
> Windows internals and is demonstrated against a small test program you build
> yourself (`target.exe`). Do **not** use it against software protected by
> anti-cheat or other integrity systems — injecting into such processes (by any
> method, including `SetWindowsHookEx`) is detectable and may get your account
> banned or violate that software's terms.

## How it works

```
installer.exe
   1. find the target window, get its thread id
   2. SetPropW(target, "__KeepWindowActive__", 1)   <- flag: keep this window active
   3. InstallHook(targetThreadId)
        -> SetWindowsHookEx(WH_CALLWNDPROC, ..., threadId)   [clean entry point]
           the system loads hookproc.dll INTO the target process

inside the target process, hookproc.dll then applies ONE of:
   - [default]      subclass the window's WndProc, rewrite focus messages
   - [USE_DR_HOOK]  set DR0 on GetForegroundWindow + VEH, fake the return value
```

The `WH_CALLWNDPROC` hook itself is read-only and cannot modify messages — it is
used only as the entry point. The actual rewriting happens via subclassing (or
the DR hook) inside the target.

## Build

Open **"x64 Native Tools Command Prompt for VS"** (MSVC), then:

```bat
:: default mode (subclassing, rewrites window messages)
build.bat

:: debug-register hook mode (hooks GetForegroundWindow, no bytes changed)
build.bat dr
```

Or manually:

```bat
cl /nologo /EHsc /LD /Fo"obj/" /Fe"bin/" hookproc.cpp user32.lib
cl /nologo /EHsc /Fo"obj/" /Fe"bin/" installer.cpp user32.lib
```

For the DR hook variant, add `/DUSE_DR_HOOK` to the `hookproc.cpp` command.

> All three (hookproc.dll, installer.exe, and your test target) must be **x64**.

## Usage

1. Build and run your own test `target.exe` (a simple window that reports its
   focus state — not included here; write a minimal one to experiment safely).
2. Put `hookproc.dll` where `installer.exe` can find it (same folder).
3. Run:
   ```bat
   bin\installer.exe TARGET
   ```
   (`TARGET` = a keyword in the target window's title.)
4. Use [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview)
   to watch the `[SUBCLASS]` / `[DRHOOK]` / `[HOOKPROC]` messages.
5. Press Enter in the installer to uninstall cleanly.

## Files

| File            | Purpose                                                        |
|-----------------|----------------------------------------------------------------|
| `hookproc.cpp`  | The hook DLL (entry point + modification technique).           |
| `installer.cpp` | Installs the hook, sets the window flag, handles clean removal.|
| `build.bat`     | One-command build, with optional `dr` argument.                |

## Notes & limitations

- `WH_CALLWNDPROC` cannot itself intercept/modify messages; it is only the
  loader entry point.
- Subclassing with `SetWindowLongPtr` is used for clarity; production code should
  prefer `SetWindowSubclass` / `RemoveWindowSubclass` for proper subclass-chain
  and unload handling.
- The DR-hook variant only arms threads that exist at install time, and uses
  CPU debug registers (limited to 4, and shared with debuggers).
- Clean uninstall is important: if the DLL unloads while the window's WndProc
  still points into it, the target will crash. This project restores state in
  `DLL_PROCESS_DETACH` and nudges the target to process messages on uninstall.

## License

MIT — see [LICENSE](LICENSE).
