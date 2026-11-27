C (gnu11) discord bot using [concord-lib](https://cogmasters.github.io/concord/)

built by default against x86_64 gnu11 but is extensible to ARMv7

---

## dependencies:
`libcurl` `concord` [cJSON](https://github.com/DaveGamble/cJSON) `sqlite3` `sys/wait`

---

---

## Tests

With Concord headers and SQLite development files installed, run:

```sh
make test
```

For a non-system Concord installation, pass its include directory:

```sh
make test CPPFLAGS="-I/path/to/concord-prefix/include"
```

The message-limit regression tests use an in-memory database and stub Discord
requests, so no bot token or network connection is needed. They cover media
classification, daily limits, partial message updates, edit deduplication,
UTC rollover, exemptions, and disabled limits.

## Unit service:

Only requires: ```
[Install] 
WantedBy=multi-user.target```

rest is preference
