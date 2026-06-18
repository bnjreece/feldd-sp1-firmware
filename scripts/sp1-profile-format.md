# SP-1 profile format (canonical)

The wire + file contract for an SP-1 controller profile. This is the single
source of truth shared by two clients that MUST stay byte-identical:

- the firmware (`firmware/app/src/profile.h` / `profile.c`),
- the host CLI (`scripts/sp1ctl.py`, proven by `--selftest`),
- the feldd-sp-1 web codec (`lib/feldd/codec.ts`, mirrored + selftest-checked).

`PROFILE_VERSION = 1`. A device advertises it as `pver` in `hello_r`; a client
refuses to edit a device whose `pver` differs from the layout below.

## 1. Packed binary blob (69 bytes, little-endian)

`sizeof(struct profile) == 69`. Fields are tightly packed, no padding.

| Offset | Bytes | Field | Notes |
|-------:|------:|-------|-------|
| 0 | 1 | `version` | = 1 |
| 1 | 1 | `channel` | MIDI channel, 0..15 |
| 2 | 20 | `fader[4]` | 4 faders x 5 bytes each (see below) |
| 22 | 18 | `button[9]` | 9 buttons x 2 bytes each (see below) |
| 40 | 4 | `shift.fader_cc[4]` | one CC per fader on the shift layer |
| 44 | 9 | `shift.button_value[9]` | one value per button on the shift layer |
| 53 | 16 | `name[16]` | UTF-8, NUL-padded, max 16 bytes |
| | **69** | total | |

Each **fader** (5 bytes): `cc` (1), `min` (1), `max` (1), `curve` (1), `invert` (1).
Each **button** (2 bytes): `type` (1), `value` (1).

All `cc` / `min` / `max` / `value` are 0..127. `invert` is 0 or 1.

### Enums

`curve`: `0 = linear`, `1 = log`, `2 = exp`.

`button.type`: `0 = none`, `1 = note`, `2 = cc_toggle`, `3 = cc_momentary`,
`4 = transport`, `5 = profile_switch`.

## 2. Base64 (the wire transport for `read` / `write`)

The 69-byte blob is encoded with **standard** base64 (`A-Za-z0-9+/`). Because
`69 % 3 == 0`, there is **no `=` padding** and the encoded string is exactly
**92 characters**. Clients MUST produce padding-free, 92-char strings; a `write`
of a blob is well under the protocol's 256-byte line cap.

## 3. Friendly JSON (`.feldd`, for import/export, templates, and the gallery)

Human-readable, git-friendly, byte-for-byte convertible to/from the blob:

```json
{ "format": "sp1-profile", "version": 1, "name": "op-xy mix", "channel": 0,
  "faders": [ {"cc":7,"min":0,"max":127,"curve":"linear","invert":false}, ...x4 ],
  "buttons": [ {"type":"cc_momentary","value":20}, ...x9 ],
  "shift": { "fader_cc": [..x4], "button_value": [..x9] } }
```

Bundle (export-all, 8 slots + active index):

```json
{ "format": "sp1-bundle", "version": 1, "active": 0, "profiles": [ ...x8 ] }
```

`curve` and `button.type` use the string names above; conversion to/from the
numeric enum values is part of the codec.

## 4. Invariants (the parity gate)

1. `serialize(parse(blob)) == blob` byte-for-byte.
2. `profileToBase64(p)` is 92 chars and contains no `=`.
3. Friendly-JSON round-trips through the blob with no loss.
4. Validation ranges (rejected by `profile_validate` / the gallery validator):
   `channel` 0..15; every `cc` / `min` / `max` / `value` 0..127; `curve` and
   `button.type` within the enums; `name` <= 16 UTF-8 bytes.

`python3 scripts/sp1ctl.py --selftest` is the reference parity check (it proves
the blob layout, the no-padding 92-char base64, and the firmware-parity of the
alphabet). The web codec ports `encode_profile` / `decode_profile` and replicates
these assertions as a unit test.
