/*
 * ProFTPD: mod_log_zmq -- logs data via ZeroMQ (using JSON)
 * Copyright (c) 2013-2023 TJ Saunders
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
 * $Archive: mod_log_zmq.a$
 */

#include "conf.h"
#include "privs.h"
#include "jot.h"
#include "logfmt.h"

#include <zmq.h>
#include <czmq.h>

#define MOD_LOG_ZMQ_VERSION     "mod_log_zmq/0.1"

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030701
# error "ProFTPD 1.3.7rc1 or later required"
#endif

module log_zmq_module;

static int log_zmq_engine = FALSE;
static int log_zmq_logfd = -1;
static pool *log_zmq_pool = NULL;

static pr_table_t *jot_logfmt2json = NULL;

/* DeliveryMode values */
#define LOG_ZMQ_DELIVERY_MODE_OPTIMISTIC	1
#define LOG_ZMQ_DELIVERY_MODE_GUARANTEED	2
static int log_zmq_delivery_mode = LOG_ZMQ_DELIVERY_MODE_OPTIMISTIC;

/* SocketMode values */
#define LOG_ZMQ_SOCKET_MODE_BIND		1
#define LOG_ZMQ_SOCKET_MODE_CONNECT		2
static int log_zmq_socket_mode = LOG_ZMQ_SOCKET_MODE_BIND;

/* Default timeout: 500 millisecs */
#define LOG_ZMQ_TIMEOUT_DEFAULT			500

static void *zctx = NULL;
static void *zsock = NULL;
static array_header *endpoints = NULL;

/* Entries in the field table identify the field name, and the data type:
 * Boolean, number, or string.
 */
struct field_info {
  unsigned int field_type;
  const char *field_name;
  size_t field_namelen;
};

/* For tracking the size of deleted files. */
static off_t log_zmq_dele_filesz = 0;

static const char *trace_channel = "log_zmq";

/* Necessary prototypes */
static int log_zmq_sess_init(void);

/* Message framing?
 *
 * Fluentd style:
 *  tag (topic name/prefix)
 *  timestamp (format?)
 *    According to:
 *      http://www.ruby-doc.org/core-2.0/Time.html
 *
 *    It looks like the format is (strftime(3) pattern):
 *      "%Y-%m-%d %H:%M:%S %z"
 *
 *    e.g.: "2012-11-10 18:16:12 +0100"
 *
 *  payload
 *
 * 0MQ Guide: Pub-Sub Message Envelopes:
 *  http://zguide.zeromq.org/page:all#Pub-Sub-Message-Envelopes
 *
 *  tag/topic name
 *  sender address
 *  payload
 */
static int log_zmq_add_msg_envelope(zmsg_t **msg) {
  return 0;
}

static int log_zmq_send_msg(const char *addr, char *payload,
    size_t payload_len) {
  int res = 0;
  zmsg_t *msg = NULL;

  msg = zmsg_new();
  if (msg == NULL) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error allocating message for LogZMQEndpoint '%s': %s", addr,
      zmq_strerror(zmq_errno()));
    return -1;
  }

  if (log_zmq_add_msg_envelope(&msg) < 0) {
    zmsg_destroy(&msg);
    return -1;
  }

  res = zmsg_addmem(msg, payload, payload_len);
  if (res < 0) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error adding payload to message for LogZMQEndpoint '%s': %s", addr,
      zmq_strerror(zmq_errno()));
    zmsg_destroy(&msg);

    return -1;
  }

  pr_trace_msg(trace_channel, 12, "ZMQ message: %lu frames (%lu bytes)",
    (unsigned long) zmsg_size(msg), (unsigned long) zmsg_content_size(msg));

  res = zmsg_send(&msg, zsock);
  if (res < 0) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error sending message to LogZMQEndpoint '%s': %s", addr,
      zmq_strerror(zmq_errno()));
    zmsg_destroy(&msg);
  }

  return res;
}

/* Logging */

static void log_event(config_rec *c, cmd_rec *cmd) {
  pool *tmp_pool;
  int res;
  pr_jot_ctx_t *jot_ctx;
  pr_jot_filters_t *jot_filters;
  pr_json_object_t *json = NULL;
  const char *fmt_name = NULL;
  char *payload = NULL;
  size_t payload_len = 0;
  unsigned char *log_fmt;

  jot_filters = c->argv[1];
  fmt_name = c->argv[2];
  log_fmt = c->argv[3];

  if (jot_filters == NULL ||
      fmt_name == NULL ||
      log_fmt == NULL) {
    return;
  }

  tmp_pool = make_sub_pool(cmd->tmp_pool);
  jot_ctx = pcalloc(tmp_pool, sizeof(pr_jot_ctx_t));
  json = pr_json_object_alloc(tmp_pool);
  jot_ctx->log = json;
  jot_ctx->user_data = jot_logfmt2json;

  res = pr_jot_resolve_logfmt(tmp_pool, cmd, jot_filters, log_fmt, jot_ctx,
    pr_jot_on_json, NULL, NULL);

  if (res == 0) {
    payload = pr_json_object_to_text(tmp_pool, json, "");
    payload_len = strlen(payload);
    pr_trace_msg(trace_channel, 8, "generated JSON payload for %s: %.*s",
      (char *) cmd->argv[0], (int) payload_len, payload);

  } else {
    /* EPERM indicates that the message was filtered. */
    if (errno != EPERM) {
      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "error generating JSON formatted log message: %s", strerror(errno));
    }

    payload = NULL;
    payload_len = 0;
  }

  pr_json_object_free(json);

  if (payload_len > 0) {
    /* Send payload via ZMQ */
    if (log_zmq_send_msg(c->argv[0], payload, payload_len) < 0) {
      pr_trace_msg(trace_channel, 1, "error sending ZMQ message: %s",
        strerror(errno));
    }
  }

  destroy_pool(tmp_pool);
}

static void log_events(cmd_rec *cmd) {
  register unsigned int i;
  config_rec **elts;

  elts = endpoints->elts;
  for (i = 0; i < endpoints->nelts; i++) {
    config_rec *c;

    pr_signals_handle();

    c = elts[i];
    log_event(c, cmd);
  }
}

/* Command handlers
 */

MODRET log_zmq_any(cmd_rec *cmd) {
  if (log_zmq_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (endpoints == NULL ||
      endpoints->nelts == 0) {
    /* No configured endpoints means no logging work for us to do. */
    return PR_DECLINED(cmd);
  }

  log_events(cmd);
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
    if (pr_fsio_stat(path, &st) == 0) {
      log_zmq_dele_filesz = st.st_size;
    }
  }

  return PR_DECLINED(cmd);
}

static unsigned char *find_log_fmt(server_rec *s, const char *fmt_name) {
  config_rec *c;
  unsigned char *log_fmt = NULL;

  c = find_config(s->conf, CONF_PARAM, "LogFormat", FALSE);
  while (c != NULL) {
    pr_signals_handle();

    if (strcmp(fmt_name, c->argv[0]) == 0) {
      log_fmt = c->argv[1];
      break;
    }

    c = find_config_next(c, c->next, CONF_PARAM, "LogFormat", FALSE);
  }

  return log_fmt;
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
  const char *fmt_name, *rules;
  unsigned char *log_fmt = NULL, *custom_fmt;
  size_t log_fmtlen = 0;
  pr_jot_filters_t *jot_filters;

  /* XXX Future enhancement to support <Anonymous>-specific notifying? */
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  CHECK_ARGS(cmd, 2);

  c = add_config_param(cmd->argv[0], 4, NULL, NULL, NULL, NULL);

  rules = "ALL";
  jot_filters = pr_jot_filters_create(c->pool, rules,
    PR_JOT_FILTER_TYPE_COMMANDS_WITH_CLASSES,
    PR_JOT_FILTER_FL_ALL_INCL_ALL);
  if (jot_filters == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unable to use events '", rules,
      "': ", strerror(errno), NULL));
  }

  fmt_name = cmd->argv[2];

  /* Make sure that the given LogFormat name is known. */
  log_fmt = find_log_fmt(cmd->server, fmt_name);
  if (log_fmt == NULL) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "no LogFormat '", fmt_name,
      "' configured", NULL));
  }

  /* We customize the given LogFormat a little, for backward compatibility,
   * adding in the variables for including the "connecting", "disconnecting"
   * keys in the emitted JSON message.
   */
  log_fmtlen = strlen((char *) log_fmt);
  custom_fmt = pcalloc(c->pool, log_fmtlen + 5);
  memcpy(custom_fmt, log_fmt, log_fmtlen);

  /* Now we very naughtily, programmatically add in the metas. */
  custom_fmt[log_fmtlen] = LOGFMT_META_START;
  custom_fmt[log_fmtlen+1] = LOGFMT_META_CONNECT;

  custom_fmt[log_fmtlen+2] = LOGFMT_META_START;
  custom_fmt[log_fmtlen+3] = LOGFMT_META_DISCONNECT;

  c->argv[0] = pstrdup(c->pool, cmd->argv[1]);
  c->argv[1] = jot_filters;
  c->argv[2] = pstrdup(c->pool, fmt_name);
  c->argv[3] = custom_fmt;

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

/* usage: LogZMQLog path|"none" */
MODRET set_logzmqlog(cmd_rec *cmd) {
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  return PR_HANDLED(cmd);
}

/* usage: LogZMQMaxPendingMessages count */
MODRET set_logzmqmaxpendingmsgs(cmd_rec *cmd) {
  config_rec *c;
  int hwm = 0;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  hwm = atoi(cmd->argv[1]);
  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = hwm;

  return PR_HANDLED(cmd);
}

/* usage: LogZMQMessageEnvelope ... */
MODRET set_logzmqmessageenvelope(cmd_rec *cmd) {
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  return PR_HANDLED(cmd);
}

/* usage: LogZMQSocketMode bind|connect */
MODRET set_logzmqsocketmode(cmd_rec *cmd) {
  config_rec *c;
  int socket_mode = 0;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  if (strcasecmp(cmd->argv[1], "bind") == 0) {
    socket_mode = LOG_ZMQ_SOCKET_MODE_BIND;

  } else if (strcasecmp(cmd->argv[1], "connect") == 0) {
    socket_mode = LOG_ZMQ_SOCKET_MODE_CONNECT;

  } else {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unsupported socket mode: '",
      cmd->argv[1], "'", NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = socket_mode;

  return PR_HANDLED(cmd);
}

/* usage: LogZMQTimeout millisecs */
MODRET set_logzmqtimeout(cmd_rec *cmd) {
  config_rec *c;
  int timeout = 0;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  timeout = atoi(cmd->argv[1]);
  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = timeout;

  return PR_HANDLED(cmd);
}

/* Event listeners
 */

static void log_zmq_exit_ev(const void *event_data, void *user_data) {
  if (zctx != NULL) {
    int linger_timeout;

    /* Set a lingering timeout for a short time, to ensure that the last
     * message sent gets out.
     */
    linger_timeout = 750;
    zmq_setsockopt(zsock, ZMQ_LINGER, &linger_timeout, sizeof(linger_timeout));

    zmq_ctx_destroy(zctx);
    zctx = NULL;
    zsock = NULL;
  }
}

#if defined(PR_SHARED_MODULE)
static void log_zmq_mod_unload_ev(const void *event_data, void *user_data) {
  if (strcmp("mod_log_zmq.c", (char *) event_data) != 0) {
    return;
  }

  pr_event_unregister(&log_zmq_module, NULL, NULL);
}
#endif /* PR_SHARED_MODULE */

static void log_zmq_restart_ev(const void *event_data, void *user_data) {
  destroy_pool(log_zmq_pool);
  endpoints = NULL;

  log_zmq_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(log_zmq_pool, MOD_LOG_ZMQ_VERSION);
}

static void log_zmq_sess_reinit_ev(const void *event_data, void *user_data) {
  int res;

  /* A HOST command changed the main_server pointer, reinitialize ourselves. */

  pr_event_unregister(&log_zmq_module, "core.exit", log_zmq_exit_ev);
  pr_event_unregister(&log_zmq_module, "core.session-reinit",
    log_zmq_sess_reinit_ev);

  log_zmq_engine = FALSE;

  (void) close(log_zmq_logfd);
  log_zmq_logfd = -1;

  if (zctx != NULL) {
    zmq_ctx_destroy(zctx);
    zctx = NULL;
  }

  res = log_zmq_sess_init();
  if (res < 0) {
    pr_session_disconnect(&log_zmq_module,
      PR_SESS_DISCONNECT_SESSION_INIT_FAILED, NULL);
  }
}

/* Initialization functions
 */

static int log_zmq_init(void) {
  int zmq_major, zmq_minor, zmq_patch;

#if defined(PR_SHARED_MODULE)
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

  if (jot_logfmt2json == NULL) {
    jot_logfmt2json = pr_jot_get_logfmt2json(log_zmq_pool);
  }

  return 0;
}

static int log_zmq_sess_init(void) {
  config_rec *c;
  int timeout = LOG_ZMQ_TIMEOUT_DEFAULT;

  pr_event_register(&log_zmq_module, "core.session-reinit",
    log_zmq_sess_reinit_ev, NULL);

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

    if (strcasecmp(path, "none") != 0) {
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

  c = find_config(main_server->conf, CONF_PARAM, "LogZMQSocketMode", FALSE);
  if (c != NULL) {
    log_zmq_socket_mode = *((int *) c->argv[0]);
  }

  zctx = zmq_ctx_new();
  if (zctx == NULL) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error creating ZMQ context: %s: disabling module",
      zmq_strerror(zmq_errno()));
    log_zmq_engine = FALSE;
    return 0;
  }

  zsock = zmq_socket(zctx,
    log_zmq_delivery_mode == LOG_ZMQ_DELIVERY_MODE_OPTIMISTIC ?
      ZMQ_PUB : ZMQ_PUSH);
  if (zsock == NULL) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error creating ZMQ socket: %s: disabling module",
      zmq_strerror(zmq_errno()));
    zmq_ctx_destroy(zctx);
    zctx = NULL;

    log_zmq_engine = FALSE;
    return 0;
  }

#if defined(PR_USE_IPV6)
  if (pr_netaddr_use_ipv6()) {
    int ipv4_only = 0;

    /* Enable IPv6 ZMQ sockets. */
    if (zmq_setsockopt(zsock, ZMQ_IPV4ONLY, &ipv4_only, sizeof(int)) < 0) {
      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "error setting IPV4ONLY option to 'false': %s",
        zmq_strerror(zmq_errno()));
    }
  }
#endif /* PR_USE_IPV6 */

  /* XXX Any need to set the ZMQ_IDENTITY sockopt? */

  c = find_config(main_server->conf, CONF_PARAM, "LogZMQMaxPendingMessages",
    FALSE);
  if (c != NULL) {
    int val;

    val = *((int *) c->argv[0]);

    if (zmq_setsockopt(zsock, ZMQ_SNDHWM, &val, sizeof(int)) < 0) {
      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "error setting LogZMQMaxPendingMessages to %d: %s", val,
        zmq_strerror(zmq_errno()));
    }
  }

  c = find_config(main_server->conf, CONF_PARAM, "LogZMQTimeout", FALSE);
  if (c != NULL) {
    timeout = *((int *) c->argv[0]);
  }

  if (zmq_setsockopt(zsock, ZMQ_SNDTIMEO, &timeout, sizeof(int)) < 0) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error setting LogZMQTimeout to %d millisecs: %s", timeout,
      zmq_strerror(zmq_errno()));
  }

  /* Look up LogZMQEndpoint directives, bind/connect socket to those
   * addresses.
   */
  c = find_config(main_server->conf, CONF_PARAM, "LogZMQEndpoint", FALSE);
  while (c != NULL) {
    char *addr = NULL;

    pr_signals_handle();

    addr = c->argv[0];

    if (log_zmq_socket_mode == LOG_ZMQ_SOCKET_MODE_BIND) {
      if (zmq_bind(zsock, addr) < 0) {
        (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
          "error binding to LogZMQEndpoint '%s': %s", addr,
          zmq_strerror(zmq_errno()));
        c = find_config_next(c, c->next, CONF_PARAM, "LogZMQEndpoint", FALSE);
        continue;
      }

    } else if (log_zmq_socket_mode == LOG_ZMQ_SOCKET_MODE_CONNECT) {
      if (zmq_connect(zsock, addr) < 0) {
        (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
          "error connecting to LogZMQEndpoint '%s': %s", addr,
          zmq_strerror(zmq_errno()));
        c = find_config_next(c, c->next, CONF_PARAM, "LogZMQEndpoint", FALSE);
        continue;
      }
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
    return 0;
  }

  pr_event_register(&log_zmq_module, "core.exit", log_zmq_exit_ev, NULL);
  return 0;
}

/* Module API tables
 */

static conftable log_zmq_conftab[] = {
  { "LogZMQDeliveryMode",	set_logzmqdeliverymode,		NULL },
  { "LogZMQEndpoint",		set_logzmqendpoint,		NULL },
  { "LogZMQEngine",		set_logzmqengine,		NULL },
  { "LogZMQLog",		set_logzmqlog,			NULL },
  { "LogZMQMaxPendingMessages", set_logzmqmaxpendingmsgs,	NULL },
  { "LogZMQMessageEnvelope",	set_logzmqmessageenvelope,	NULL },
  { "LogZMQSocketMode",		set_logzmqsocketmode,		NULL },
  { "LogZMQTimeout", 		set_logzmqtimeout,		NULL },

  { NULL }
};

static cmdtable log_zmq_cmdtab[] = {
  { PRE_CMD,		C_DELE,	G_NONE,	log_zmq_pre_dele,	FALSE, FALSE },
  { LOG_CMD,		C_ANY,	G_NONE, log_zmq_any,		FALSE, FALSE },
  { LOG_CMD_ERR,	C_ANY,	G_NONE, log_zmq_any,		FALSE, FALSE },

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
