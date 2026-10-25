# <Vendor> <Model> – interface profile

Copy this directory layout to `docs/devices/<vendor>-<model>/`:

```
README.md       this text, filled in
profile.yaml    machine-readable part (see below)
examples/       messages, one file each, with source line at the top
```

## Sources

Every statement needs one. Allowed: freely downloadable manuals (link, document
number, revision, page – do not copy the manual), open-source code, own
reverse engineering on an instrument you own or operate, contributions that
name their source. Not allowed: documents under NDA, captures from sites with
patient data.

| # | Source | Revision | Pages |
|---|---|---|---|
| 1 | | | |

## Transport

Serial (baud, data bits, parity, stop bits, handshake) or TCP (which side
listens, port). LIS01-A2 deviations: frame length, checksum case, timing,
behaviour on contention, CR LF handling.

## Records and fields

Which records the instrument sends and accepts; per record the fields it
fills or reads, with format (dates, units, flags). Unused fields can be left out.

## Character set and special data

Charset on the wire, handling of non-ASCII, escapes used or ignored, images
or binary data (how they are encoded and split).

## Known issues and workarounds

Deviations from the standard and how to live with them.

## profile.yaml

`lisbench decode|validate|monitor|sim --profile <id>` reads `lis01.max_text`,
`lis01.contention_winner` (`instrument`, `computer` or `unclear`) and
`lis02.charset`, `non_ascii`, `local_escape`; options given on the command
line win. The other keys document the instrument.

```yaml
id: vendor-model
vendor: Vendor
model: Model
sources: [1]
transport:
  kind: serial            # or tcp
  serial: {baud: 9600, data_bits: 8, parity: none, stop_bits: 1}
lis01:
  max_text: 240
  checksum_lowercase: false
lis02:
  delimiters: "|\\^&"
  charset: latin1         # latin1 | utf8
  non_ascii: ascii-hex    # ascii-hex | raw | transliterate
  order_without_patient: false
records:
  sends: [H, P, O, R, C, L]
  accepts: [H, P, O, C, L, Q]
```
