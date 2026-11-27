#include "mod/msglimit.h"
#include "utils/utils.h"
#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <time.h>

static sqlite3 *db;
static int deletes, fetches;
static struct discord_ret_message pending;
static struct discord_user author = {.id = 42};
static struct discord_attachments attachments = {.size = 1};
static struct discord_embeds embeds = {.size = 1};
static struct discord_sticker_items stickers = {.size = 1};

u64snowflake g_app_id = 99;
sqlite3 *customcom_get_db(void) { return db; }
UserCtx get_ctx_from_message(const struct discord_message *event) {
  (void)event;
  return (UserCtx){0};
}
bool check_perm_byrole(const UserCtx *ctx, u64snowflake ids[], int size) {
  (void)ctx;
  (void)ids;
  (void)size;
  return true;
}
CCORDcode discord_create_message(struct discord *client, u64snowflake channel,
                                struct discord_create_message *params,
                                struct discord_ret_message *ret) {
  (void)client;
  (void)channel;
  (void)params;
  (void)ret;
  return CCORD_OK;
}
CCORDcode discord_delete_message(struct discord *client, u64snowflake channel,
                                u64snowflake id,
                                struct discord_delete_message *params,
                                struct discord_ret *ret) {
  (void)client;
  (void)channel;
  (void)id;
  (void)params;
  (void)ret;
  ++deletes;
  return CCORD_OK;
}
CCORDcode discord_get_channel_message(struct discord *client,
                                     u64snowflake channel, u64snowflake id,
                                     struct discord_ret_message *ret) {
  (void)client;
  (void)channel;
  (void)id;
  ++fetches;
  pending = *ret;
  return CCORD_OK;
}

static void sql(const char *query) {
  assert(sqlite3_exec(db, query, NULL, NULL, NULL) == SQLITE_OK);
}
static int scalar(const char *query) {
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK);
  int result = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
  sqlite3_finalize(stmt);
  return result;
}
static int media_count(void) {
  return scalar("SELECT COALESCE(SUM(media_count), 0) FROM msglimit_counts");
}
static void reset(void) {
  sql("DELETE FROM msglimit_settings; DELETE FROM msglimit_counts;"
      "DELETE FROM msglimit_counted;"
      "INSERT INTO msglimit_settings VALUES (1, 42, 1, 1)");
  deletes = fetches = 0;
  pending = (struct discord_ret_message){0};
}
static struct discord_message message(int sequence) {
  /* Noon UTC avoids tests depending on the current time of day. */
  time_t now = time(NULL);
  u64unix_ms timestamp = (u64unix_ms)(now / 86400 * 86400 + 43200) * 1000;
  return (struct discord_message){
      .id = ((timestamp - 1420070400000ULL) << 22) + sequence,
      .guild_id = 1, .channel_id = 2, .author = &author,
      .content = "", .timestamp = timestamp,
  };
}
static void complete_fetch(struct discord_message result) {
  struct discord_response response = {.keep = pending.keep};
  /* The REST API does not have to include a guild ID. */
  result.guild_id = 0;
  assert(pending.done);
  pending.done(NULL, &response, &result);
}

static void test_media_kinds(void) {
  for (int kind = 0; kind < 5; ++kind) {
    reset();
    struct discord_message first = message(1);
    switch (kind) {
    case 0: first.attachments = &attachments; break;
    case 1: first.embeds = &embeds; break;
    case 2: first.sticker_items = &stickers; break;
    case 3: first.flags = 1ULL << 14; break;
    case 4: first.content = "https://example.com/video"; break;
    }
    assert(!msglimit_enforce(NULL, &first));
    assert(media_count() == 1);
    struct discord_message second = first;
    second.id++;
    assert(msglimit_enforce(NULL, &second));
    assert(media_count() == 2 && deletes == 1);
  }
}
static void test_partial_updates(void) {
  reset();
  struct discord_message first = message(1);
  first.attachments = &attachments;
  assert(!msglimit_enforce(NULL, &first));

  struct discord_message update = message(2);
  update.author = NULL;
  update.content = NULL;
  update.timestamp = 0;
  update.embeds = &embeds;
  msglimit_enforce_edit(NULL, &update);
  assert(fetches == 1 && media_count() == 1 && deletes == 0);
  struct discord_message fetched = message(2);
  fetched.embeds = &embeds;
  complete_fetch(fetched);
  assert(media_count() == 2 && deletes == 1);

  /* Repeated link-preview updates must not spend another media slot. */
  msglimit_enforce_edit(NULL, &update);
  complete_fetch(fetched);
  assert(media_count() == 2);

  /* Author present, but content omitted: still resolve the partial payload. */
  reset();
  update.author = &author;
  msglimit_enforce_edit(NULL, &update);
  assert(fetches == 1 && media_count() == 0);
  complete_fetch(fetched);
  assert(media_count() == 1);

  /* No successful callback (e.g. a deleted message) means no increment. */
  reset();
  msglimit_enforce_edit(NULL, &update);
  assert(fetches == 1 && media_count() == 0);
}
static void test_edit_and_dedupe(void) {
  reset();
  struct discord_message event = message(1);
  event.content = "plain text";
  assert(!msglimit_enforce(NULL, &event));
  assert(media_count() == 0);
  event.content = "https://example.com";
  msglimit_enforce_edit(NULL, &event);
  assert(media_count() == 1 && deletes == 0);
  event.embeds = &embeds;
  msglimit_enforce_edit(NULL, &event);
  assert(media_count() == 1 && deletes == 0);
  assert(scalar("SELECT text_count FROM msglimit_counts") == 1);
  event.id++;
  msglimit_enforce_edit(NULL, &event);
  assert(media_count() == 2 && deletes == 1);
}
static void test_utc_rollover(void) {
  reset();
  struct discord_message event = message(1);
  event.embeds = &embeds;
  event.timestamp -= 86400000;
  event.id = ((event.timestamp - 1420070400000ULL) << 22) + 1;
  msglimit_enforce_edit(NULL, &event);
  assert(media_count() == 0);
  event.timestamp = 0;
  msglimit_enforce_edit(NULL, &event);
  assert(media_count() == 0 && fetches == 0);
  event = message(2);
  event.embeds = &embeds;
  event.timestamp = 0;
  msglimit_enforce_edit(NULL, &event);
  assert(media_count() == 1);
}
static void test_exemptions_and_disabled_limits(void) {
  reset();
  struct discord_message event = message(1);
  event.attachments = &attachments;
  event.webhook_id = 7;
  assert(!msglimit_enforce(NULL, &event));
  event.webhook_id = 0;
  struct discord_user self = {.id = 99, .bot = true};
  event.author = &self;
  assert(!msglimit_enforce(NULL, &event));
  event.author = &author;
  event.guild_id = 0;
  assert(!msglimit_enforce(NULL, &event));
  msglimit_enforce_edit(NULL, &event);
  assert(media_count() == 0 && fetches == 0);
  event.guild_id = 1;
  sql("UPDATE msglimit_settings SET media_limit = 0");
  assert(!msglimit_enforce(NULL, &event));
  msglimit_enforce_edit(NULL, &event);
  assert(media_count() == 0);

  /* Ordinary replies stay in the text bucket, unlike forwarded snapshots. */
  reset();
  event = message(1);
  struct discord_message_reference reference = {.message_id = 123};
  event.message_reference = &reference;
  assert(!message_is_forwarded(&event));
  assert(!msglimit_enforce(NULL, &event));
  assert(media_count() == 0);
  event.id++;
  assert(msglimit_enforce(NULL, &event));
}
int main(void) {
  assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
  msglimit_db_init();
  test_media_kinds();
  test_partial_updates();
  test_edit_and_dedupe();
  test_utc_rollover();
  test_exemptions_and_disabled_limits();
  assert(sqlite3_close(db) == SQLITE_OK);
  puts("msglimit regression tests passed");
  return 0;
}
