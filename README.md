# cGotchi

Lightweight ncurses Wi-Fi companion. It watches nearby access points, keeps a tiny pet mood around what the air looks like, and logs new SSIDs and local IPv4 addresses.

Single file: `cGotchi.c`. Built for Debian-like boxes with `nmcli` (WalnutOS, Raspberry Pi OS, desktop Debian/Ubuntu).

## What it does

- Lists nearby APs (SSID, signal, security, frequency, BSSID)
- Pins the scan to one Wi-Fi radio, or merges **all** Wi-Fi interfaces
- Treats new BSSIDs as events and appends them to a log
- Records IPv4 addresses as they appear on local interfaces
- Saves pet name, mood stats, scan counts, and radio choice in `~/.cgotchi`

It is a passive observer. It does not join networks, deauth, inject, or crack handshakes.

## Requirements

- gcc
- ncurses (`libncurses-dev` / `libncursesw5-dev`)
- NetworkManager + `nmcli` on `PATH`
- A Wi-Fi interface NM can see (`nmcli device` shows `wifi`)

```bash
sudo apt install -y build-essential libncurses-dev network-manager
```

On Walnut Pi / WalnutOS the onboard radio is usually `wlan0` (Unisoc). A USB stick such as Realtek RTL8188ETV (`0bda:0179`) shows up as `wlan1` once `r8188eu` or `rtl8xxxu` is bound.

## Build

```bash
make
./cGotchi
```

Install to `/usr/local/bin`:

```bash
sudo make install
```

Clean:

```bash
make clean
```

## Keys

| Key | Action |
|-----|--------|
| `h` | Help |
| `Enter` | Info panel for the highlighted AP |
| `s` | Scan using the current radio (no forced rescan) |
| `R` | Force `nmcli device wifi rescan`, then list |
| `w` | Radio picker |
| `↑` / `↓` | Move the AP highlight |
| `a` | Toggle auto-scan (~25 s) |
| `l` | Toggle log pane vs IP pane |
| `n` | Rename the pet |
| `o` | Hide / show open APs |
| `i` | Refresh local IPv4 list |
| `q` | Quit |

No on-screen key legend. Press `h` for help.

## Radio picker (`w`)

| Entry | Scan command |
|-------|----------------|
| **auto** | `nmcli device wifi list` (NM default radio) |
| **ALL** | rescan + list on every `wifi` device, merge by BSSID, keep the stronger RSSI |
| **wlan0**, **wlan1**, … | `nmcli device wifi list ifname <iface>` |

Choice is stored in `~/.cgotchi` and shown in the header (`auto` / `ALL` / `wlan1`). Each AP row can show which iface heard it.

ALL is sequential, not simultaneous. Same BSSID seen on two sticks becomes one row.

Disable MAC randomization if a Realtek stick returns an empty scan:

```bash
sudo tee /etc/NetworkManager/conf.d/80-wifi.conf >/dev/null <<'EOF'
[device]
wifi.scan-rand-mac-address=no
EOF
sudo systemctl restart NetworkManager
```

## Files

| Path | Purpose |
|------|---------|
| `~/.cgotchi` | Pet state + selected radio |
| `/home/working/ogotchi_log.txt` | Append-only SSID / IP log |

Log lines look like:

```
2026-09-28T06:18:01 SSID  ExampleNet  -42 WPA2  2437 MHz  aa:bb:cc:dd:ee:ff  wlan1
2026-09-28T06:18:01 IP    192.168.1.40  wlan0
```

Change `LOG_PATH` at the top of `cGotchi.c` if `/home/working` is not what you want.

## Layout

```
cGotchi (^_^)                 curious AUTO wlan1
Observer  0.1d
bor [==......]                n12 i3 s4
exc [====....]                New signals on this block.
en  [=======.]
AP                            IP
 *CafeNet   -41 wlan1         192.168.1.40
  Home      -58 wlan0
```

Yellow rows are first-seen BSSIDs. Highlighted row is the current cursor. Enter opens INFO (SSID, BSSID, signal, security, frequency, band, channel, radio, first-seen). `h` opens HELP.

## Makefile

```
make            # gcc -O2 -Wall -Wextra -s -o cGotchi cGotchi.c -lncurses
make install    # PREFIX=/usr/local
make clean
```

Override if needed: `make CC=gcc CFLAGS='-O2 -g'`.

## Notes

- Needs a real terminal. SSH is fine; pipe-to-file is not.
- `nmcli` must be able to scan without a TTY prompt. Run as a user in the right groups, or use a system that already talks to NetworkManager.
- Two kernel modules bound to one Realtek stick (`r8188eu` and `rtl8xxxu`) can fight. Blacklist one:

```bash
echo 'blacklist rtl8xxxu' | sudo tee /etc/modprobe.d/blacklist-rtl8xxxu.conf
sudo modprobe -r rtl8xxxu
```

- 2.4 GHz USB N sticks will not see 5 GHz APs. ALL still only sees what each radio can hear.
