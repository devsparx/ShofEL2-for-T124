# Jibo Persistent SSH via `/jibo/mode.json`

This procedure patches Jibo's `/var` partition so the robot boots in
`int-developer` mode and starts SSH. It was verified on hardware from macOS
using the portable libusb backend.

## What This Does

Jibo stores its mode at:

```text
/var/jibo/mode.json
```

On a normal consumer unit this file contains:

```json
{"mode":"normal"}
```

Replacing it with:

```json
{"mode":"int-developer"}
```

enables developer-mode services, including SSH as `root`.

## Requirements

- macOS with this branch built successfully.
- Homebrew `e2fsprogs` for `debugfs`:

```sh
brew install e2fsprogs
```

- Jibo connected over USB and able to enter RCM/APX mode.

On macOS, the RCM device appears in `ioreg` as NVIDIA APX with vendor/product
`0955:7740`:

```sh
ioreg -p IOUSB -l -w 0 | rg -i -C 3 'APX|0955|7740|NVIDIA'
```

## Important Details

- `shofel2_t124` parses eMMC sector arguments as hexadecimal. Use the `0x...`
  values below. Do not pass the decimal sector values bare.
- Each `shofel2_t124` invocation consumes the current RCM exploit session. Use
  a fresh Jibo RCM boot before the read and another fresh Jibo RCM boot before
  the write.
- The patch is made to a local image first. The writeback is the destructive
  step.

Verified `/var` partition:

| Field | Value |
| --- | --- |
| Device partition | `mmcblk0p5` |
| Start sector | decimal `8294434`, hex `0x7e9022` |
| Sector count | decimal `1024000`, hex `0xfa000` |
| Size | `524288000` bytes / `500 MiB` |

## Procedure

Build first:

```sh
cd /Users/tim/Projects/jibo-shofel-macos/src
make
```

Put Jibo into a fresh RCM boot: power-cycle it, hold the lower-small button, and
press the large button. Confirm APX is visible:

```sh
ioreg -p IOUSB -l -w 0 | rg -i -C 3 'APX|0955|7740|NVIDIA'
```

Dump `/var`:

```sh
./shofel2_t124 EMMC_READ 0x7e9022 0xfa000 /tmp/jibo-var.img
```

Validate the image:

```sh
stat -f 'size=%z bytes' /tmp/jibo-var.img
/opt/homebrew/opt/e2fsprogs/sbin/e2fsck -fn /tmp/jibo-var.img
/opt/homebrew/opt/e2fsprogs/sbin/debugfs -R 'cat /jibo/mode.json' /tmp/jibo-var.img
```

Expected size:

```text
size=524288000 bytes
```

Make a local backup and patch the image:

```sh
cp -c /tmp/jibo-var.img /tmp/jibo-var.orig.img
printf '%s' '{"mode":"int-developer"}' > /tmp/new-mode.json
printf '%s\n' \
  'rm /jibo/mode.json' \
  'write /tmp/new-mode.json /jibo/mode.json' \
  > /tmp/jibo-debugfs.cmds
/opt/homebrew/opt/e2fsprogs/sbin/debugfs -w -f /tmp/jibo-debugfs.cmds /tmp/jibo-var.img
```

Verify the patched image:

```sh
/opt/homebrew/opt/e2fsprogs/sbin/debugfs -R 'ls -l /jibo' /tmp/jibo-var.img
/opt/homebrew/opt/e2fsprogs/sbin/debugfs -R 'cat /jibo/mode.json' /tmp/jibo-var.img
/opt/homebrew/opt/e2fsprogs/sbin/e2fsck -fn /tmp/jibo-var.img
```

Expected `mode.json` line in `ls -l /jibo`:

```text
100644 ... mode.json
```

Expected content:

```json
{"mode":"int-developer"}
```

Put Jibo into a fresh RCM boot again. Confirm APX is visible, then write the
patched image back:

```sh
./shofel2_t124 EMMC_WRITE 0x7e9022 /tmp/jibo-var.img
```

When the command prints `Write complete.`, power-cycle Jibo normally with no RCM
button combo.

## SSH

After normal boot, discover Jibo's IP address from your router, ARP table, or
network scanner, then try:

```sh
ssh root@<jibo-ip>
```

Password:

```text
jibo
```

## Post-SSH Exploration Targets

For embodied Herald/Jibo work, inspect these top-of-stack systems first:

- `CAI` / character AI behavior code.
- Valence system: affect state, gesture selection, timing, inhibition.
- `ESML`: embodied speech markup language emitted to Jibo.
- Skill runtime and cloud skill interface. Jibo's top-of-stack code is old
  TypeScript, so expect callback-era APIs.
- `jibo-log`: determine whether it wraps stdout, syslog, a local service, or a
  remote sink. Redirecting or teeing this over Wi-Fi to the Mac will make
  cableless development much easier.

Useful first searches once SSH is available:

```sh
find / -iname '*cai*' -o -iname '*esml*' -o -iname '*skill*' -o -iname '*jibo-log*' 2>/dev/null
find / -name '*.ts' -o -name '*.js' 2>/dev/null | head -200
rg -n 'ESML|CAI|valence|cloud skill|jibo-log|skill|animation|dance|gesture' / 2>/dev/null
```

## Codex Prompt for Another Operator

Use this with Codex CLI from this repository:

```text
You are helping me enable persistent SSH on a Jibo using the verified
JIBO_PERSISTENT_SSH.md procedure in this repo. Read that file first. I will put
Jibo into fresh RCM when you ask. Use explicit hex sector arguments:
EMMC_READ 0x7e9022 0xfa000 and EMMC_WRITE 0x7e9022. Patch /jibo/mode.json inside
the dumped /var image to {"mode":"int-developer"}, verify image size is
524288000 bytes, verify e2fsck -fn is clean, and do not write back until the
patched image has been verified. Remember that each shofel2_t124 invocation
needs a fresh RCM boot.
```
