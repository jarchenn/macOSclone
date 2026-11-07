# Desktop (0.7)

Since 0.7 the desktop is ordinary user space. The kernel provides the
devices and IPC; everything visible is drawn by processes.

```
 kernel                          user space
 ──────                          ──────────
 /dev/display  ◄── present ───── display (pid 2, started and restarted by init)
   scan-out, mode setting          compositor, window manager, decorations,
 /dev/input    ──── events ───►    menu bar, dock, cursor layer, keymaps,
   PS/2 scan codes, mouse+wheel    key repeat, clipboard, app launcher
 shared memory ◄──── buffers ────┐     ▲  local socket "system.display"
 local sockets ◄──── messages ───┤     │  (gui/proto.h)
 /dev/ptmx     ◄──── pty ────────┤   terminal  files  notes  settings  welcome
                                 └── each a process linked with the client
                                     library and toolkit (user/gui/lib)
```

## Source layout

| Path | Contents |
|---|---|
| `user/gui/server/` | display server: `main.c` (input routing, menus, dock, clipboard, launcher, settings), `wm.c` (windows, configure exchange, decorations), `compose.c` (scene + cursor layer, scan-out), `keymap.c` |
| `user/gui/lib/` | client library (`client.c`), toolkit (`tk.c`), file dialogs (`dialogs.c`), POSIX-backed VFS calls for the 0.1 apps (`ufs.c`), text buffer, app helpers |
| `user/gui/common/` | shared by server and apps: TrueType text (`text.c`), theme and wallpaper, drawing widgets, protocol helpers |
| `user/gui/apps/*` | Terminal, Files, Notes (the editor), Settings, Welcome |
| `kernel/graphics/{rect,surface}.c` | drawing primitives, compiled into both the kernel (console, panic screen) and user space |
| `kernel/drivers/display.c`, `input_dev.c`, `kernel/core/local.c`, `memory/uvm.c` (shm) | the kernel side |
| `initramfs/system/keymaps/` | `us.kmap`, `us-intl.kmap` |

## Kernel interfaces

* **Shared memory:** `SYS_SHM_CREATE(size)` returns a descriptor;
  `mmap(MAP_SHARED)` maps it. Pages are allocated at creation, never
  copied on write, shared across `fork`. Limit 64 MiB per object.
* **Local sockets:** `SYS_SOCKETPAIR`, `SYS_LOCAL_LISTEN(name)`,
  `SYS_LOCAL_CONNECT(name)`, `SYS_ACCEPT`, `SYS_SENDMSG`/`SYS_RECVMSG`
  (message boundaries, up to 64 KiB and 8 descriptors per message). Each
  endpoint queues at most 256 KiB or 256 messages; senders block or get
  EAGAIN. `read`/`write`/`poll` work on them.
* **`/dev/display`** (root, one owner): `AURORA_DISP_INFO` (current mode,
  available modes), `AURORA_DISP_SET_MODE` (Bochs DISPI), `AURORA_DISP_ATTACH`
  (a shared-memory XRGB8888 buffer), `AURORA_DISP_PRESENT` (up to 32 damage
  rectangles, converted to the scan-out format). While no process owns it
  the kernel's diagnostic console draws on the screen.
* **`/dev/input`** (root, one owner): 16-byte records, key scan codes (set 1,
  `0x100|code` for `E0` prefixes) with press/release, mouse deltas, buttons
  and wheel (IntelliMouse 4-byte packets). `AURORA_INPUT_SET_LEDS`.
* **`/dev/ptmx`:** a pseudo-terminal whose master is the descriptor;
  `AURORA_TIOCOPENSLAVE` gives the slave, `AURORA_TIOCTAKEUNREAD` returns input
  a finished program left unread. `AURORA_SPAWN_SETSID` makes a spawned
  program a session leader with the pty as its controlling terminal.
* **`/dev/console`:** writes go to the kernel log (init and the server use it).
* **`SYS_POWER`** (restart, power off), **`SYS_KLOG`** (heap and storage
  counters), **`SYS_CPUINFO`**, **`SYS_PROCLIST`**, **`SYS_TEST_EVENT`**
  (test builds: `@@` markers).

## Display protocol (`user/gui/include/gui/proto.h`)

One local-socket message per protocol message, starting with
`{type, window}`. Pixels never travel in messages.

1. App: `C_HELLO` → server: `S_WELCOME` (screen size, work area, theme).
2. App: `C_CREATE` (size, minimum/maximum, flags, title) → server:
   `S_CONFIGURE(w, h, serial)` with the limits applied.
3. App allocates a shared buffer of that size, sends `C_ATTACH` with the
   descriptor, draws, and sends `C_COMMIT(buffer, ack_serial, damage)`.
   The window appears on its first commit.
4. The server composites from the committed buffer and sends `S_RELEASE`
   for the one it showed before. An app draws only into a buffer it holds,
   so there are always two: one on screen, one being drawn.
5. Resize, maximize and resolution changes send a new `S_CONFIGURE`; until
   the app commits a buffer of the new size (acknowledging the serial) the
   server shows the old buffer, clipped, with the window colour around it.

Input arrives as `S_KEY` (every press, repeats included), `S_TEXT`
(characters after the keymap, dead keys composed) and `S_MOUSE`
(down/up/move/wheel/leave, content coordinates, double clicks). The server
also carries the clipboard (`C_SET_CLIPBOARD`/`C_GET_CLIPBOARD`, UTF-8 text,
kept by the server), app launches (`C_LAUNCH`; apps are single-instance and
a running one gets `S_OPEN`), the theme, the resolution, the keymap and the
key-repeat settings (`C_GET_SETTINGS`/`S_SETTINGS`).

## Server details

* **Compositing:** a scene buffer (wallpaper, windows, menu bar, dock,
  menus) is recomposed only in damaged rectangles (at most 16, merged);
  the presented buffer is the scene plus the cursor. Frames are paced at
  most every 16 ms; cursor motion copies two small rectangles.
* **Decorations** are drawn by the server: title bar, close/minimize/
  maximize controls, shadow, border, resize grip (resizable windows only).
* **Focus and z-order:** click-to-focus; Alt+Tab (and Ctrl+Tab) bring the
  next window forward; Ctrl+W/M close or minimize; Ctrl+1..4 launch apps.
* **Keyboard:** layouts from `/system/keymaps/*.kmap` (US and
  US-International with dead keys and AltGr). Repeat delay and rate are
  configurable (Settings > Keyboard); Caps Lock drives the LED.
* **Resolution:** Settings > Display (or `display WxH` in the Terminal)
  changes the mode through `/dev/display`; every window is moved into the
  new work area, maximized and oversized ones are configured to fit, and
  apps get `S_SCREEN`.
* **Settings** persist in `/home/.display.conf` (theme, keymap, repeat).

## Text

`user/gui/common/text.c` rasterizes DejaVu Sans, Sans Bold and Sans Mono with
stb_truetype into a per-font, per-size glyph cache (512 entries), blends
coverage in linear light (sRGB → linear tables), applies pair kerning for the
proportional faces, and decodes UTF-8 (invalid bytes become U+FFFD). Layout
helpers measure, fit with an ellipsis, hit-test and word-wrap. If a font file
is missing, the 0.1 Spleen bitmap fonts are used (ASCII only).

## Toolkit and dialogs

`user/gui/lib/tk.c`: label, button, text field (UTF-8 editing, selection),
list (keyboard and wheel scrolling, double-click), scroll view, menu button;
a panel does hit testing, Tab focus order and mouse capture. The client
library supplies the event loop, timers and descriptor watches.
`dialogs.c` builds the open and save dialogs from it as sheets over the
app's window: folder browsing (Up, double-click or Enter), New Folder, a
name field for Save and an overwrite confirmation.

## Limits

* Software rendering only; one monitor; no integer 2x scaling.
* Latin text: no shaping, no right-to-left, no input method yet (text
  events are where one would plug in).
* At most 24 connected apps and 32 windows; apps are single-instance.
* Dialogs are sheets inside the app window, not separate windows.
