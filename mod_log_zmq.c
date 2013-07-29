/*
 * ProFTPD: mod_log_zmq -- logs JSON data via ZeroMQ
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

#include "mod_log_zmq.h"
#include "json.h"

#ifndef HAVE_LIBZMQ
# error "ZeroMQ (libzmq) library required"
#endif /* HAVE_LIBZMQ */

#ifndef HAVE_LIBCZMQ
# error "CZeroMQ (libczmq) library required"
#endif /* HAVE_LIBCZMQ */

#include <zmq.h>
#include <czmq.h>

module log_zmq_module;

static int log_zmq_engine = FALSE;
int log_zmq_logfd = -1;
pool *log_zmq_pool = NULL;

static zctx_t *zctx = NULL;
static void *publisher = NULL;
static array_header *notifys = NULL;

static const char *trace_channel = "log_zmq";

/* Command handlers
 */

/* Configuration handlers
 */

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

/* usage: LogZMQNotify fmt-name address topic-name */
MODRET set_logzmqnotify(cmd_rec *cmd) {
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
   */

  c = add_config_param(cmd->argv[0], 3, NULL, NULL, NULL);

  c->argv[1] = pstrdup(c->pool, cmd->argv[2]);
  if (cmd->argc == 4) {
    c->argv[2] = pstrdup(c->pool, cmd->argv[3]);
  }

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
    publisher = NULL;
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

  zctx = zctx_new();
  if (zctx == NULL) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error creating ZMQ context: %s: disabling module",
      zmq_strerror(zmq_errno()));
    log_zmq_engine = FALSE;
    return 0;
  }

  publisher = zsocket_new(zctx, ZMQ_PUB);
  if (publisher == NULL) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error creating ZMQ publisher socket: %s: disabling module",
      zmq_strerror(zmq_errno()));
    zctx_destroy(&zctx);
    log_zmq_engine = FALSE;
    return 0;
  }

  /* XXX Look up LogZMQNotify directives, bind publisher to those addresses */

  return 0;
}

/* Module API tables
 */

static conftable log_zmq_conftab[] = {
  { "LogZMQEngine",	set_logzmqengine,	NULL },
  { "LogZMQLog",	set_logzmqlog,		NULL },
  { "LogZMQNotify",	set_logzmqnotify,	NULL },

  { NULL }
};

static cmdtable log_zmq_cmdtab[] = {
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
