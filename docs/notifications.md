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

## Not planned yet

Notification actions (buttons inside a notification), notifications that
survive a reboot, per-program sound choices, and Linux desktop
notifications (D-Bus `org.freedesktop.Notifications`).
