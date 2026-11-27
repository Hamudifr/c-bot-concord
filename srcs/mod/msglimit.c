#include "mod/msglimit.h"
#include "customcom/customcom.h"
#include "utils/utils.h"
#include <concord/discord_codecs.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern u64snowflake g_app_id;

void msglimit_db_init(void) {
  sqlite3 *db = customcom_get_db();
  sqlite3_exec(db,
               "CREATE TABLE IF NOT EXISTS msglimit_settings ("
               "guild_id INTEGER, user_id INTEGER, "
               "text_limit INTEGER DEFAULT 0, media_limit INTEGER DEFAULT 0, "
               "PRIMARY KEY(guild_id, user_id));",
               NULL, NULL, NULL);
  sqlite3_exec(db,
               "CREATE TABLE IF NOT EXISTS msglimit_counts ("
               "guild_id INTEGER, user_id INTEGER, day TEXT, "
               "text_count INTEGER DEFAULT 0, media_count INTEGER DEFAULT 0, "
               "PRIMARY KEY(guild_id, user_id, day));",
               NULL, NULL, NULL);
  /* one row per (message, bucket) so a message is only ever counted once per
   * bucket, no matter how many MESSAGE_UPDATE events Discord fires for it */
  sqlite3_exec(db,
               "CREATE TABLE IF NOT EXISTS msglimit_counted ("
               "message_id INTEGER, bucket TEXT, day TEXT, "
               "PRIMARY KEY(message_id, bucket));",
               NULL, NULL, NULL);
  /* keep the dedupe table from growing without bound */
  sqlite3_exec(db,
               "DELETE FROM msglimit_counted "
               "WHERE day < date('now', '-7 day');",
               NULL, NULL, NULL);
}

static void get_today_utc(char *buf, size_t sz) {
  time_t now = time(NULL);
  struct tm *tmv = gmtime(&now);
  strftime(buf, sz, "%Y-%m-%d", tmv);
}

/* UTC day of a discord timestamp (milliseconds since epoch) */
static void day_from_ms(u64unix_ms ms, char *buf, size_t sz) {
  time_t t = (time_t)(ms / 1000);
  struct tm *tmv = gmtime(&t);
  strftime(buf, sz, "%Y-%m-%d", tmv);
}

/* Returns true if this (message, bucket) pair had not been counted yet, and
 * claims it. Returns false if it was already counted -- caller must then skip
 * the increment. This is what stops a single message from being counted twice
 * (once on MESSAGE_CREATE, again when Discord attaches a link embed and fires
 * MESSAGE_UPDATE). Keyed per bucket, so a text message later edited to add a
 * link still gets counted once against media. */
static bool msglimit_claim(sqlite3 *db, u64snowflake message_id,
                           const char *bucket, const char *day) {
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(db,
                     "INSERT OR IGNORE INTO msglimit_counted "
                     "(message_id, bucket, day) VALUES (?, ?, ?)",
                     -1, &stmt, NULL);
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)message_id);
  sqlite3_bind_text(stmt, 2, bucket, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, day, -1, SQLITE_STATIC);
  sqlite3_step(stmt);
  bool fresh = sqlite3_changes(db) > 0;
  sqlite3_finalize(stmt);
  return fresh;
}

static bool message_is_media(const struct discord_message *event) {
  if (event->attachments && event->attachments->size > 0)
    return true;
  if (event->sticker_items && event->sticker_items->size > 0)
    return true;
  if (event->embeds && event->embeds->size > 0)
    return true;
  if (message_is_forwarded(event))
    return true;
  if (event->content &&
      (strstr(event->content, "http://") || strstr(event->content, "https://")))
    return true;
  return false;
}

/* Only this bot is exempt from limits. Other bots/apps are NOT exempt -- a
 * user's own bot posting on their behalf was a trivial bypass. Such a bot is
 * limited under its OWN user id, since Discord exposes no owner link.
 * True webhooks are skipped: they carry no attributable user id. */
static bool msglimit_is_exempt(const struct discord_message *event) {
  if (!event->author)
    return true;
  if (event->webhook_id)
    return true;
  if (event->author->id == g_app_id)
    return true;
  return false;
}

void msglimit_command(struct discord *client,
                      const struct discord_message *event) {
  if (event->author->bot)
    return;

  UserCtx ctx = get_ctx_from_message(event);
  u64snowflake allowlist[] = {1155152569526669391ULL};
  if (!check_perm_byrole(&ctx, allowlist, 1)) {
    fprintf(stderr, "msglimit_command rejected, invalid permissions\n");
    return;
  }

  char type[16] = "";
  int n1 = 0;
  sscanf(event->content, " %15s%n", type, &n1);

  const char *col = NULL;
  if (strcmp(type, "text") == 0)
    col = "text_limit";
  else if (strcmp(type, "media") == 0)
    col = "media_limit";

  u64snowflake target_uid = 0;
  if (event->mentions && event->mentions->size > 0) {
    target_uid = event->mentions->array[0].id;
  }

  char uidtok[64] = "";
  int n2 = 0;
  sscanf(event->content + n1, " %63s%n", uidtok, &n2);
  if (!target_uid) {
    sscanf(uidtok, "%" SCNu64, &target_uid);
  }

  int limit = -1;
  sscanf(event->content + n1 + n2, " %d", &limit);

  if (!col || !target_uid || limit < 0) {
    struct discord_create_message reply = {
        .content = "usage: +msglimit text <uid|mention> <int> | +msglimit "
                   "media <uid|mention> <int>"};
    discord_create_message(client, event->channel_id, &reply, NULL);
    return;
  }

  sqlite3 *db = customcom_get_db();
  sqlite3_stmt *stmt;
  char sql[256];
  snprintf(
      sql, sizeof sql,
      "INSERT INTO msglimit_settings (guild_id, user_id, %s) VALUES (?, ?, ?) "
      "ON CONFLICT(guild_id, user_id) DO UPDATE SET %s = excluded.%s",
      col, col, col);

  sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event->guild_id);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)target_uid);
  sqlite3_bind_int(stmt, 3, limit);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  char buf[128];
  if (limit == 0) {
    snprintf(buf, sizeof buf, "msglimit %s disabled for <@%" PRIu64 ">.", type,
             target_uid);
  } else {
    snprintf(buf, sizeof buf,
             "msglimit %s for <@%" PRIu64 "> set to %d (resets 00:00 UTC).",
             type, target_uid, limit);
  }
  struct discord_create_message reply = {.content = buf};
  discord_create_message(client, event->channel_id, &reply, NULL);
}

bool msglimit_enforce(struct discord *client,
                      const struct discord_message *event) {
  if (msglimit_is_exempt(event))
    return false;
  if (!event->guild_id)
    return false;

  sqlite3 *db = customcom_get_db();
  sqlite3_stmt *stmt;
  int text_limit = 0, media_limit = 0;

  sqlite3_prepare_v2(db,
                     "SELECT text_limit, media_limit FROM msglimit_settings "
                     "WHERE guild_id = ? AND user_id = ?",
                     -1, &stmt, NULL);
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event->guild_id);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event->author->id);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    text_limit = sqlite3_column_int(stmt, 0);
    media_limit = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);

  bool is_media = message_is_media(event);
  int limit = is_media ? media_limit : text_limit;
  if (limit <= 0)
    return false;
  char day[16];
  get_today_utc(day, sizeof day);
  const char *col = is_media ? "media_count" : "text_count";

  char sql[256];
  if (msglimit_claim(db, event->id, col, day)) {
    snprintf(sql, sizeof sql,
             "INSERT INTO msglimit_counts (guild_id, user_id, day, %s) VALUES "
             "(?, ?, ?, 1) "
             "ON CONFLICT(guild_id, user_id, day) DO UPDATE SET %s = %s + 1",
             col, col, col);
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event->guild_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event->author->id);
    sqlite3_bind_text(stmt, 3, day, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  snprintf(
      sql, sizeof sql,
      "SELECT %s FROM msglimit_counts WHERE guild_id=? AND user_id=? AND day=?",
      col);
  sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event->guild_id);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event->author->id);
  sqlite3_bind_text(stmt, 3, day, -1, SQLITE_STATIC);
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if (count > limit) {
    discord_delete_message(client, event->channel_id, event->id, NULL, NULL);
    return true;
  }
  return false;
}

bool message_is_forwarded(const struct discord_message *event) {
  /* HAS_SNAPSHOT marks forwarded messages. Unlike message_reference.type,
   * flags are decoded by stock Concord too. Ordinary replies are not media. */
  return (event->flags & (1ULL << 14)) != 0;
}

static bool apply_media_check(struct discord *client,
                              const struct discord_message *event) {
  if (!event->guild_id)
    return false;
  if (msglimit_is_exempt(event))
    return false;
  if (!message_is_media(event))
    return false;

  sqlite3 *db = customcom_get_db();
  sqlite3_stmt *stmt;
  int media_limit = 0;

  sqlite3_prepare_v2(db,
                     "SELECT media_limit FROM msglimit_settings WHERE guild_id "
                     "= ? AND user_id = ?",
                     -1, &stmt, NULL);
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event->guild_id);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event->author->id);
  if (sqlite3_step(stmt) == SQLITE_ROW)
    media_limit = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if (media_limit <= 0)
    return false;

  char day[16];
  get_today_utc(day, sizeof day);

  /* MESSAGE_UPDATE may omit timestamp. The snowflake always encodes the
   * creation time, so old edits cannot spend today's budget even after the
   * dedupe entry has been pruned. */
  u64unix_ms created_ms = event->timestamp;
  if (!created_ms)
    created_ms = (event->id >> 22) + 1420070400000ULL;
  char created[16];
  day_from_ms(created_ms, created, sizeof created);
  if (strcmp(created, day) != 0)
    return false;

  if (msglimit_claim(db, event->id, "media_count", day)) {
    sqlite3_prepare_v2(db,
                       "INSERT INTO msglimit_counts (guild_id, user_id, day, "
                       "media_count) VALUES (?, ?, ?, 1) "
                       "ON CONFLICT(guild_id, user_id, day) DO UPDATE SET "
                       "media_count = media_count + 1",
                       -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event->guild_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event->author->id);
    sqlite3_bind_text(stmt, 3, day, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  sqlite3_prepare_v2(db,
                     "SELECT media_count FROM msglimit_counts WHERE guild_id=? "
                     "AND user_id=? AND day=?",
                     -1, &stmt, NULL);
  sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event->guild_id);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event->author->id);
  sqlite3_bind_text(stmt, 3, day, -1, SQLITE_STATIC);
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if (count > media_limit) {
    discord_delete_message(client, event->channel_id, event->id, NULL, NULL);
    return true;
  }
  return false;
}

static void msglimit_check_fetched(struct discord *client,
                                   struct discord_response *response,
                                   const struct discord_message *message) {
  const struct discord_message *update = response->keep;
  /* REST messages may omit guild_id; keep it from the gateway event. */
  struct discord_message complete = *message;
  complete.guild_id = update->guild_id;
  apply_media_check(client, &complete);
}

void msglimit_enforce_edit(struct discord *client,
                           const struct discord_message *event) {
  if (!event->guild_id || !event->id || !event->channel_id)
    return;
  if (event->author && msglimit_is_exempt(event))
    return;

  /* Gateway updates are partial, especially when a link preview arrives.
   * Resolve missing author/content instead of treating it as an exemption or
   * assuming the update describes the whole message. Concord keeps the event
   * alive until the asynchronous request completes. Failed/deleted-message
   * fetches do not count anything. */
  if (!event->author || !event->content) {
    struct discord_ret_message ret = {
        .done = msglimit_check_fetched,
        .keep = event,
    };
    discord_get_channel_message(client, event->channel_id, event->id, &ret);
    return;
  }
  apply_media_check(client, event);
}
