
C (gnu11) discord bot using [concord-lib](https://cogmasters.github.io/concord/)

built by default against x86_64 gnu11 but is extensible to ARMv7

---

## dependencies:
`libcurl` `concord` [cJSON](https://github.com/DaveGamble/cJSON) `sqlite3` `sys/wait`

---

---

## Unit service:

Only requires: ```
[Install] 
WantedBy=multi-user.target```

rest is preference


## Build and Concord dependency

The repository expects an installed Concord prefix at `./concord` (headers under `concord/include/concord/`, static library under `concord/lib/`). The generated `concord/` directory and the source checkout are local build dependencies and are not committed.

The tracked `CONCORD_REVISION` file pins the upstream Concord revision used by CI. The CI workflow builds curl 8.23.0 with WebSocket support, builds that exact Concord revision, installs it into the expected prefix, and compiles the bot.

### Build against the pinned upstream Concord v3 API

Requirements: GNU make, a GNU11 C compiler, SQLite development headers, OpenSSL development headers, zlib development headers, and curl 8.7.1+ built with WebSocket support. CI currently builds curl 8.23.0.

```sh
git clone https://github.com/Cogmasters/concord.git .concord-src
git -C .concord-src checkout --detach "$(cat CONCORD_REVISION)"
export CFLAGS="-O2 -I/usr/local/include"
make -C .concord-src PREFIX="$PWD/concord" -j2
make -C .concord-src PREFIX="$PWD/concord" install
make -j2 CONCORD_API=v3
```

### Build against the existing legacy Concord API

The default Makefile mode remains `CONCORD_API=legacy` to preserve compatibility with existing local Concord checkouts that expose `discord_response.raw_error` and `discord_message_reference.type`.

```sh
make -j2
```

The two API modes are explicit because upstream Concord v3 changed the response error representation and no longer exposes the message-reference type in its public message structure. In v3 mode, forwarded-message media classification is limited to media present in the resolved `referenced_message`; the legacy mode retains the old explicit forward-type check.

### Validation boundary

The GitHub workflow verifies compilation against the pinned upstream v3 revision. It does not prove that an existing ignored local `concord/` directory matches that revision. Live Discord staging, rate-limit behavior under real traffic, and end-to-end command/sticker tests still require a configured test bot and guild; no token or live deployment is part of CI.
