# System sounds and notifications

Two items from `agents/agent-notes.md`:

- **Audio:** sounds for notifications, startup, shutdown and similar
  events.
- **Notification Center:** notifications show briefly, unless system
  settings or a running program suppress them. Afterwards they move to a
  Notification Center, where the person can read them again and dismiss
  them.

Both build on the sound work in `docs/sound.md`. A notification plays a
sound, so the sounds come first.

## Design

- **Sounds are made in the kernel, not loaded from files.** Each system
  sound is a short sum of sine tones with an envelope, rendered once at
  boot into a mono 48 kHz buffer (each under 1 s, about 100 KB in all).
  This needs no audio files, licenses or disk access, so the sounds also
  work on the live ISO and before a disk mounts. The sounds can be
  replaced with recorded ones later without changing the callers.
- **A kernel voice in the mixer.** `kernel/sound.c` gets one system voice
  next to the beep. It plays a rendered buffer over the program streams,
  scaled by the master volume, so it never takes one of the 8 program
  stream slots. A new sound replaces the one playing.
- **The events are:** startup (desktop shown after login or autologin),
  log out, shut down and restart, notification, warning (error and
  permission dialogs) and a short click when a volume key changes the
  level. Shut down and restart wait for their sound to finish, up to
  1.5 s, before the machine powers off.
- **Settings.** A new Settings > SOUND page holds the master volume (it
  moves there from the volume keys alone), SYSTEM SOUNDS on or off, and
  DO NOT DISTURB. These are personal settings, kept in the account's
  `AppData/DESKTOP.CFG` (`sounds=`, `dnd=`), as the CMOS settings byte has
  no free bits. The live session keeps them in memory.
- **Notifications are kept by the kernel** (`kernel/notify.c`): up to 64
  per session, each with a source (the program's name, or OPOS), a title,
  a body, a time and a read flag. They are cleared at log out and not
  saved to disk.
- **Toasts.** A new notification shows as a toast in the screen corner
  next to the taskbar's clock end for 5 s, and plays the notification
  sound. Several stack, at most 3 at a time. Clicking a toast opens the
  Notification Center; Escape or its close box dismisses it.
- **Suppression.** No toast and no sound appear while DO NOT DISTURB is
  on, or while the front window asked not to be disturbed (a full-screen
  game, a presentation). The notification still goes to the Notification
  Center, unread.
- **Notification Center.** A panel along the screen edge, opened by a
  bell button on the taskbar (showing the unread count) or Super+N. It
  lists notifications newest first, marks them read when opened, and
  dismisses one (Delete or its close box) or all (CLEAR ALL). It also
  holds the DO NOT DISTURB toggle.
- **Programs.** Programs post notifications with a new OPOS call. Disk
  programs need a new permission, `notifications`, which they ask for on
  first use like the network permission; the Security Center lists and
  revokes it. Built-in programs that need it hold it. The kernel limits a
  program to 10 notifications a minute, and shows the executable's name
  next to any name the program chooses (as the permission prompts do).
- **OPOS itself notifies** when an OPOS update or package updates are
  available (the checks the UPDATES page and the Store already make), when
  a package install finishes, and when a program crashes.

## Steps

1. **System sounds.** The system voice in `sound.c`, the rendered sounds
   (`kernel/system_sounds.c`), the desktop calling them for startup, log
   out, shut down, restart, warning and volume keys, and Settings > SOUND
   with the volume and SYSTEM SOUNDS. Test: QEMU `wavcapture` and
   `tools/check_tones.py` for each event, then by ear in VMware.
2. **Notifications and toasts.** `kernel/notify.c`, the toasts, DO NOT
   DISTURB on the SOUND page, and OPOS's own notifications (updates,
   package installs, crashes). A kernel-shell command `notify TITLE BODY`
   for testing.
3. **Notification Center.** The taskbar bell with the unread count, the
   panel, Super+N, dismissing and CLEAR ALL.
4. **Programs.** OPOS calls `0x4f500050` notify(title, body) and
   `0x4f500051` do-not-disturb(window, on), SDK `opos_notify` and
   `opos_window_quiet`, the `notifications` permission in manifests,
   prompts and the Security Center, and `/bin/notify TITLE BODY` for the
   Terminal and scripts. SDL programs can mark their window quiet, and
   the Doom package does so while a game is running.

Each step is one PR, stacked like the sound work.

## As built in step 1

- `kernel/system_sounds.c` holds six sounds: startup (C5 E5 G5 C6 rising,
  1.2 s), shutdown (G5 E5 C5 falling, 1.2 s), logout (E5 C5, 0.8 s),
  notification (A5 E6, 0.8 s), warning (B4 G4, 0.65 s) and volume (a
  45 ms click at 1600 Hz). Each note is a sine plus a quarter-level
  second harmonic, with a 5 ms attack, an exponential decay and a 10 ms
  fade at its end. The sine comes from Bhaskara's approximation in
  fixed point (the kernel builds with `-mgeneral-regs-only`). A sound is
  rendered into one 1.3 s buffer when it is played.
- `kernel/sound.c` mixes the system voice into both channels before the
  master volume, so muting silences it too. `sound_voice_play` replaces
  the sound playing; the renderer stops the voice before it reuses the
  buffer.
- The desktop plays: startup once per boot when the desktop or login
  screen first shows; logout before the account's windows close (with
  that account's setting); shutdown before shutting down or restarting
  from the Start menu, the login screen or UPDATES > RESTART NOW, waiting
  for it up to 1.5 s; warning when the administrator password prompt or a
  program permission prompt opens; volume after each volume step. The
  kernel shell's `reboot` and `shutdown` stay silent.
- Settings > SOUND has VOLUME (5% steps, as the volume keys), MUTE and
  SYSTEM SOUNDS. SYSTEM SOUNDS is saved as `sounds=on|off` in
  DESKTOP.CFG and plays the notification sound when turned on. The
  volume stays machine-wide (OPOS.CFG and CMOS, as in step 2 of
  `docs/sound.md`).
- Kernel shell: `chime NAME` plays a sound even when SYSTEM SOUNDS is off.
- Tested in QEMU (KVM, HDA, `wavcapture`): each `chime` was recorded,
  and every note appears at its frequency and start time (Goertzel
  filter per 50 ms window), with no clipping at volume 100. Booting to
  the desktop played the startup sound. Settings > SOUND was checked on a
  screenshot. The shutdown sound was not checked automatically (the
  Start menu needs the mouse, which the QEMU harness cannot move).

## As built in step 2

- `kernel/notify.c` keeps the session's last 64 notifications (source up
  to 31 characters, title 63, body 191; control characters become
  spaces), each with an id, the RTC time, a read flag and a "toast
  pending" flag. Changes and copies run with interrupts off, so program
  threads can post in step 4. Log out clears the list.
- Toasts (desktop.c "toasts"): at most 3, the newest by the clock, the
  others stacked away from the taskbar (and clear of the volume popup).
  Each shows source and time, title, and the body wrapped to two lines
  (cut with "..."). A toast stays 5 s; a fourth notification waits for a
  free slot. One notification sound plays per desktop pass, however many
  toasts it opens. Clicking a toast marks it read and hides it (step 3
  makes this open the Notification Center); its X dismisses the
  notification. New toasts wait while the login screen or a password or
  permission prompt is up.
- DO NOT DISTURB is on Settings > SOUND, saved as `dnd=on|off` in
  DESKTOP.CFG. While on, new notifications skip the toast and the sound
  and stay unread in the list.
- OPOS notifies:
  - "OPOS x.y.z IS AVAILABLE" when an update check finds a new version
    and the UPDATES page is not the front window (once per version);
  - "N PACKAGE UPDATES" when the Store's list shows updates and the Store
    is not in front (again only when the number changes);
  - the Store's result line ("INSTALLED ...") when an install or removal
    ends while the Store is not in front;
  - "NAME STOPPED UNEXPECTEDLY" when a program started from the desktop
    ends on SIGILL, SIGABRT, SIGBUS, SIGFPE or SIGSEGV.
  No background update check was added: these come from the checks the
  UPDATES page and the Store already run.
- The desktop Terminal's `notify TITLE [BODY]` posts a test notification
  (the first word is the title), from source TERMINAL.
- Tested in QEMU (KVM, HDA): one long notification wrapped and was cut
  correctly, the notification sound was recorded (A5 then E6); four
  quick notifications showed three toasts with the newest by the clock,
  the fourth appeared when the first expired, and all were gone after
  5 s. With DO NOT DISTURB on, `notify` showed no toast and the
  recording was silent. Not tested: clicks on toasts (the QEMU harness
  cannot click), the update, Store and crash notifications.

## As built in step 3

- A bell on the taskbar, left of the network icon (or of the clock), is
  orange with the unread count beside it ("9+" above nine) and grey when
  everything is read. On a side taskbar it sits above the network icon.
  Window buttons stop before it.
- The bell, Super+N, or a click on a toast opens the Notification Center:
  a panel by the clock, as wide as a toast, with a NOTIFICATIONS (N)
  header, a DND ON/OFF button (the same setting as Settings > SOUND) and
  CLEAR ALL. Entries are listed newest first in toast layout; unread ones
  have an orange bar on the left. It grows to fit the screen and shows
  "1-5 OF 9" when they do not all fit; the wheel scrolls.
- Keys while it is open: Up/Down select, Delete dismisses, Escape closes;
  other keys are taken by the panel. Mouse: the X on an entry dismisses
  it, a click outside closes the panel and still reaches what was under
  it.
- Closing the panel marks every notification read. While it is open, new
  notifications go straight into the list (with the sound unless DO NOT
  DISTURB), without toasts. Opening it hides the toasts.
- Step 2 fix: a program that ends on a CPU exception exits with
  128 + the exception vector (`user_exception_dispatch`), not
  128 + a signal number. Crash notifications now also cover divide
  errors, stack faults, general protection faults (141), page faults
  (142), alignment checks and SIMD errors; 134 reads "an invalid
  instruction or SIGABRT".
- The desktop Terminal's new `fault` command runs the kernel's page-fault
  self-test program (the kernel shell's `run fault`), to check the crash
  notification.
- Tested in QEMU: after five `notify` commands the bell showed 5; Super+N
  opened the panel with all five unread; Down then Delete removed
  "Four" and the bell showed 4; Escape closed it and the bell turned
  grey. `fault` posted "FAULT-TEST STOPPED UNEXPECTEDLY / IT ENDED WITH A
  PAGE FAULT". Not tested: mouse clicks (the QEMU harness cannot click)
  and scrolling past a full panel.

## Not planned yet

Notification actions (buttons inside a notification), notifications that
survive a reboot, per-program sound choices, and Linux desktop
notifications (D-Bus `org.freedesktop.Notifications`).
