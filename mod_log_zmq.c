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

/* SocketMode values */
#define LOG_ZMQ_SOCKET_MODE_BIND		1
#define LOG_ZMQ_SOCKET_MODE_CONNECT		2
static int log_zmq_socket_mode = LOG_ZMQ_SOCKET_MODE_BIND;

/* Message/payload format values */
#define LOG_ZMQ_PAYLOAD_FMT_JSON		1
#define LOG_ZMQ_PAYLOAD_FMT_MSGPACK		2
static int log_zmq_payload_fmt = LOG_ZMQ_PAYLOAD_FMT_JSON;

/* Event flags */
#define LOG_ZMQ_EVENT_FL_CONNECT		1
#define LOG_ZMQ_EVENT_FL_REQUEST		2
#define LOG_ZMQ_EVENT_FL_DISCONNECT		3

/* Default timeout: 500 millisecs */
#define LOG_ZMQ_TIMEOUT_DEFAULT			500

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

/* The LogFormat "meta" values are in the unsigned char range; for our
 * specific "meta" values, then, choose something greater than 256.
 */
#define LOG_ZMQ_META_CONNECT			427
#define LOG_ZMQ_META_DISCONNECT			428

#define LOG_ZMQ_FIELD_TYPE_BOOLEAN		1
#define LOG_ZMQ_FIELD_TYPE_NUMBER		2
#define LOG_ZMQ_FIELD_TYPE_STRING		3

/* For tracking the size of deleted files. */
static off_t log_zmq_dele_filesz = 0;

static const char *trace_channel = "log_zmq";

/* Necessary prototypes */
static int log_zmq_sess_init(void);

/* Out-of-memory handling. */
static void log_zmq_oom(void) {
  pr_log_pri(PR_LOG_CRIT, MOD_LOG_ZMQ_VERSION ": Out of memory!");
  _exit(1);
}

/* Key comparison for the ID/name table. */
static int field_idcmp(const void *k1, size_t ksz1, const void *k2,
  size_t ksz2) {

  /* Return zero to indicate a match, non-zero otherwise. */
  return (*((unsigned int *) k1) == *((unsigned int *) k2) ? 0 : 1);
}

/* Key "hash" callback for ID/name table. */
static unsigned int field_idhash(const void *k, size_t ksz) {
  unsigned int c;
  unsigned int res;

  memcpy(&c, k, ksz);
  res = (c << 8);

  return res;
}

static int field_add(pool *p, unsigned int id, const char *name,
    unsigned int field_type) {
  unsigned int *k;
  struct field_info *fi;
  int res;

  k = palloc(p, sizeof(unsigned int));
  *k = id;

  fi = palloc(p, sizeof(struct field_info));
  fi->field_type = field_type;
  fi->field_name = name;
  fi->field_namelen = strlen(name) + 1;

  res = pr_table_kadd(field_idtab, (const void *) k, sizeof(unsigned int),
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

  field_add(p, LOGFMT_META_ENV_VAR, "ENV:",
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

  field_add(p, LOGFMT_META_SECONDS, "transfer_secs",
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

  field_add(p, LOGFMT_META_NOTE_VAR, "NOTE:",
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

  field_add(p, LOG_ZMQ_META_CONNECT, "connecting",
    LOG_ZMQ_FIELD_TYPE_BOOLEAN);

  field_add(p, LOG_ZMQ_META_DISCONNECT, "disconnecting",
    LOG_ZMQ_FIELD_TYPE_BOOLEAN);

  return 0;
}

static void log_zmq_mkjson(void *json, const char *field_name,
    size_t field_namelen, unsigned int field_type, const void *field_value) {
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

static char *get_meta_arg(pool *p, unsigned char *m, size_t *arglen) {
  char buf[PR_TUNABLE_PATH_MAX+1], *ptr;
  size_t len;

  ptr = buf;
  len = 0;

  while (*m != LOGFMT_META_ARG_END) {
    pr_signals_handle();
    *ptr++ = (char) *m++;
    len++;
  }

  *ptr = '\0';
  *arglen = len;

  return pstrdup(p, buf);
}

static int find_next_meta(pool *p, int flags, cmd_rec *cmd, unsigned char **fmt,
    void *obj,
    void (*mkfield)(void *, const char *, size_t, unsigned int, const void *)) {
  struct field_info *fi;
  unsigned char *m;
  unsigned int meta;

  m = (*fmt) + 1;

  meta = *m;
  fi = pr_table_kget(field_idtab, (const void *) &meta, sizeof(unsigned int),
    NULL);

  switch (*m) {
    case LOGFMT_META_BYTES_SENT:
      if (session.xfer.p) {
        double bytes_sent;

        bytes_sent = session.xfer.total_bytes;
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          &bytes_sent);

      } else if (pr_cmd_cmp(cmd, PR_CMD_DELE_ID) == 0) {
        double bytes_sent;

        bytes_sent = log_zmq_dele_filesz;
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          &bytes_sent);
      }

      m++;
      break;

    case LOGFMT_META_FILENAME:
      if (pr_cmd_cmp(cmd, PR_CMD_RNTO_ID) == 0) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          dir_abs_path(p, pr_fs_decode_path(p, cmd->arg), TRUE));

      } else if (pr_cmd_cmp(cmd, PR_CMD_RETR_ID) == 0) {
        char *path;

        path = pr_table_get(cmd->notes, "mod_xfer.retr-path", NULL);
        if (path != NULL) {
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            dir_abs_path(p, path, TRUE));
        }

      } else if (pr_cmd_cmp(cmd, PR_CMD_APPE_ID) == 0 ||
                 pr_cmd_cmp(cmd, PR_CMD_STOR_ID) == 0) {
        char *path;

        path = pr_table_get(cmd->notes, "mod_xfer.store-path", NULL);
        if (path != NULL) {
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            dir_abs_path(p, path, TRUE));
        }

      } else if (session.xfer.p &&
                 session.xfer.path) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            dir_abs_path(p, session.xfer.path, TRUE));

      } else if (pr_cmd_cmp(cmd, PR_CMD_CDUP_ID) == 0 ||
                 pr_cmd_cmp(cmd, PR_CMD_PWD_ID) == 0 ||
                 pr_cmd_cmp(cmd, PR_CMD_XCUP_ID) == 0 ||
                 pr_cmd_cmp(cmd, PR_CMD_XPWD_ID) == 0) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            dir_abs_path(p, pr_fs_getcwd(), TRUE));

      } else if (pr_cmd_cmp(cmd, PR_CMD_CWD_ID) == 0 ||
                 pr_cmd_cmp(cmd, PR_CMD_XCWD_ID) == 0) {

        /* Note: by this point in the dispatch cycle, the current working
         * directory has already been changed.  For the CWD/XCWD commands,
         * this means that dir_abs_path() may return an improper path,
         * with the target directory being reported twice.  To deal with this,
         * don't use dir_abs_path(), and use pr_fs_getvwd()/pr_fs_getcwd()
         * instead.
         */
        if (session.chroot_path) {
          /* Chrooted session. */
          if (strncmp(pr_fs_getvwd(), "/", 2) == 0) {
            mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
              session.chroot_path);

          } else {
            mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
              pdircat(p, session.chroot_path, pr_fs_getvwd(), NULL));
          }

        } else {
          /* Non-chrooted session. */
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            pr_fs_getcwd());
        }

      } else if (pr_cmd_cmp(cmd, PR_CMD_SITE_ID) == 0 &&
                 (strncasecmp(cmd->argv[1], "CHGRP", 6) == 0 ||
                  strncasecmp(cmd->argv[1], "CHMOD", 6) == 0 ||
                  strncasecmp(cmd->argv[1], "UTIME", 6) == 0)) {
        register unsigned int i;
        char *tmp = "";

        for (i = 3; i <= cmd->argc-1; i++) {
          tmp = pstrcat(cmd->tmp_pool, tmp, *tmp ? " " : "",
            pr_fs_decode_path(cmd->tmp_pool, cmd->argv[i]), NULL);
        }

        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          dir_abs_path(p, tmp, TRUE));

      } else {
        /* Some commands (i.e. DELE, MKD, RMD, XMKD, and XRMD) have associated
         * filenames that are not stored in the session.xfer structure; these
         * should be expanded properly as well.
         */
        if (pr_cmd_cmp(cmd, PR_CMD_DELE_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_MDTM_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_MKD_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_MLSD_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_MLST_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_RMD_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_XMKD_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_XRMD_ID) == 0) {
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            dir_abs_path(p, pr_fs_decode_path(p, cmd->arg), TRUE));

        } else if (pr_cmd_cmp(cmd, PR_CMD_MFMT_ID) == 0) {
          /* MFMT has, as its filename, the second argument. */
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            dir_abs_path(p, pr_fs_decode_path(p, cmd->argv[2]), TRUE));
        }
      }

      m++;
      break;

    case LOGFMT_META_ENV_VAR:
      m++;

      if (*m == LOGFMT_META_START &&
          *(m+1) == LOGFMT_META_ARG) {
        char *key, *env = NULL;
        size_t keylen = 0;

        key = get_meta_arg(p, (m+2), &keylen);
        m += keylen;

        env = pr_env_get(p, key); 
        if (env != NULL) {
          char *field_name;
          size_t field_namelen;

          field_name = pstrcat(p, fi->field_name, key, NULL);
          field_namelen = strlen(field_name);

          mkfield(obj, field_name, field_namelen, fi->field_type, env);
        }
      }
      break;
   

    case LOGFMT_META_REMOTE_HOST: {
      const char *name;

      name = pr_netaddr_get_sess_remote_name();
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, name);

      m++;
      break;
    }

    case LOGFMT_META_REMOTE_IP: {
      const char *ipstr;

      ipstr = pr_netaddr_get_ipstr(pr_netaddr_get_sess_local_addr());
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        ipstr);

      m++;
      break;
    }

    case LOGFMT_META_IDENT_USER: {
      char *ident_user;

      ident_user = pr_table_get(session.notes, "mod_ident.rfc1413-ident", NULL);
      if (ident_user != NULL) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          ident_user);
      }

      m++;
      break;
    }

    case LOGFMT_META_PID: {
      double sess_pid;

      sess_pid = session.pid;
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        &sess_pid);

      m++;
      break;
    }

    case LOGFMT_META_TIME: {
      char *time_fmt = "%Y-%m-%d %H:%M:%S %z", ts[128];
      struct tm *tm;
      time_t now;

      m++;

      now = time(NULL);
      tm = pr_gmtime(NULL, &now);

      if (*m == LOGFMT_META_START &&
          *(m+1) == LOGFMT_META_ARG) {
        size_t fmtlen = 0;

        time_fmt = get_meta_arg(p, (m+2), &fmtlen);
      }

      strftime(ts, sizeof(ts)-1, time_fmt, tm);
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, ts);

      break;
    }

    case LOGFMT_META_SECONDS:
      if (session.xfer.p != NULL) {
        /* Make sure that session.xfer.start_time actually has values (which
         * is not always the case).
         */
        if (session.xfer.start_time.tv_sec != 0 ||
            session.xfer.start_time.tv_usec != 0) {
          struct timeval end_time;
          double xfer_secs;

          gettimeofday(&end_time, NULL);
          end_time.tv_sec -= session.xfer.start_time.tv_sec;

          if (end_time.tv_usec >= session.xfer.start_time.tv_usec) {
            end_time.tv_usec -= session.xfer.start_time.tv_usec;

          } else {
            end_time.tv_usec = 1000000L - (session.xfer.start_time.tv_usec -
              end_time.tv_usec);
            end_time.tv_sec--;
          }

          xfer_secs = end_time.tv_sec;
          xfer_secs += (double) ((double) end_time.tv_usec / (double) 1000);

          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            &xfer_secs);
        }
      }

      m++;
      break;

    case LOGFMT_META_COMMAND:
      if (pr_cmd_cmp(cmd, PR_CMD_PASS_ID) == 0 &&
          session.hide_password) {
        char *full_cmd = "PASS (hidden)";

        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          full_cmd);

      } else if (flags == LOG_ZMQ_EVENT_FL_REQUEST) {
        char *full_cmd;

        full_cmd = get_full_cmd(cmd);
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          full_cmd);
      }

      m++;
      break;

    case LOGFMT_META_LOCAL_NAME:
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        cmd->server->ServerName);
      m++;
      break;

    case LOGFMT_META_LOCAL_PORT: {
      double server_port;

      server_port = cmd->server->ServerPort;
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        &server_port);

      m++;
      break;
    }

    case LOGFMT_META_LOCAL_IP: {
      const char *ipstr;

      ipstr = pr_netaddr_get_ipstr(pr_netaddr_get_sess_local_addr());
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        ipstr);

      m++;
      break;
    }

    case LOGFMT_META_LOCAL_FQDN: {
      const char *dnsstr;

      dnsstr = pr_netaddr_get_dnsstr(pr_netaddr_get_sess_local_addr());
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        dnsstr);

      m++;
      break;
    }

    case LOGFMT_META_USER:
      if (session.user != NULL) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          session.user);
      }

      m++;
      break;

    case LOGFMT_META_ORIGINAL_USER: {
      char *orig_user = NULL;

      orig_user = pr_table_get(session.notes, "mod_auth.orig-user", NULL);
      if (orig_user != NULL) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          orig_user);
      }

      m++;
      break;
    }

    case LOGFMT_META_RESPONSE_CODE: {
      char *resp_code = NULL;
      int res;

      res = pr_response_get_last(cmd->tmp_pool, &resp_code, NULL);
      if (res == 0 &&
          resp_code != NULL) {
        double code;

        code = atoi(resp_code);
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, &code);

      /* Hack to add return code for proper logging of QUIT command. */
      } else if (pr_cmd_cmp(cmd, PR_CMD_QUIT_ID) == 0) {
        double code = 221;

        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, &code);
      }

      m++;
      break;
    }

    case LOGFMT_META_CLASS:
      if (session.conn_class != NULL) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          session.conn_class);
      }

      m++;
      break;

    case LOGFMT_META_ANON_PASS: {
      char *anon_pass;

      anon_pass = pr_table_get(session.notes, "mod_auth.anon-passwd", NULL);
      if (anon_pass == NULL) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          anon_pass);
      }

      m++;
      break;
    }

    case LOGFMT_META_METHOD: {
      if (flags == LOG_ZMQ_EVENT_FL_REQUEST) {
        if (pr_cmd_cmp(cmd, PR_CMD_SITE_ID) != 0) {
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            cmd->argv[0]);

        } else {
          char buf[32], *ptr;

          /* Make sure that the SITE command used is all in uppercase,
           * for logging purposes.
           */
          for (ptr = cmd->argv[1]; *ptr; ptr++) {
            *ptr = toupper((int) *ptr);
          }

          memset(buf, '\0', sizeof(buf));
          snprintf(buf, sizeof(buf)-1, "%s %s", cmd->argv[0], cmd->argv[1]);

          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, buf);
        }
      }

      m++;
      break;
    }

    case LOGFMT_META_XFER_PATH:
      if (pr_cmd_cmp(cmd, PR_CMD_RNTO_ID) == 0) {
        char *path;

        path = dir_best_path(cmd->tmp_pool,
          pr_fs_decode_path(cmd->tmp_pool, cmd->arg));
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, path);

      } else if (session.xfer.p &&
                 session.xfer.path) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          session.xfer.path);

      } else {
        /* Some commands (i.e. DELE, MKD, XMKD, RMD, XRMD) have associated
         * filenames that are not stored in the session.xfer structure; these
         * should be expanded properly as well.
         */
        if (pr_cmd_cmp(cmd, PR_CMD_DELE_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_MKD_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_XMKD_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_RMD_ID) == 0 ||
            pr_cmd_cmp(cmd, PR_CMD_XRMD_ID) == 0) {
          char *path;

          path = dir_best_path(cmd->tmp_pool,
            pr_fs_decode_path(cmd->tmp_pool, cmd->arg));
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, path);
        }
      }

      m++;
      break;

    case LOGFMT_META_DIR_NAME:
      if (pr_cmd_cmp(cmd, PR_CMD_CDUP_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_CWD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_MLSD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_MKD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_RMD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XCWD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XCUP_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XMKD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XRMD_ID) == 0) {
        char *path, *tmp;

        path = pr_fs_decode_path(p, cmd->arg);
        tmp = strrchr(path, '/');

        if (tmp != NULL) {
          if (tmp != path) {
            mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
              tmp + 1);

          } else if (*(tmp+1) != '\0') {
            mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
              tmp + 1);

          } else {
            mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
              path);
          }

        } else {
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, path);
        }

      } else {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          pr_fs_getvwd());
      }

      m++;
      break;

    case LOGFMT_META_DIR_PATH:
      if (pr_cmd_cmp(cmd, PR_CMD_CDUP_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_LIST_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_MLSD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_MKD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_NLST_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_RMD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XCUP_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XMKD_ID) == 0 ||
          pr_cmd_cmp(cmd, PR_CMD_XRMD_ID) == 0) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          dir_abs_path(p, pr_fs_decode_path(p, cmd->arg), TRUE));

      } else if (pr_cmd_cmp(cmd, PR_CMD_CWD_ID) == 0 ||
                 pr_cmd_cmp(cmd, PR_CMD_XCWD_ID) == 0) {

        /* Note: by this point in the dispatch cycle, the current working
         * directory has already been changed.  For the CWD/XCWD commands,
         * this means that dir_abs_path() may return an improper path,
         * with the target directory being reported twice.  To deal with this,
         * don't use dir_abs_path(), and use pr_fs_getvwd()/pr_fs_getcwd()
         * instead.
         */

        if (session.chroot_path) {
          /* Chrooted session. */
          if (strncmp(pr_fs_getvwd(), "/", 2) == 0) {
            mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
              session.chroot_path);

          } else {
            mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
              pdircat(p, session.chroot_path, pr_fs_getvwd(), NULL));
          }

        } else {
          /* Non-chrooted session. */
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            pr_fs_getcwd());
        }
      }

      m++;
      break;

    case LOGFMT_META_CMD_PARAMS:
      if (pr_cmd_cmp(cmd, PR_CMD_PASS_ID) == 0) {
        const char *params = "(hidden)";

        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, params);

      } else if (LOG_ZMQ_EVENT_FL_REQUEST && cmd->argc > 1) {
        const char *params;

        params = pr_fs_decode_path(p, cmd->arg);
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, params);
      }

      m++;
      break;

    case LOGFMT_META_RESPONSE_STR: {
      char *resp_msg = NULL;
      int res;

      res = pr_response_get_last(p, NULL, &resp_msg);
      if (res == 0 &&
          resp_msg != NULL) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          resp_msg);
      }

      m++;
      break;
    }

    case LOGFMT_META_PROTOCOL: {
      const char *proto;

      proto = pr_session_get_protocol(0);
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, proto);

      m++;
      break;
    }

    case LOGFMT_META_VERSION: {
      const char *version;

      version = PROFTPD_VERSION_TEXT;
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, version);

      m++;
      break;
    }

    case LOGFMT_META_RENAME_FROM:
      if (pr_cmd_cmp(cmd, PR_CMD_RNTO_ID) == 0) {
        char *rnfr_path;

        rnfr_path = pr_table_get(session.notes, "mod_core.rnfr-path", NULL);
        if (rnfr_path != NULL) {
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            rnfr_path);
        }
      }

      m++;
      break;

    case LOGFMT_META_FILE_MODIFIED: {
      bool modified = false;
      char *val;

      val = pr_table_get(cmd->notes, "mod_xfer.file-modified", NULL);
      if (val != NULL) {
        if (strncmp(val, "true", 5) == 0) {
          modified = true;
        }
      }

      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        &modified);

      m++;
      break;
    }

    case LOGFMT_META_UID: {
      double sess_uid;

      sess_uid = session.login_uid;
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        &sess_uid);

      m++;
      break;
    }

    case LOGFMT_META_GID: {
      double sess_gid;

      sess_gid = session.login_gid;
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        &sess_gid);

      m++;
      break;
    }

    case LOGFMT_META_RAW_BYTES_IN: {
      double bytes_rcvd;

      bytes_rcvd = session.total_raw_in;
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        &bytes_rcvd);

      m++;
      break;
    }

    case LOGFMT_META_RAW_BYTES_OUT: {
      double bytes_sent;

      bytes_sent = session.total_raw_out;
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        &bytes_sent);

      m++;
      break;
    }

    case LOGFMT_META_EOS_REASON: {
      const char *reason = NULL;
      char *details = NULL;

      reason = pr_session_get_disconnect_reason(&details);
      if (reason != NULL) {
        if (details != NULL) {
          char buf[256];

          memset(buf, '\0', sizeof(buf));
          snprintf(buf, sizeof(buf)-1, "%s: %s", reason, details);
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, buf);

        } else {
          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            reason);
        }
      }
 
      m++;
      break;
    }

    case LOGFMT_META_VHOST_IP:
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        cmd->server->ServerAddress);
      m++;
      break;

    case LOGFMT_META_NOTE_VAR:
      m++;

      if (*m == LOGFMT_META_START &&
          *(m+1) == LOGFMT_META_ARG) {
        char *key, *note = NULL;
        size_t keylen = 0;

        key = get_meta_arg(p, (m+2), &keylen);
        m += keylen;

        /* Check in the cmd->notes table first. */
        note = pr_table_get(cmd->notes, key, NULL);
        if (note == NULL) {
          /* If not there, check in the session.notes table. */
          note = pr_table_get(session.notes, key, NULL);
        }

        if (note != NULL) {
          char *field_name;
          size_t field_namelen;

          field_name = pstrcat(p, fi->field_name, note, NULL);
          field_namelen = strlen(field_name);

          mkfield(obj, field_name, field_namelen, fi->field_type, note);
        }
      }
      break;

    case LOGFMT_META_XFER_STATUS:
      /* If the current command is one that incurs a data transfer, then we
       * need to do more work.  If not, it's an easy substitution.
       */
      if (session.curr_cmd_id == PR_CMD_ABOR_ID ||
          session.curr_cmd_id == PR_CMD_APPE_ID ||
          session.curr_cmd_id == PR_CMD_LIST_ID ||
          session.curr_cmd_id == PR_CMD_MLSD_ID ||
          session.curr_cmd_id == PR_CMD_NLST_ID ||
          session.curr_cmd_id == PR_CMD_RETR_ID ||
          session.curr_cmd_id == PR_CMD_STOR_ID ||
          session.curr_cmd_id == PR_CMD_STOU_ID) {
        const char *proto;

        proto = pr_session_get_protocol(0);

        if (strncmp(proto, "ftp", 4) == 0 ||
            strncmp(proto, "ftps", 5) == 0) {
          if (!(XFER_ABORTED)) {
            int res;
            char *resp_code = NULL, *resp_msg = NULL;

            /* Get the last response code/message.  We use heuristics here to
             * determine when to use "failed" versus "success".
             */
            res = pr_response_get_last(p, &resp_code, &resp_msg);
            if (res == 0 &&
                resp_code != NULL) {
              if (*resp_code == '2') {
                const char *status;

                if (pr_cmd_cmp(cmd, PR_CMD_ABOR_ID) != 0) {
                  status = "success";

                } else {
                  /* We're handling the ABOR command, so obviously the value
                   * should be 'cancelled'.
                   */
                  status = "cancelled";
                }

                mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
                  status);

              } else if (*resp_code == '1') {
                const char *status = "timeout";

                /* If the first digit of the response code is 1, then the
                 * response code (for a data transfer command) is probably 150,
                 * which means that the transfer was still in progress (didn't
                 * complete with a 2xx/4xx response code) when we are called
                 * here, which in turn means a timeout kicked in.
                 */
                mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
                  status);

              } else {
                const char *status = "failed";

                mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
                  status);
              }

            } else {
              const char *status = "success";

              mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
                status);
            }

          } else {
            const char *status = "cancelled";

            mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
              status);
          }

        } else {
          /* mod_sftp stashes a note for us in the command notes if the
           * transfer failed.
           */
          char *status;

          status = pr_table_get(cmd->notes, "mod_sftp.file-status", NULL);
          if (status == NULL) {
            status = "success";

          } else {
            status = "failed";
          }

          mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
            status);
        }
      }

      m++;
      break;

    case LOGFMT_META_XFER_FAILURE:
      /* If the current command is one that incurs a data transfer, then we
       * need to do more work.  If not, it's an easy substitution.
       */
      if (session.curr_cmd_id == PR_CMD_APPE_ID ||
          session.curr_cmd_id == PR_CMD_LIST_ID ||
          session.curr_cmd_id == PR_CMD_MLSD_ID ||
          session.curr_cmd_id == PR_CMD_NLST_ID ||
          session.curr_cmd_id == PR_CMD_RETR_ID ||
          session.curr_cmd_id == PR_CMD_STOR_ID ||
          session.curr_cmd_id == PR_CMD_STOU_ID) {
        const char *proto;

        proto = pr_session_get_protocol(0);

        if (strncmp(proto, "ftp", 4) == 0 ||
            strncmp(proto, "ftps", 5) == 0) {

          if (!(XFER_ABORTED)) {
            int res;
            char *resp_code = NULL, *resp_msg = NULL;

            /* Get the last response code/message.  We use heuristics here to
             * determine when to use "failed" versus "success".
             */
            res = pr_response_get_last(cmd->tmp_pool, &resp_code, &resp_msg);
            if (res == 0 &&
                resp_code != NULL) {
              if (*resp_code != '2' &&
                  *resp_code != '1') {
                char *ptr;

                /* Parse out/prettify the resp_msg here */
                ptr = strchr(resp_msg, '.');
                if (ptr != NULL) {
                  mkfield(obj, fi->field_name, fi->field_namelen,
                    fi->field_type, ptr + 2);

                } else {
                  mkfield(obj, fi->field_name, fi->field_namelen,
                    fi->field_type, resp_msg);
                }
              }
            }
          }
        }
      }

      m++;
      break;

    case LOGFMT_META_MICROSECS: {
      double sess_usecs;
      struct timeval now;

      gettimeofday(&now, NULL);
      sess_usecs = now.tv_usec;
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        &sess_usecs);

      m++;
      break;
    }

    case LOGFMT_META_MILLISECS: {
      double sess_msecs;
      struct timeval now;

      gettimeofday(&now, NULL);

      /* Convert microsecs to millisecs. */
      sess_msecs = (now.tv_usec / 1000);

      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
        &sess_msecs);

      m++;
      break;
    }

    case LOGFMT_META_ISO8601: {
      char ts[128];
      struct tm *tm;
      struct timeval now;
      unsigned long millis;
      size_t len;

      gettimeofday(&now, NULL);
      tm = pr_localtime(NULL, (const time_t *) &(now.tv_sec));

      len = strftime(ts, sizeof(ts)-1, "%Y-%m-%d %H:%M:%S", tm);

      /* Convert microsecs to millisecs. */
      millis = now.tv_usec / 1000;

      snprintf(ts + len, sizeof(ts) - len - 1, ",%03lu", millis);
      mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type, ts);

      m++;
      break;
    }

    case LOGFMT_META_GROUP:
      if (session.group != NULL) {
        mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
          session.group);
      }

      m++;
      break;

    default:
      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "skipping unsupported LogFormat meta %d", *m);
      break;
  }

  *fmt = m;
  return 0;
}

static int log_zmq_mkrecord(int flags, cmd_rec *cmd,
    unsigned char *fmt, void *obj,
    void (*mkfield)(void *, const char *, size_t, unsigned int, const void *)) {

  if (flags == LOG_ZMQ_EVENT_FL_CONNECT &&
      session.prev_server == NULL) {
    unsigned int meta = LOG_ZMQ_META_CONNECT;
    struct field_info *fi;
    bool connecting = true;

    fi = pr_table_kget(field_idtab, (const void *) &meta, sizeof(unsigned int),
      NULL);

    mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
      &connecting);

  } else if (flags == LOG_ZMQ_EVENT_FL_DISCONNECT) {
    unsigned int meta = LOG_ZMQ_META_DISCONNECT;
    struct field_info *fi;
    bool disconnecting = true;

    fi = pr_table_kget(field_idtab, (const void *) &meta, sizeof(unsigned int),
      NULL);

    mkfield(obj, fi->field_name, fi->field_namelen, fi->field_type,
      &disconnecting);
  }

  while (*fmt) {
    pr_signals_handle();

    if (*fmt == LOGFMT_META_START) {
      find_next_meta(cmd->tmp_pool, flags, cmd, &fmt, obj, mkfield);

    } else {
      fmt++;
    }
  }

  return 0;
}

static int log_zmq_mkrecord_json(pool *p, int flags, cmd_rec *cmd,
    unsigned char *fmt, char **payload, size_t *payload_len) {
  int res;
  char errstr[256], *json = NULL;
  void *obj = NULL;

  /* Note: if JSON code runs out of memory, it will call exit(3); we need to
   * enable it to interact with proftpd's exiting/error handling.  Maybe
   * set an OOM handler which, in our case, raises SIGSEGV?
   */

  obj = json_mkobject();

  res = log_zmq_mkrecord(flags, cmd, fmt, obj, log_zmq_mkjson);

  if (!json_check(obj, errstr)) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "JSON structural problems: %s", errstr);
    errno = EINVAL;

    json_delete(obj);
    return -1;
  }

  json = json_encode(obj);
  pr_trace_msg(trace_channel, 3, "generated JSON payload: %s", json);

  *payload_len = strlen(json);
  *payload = palloc(p, *payload_len);
  memcpy(*payload, json, *payload_len);

  /* To avoid a memory leak via malloc(3), we have to explicitly call free(3)
   * on the returned JSON string.  Which is why we duplicate it out of the
   * given memory pool, for use outside of this function.
   */
  free(json);
  json_delete(obj);

  return 0;
}

static int log_zmq_mkrecord_msgpack(pool *p, int flags, cmd_rec *cmd,
    unsigned char *fmt, char **payload, size_t *payload_len) {
  errno = ENOSYS;
  return -1;
}

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

  res = zmsg_send(&msg, zsock);
  if (res < 0) {
    (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
      "error sending message to LogZMQEndpoint '%s': %s", addr,
      zmq_strerror(zmq_errno()));
    zmsg_destroy(&msg);
  }

  return res;
}

static int log_zmq_log_event(cmd_rec *cmd, int flags) {
  register unsigned int i;
  config_rec **elts;

  elts = endpoints->elts;
  for (i = 0; i < endpoints->nelts; i++) {
    config_rec *c;
    int res;
    char *payload = NULL;
    size_t payload_len = 0;

    pr_signals_handle();

    c = elts[i];

    switch (log_zmq_payload_fmt) {
      case LOG_ZMQ_PAYLOAD_FMT_JSON:
        res = log_zmq_mkrecord_json(cmd->tmp_pool, flags, cmd, c->argv[1],
          &payload, &payload_len);
        break;

      case LOG_ZMQ_PAYLOAD_FMT_MSGPACK:
        res = log_zmq_mkrecord_msgpack(cmd->tmp_pool, flags, cmd, c->argv[1],
          &payload, &payload_len);
        break;
    }

    if (res < 0) {
      (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
        "error generating %s payload: %s",
          log_zmq_payload_fmt == LOG_ZMQ_PAYLOAD_FMT_JSON ?
            "JSON" : "MessagePack", strerror(errno));

      payload = NULL;
      payload_len = 0;
      continue;
    }

    /* Send payload via ZMQ */
    log_zmq_send_msg(c->argv[0], payload, payload_len);
  }

  return 0;
}

/* Command handlers
 */

MODRET log_zmq_any(cmd_rec *cmd) {
  int res;

  if (log_zmq_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (endpoints == NULL ||
      endpoints->nelts == 0) {
    /* No configured endpoints means no logging work for us to do. */
    return PR_DECLINED(cmd);
  }

  res = log_zmq_log_event(cmd, LOG_ZMQ_EVENT_FL_REQUEST);
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
  char *logfmt = NULL;

  /* XXX Future enhancement to support <Anonymous>-specific notifying? */
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  CHECK_ARGS(cmd, 2);

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

    logfmt = NULL;
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

/* usage: LogZMQMessageFormat json */
MODRET set_logzmqmessageformat(cmd_rec *cmd) {
  int payload_fmt = 0;
  config_rec *c;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);
  CHECK_ARGS(cmd, 1);

  if (strcasecmp(cmd->argv[1], "json") == 0) {
    payload_fmt = LOG_ZMQ_PAYLOAD_FMT_JSON;

  } else {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unsupported message format: '",
      cmd->argv[1], "'", NULL));
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = payload_fmt;

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
    socket_mode = LOG_ZMQ_SOCKET_MODE_BIND;

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
  cmd_rec *cmd = NULL;

  cmd = pr_cmd_alloc(session.pool, 1, "DISCONNECT");
  log_zmq_log_event(cmd, LOG_ZMQ_EVENT_FL_DISCONNECT);

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

  /* Use our own OOM handler. */
  json_set_oom(log_zmq_oom);

  return 0;
}

static int log_zmq_sess_init(void) {
  config_rec *c;
  cmd_rec *cmd = NULL;
  int timeout = LOG_ZMQ_TIMEOUT_DEFAULT;

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

  c = find_config(main_server->conf, CONF_PARAM, "LogZMQSocketMode", FALSE);
  if (c != NULL) {
    log_zmq_socket_mode = *((int *) c->argv[0]);
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

#ifdef PR_USE_IPV6
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
      if (zsocket_bind(zsock, addr) < 0) {
        (void) pr_log_writefile(log_zmq_logfd, MOD_LOG_ZMQ_VERSION,
          "error binding to LogZMQEndpoint '%s': %s", addr,
          zmq_strerror(zmq_errno()));
        c = find_config_next(c, c->next, CONF_PARAM, "LogZMQEndpoint", FALSE);
        continue;
      }

    } else if (log_zmq_socket_mode == LOG_ZMQ_SOCKET_MODE_CONNECT) {
      if (zsocket_connect(zsock, addr) < 0) {
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

  cmd = pr_cmd_alloc(session.pool, 1, "CONNECT");
  log_zmq_log_event(cmd, LOG_ZMQ_EVENT_FL_CONNECT);

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
  { "LogZMQMessageFormat",	set_logzmqmessageformat,	NULL },
  { "LogZMQSocketMode",		set_logzmqsocketmode,		NULL },
  { "LogZMQTimeout", 		set_logzmqtimeout,		NULL },

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
