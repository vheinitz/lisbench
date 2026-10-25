# Examples

Start the LIS simulator, then the instrument in a second terminal:

```console
lisbench sim examples/lis.yaml
lisbench sim examples/instrument.yaml --trace
```

The same without scenario files:

```console
lisbench sim --role computer --endpoint tcp-listen://:15000 --answer-queries examples/orders.astm --serve
lisbench sim --endpoint tcp://127.0.0.1:15000 --send examples/results.astm --send examples/query.astm --receive --trace
```

Over a serial line (instrument on /dev/ttyUSB0, 9600 8N1):

```console
lisbench sim --endpoint "serial:/dev/ttyUSB0?baud=9600" --send examples/results.astm --trace
```

Fault injection from the command line: `--fault ack:2:nak` (answer the
second frame with NAK), `--fault frame:every3:corrupt`, `--fault eot:all:drop`,
`--fault ack:1:delay:20000`.

All messages here are synthetic.
