-- Synthetic example data for the mapper. Names and values are invented.
CREATE TABLE patients (pid TEXT PRIMARY KEY, last TEXT, first TEXT, birth TEXT, sex TEXT);
CREATE TABLE samples  (sample TEXT PRIMARY KEY, pid TEXT REFERENCES patients(pid), drawn TEXT);
CREATE TABLE results  (id INTEGER PRIMARY KEY, sample TEXT REFERENCES samples(sample),
                       test TEXT, value TEXT, unit TEXT, flag TEXT, note TEXT,
                       operator TEXT, finished TEXT, sent_at TEXT, error TEXT);

INSERT INTO patients VALUES ('PID-0001', 'Doe', 'Jane', '19800101', 'F'),
                            ('PID-0002', 'Müller', 'Renée', '19750505', 'F');
INSERT INTO samples VALUES ('S-1001', 'PID-0001', '20261025080000'),
                           ('S-1002', 'PID-0002', '20261025081500');
INSERT INTO results (sample, test, value, unit, flag, note, operator, finished) VALUES
  ('S-1001', 'ANA',   '1:320', 'titer', 'H', 'Pattern: speckled', 'op1', '20261025115900'),
  ('S-1001', 'dsDNA', '12.5',  'IU/mL', 'N', NULL,                'op1', '20261025115900'),
  ('S-1002', 'ANA',   '<1:80', 'titer', 'N', NULL,                'op2', '20261025120500');
