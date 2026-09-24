# CanoScan N1220U button daemon for macOS

A small, standalone macOS daemon that detects button presses on the Canon
CanoScan N1220U (USB `04a9:2207`) and runs a script in response — e.g. to
trigger a scan. It's a single C file against libusb-1.0 (native on
macOS/Apple Silicon) with no other dependencies, so it doesn't need the
Linux-oriented autotools build of the project it's derived from (dbus,
libconfuse, udev/hal have no real macOS equivalent).

The button-read protocol (the 4-byte query command and status bitmask) and
the libusb interface/endpoint discovery pattern are ported from the
"Plustek USB" scanbuttond backend of [scanbd](https://github.com/mdengler/scanbd),
a GPLv2+ Linux scanner-button daemon (`plustek.c` and `libusbi.c`). This
code is released under the same license (GPLv2 or later).

## How it works

`canoscan_n1220u_buttond.c` opens the scanner directly via libusb-1.0,
finds its bulk IN/OUT endpoints, and every 500ms repeats the same 4-byte
query the original Linux driver uses: write `{1, 2, 0, 1}` to the bulk-OUT
endpoint, read 1 status byte from bulk-IN, and test bit `0x04`. On a rising
edge it forks and execs an action script, passing it `SCANBD_DEVICE` and
`SCANBD_ACTION` environment variables (the same names scanbd itself uses
for its action scripts).

### The scanner must be "primed" via SANE first

Without a prior SANE session, the button-status query always reads back
`0x00` — the original scanbd project's `plustek.c` backend notes the same
thing. Verified on real hardware: with a fresh USB connection, the daemon
read a steady `0x00` and never changed even with the button physically
held down. Running `scanimage -L` (from Homebrew's `sane-backends`, which
includes the `plustek` SANE backend that recognizes this model) once
against the device is enough — after that, the raw status byte reflects
real state (idle `0x63`, pressed `0x66`/`0x67`, bit `0x04` set) and
press/release transitions show up immediately.

`canoscan-n1220u-buttond.sh` handles this automatically: it runs
`scanimage -L` before execing the daemon.

### USB interface handoff to the action script

The daemon holds the scanner's USB interface claimed while polling. If the
action script itself talks to the scanner (e.g. via `scanimage`), that
would normally fail with a "device busy" conflict — libusb only allows one
claimant at a time. To avoid this, the daemon releases the interface and
closes its device handle *before* running the action script, waits for the
script to finish, then reopens and reclaims the interface to resume
polling. This means button polling pauses for the duration of the script
(fine for a scan, which takes tens of seconds anyway). Verified end-to-end:
`scan.script` (below) triggers a real `scanimage` scan on button press,
and a second press right after produces a second scan with no conflicts.

### Debounce

The button-status bit comes from a self-clearing hardware latch: while
physically held down it can alternate between "pressed" and "idle" on
successive 500ms polls (observed directly on this hardware). The daemon
requires 3 consecutive idle polls before re-arming, so one physical tap
reliably fires the action script exactly once, not once per poll. The very
first poll after startup is never treated as an edge (a stale "pressed"
reading was observed as a side effect of the SANE priming call itself).

## Build

Requires libusb (`brew install libusb`) and a C compiler.

```sh
make
```

## Try it without installing anything

```sh
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:$PATH"   # for scanimage
scanimage -L                                                # prime the scanner
./canoscan-n1220u-buttond ./scan.script
```

Press the scanner's button; you should see a `button pressed` line, and
`scan.script` will scan a page (see below). Ctrl-C to stop.

Set `BUTTOND_DEBUG_RAW=1` to log the raw status byte on every poll, useful
if you're adapting this to a different Plustek-chipset model.

## Customize the action

`scan.script` scans a page via `scanimage` (color, 300dpi, full A4 area)
and saves it to `~/Desktop/Scans/scan-<timestamp>.png`, with a macOS
notification on success/failure. It resolves the SANE device name
dynamically (bus/device numbers can change across replugs), and cleans up
the output file if the scan fails. Edit it directly to change the
resolution/mode/scan area/output location, or point the daemon at your own
script entirely. Either way it receives:

- `SCANBD_DEVICE` = `canoscan_n1220u`
- `SCANBD_ACTION` = `scan`

## Install as a launchd agent (auto-start)

```sh
sudo mkdir -p /usr/local/etc/canoscan-n1220u-buttond /usr/local/var/log
sudo cp canoscan-n1220u-buttond /usr/local/bin/
sudo cp canoscan-n1220u-buttond.sh /usr/local/bin/
sudo cp scan.script /usr/local/etc/canoscan-n1220u-buttond/
sudo chmod +x /usr/local/bin/canoscan-n1220u-buttond.sh /usr/local/etc/canoscan-n1220u-buttond/scan.script

cp com.canoscan-n1220u-buttond.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.canoscan-n1220u-buttond.plist
```

Logs go to `/usr/local/var/log/canoscan-n1220u-buttond.log`.

To uninstall:

```sh
launchctl unload ~/Library/LaunchAgents/com.canoscan-n1220u-buttond.plist
rm ~/Library/LaunchAgents/com.canoscan-n1220u-buttond.plist
```

## Known limitations

- Only tested against the N1220U (`04a9:2207`, 1 button). scanbd's
  `plustek.c` backend supports several other Plustek-chipset devices
  (CanoScan LiDE 20/25/30, Epson Perfection 1260, etc.) with different
  button-bit layouts for multi-button models; this daemon only implements
  the single-button case. See `scanbtnd_get_button()` in that file's
  `switch (scanner->num_buttons)` if you want to extend it.
- If the Mac sleeps or the scanner is unplugged/replugged, the daemon will
  error out on the next poll and exit; `KeepAlive` in the launchd plist
  restarts it, and the wrapper script re-primes via SANE on every start.
