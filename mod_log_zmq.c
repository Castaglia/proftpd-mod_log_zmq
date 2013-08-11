/*
 * ProFTPD: mod_log_zmq -- logs data via ZeroMQ (using JSON)
 *
 * Copyright (c) 2013 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * This is mod_log_zmq, contrib software for proftpd 1.3.x and above.
 * For more information contact TJ Saunders <tj@castaglia.org>.
 *
 * --- DO NOT DELETE BELOW THIS LINE ----
 * $Libraries: -lzmq -lczmq$
 * $Archive: mod_log_zmq.a $
 */

#include "conf.h"
#include "privs.h"
#include "mod_log.h"
#include "mod_log_zmq.h"
#include "json.h"

#include <zmq.h>
#include <czmq.h>

module log_zmq_module;

static int log_zmq_engine = FALSE;
int log_zmq_logfd = -1;
pool *log_zmq_pool = NULL;

/* DeliveryMode values */
#define LOG_ZMQ_DELIVERY_MODE_OPTIMISTIC	1
#define LOG_ZMQ_DELIVERY_MODE_GUARANTEED	2
static int log_zmq_delivery_mode = LOG_ZMQ_DELIVERY_MODE_OPTIMISTIC;

/* Format values */
#define LOG_ZMQ_PAYLOAD_FMT_JSON		1
#define LOG_ZMQ_PAYLOAD_FMT_MSGPACK		2
static int log_zmq_payload_fmt = LOG_ZMQ_PAYLOAD_FMT_JSON;

static zctx_t *zctx = NULL;
static void *zsock = NULL;
static array_header *endpoints = NULL;
static pr_table_t *field_idtab = NULL;

/* Entries in the field table identify the field name, and the data type:
 * Boolean, number, or string.
 */

struct field_info {
  unsigned int field_type;
  const char *field_name;
  size_t field_namelen;
};

#define LOG_ZMQ_FIELD_TYPE_BOOLEAN		1
#define LOG_ZMQ_FIELD_TYPE_NUMBER		2
#define LOG_ZMQ_FIELD_TYPE_STRING		3

/* For tracking the size of deleted files. */
static off_t log_zmq_dele_filesz = 0;

static const char *trace_channel = "log_zmq";

/* Necessary prototypes */
static int log_zmq_sess_init(void);

/* Key comparison for the ID/name table. */
static int field_idcmp(const void *k1, size_t ksz1, const void *k2,
  size_t ksz2) {

  /* Return zero to indicate a match, non-zero otherwise. */
  return (*((unsigned char *) k1) == *((unsigned char *) k2) ? 0 : 1);
}

/* Key "hash" callback for ID/name table. */
static unsigned int field_idhash(const void *k, size_t ksz) {
  unsigned char c;
  unsigned int res;

  memcpy(&c, k, ksz);
  res = (unsigned int) (c << 8);

  return res;
}

static int field_add(pool *p, unsigned char id, const char *name,
    unsigned int field_type) {
  unsigned char *k;
  struct field_info *fi;
  int res;

  k = palloc(p, sizeof(unsigned char));
  *k = id;

  fi = palloc(p, sizeof(struct field_info));
  fi->field_type = field_type;
  fi->field_name = name;
  fi->field_namelen = strlen(name) + 1;

  res = pr_table_kadd(field_idtab, (const void *) k, sizeof(unsigned char),
    fi, sizeof(struct field_info *));
  return res;
}

static int log_zmq_mkfieldtab(pool *p) {
  field_idtab = pr_table_alloc(p, 0);
  if (pr_table_ctl(field_idtab, PR_TABLE_CTL_SET_KEY_CMP,
    (void *) field_idcmp) < 0) {
    int xerrno = errno;

    pr_log_pri(PR_LOG_INFO, "error setting key comparison callback for "
      "field ID/names: %s", strerror(errno));

    errno = xerrno;
    return -1;
  }

  if (pr_table_ctl(field_idtab, PR_TABLE_CTL_SET_KEY_HASH,
    (void *) field_idhash) < 0) {
    int xerrno = errno;

    pr_log_pri(PR_LOG_INFO, "error setting key hash callback for "
      "field ID/names: %s", strerror(errno));

    errno = xerrno;
    return -1;
  }

  /* Now populate the table with the ID/name values.  The key is the
   * LogFormat "meta" ID, and the value is the corresponding name string,
   * for use e.g. as JSON object member names.
   */

  field_add(p, LOGFMT_META_BYTES_SENT, "bytes_sent",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_FILENAME, "file",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_ENV_VAR, "env:",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_REMOTE_HOST, "remote_dns",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_REMOTE_IP, "remote_ip",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_IDENT_USER, "identd_user",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_PID, "pid",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_TIME, "local_time",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_SECONDS, "secs",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_COMMAND, "raw_command",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_LOCAL_NAME, "server_name",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_LOCAL_PORT, "local_port",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_LOCAL_IP, "local_ip",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_LOCAL_FQDN, "server_dns",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_USER, "user",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_ORIGINAL_USER, "original_user",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_RESPONSE_CODE, "response_code",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_CLASS, "connection_class",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_ANON_PASS, "anon_password",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_METHOD, "command",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_XFER_PATH, "transfer_path",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_DIR_NAME, "dir_name",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_DIR_PATH, "dir_path",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_CMD_PARAMS, "command_params",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_RESPONSE_STR, "response_msg",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_PROTOCOL, "protocol",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_VERSION, "server_version",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_RENAME_FROM, "rename_from",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_FILE_MODIFIED, "file_modified",
    LOG_ZMQ_FIELD_TYPE_BOOLEAN);

  field_add(p, LOGFMT_META_UID, "uid",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_GID, "gid",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_RAW_BYTES_IN, "session_bytes_rcvd",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_RAW_BYTES_OUT, "session_bytes_sent",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_EOS_REASON, "session_end_reason",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_VHOST_IP, "server_ip",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_NOTE_VAR, "note:",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_XFER_STATUS, "transfer_status",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_XFER_FAILURE, "transfer_failure",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_MICROSECS, "microsecs",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_MILLISECS, "millisecs",
    LOG_ZMQ_FIELD_TYPE_NUMBER);

  field_add(p, LOGFMT_META_ISO8601, "timestamp",
    LOG_ZMQ_FIELD_TYPE_STRING);

  field_add(p, LOGFMT_META_GROUP, "group",
    LOG_ZMQ_FIELD_TYPE_STRING);

  return 0;
}

static void log_zmq_mkjson(void *json, const char *field_name,
    size_t field_namelen, unsigned int field_type, void *field_value) {
  JsonNode *field = NULL;

  switch (field_type) {
    case LOG_ZMQ_FIELD_TYPE_STRING:
      field = json_mkstring((const char *) field_value);
      break;

    case LOG_ZMQ_FIELD_TYPE_NUMBER:
      field = json_mknumber(*((double *) field_value));
      break;

    case LOG_ZMQ_FIELD_TYPE_BOOLEAN:
      field = json_mkbool(*((bool *) field_value));
      break;

    default:
      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "unsupported field type: %u", field_type);
  }

  if (field != NULL) {
    json_append_member(json, field_name, field);
  }
}

static int find_next_meta(pool *p, cmd_rec *cmd, unsigned char **fmt,
    void *obj,
    void (*mkfield)(void *, const char *, size_t, unsigned int, void *)) {
  struct field_info *fi;
  char buf[PR_TUNABLE_PATH_MAX+1], *ptr = NULL;
  unsigned char *m;

  m = (*fmt) + 1;
  switch (*m) {
    case LOGFMT_META_BYTES_SENT:
      break;

    case LOGFMT_META_FILENAME:
      break;

    case LOGFMT_META_ENV_VAR:
      break;

    case LOGFMT_META_REMOTE_HOST:
      break;

    case LOGFMT_META_REMOTE_IP:
      break;

    case LOGFMT_META_IDENT_USER:
      break;

    case LOGFMT_META_PID:
      break;

    case LOGFMT_META_TIME:
      break;

    case LOGFMT_META_SECONDS:
      break;

    case LOGFMT_META_COMMAND:
      break;

    case LOGFMT_META_LOCAL_NAME:
      break;

    case LOGFMT_META_LOCAL_PORT:
      break;

    case LOGFMT_META_LOCAL_IP:
      break;

    case LOGFMT_META_LOCAL_FQDN:
      break;

    case LOGFMT_META_USER:
      break;

    case LOGFMT_META_ORIGINAL_USER:
      break;

    case LOGFMT_META_RESPONSE_CODE:
      break;

    case LOGFMT_META_CLASS:
      break;

    case LOGFMT_META_ANON_PASS: {
      char *anon_pass;

      fi = pr_table_kget(field_idtab, (const void *) m, sizeof(unsigned char),
        NULL);

      anon_pass = pr_table_get(session.notes, "mod_auth.anon-passwd", NULL);
      if (anon_pass == NULL) {
        anon_pass = "UNKNOWN";
      }

      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        anon_pass);

      m++;
      break;
    }

    case LOGFMT_META_METHOD:
      break;

    default:
      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "skipping unsupported LogFormat meta %d", *m);
      break;
  }

  return 0;
}

static int log_zmq_mkrecord(const char *event_name, cmd_rec *cmd,
    unsigned char *fmt, void *obj,
    void (*mkfield)(void *, const char *, size_t, unsigned int, void *)) {

  while (*fmt) {
    pr_signals_handle();

    if (*fmt == LOGFMT_META_START) {
      find_next_meta(cmd->tmp_pool, cmd, &fmt, obj, mkfield);

    } else {
      fmt++;
    }
  }

  return 0;
}

/* Command handlers
 */

MODRET log_zmq_any(cmd_rec *cmd) {
  register unsigned int i;
  config_rec **elts;

  if (log_zmq_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (endpoints == NULL ||
      endpoints->nelts == 0) {
    /* No configured endpoints means no logging work for us to do. */
    return PR_DECLINED(cmd);
  }

  elts = endpoints->elts;
  for (i = 0; i < endpoints->nelts; i++) {
    char errstr[256];
    config_rec *c;
    int res;
    void *obj = NULL;

    c = elts[i];

    obj = json_mkobject();
    res = log_zmq_mkrecord(cmd->argv[0], cmd, c->argv[1], obj, log_zmq_mkjson);

    if (!json_check(obj, errstr)) {
      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "JSON structural problems: %s", errstr);

    } else {
      char *payload;

      payload = json_encode(obj);

      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "generated payload: '%s'", payload);

      /* XXX send payload via socket */
    }

    json_delete(obj);
  }

  return PR_DECLINED(cmd);
}

MODRET log_zmq_pre_dele(cmd_rec *cmd) {
  char *path;

  if (log_zmq_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  log_zmq_dele_filesz = 0;

  path = dir_canonical_path(cmd->tmp_pool,
    pr_fs_decode_path(cmd->tmp_pool, cmd->arg));
  if (path) {
    struct stat st;

    /* Briefly cache the size of the file being deleted, so that it can be
     * logged properly using %b.
     */
    pr_fs_clear_cache();
    if (pr_fsio_stat(path, &st) == 0)
      log_zmq_dele_filesz = st.st_size;
  }

  return PR_DECLINED(cmd);
}

MODRET log_zmq_post_host(cmd_rec *cmd) {
  /* If the HOST command changed the main_server pointer, reinitialize
   * ourselves.
   */
  if (session.prev_server != NULL) {
    int res;

    log_zmq_engine = FALSE;

    (void) close(log_zmq_logfd);
    log_zmq_logfd = -1;

    if (zctx != NULL) {
      zctx_destroy(&zctx);
    }

    res = log_zmq_sess_init();
    if (res < 0) {
      pr_session_disconnect(&log_zmq_module,
        PR_SESS_DISCONNECT_SESSION_INIT_FAILED, NULL);
    }
  }

  return PR_DECLINED(cmd);
}

/* Configuration handlers
 */

/* usage: LogZMQDeliveryMode optimistic|guaranteed */
MODRET set_logzmqdeliverymode(cmd_rec *cmd) {
  config_rec *c;
  int delivery_mode = 0;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1); 

  if (strcasecmp(cmd->argv[1], "optimistic") == 0) {
    delivery_mode = LOG_ZMQ_DELIVERY_MODE_OPTIMISTIC;

  } else if (strcasecmp(cmd->argv[1], "guaranteed") == 0) {
    delivery_mode = LOG_ZMQ_DELIVERY_MODE_GUARANTEED;

  } else {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unsupported delivery mode: '",
      cmd->argv[1], "'", NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = delivery_mode;

  return PR_HANDLED(cmd);
}

/* usage: LogZMQEndpoint address logfmt-name */
MODRET set_logzmqendpoint(cmd_rec *cmd) {
  config_rec *c;
  unsigned char *logfmt = NULL;

  /* XXX Future enhancement to support <Anonymous>-specific notifying? */
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  CHECK_ARGS(cmd, 2)

  /* Double-check that logfmt-name is valid, defined, etc. Look up the
   * format string, and stash a pointer to that in the config_rec (but NOT
   * a copy of the format string; don't need to use that much memory.
   */
  c = find_config(cmd->server->conf, CONF_PARAM, "LogFormat", FALSE);
  while (c != NULL) {
    if (strcmp(c->argv[0], cmd->argv[2]) == 0) {
      logfmt = c->argv[1];
      break;
    }

    c = find_config_next(c, c->next, CONF_PARAM, "LogFormat", FALSE);
  }

  if (logfmt == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "no LogFormat '", cmd->argv[2],
      "' configured", NULL));
  }

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);
  c->argv[0] = pstrdup(c->pool, cmd->argv[1]);
  c->argv[1] = logfmt;

  return PR_HANDLED(cmd);
}

/* usage: LogZMQEngine on|off */
MODRET set_logzmqengine(cmd_rec *cmd) {
  int engine;
  config_rec *c;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  engine = get_boolean(cmd, 1);
  if (engine == -1) {
    CONF_ERROR(cmd, "expected Boolean parameter");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = engine;

  return PR_HANDLED(cmd);
}

/* usage: LogZMQFormat json */
MODRET set_logzmqformat(cmd_rec *cmd) {
  int payload_fmt = 0;
  config_rec *c;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  if (strcasecmp(cmd->argv[1], "json") == 0) {
    payload_fmt = LOG_ZMQ_PAYLOAD_FMT_JSON;

  } else {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unsupported payload format: '",
      cmd->argv[1], "'", NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = payload_fmt;

  return PR_HANDLED(cmd);
}

/* usage: LogZMQLog path|"none" */
MODRET set_logzmqlog(cmd_rec *cmd) {
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* Event listeners
 */

static void log_zmq_exit_ev(const void *event_data, void *user_data) {
  /* XXX Log EXIT message */

  if (zctx != NULL) {
    /* Set a lingering timeout for a short time, to ensure that the last
     * message sent gets out.
     */
    zctx_set_linger(zctx, 750);

    zctx_destroy(&zctx);
    zsock = NULL;
  }
}

#ifdef PR_SHARED_MODULE
static void log_zmq_mod_unload_ev(const void *event_data, void *user_data) {
  if (strcmp("mod_log_zmq.c", (char *) event_data) == 0) {
    pr_event_unregister(&log_zmq_module, NULL);
  }
}
#endif /* PR_SHARED_MODULE */

static void log_zmq_restart_ev(const void *event_data, void *user_data) {
  destroy_pool(log_zmq_pool);
  field_idtab = NULL;
  endpoints = NULL;

  log_zmq_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(log_zmq_pool, MOD_LOG_ZMQ_VERSION);

  if (log_zmq_mkfieldtab(log_zmq_pool) < 0) {
    /* XXX exit here */
  }

}

/* Initialization functions
 */

static int log_zmq_init(void) {
  int zmq_major, zmq_minor, zmq_patch;

  pr_event_register(&log_zmq_module, "core.exit", log_zmq_exit_ev, NULL);
#ifdef PR_SHARED_MODULE
  pr_event_register(&log_zmq_module, "core.module-unload",
    log_zmq_mod_unload_ev, NULL);
#endif /* PR_SHARED_MODULE */
  pr_event_register(&log_zmq_module, "core.restart", log_zmq_restart_ev, NULL);

  zmq_version(&zmq_major, &zmq_minor, &zmq_patch);

  pr_log_debug(DEBUG0, MOD_LOG_ZMQ_VERSION ": using czmq-%d.%d.%d",
    CZMQ_VERSION_MAJOR, CZMQ_VERSION_MINOR, CZMQ_VERSION_PATCH);
  pr_log_debug(DEBUG0, MOD_LOG_ZMQ_VERSION ": using zmq-%d.%d.%d",
    zmq_major, zmq_minor, zmq_patch);

  if (zmq_major != ZMQ_VERSION_MAJOR) {
    pr_log_pri(PR_LOG_ERR, MOD_LOG_ZMQ_VERSION
      ": compiled against zmq-%d.%d.%d headers, but linked to "
      "zmq-%d.%d.%d library",
      ZMQ_VERSION_MAJOR, ZMQ_VERSION_MINOR, ZMQ_VERSION_PATCH,
      zmq_major, zmq_minor, zmq_patch);
  }

  log_zmq_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(log_zmq_pool, MOD_LOG_ZMQ_VERSION);

  if (log_zmq_mkfieldtab(log_zmq_pool) < 0) {
    return -1;
  }

  return 0;
}

static int log_zmq_sess_init(void) {
  config_rec *c;

  c = find_config(main_server->conf, CONF_PARAM, "LogZMQEngine", FALSE);
  if (c != NULL) {
    log_zmq_engine = *((int *) c->argv[0]);
  }

  if (log_zmq_engine == FALSE) {
    return 0;
  }

  c = find_config(main_server->conf, CONF_PARAM, "LogZMQLog", FALSE);
  if (c != NULL) {
    char *path;

    path = c->argv[0];

    if (strncasecmp(path, "none", 5) != 0) {
      int res, xerrno;

      pr_signals_block();
      PRIVS_ROOT
      res = pr_log_openfile(path, &log_zmq_logfd, 0660);
      xerrno = errno;
      PRIVS_RELINQUISH
      pr_signals_unblock();

      if (res < 0) {
        pr_log_pri(PR_LOG_NOTICE, MOD_LOG_ZMQ_VERSION
          ": error opening LogZMQLog '%s': %s", path, strerror(xerrno)); 
      }
    }
  }

  c = find_config(main_server->conf, CONF_PARAM, "LogZMQDeliveryMode", FALSE);
  if (c != NULL) {
    log_zmq_delivery_mode = *((int *) c->argv[0]);
  }

  zctx = zctx_new();
  if (zctx == NULL) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error creating ZMQ context: %s: disabling module",
      zmq_strerror(zmq_errno()));
    log_zmq_engine = FALSE;
    return 0;
  }

  zsock = zsocket_new(zctx,
    log_zmq_delivery_mode == LOG_ZMQ_DELIVERY_MODE_OPTIMISTIC ?
      ZMQ_PUB : ZMQ_PUSH);
  if (zsock == NULL) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error creating ZMQ socket: %s: disabling module",
      zmq_strerror(zmq_errno()));
    zctx_destroy(&zctx);
    log_zmq_engine = FALSE;
    return 0;
  }

  /* Look up LogZMQEndpoint directives, bind socket to those addresses */
  c = find_config(main_server->conf, CONF_PARAM, "LogZMQEndpoint", FALSE);
  while (c != NULL) {
    char *addr = NULL;

    pr_signals_handle();

    addr = c->argv[1];
    if (zsocket_bind(zsock, addr) < 0) {
      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "error binding to LogZMQEndpoint '%s': %s", addr,
        zmq_strerror(zmq_errno()));
      c = find_config_next(c, c->next, CONF_PARAM, "LogZMQEndpoint", FALSE);
      continue;
    }

    if (endpoints == NULL) {
      endpoints = make_array(log_zmq_pool, 1, sizeof(config_rec *));
    }

    *((config_rec **) push_array(endpoints)) = c;
    c = find_config_next(c, c->next, CONF_PARAM, "LogZMQEndpoint", FALSE);
  }

  /* If no endpoints are configured, log a warning and disable ourselves. */
  if (endpoints == NULL) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "no LogZMQEndpoints configured, disabling module");
    log_zmq_engine = FALSE;
  }

  return 0;
}

/* Module API tables
 */

static conftable log_zmq_conftab[] = {
  { "LogZMQDeliveryMode",	set_logzmqdeliverymode,	NULL },
  { "LogZMQEndpoint",		set_logzmqendpoint,	NULL },
  { "LogZMQEngine",		set_logzmqengine,	NULL },
  { "LogZMQFormat",		set_logzmqformat,	NULL },
  { "LogZMQLog",		set_logzmqlog,		NULL },

  { NULL }
};

static cmdtable log_zmq_cmdtab[] = {
  { PRE_CMD,		C_DELE,	G_NONE,	log_zmq_pre_dele,	FALSE, FALSE },
  { LOG_CMD,		C_ANY,	G_NONE, log_zmq_any,		FALSE, FALSE },
  { LOG_CMD_ERR,	C_ANY,	G_NONE, log_zmq_any,		FALSE, FALSE },
  { POST_CMD,		C_HOST, G_NONE,	log_zmq_post_host,	FALSE, FALSE },

  { 0, NULL }
};

module log_zmq_module = {
  NULL, NULL,

  /* Module API version 2.0 */
  0x20,

  /* Module name */
  "log_zmq",

  /* Module configuration handler table */
  log_zmq_conftab,

  /* Module command handler table */
  log_zmq_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization function */
  log_zmq_init,

  /* Session initialization function */
  log_zmq_sess_init,

  /* Module version */
  MOD_LOG_ZMQ_VERSION
};
