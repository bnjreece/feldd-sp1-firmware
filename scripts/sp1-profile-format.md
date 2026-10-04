# SP-1 profile format (canonical)

The wire + file contract for an SP-1 controller profile. This is the single
source of truth shared by three clients that MUST stay byte-identical:

- the firmware (`firmware/app/src/profile.h` / `profile.c`),
- the host CLI (`scripts/sp1ctl.py`, proven by `--selftest`),
- the feldd-sp-1 web codec (`lib/feldd/codec.ts`, mirrored + selftest-checked).

> ## ⚠️ This document describes v2. The current format is **v10**.
>
> Everything below the "Version history" table still describes `PROFILE_VERSION = 2`
> and has not been revised since. **`profile.h` is authoritative**; treat the prose
> here as historical unless it agrees with it.
>
> | Ver | Bytes | base64 | Adds |
> |---|---|---|---|
> | 1 | 69 | 92 | base image |
> | 2 | 82 | 112 | per-control MIDI channel |
> | 3 | 100 | — | `button_key` / `button_mod` (USB-HID) |
> | 4 | 118 | — | shift-layer keyboard |
> | 5 | 180 | — | layers L3/L4 |
> | 6 | 294 | — | per-layer `layer_ext` |
> | 8 | 528 | — | `chord6` grid + `fader_role` (v7 retired) |
> | 9 | **1038** | 1384 | **hard break** — interior arrays resized, `NUM_LAYERS` 4 → 8 |
> | 10 | **1065** | 1420 | **tail append** — per-profile jack settings |
>
> ### v10 — per-profile jack settings
>
> The TRS jack's role becomes part of the profile, so stepping profiles
> re-purposes the port. A **pure tail append**: bytes `[0..1037]` keep the v9
> layout and only byte 0, the version, changes, which is what lets a stored v9
> profile be widened rather than reseeded. Anything added later must append for the same reason.
>
> | Offset | Field | Scope | Range | Default |
> |---|---|---|---|---|
> | 1038 | `trs_mode` | profile | `0` MIDI, `1` trigger, `2` sync | `0` |
> | 1039 | `pulse_width` | profile | **1–255**, units of 100 µs | `100` (10 ms) |
> | 1040–1047 | `trigger_note[8]` | per layer | 0–127 | `51` |
> | 1048–1055 | `trigger_channel[8]` | per layer | **`0` omni, `1`–`16` = channel 1–16** | `0` |
> | 1056–1063 | `sync_div[8]` | per layer | **1–24** clock ticks per pulse | `12` |
> | 1064 | reserved | — | **must be 0** | `0` |
>
> Three encodings are easy to get wrong:
>
> - **`pulse_width` is not milliseconds** — 100 µs units, so 10 ms is `100`. **`0`
>   is invalid**, deliberately, so an accidentally zero-filled tail fails loudly
>   instead of emitting a 0 ms pulse that silently does nothing.
> - **`trigger_channel` is 1-based**, unlike the transmit-channel fields elsewhere
>   in the profile (which are 0–15 because they feed a status byte directly). It is
>   a *filter with an "any" option*, so it needs a 17th value and cannot share that
>   encoding; given it must differ, it differs toward the form a human reads.
> - **`sync_div` is ticks per pulse** against 24 PPQN — a divisor, not a rate.
>   `12` = 1/8 = 2 PPQN, the Pocket Operator / Volca rate.
>
> **`1065 % 3 == 0`**, so base64 still carries no `=` padding. The reserved byte at
> 1064 is what pads it there, and is claimable by a future field *without* another
> version bump — do not use it as scratch space.
>
> Two limits constrain further growth, and the tighter one is **not** the obvious
> one: the USB-CDC frame buffers cap out around `sizeof` **1107**, while NVS sector
> packing allows **1352**. A change that grows the struct past ~1107 must raise
> `CONFIG_CDC_LINE_CAP` / `CONFIG_CDC_RESP_CAP` in the same change — an oversized
> frame is silently dropped rather than erroring.

`PROFILE_VERSION = 2`. A device advertises it as `pver` in `hello_r`. **v2 adds a
per-control MIDI channel** so one profile can drive several tracks at once (4
faders -> 4 channels = a mixer). v2 is a strict superset of v1: the first 69
bytes are byte-identical to v1, with 13 channel bytes appended. The firmware only
ever speaks v2; clients still *decode* a legacy v1 blob (filling each control's
channel from the profile-wide `channel`) and always *encode* v2.

## 1. Packed binary blob (82 bytes, little-endian)

`sizeof(struct profile) == 82`. Fields are tightly packed, no padding.

| Offset | Bytes | Field | Notes |
|-------:|------:|-------|-------|
| 0 | 1 | `version` | = 2 |
| 1 | 1 | `channel` | profile-wide MIDI channel, 0..15 (UI "set all" default / v1 fallback) |
| 2 | 20 | `fader[4]` | 4 faders x 5 bytes each (see below) |
| 22 | 18 | `button[9]` | 9 buttons x 2 bytes each (see below) |
| 40 | 4 | `shift.fader_cc[4]` | one CC per fader on the shift layer |
| 44 | 9 | `shift.button_value[9]` | one value per button on the shift layer |
| 53 | 16 | `name[16]` | UTF-8, NUL-padded, max 16 bytes |
| 69 | 4 | `fader_channel[4]` | **v2** — MIDI channel 0..15 per fader |
| 73 | 9 | `button_channel[9]` | **v2** — MIDI channel 0..15 per button |
| | **82** | total | |

Each **fader** (5 bytes): `cc` (1), `min` (1), `max` (1), `curve` (1), `invert` (1).
Each **button** (2 bytes): `type` (1), `value` (1).

All `cc` / `min` / `max` / `value` are 0..127. `invert` is 0 or 1. Every channel
field (`channel`, `fader_channel[*]`, `button_channel[*]`) is 0..15.

The mapping engine sends each fader on `fader_channel[i]` and each button on
`button_channel[i]`; the profile-wide `channel` is only the default the UI applies
when you "set all" and the value a v1 blob fills every control with on decode.

### Enums

`curve`: `0 = linear`, `1 = log`, `2 = exp`.

`button.type`: `0 = none`, `1 = note`, `2 = cc_toggle`, `3 = cc_momentary`,
`4 = transport`, `5 = profile_switch`.

### v1 compatibility

A **v1** blob is the first 69 bytes of this layout with `version = 1` and no
trailing channel bytes; base64 is 92 chars (no padding). Decoders accept it and
set every `fader_channel` / `button_channel` to the profile-wide `channel`.
Encoders never emit v1.

## 2. Base64 (the wire transport for `read` / `write`)

The 82-byte blob is encoded with **standard** base64 (`A-Za-z0-9+/`), **canonical
padding**. Because `82 % 3 == 1`, the encoded string is exactly **112 characters**
and ends in `==`. Clients MUST produce the canonical padded 112-char string — the
firmware decoder validates the exact length and padding. (A legacy v1 blob is 92
chars with no padding, since `69 % 3 == 0`.) A `write` of a blob is well under the
protocol's 256-byte line cap.

## 3. Friendly JSON (`.feldd`, for import/export, templates, and the gallery)

Human-readable, git-friendly, byte-for-byte convertible to/from the blob. Each
fader/button may carry its own `channel` (0..15); when omitted, the codec defaults
it to the profile-wide `channel`:

```json
{ "format": "sp1-profile", "version": 2, "name": "op-xy mix", "channel": 0,
  "faders": [ {"cc":7,"min":0,"max":127,"curve":"linear","invert":false,"channel":0}, ...x4 ],
  "buttons": [ {"type":"cc_momentary","value":20,"channel":0}, ...x9 ],
  "shift": { "fader_cc": [..x4], "button_value": [..x9] } }
```

Bundle (export-all, 8 slots + active index):

```json
{ "format": "sp1-bundle", "version": 1, "active": 0, "profiles": [ ...x8 ] }
```

`curve` and `button.type` use the string names above; conversion to/from the
numeric enum values is part of the codec. A v1 `.feldd` (no per-control `channel`,
`version: 1`) imports cleanly and is upgraded to v2 on the next encode.

## 4. Invariants (the parity gate)

1. `serialize(parse(blob)) == blob` byte-for-byte.
2. `profileToBase64(p)` is 112 chars and ends in `==` (canonical padding).
3. Friendly-JSON round-trips through the blob with no loss.
4. A v1 blob (69 bytes / 92-char base64) decodes, then re-encodes to v2 (82 bytes
   / 112-char base64) with every control on the former profile-wide channel.
5. Validation ranges (rejected by `profile_validate` / the gallery validator):
   every channel field 0..15; every `cc` / `min` / `max` / `value` 0..127; `curve`
   and `button.type` within the enums; `name` <= 16 UTF-8 bytes.

`python3 scripts/sp1ctl.py --selftest` is the reference parity check (it proves
the blob layout, the canonical 112-char base64, and the firmware-parity of the
alphabet). The web codec ports `encode_profile` / `decode_profile` and replicates
these assertions as a unit test (`lib/feldd/codec.test.ts`).
