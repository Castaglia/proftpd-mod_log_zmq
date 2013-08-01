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

#include "mod_log.h"
#include "mod_log_zmq.h"
#include "json.h"

#ifndef HAVE_ZMQ_H
# error "ZeroMQ (libzmq) library required"
#endif /* HAVE_ZMQ_H */

#ifndef HAVE_CZMQ_H
# error "CZeroMQ (libczmq) library required"
#endif /* HAVE_CZMQ_H */

#include <zmq.h>
#include <czmq.h>

module log_zmq_module;

static int log_zmq_engine = FALSE;
int log_zmq_logfd = -1;
pool *log_zmq_pool = NULL;

/* DeliveryMode values */
#define LOG_ZMQ_DELIVERY_MODE_OPTIMISTIC	1
#define LOG_ZMQ_DELIVERY_MODE_GUARANTEED	2

/* Format values */
#define LOG_ZMQ_PAYLOAD_FMT_JSON		1

static zctx_t *zctx = NULL;
static void *zsock = NULL;
static array_header *endpoints = NULL;

/* For tracking the size of deleted files. */
static off_t log_zmq_dele_filesz = 0;

static const char *trace_channel = "log_zmq";

/* Necessary prototypes */
static int log_zmq_sess_init(void);

/* Command handlers
 */

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
    CONF_ERROR(cmd, pstrcat("unsupported delivery mode: '", cmd->argv[1], "'",
      NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = delivery_mode;

  return PR_HANDLED(cmd);
}

/* usage: LogZMQEndpoint fmt-name address */
MODRET set_logzmqendpoint(cmd_rec *cmd) {
  config_rec *c;

  /* XXX Future enhancement to support <Anonymous>-specific notifying? */
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (cmd->argc < 3 ||
      cmd->argc > 4) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  /* XXX Double-check that fmt-name is valid, defined, etc. Look up the
   * format string, and stash a pointer to that in the config_rec (but NOT
   * a copy of the format string; don't need to use that much memory.
   *
   * Requires a tweak to mod_log so that LogFormat config directives are
   * added to the config tree.
   */

  c = add_config_param(cmd->argv[0], 3, NULL, NULL, NULL);

  c->argv[1] = pstrdup(c->pool, cmd->argv[2]);
  if (cmd->argc == 4) {
    c->argv[2] = pstrdup(c->pool, cmd->argv[3]);
  }

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
    CONF_ERROR(cmd, "unsupported payload format: '", cmd->argv[1], "'", NULL);
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

/* Command handlers
 */

MODRET log_zmq_any(cmd_rec *cmd) {
  unsigned char *fmt;

  /* XXX For each endpoint, do this.  We don't use log classes, and instead
   * treat each log as if it were CL_ALL.
   *
   * for each endpoint
   *  get LogFormat
   *  create JsonNode
   *  iterate LogFormat, building JsonNode
   *  stringify JsonNode into JSON
   *  send JSON via zsock
   */

  /* XXX Create JsonNode here, and pass ref to it to the meta-examining func. */

  fmt = c->argv[1];
  while (*fmt) {
    pr_signals_handle();

    if (*fmt == LOGFMT_META_START) {
    
    } else {
      fmt++;
    }
  }

  return PR_DECLINED(cmd);
}

MODRET log_zmq_pre_dele(cmd_rec *cmd) {
  char *path;

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
        PR_SESS_DISCONNECT_SESS_INIT_FAILED, NULL);
    }
  }

  return PR_DECLINED(cmd);
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

/* Initialization functions
 */

static int log_zmq_init(void) {
  int zmq_major, zmq_minaor, zmq_patch;

  pr_event_register(&log_zmq_module, "core.exit", log_zmq_exit_ev, NULL);
#ifdef PR_SHARED_MODULE
  pr_event_register(&log_zmq_module, "core.module-unload",
    log_zmq_mod_unload_ev, NULL);
#endif /* PR_SHARED_MODULE */

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
    delivery_mode = *((int *) c->argv[0]);
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
    delivery_mode == LOG_ZMQ_DELIVERY_MODE_OPTIMISTIC ? ZMQ_PUB : ZMQ_PUSH);
  if (zsock == NULL) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error creating ZMQ socket: %s: disabling module",
      zmq_strerror(zmq_errno()));
    zctx_destroy(&zctx);
    log_zmq_engine = FALSE;
    return 0;
  }

  /* XXX Look up LogZMQEndpoint directives, bind socket to those addresses */

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
