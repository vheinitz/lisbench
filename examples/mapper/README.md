# Mapper example

`schema.sql` creates a small synthetic database (patients, samples,
results); `export.yaml` maps it to one LIS02-A2 message per sample.

```console
sqlite3 example.db < examples/mapper/schema.sql            # or any SQLite tool
EACH="SELECT DISTINCT sample FROM results WHERE sent_at IS NULL"

# show what would be sent
lisbench map examples/mapper/export.yaml --db example.db --each "$EACH"

# send to a LIS (here the simulator) and mark the results as sent
lisbench sim examples/lis.yaml &
lisbench map examples/mapper/export.yaml --db example.db --each "$EACH" --to tcp://127.0.0.1:15000

# or write files instead (name must contain {unique}; written via temp + rename)
lisbench map examples/mapper/export.yaml --db example.db --each "$EACH" --to "file:out/result_{unique}.astm"
```

A second run sends nothing: `on_success` has set `sent_at`. If the LIS is
not reachable, `on_failure` stores the error text and the results stay
unsent.

Success means that the LIS acknowledged every frame (LIS01-A2). LIS02-A2 has
no application-level acknowledgement, so the LIS may still reject the
content. The header carries a message control ID that is the same for the
same arguments, so that a message sent twice can be recognised.
