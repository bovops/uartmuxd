# userspace-uartmux

Userspace replacement for `uartmux.ko` from PEJIR01 firmware.

It opens one physical UART (default `/dev/ttyS1`) and exposes multiple logical
channels as PTY links:

- `/dev/umxtty0` -> MCU service channel
- `/dev/umxtty1` -> Zigbee channel
- `/dev/umxtty2` -> ZWave channel

Protocol is hash-addressed userspace muxing (`#<digit>` address select plus
payload), with logical channels mapped to DLCI-like indexes (`umxtty0..N`).

## Build

```sh
cd /path/to/userspace-uartmux
make
```

## Run

```sh
./uartmuxd -d /dev/ttyS1 -b 115200
```

By default daemon sends `#0on 6` once to `/dev/ttyS1` (same mode command as
stock init script). To disable this, pass an empty startup command:
`--on-cmd ""`.

Default serial flow control is disabled (`--no-flow` behavior) to match stock
`stty -F /dev/ttyS1 115200`.

## OpenWrt integration (24.10)

1. Copy binary:
```sh
install -m 0755 uartmuxd /usr/sbin/uartmuxd
```
2. Copy init script:
```sh
install -m 0755 openwrt-package/files/uartmuxd.init /etc/init.d/uartmuxd
```
3. Enable/start:
```sh
/etc/init.d/uartmuxd enable
/etc/init.d/uartmuxd start
```

### Build as OpenWrt package

Package skeleton is in `openwrt-package/`.

From OpenWrt SDK root:

```sh
mkdir -p package/uartmuxd
cp -a /path/to/userspace-uartmux/openwrt-package/* package/uartmuxd/
make package/uartmuxd/{clean,compile} V=s
```

