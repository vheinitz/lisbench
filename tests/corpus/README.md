# Test corpus

All messages here are synthetic and written for this project. Names, IDs and
values are invented. Messages taken from public instrument manuals will go to
`devices/<vendor>-<model>/` with the source (document, revision, page) next to
them.

- `valid_*.astm` parse and validate without errors.
- `invalid_*.astm` parse, but the validator must report at least one error.

Records are separated by CR, as on the wire.
