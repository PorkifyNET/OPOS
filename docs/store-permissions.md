# Permissions when installing from the OPOS Store

Before, a Store program asked for each permission (network, reading the
clipboard, shared files, notifications) the first time it needed it, and
the Store only listed them as "MAY ASK FOR". Now the Store asks first,
when you install or update a program, so you can decide in one place
before the program runs. Leaving a permission to be asked about later
works as before.

## What the person sees

INSTALL (or UPDATE) on a program's About page opens a sheet:

- The program's icon and name and "MAY ASK TO USE:", or "MAY NOW ASK TO
  USE:" for an update.
- One row per permission: its name, what it lets the program do, and
  three choices: **ALLOW**, **DENY** and **ASK WHEN NEEDED**. Every row
  starts on ASK WHEN NEEDED.
- A note that ASK WHEN NEEDED means the program asks the first time it
  needs the permission, and that the Security Center changes any of
  this later.
- INSTALL (or UPDATE) and CANCEL. The keyboard starts on INSTALL, so
  Enter installs with every permission left to be asked about, as before.

Keys: Up, Down and Tab move between rows and buttons, Left and Right
change a row's choice or switch between INSTALL and CANCEL, Enter or
Space presses a button (on a row, Enter moves to the next one), Esc
cancels.

The sheet does not open when there is nothing to decide: the program
asks for nothing, or this account already allowed or denied everything
it asks for. An update lists only what is still undecided, so choices
made earlier (in the sheet, when the program asked or in the Security
Center) are kept.

A permission this OPOS does not know (from a newer Store) is listed as
"SOMETHING THIS OPOS DOES NOT KNOW" without choices: it cannot be given.
An update lists it only next to something else to decide.

## How it works

- The rows come from the program's `asks:` line in the signed index
  (`docs/packages.md`), so a Store mirror cannot change them. Rows already
  decided for this account are left out.
- The choices are saved with `permission_grants_decide()` for the
  program's Store grant key (`store:<id>`), the same record the program's
  own requests and the Security Center use. ASK WHEN NEEDED saves nothing.
- They are saved only after `pkg` installed the program (it ended with
  status 0), so a failed or cancelled install leaves no record. If they
  could not be saved, the Store says so and points to the Security Center.
- DENY on an update also takes the permission from copies of the program
  that are already running, as the Security Center does.
- Removing a program forgets its record, as before, so installing it
  again opens the sheet again.
- UPDATE ALL and `pkg install` / `pkg update` in the Terminal do not show
  the sheet: what they install asks when needed, as before.
- All of it is in the Store window (`kernel/desktop.c`, "The permissions
  sheet"). The package format, the index and `pkg` are unchanged.

## Testing

In QEMU, with a test Store holding a package whose manifest lists all
four permissions:

- INSTALL shows four rows on ASK WHEN NEEDED. With NETWORK set to ALLOW
  and NOTIFICATIONS to DENY, the Security Center lists the program with
  NETWORK ALLOWED, NOTIFY DENIED and the others NOT ASKED.
- A package that asks for nothing (Minesweeper) installs at once.
- Esc on the sheet installs nothing.
- Version 1.1 of the test package: UPDATE shows only SHARED FILES and
  READING THE CLIPBOARD ("MAY NOW ASK TO USE:"), and the earlier choices
  are kept after the update.
- In a narrow Store window, the choices go below each row's text.
