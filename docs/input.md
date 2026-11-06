# Input, timing and interrupts

The reference machine uses legacy devices: 8259 PIC pair, 8253/8254 PIT and
the i8042 PS/2 controller. APIC/IOAPIC support is roadmap work; the only APIC
code in 0.1 puts the local APIC into virtual-wire mode (see boot.md).

## PIC

Remapped to vectors 0x20–0x2F, everything masked, then only installed sources
are unmasked: IRQ0 (PIT), IRQ1 (keyboard), IRQ2 (cascade, when a slave IRQ is
used) and IRQ12 (mouse). EOI is sent to the slave and then the master for
IRQs ≥ 8. Spurious IRQ7/IRQ15 are detected by reading the ISR; a spurious
IRQ15 still gets a master EOI.

## Timer

PIT channel 0, mode 2, divisor 4773 → 1193182 / 4773 = 249.98 Hz. The IRQ
handler increments a 64-bit tick counter (release store); readers use acquire
loads. Milliseconds are computed from ticks × divisor / 1.193182 MHz, not from
the nominal 250 Hz. Uptime in the menu bar, Terminal and About comes from this
counter and is labelled "Uptime" — there is no wall clock.

## PS/2 controller

Initialisation runs before `sti`, polling the controller with bounded
timeouts (200 000 polls ≈ 200 ms worst case per byte):

1. disable both ports, flush the output buffer;
2. read the configuration byte, disable IRQs, **enable set-2→set-1
   translation**, write it back; controller self-test, re-write config;
3. detect the aux port; run interface tests;
4. keyboard: reset (expect ACK + 0xAA), select scan code set 2, enable
   scanning — each command waits for 0xFA and retries on 0xFE;
5. mouse (via 0xD4 prefix): reset, defaults, enable reporting;
6. enable IRQ1 (and IRQ12 if the mouse acknowledged).

All ACK/self-test bytes are consumed here, so they never enter the input
stream. The decoder additionally discards 0xFA/0xFE/0xEE/0x00/0xFF if one ever
appears later (but not 0xAA, which is the Left-Shift break code in set 1).

IRQ1 and IRQ12 share one routine: while the status register has output
available, read a byte and route it by status bit 5 (AUX) to the mouse or
keyboard ring. Rings are 512 bytes, single-producer/single-consumer; when full
the **newest** byte is dropped and counted (`mem` shows the counters).

QEMU's emulated i8042 buffers only 16 scancode bytes itself; input injected
faster than the guest drains it is dropped by the emulator before it reaches
the kernel. The integration test sends bursts sized to that buffer and checks
the kernel-side drop counters stay at zero.

## Keyboard (US layout)

Scan code set 1 (translated). Handles make/break, `E0` extended keys (arrows,
Home/End, PgUp/PgDn, Insert/Delete, right Ctrl/Alt, GUI, keypad Enter and /),
the `E1` Pause sequence, "fake shift" `E0 2A/AA`, both Shift/Ctrl/Alt keys
independently (no stuck modifiers when one is released) and Caps Lock (toggles
once per physical press, ignores typematic repeats, affects letters only).
Keypad navigation keys work as arrows (NumLock is not tracked). The desktop
drives the Caps Lock LED (`ED` command, bounded waits); Num/Scroll Lock LEDs
are not tracked.

## Mouse

Standard 3-byte packets (no wheel negotiation). Synchronisation: the first
byte must have bit 3 set, otherwise it is skipped; a partial packet older than
50 ms is discarded. X/Y are 9-bit two's complement using the sign bits; if an
overflow bit is set the motion is discarded and buttons kept. Y is inverted
for screen coordinates. The cursor is clamped to the screen. No acceleration
is applied, which keeps test positioning deterministic. Left button: click,
drag (title bar to move, lower-right grip to resize, text in Notes to select);
right button and wheel are not used.

## Event queue and routing

Decoded events go into a 256-entry FIFO. Consecutive motion events with the
same button state are coalesced (deltas summed, saturating at int16); when
still full, new events are dropped and counted. The desktop then routes:

* global shortcuts (Ctrl+Tab, Ctrl+W, Ctrl+M, Ctrl+1..4, Esc for menus);
* mouse press: open menu → menu bar → dock → topmost window (focus + raise);
  title bar starts a drag with pointer capture until release, controls act on
  release over the same control, content gets `MOUSE_DOWN` with a click count
  (double click: < 450 ms and ≤ 5 px);
* keys go only to the focused, non-minimized window.

## Idle

```
cli
if (no bytes in rings, no queued events, no damage) sched_block(until next 250 ms tick)
sti
```

The desktop is task 0. Blocking with interrupts off and being woken by the
PS/2 IRQ (or by a process writing to the Terminal) means no input is missed.
User processes and the network task run while it sleeps; the idle task halts
with `sti; hlt` when nothing is runnable.
