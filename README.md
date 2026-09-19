# vscreen — use a 5K iMac as a second monitor for a GNOME/Wayland laptop

```
Linux laptop (GNOME, Wayland)                          iMac 27" 5K (2014)
┌──────────────────────────────┐                 ┌──────────────────────────┐
│ Mutter virtual monitor       │                 │ VScreen.app              │
│   └─ PipeWire frames (BGRx)  │   Ethernet      │   TCP :7310              │
│       └─ H.264 (VAAPI/NVENC) ├───── TCP ──────►│   └─ H.264 hw decode     │
│           └─ vscreen-send    │   (gigabit)     │       └─ full screen     │
└──────────────────────────────┘                 └──────────────────────────┘
```

GNOME sees a real extra monitor (arrange it in Settings → Displays); the
mouse and keyboard just move onto it. The iMac only displays.

## Hardware link

A plain USB-A↔USB-A or USB-C↔USB-C cable **does not work** between two
computers (both are USB hosts), and USB "data link" bridge cables have no
network driver on macOS. Use Ethernet instead:

- **USB 3 → Gigabit Ethernet adapter** on the laptop (any Realtek RTL8153 or
  ASIX AX88179 one, ~$15)
- **Ethernet cable** straight into the iMac's Ethernet port (the iMac keeps
  using Wi-Fi for internet)

Gigabit gives ~940 Mbit/s; the stream uses ~40. Wi-Fi also works, with more
latency and jitter.

Give both ends a fixed address on the direct link:

```sh
# laptop — find the adapter name with `ip link` (e.g. enx00e04c68xxxx)
nmcli connection add type ethernet ifname enx00e04c68xxxx con-name vscreen \
    ipv4.method manual ipv4.addresses 10.77.0.1/24 ipv4.never-default yes ipv6.method disabled
```

iMac: System Settings → Network → Ethernet → Details → TCP/IP →
Configure IPv4 *Manually*, IP `10.77.0.2`, subnet `255.255.255.0`, router empty.

## iMac: VScreen.app

```sh
mac/build.sh
open mac/build/VScreen.app
```

It opens full screen and shows the addresses it's listening on.

- ⌃⌘F toggle full screen (swipe with three fingers / ⌃← → to get back to macOS)
- ⌘I show fps / bitrate
- ⌘S toggle sharp (nearest-neighbour) scaling — 2560×1440 maps exactly 2×2 onto the 5K panel
- other port: `open mac/build/VScreen.app --args -port 7400`

If the macOS firewall is on, allow incoming connections for VScreen.

## Laptop: vscreen-send

```sh
sudo apt install build-essential pkg-config libglib2.0-dev libpipewire-0.3-dev \
    libavcodec-dev libavutil-dev libswscale-dev
# GPU encoding: Intel → intel-media-va-driver-non-free, AMD → mesa-va-drivers
make -C linux
linux/vscreen-send 10.77.0.2
```

Options:

| flag | default | |
|---|---|---|
| `-s WxH` | 2560x1440 | virtual monitor size; 2560×1440 is pixel-exact 2× on the iMac |
| `-r FPS` | 60 | max frame rate (frames are only sent when the screen changes) |
| `-b MBIT` | 40 | H.264 bitrate |
| `-e ENC` | auto | `h264_vaapi`, `h264_nvenc`, `libx264` |
| `-d DEV` | /dev/dri/renderD128 | VAAPI device |
| `-p PORT` | 7310 | |

It retries until VScreen is reachable. The virtual monitor exists only while
connected, so windows move back to the laptop screen when the iMac app quits.
GNOME shows its screen-sharing indicator while it runs; stopping sharing from
there ends vscreen-send.

## How latency is kept low

- Mutter only emits a frame when the monitor content changes.
- The capture thread keeps only the newest frame; if encoding falls behind,
  intermediate frames are skipped rather than queued.
- H.264 with no B-frames and no lookahead; one packet out per frame in.
- If the kernel socket queue holds more than ~1/30 s of video, the sender
  waits and then encodes the newest frame instead of piling up.
- The iMac enqueues each frame with *display immediately* on a hardware
  decoding layer.

Expected glass-to-glass latency on gigabit Ethernet is a few tens of
milliseconds, dominated by the two displays' refresh.

## Requirements and limits

- GNOME on Wayland (uses `org.gnome.Mutter.ScreenCast.RecordVirtual`). KDE
  or wlroots desktops would need a different capture backend.
- H.264 only: the 2014 iMac has no hardware HEVC decoder. H.264 hardware
  decoding tops out around 4096 px wide, so don't use 5120×2880.
- No audio.

## Testing the receiver without the laptop

```sh
tools/test-sender.py 127.0.0.1 --seconds 10
```

streams an ffmpeg test pattern using the same protocol.

## Protocol

See [`linux/protocol.h`](linux/protocol.h).
