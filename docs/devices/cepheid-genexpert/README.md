# Cepheid GeneXpert – interface profile

Written 2026-10-25 from the public interface specification; not checked on
an instrument yet. Request: "3 ja GeneXpert". All example values are
invented; the examples follow the field tables of the specification, they
are not copied from it.

## Sources

The specification is free to download but licensed for purchasers and
those working for them, so it is linked here, not copied. Section numbers
refer to Rev. F.

| # | Source | Revision | Covers |
|---|---|---|---|
| 1 | [GeneXpert LIS Interface Protocol Specification, 302-2261](https://infomine.cepheid.com/sites/default/files/2025-02/302-2261,%20Rev.%20F%20LIS%20Protocol%20Specification.pdf) | Rev. F, June 2023 | GeneXpert Dx ≥ 4.7b, Xpress ≥ 5.1, Infinity Xpertise ≥ 6.4b, Cepheid OS 1.0 / ≥ 2.0 |
| 2 | [Same document](https://infomine.cepheid.com/sites/default/files/2021-10/LIS%20Protocol%20Specification%20302-2261,%20Rev.%20C.pdf) | Rev. C, September 2020 | older software |

## Transport

- TCP only, no serial port (§2). Either side can listen: "LIS as server" or
  "LIS as client" is a setting on the instrument PC.
- TLS 1.2/1.3 only with Cepheid OS ≥ 2.0 and only with the instrument as
  client (§2.2). lisbench has no TLS yet; use a TLS terminator (e.g.
  `socat`/`stunnel`) in front of it.
- LIS01-A2 (the document calls it ASTM E1381-02) framing with standard frame
  length (240 characters of text), checksum and timeouts (§3.2).
- **HL7 mode uses the same LIS01-A2 framing, not MLLP** (§1.9.1). The protocol
  is not detected automatically; both sides must be set to the same one.

## Records and fields

Records sent and accepted: H, P, O, R, C, Q, L; no S or M (§4.2.1.1). Xpress
5.1 is result upload only (no Q, no orders).

| Record | Notes |
|---|---|
| H | Delimiters `\|@^\`. Field 5 `system^GeneXpert^software version`, field 10 receiver ID, field 12 `P`, field 13 version **`1394-97`** (not `LIS2-A2`), field 14 timestamp (§6.4.1.1). Message ID up to 32 characters. |
| Q | Query for all orders: field 3 `ALL`; for one specimen: field 3 `^<specimen>^^`; field 13 request status codes `O@N` (§6.3.1, §6.3.2). Cancel: field 13 `A` plus a C record with the reason. |
| P | Upload: mostly empty. Download: patient IDs and name (§6.4.2.2). |
| O | Specimen ID up to 25 characters (field 3), instrument specimen ID (field 4, only with "Use Instrument Specimen ID"), test `^^^<host test code>` (field 5), field 16 always `ORH` (§1.9.2), field 26 report type: `Q` in a query answer, `F` in an upload. |
| R | Field 3 has **eight components**: panel ID, –, test ID, test name, test version, analyte name, complementary result name (`Ct`, `EndPt`, `Delta Ct`, `Conc/LOG`). Field 4 is `qualitative^quantitative`. Field 9 status `F`/`I`/`X`/`C`, field 11 operator, 12/13 start and end, field 14 `PC^instrument S/N^module S/N^cartridge S/N^reagent lot^expiry` (§6.4.1.7). A multi-result test is one main R record followed by R records per analyte and complementary value. |
| L | Field 3: `N` from the instrument; from the LIS `F` (orders follow the query) or `I` (no orders) (§6.3.1.3, §6.3.1.4, §6.4.1.8). |

Workflows (§1.8): instrument queries all orders; instrument queries per
specimen; instrument uploads results; LIS asks for results (one patient per
request). Unsolicited orders from the LIS are **not** supported. A query for
a patient ID alone goes out as a query for all (§1.9.4).

## Character set and special data

- 8-bit, single byte; bytes 128–254 follow the code page configured on the
  instrument (§4.3.3). Allowed bytes: 9, 13, 32–126, 128–254.
- Escapes use the escape delimiter `\`: `\F\ \S\ \R\ \E\`, hex `\Xhh..\`.
  `\H\` and `\N\` (highlighting) are ignored by the instrument.
- Characters outside the code page are sent as `\Z` + UTF-16 code units in
  hex, e.g. `\Z03B2\` for β (§4.4.4). Read them with
  `--local-escape utf16-hex` (CLI) or `local_escape: utf16-hex` (mapper).
- lisbench's `latin1` is ISO 8859-1. If the instrument uses Windows-1252,
  bytes 0x80–0x9F (€, „ …) differ.

## Known issues and workarounds

1. **Contention rule contradicts itself.** §3.2.2.2 gives the instrument
   priority (as the standard does); the transmission table in §3.2.3.1 shows
   the LIS as master and the instrument giving up the line, with the LIS
   resending ENQ after 2 s. If both sides back off, transfers stall until the
   20 s hold-off ends. `timing: {contention_winner: computer}` switches the
   lisbench side; to be confirmed on an instrument.
2. **Receiver interrupt ignored.** An EOT in place of ACK does not stop the
   instrument; it finishes the message, then waits 15 s before the next ENQ
   (§3.2.3.1).
3. **Link check:** ENQ, any of ACK/NAK/ENQ within 15 s, then EOT with no
   frames (§3.2.6.1). `lisbench monitor` shows it as `empty transfer, no
   frames`, not as an aborted transfer.
4. **One query at a time; 60 s.** An unanswered query is cancelled after 60 s
   (Q field 13 `A` with a comment) and counts as "no orders". Always answer,
   also with `H` + `L|1|I` when there is nothing (§6.3.1.2, §6.3.1.4).
5. **Records are repeated after a failure.** After an aborted transfer the
   instrument resends from the last record not yet "stored", repeating the
   header and the parent records needed to rebuild the hierarchy (§5.1). The
   LIS must be ready for duplicates.
6. **Errors in downloads are skipped, not refused.** The instrument skips the
   faulty part and continues; an error in H drops the message, in P all its
   orders, in O only that order (§5.2). Results are reported via the
   instrument's own log, not on the link.
7. **Trailing empty fields:** §4.4.2 says they are left out, the examples in
   the same document keep them (`|^27.4|||`). Accept both.
8. **HL7 results are marked "uploaded" at EOT**, not on the LIS's ACK message
   (§1.9.1).
9. **When the LIS requests final results**, pending orders come back as well
   (§1.9.3).
10. **Example in §3.2.3.1:** the fourth frame contains `65e04320` where the
    same frame elsewhere in the document has `604320`. The stated checksum
    `80` and the 240-character limit only hold for `604320`. Frames 2 and 5
    of that example match lisbench's checksum exactly; frame 3 matches once a
    space lost in the PDF line break ("NOT DETECTED") is restored.

## Files here

| File | What |
|---|---|
| `profile.yaml` | machine-readable summary (not yet read by lisbench) |
| `lis.yaml` | `lisbench sim` scenario: a LIS for a GeneXpert, answers queries |
| `instrument.yaml` | `lisbench sim` scenario: plays a GeneXpert towards a LIS under test |
| `examples/query-all.astm` | instrument queries all orders |
| `examples/query-specimen.astm` | instrument queries one specimen |
| `examples/orders.astm` | LIS answers with one order (`L\|1\|F`) |
| `examples/no-orders.astm` | LIS answers "nothing to do" (`L\|1\|I`) |
| `examples/results.astm` | multi-result upload with Ct/EndPt values and a `\Z` escape |

```sh
lisbench decode --local-escape utf16-hex docs/devices/cepheid-genexpert/examples/results.astm
lisbench sim docs/devices/cepheid-genexpert/lis.yaml          # terminal 1
lisbench sim docs/devices/cepheid-genexpert/instrument.yaml   # terminal 2
```
