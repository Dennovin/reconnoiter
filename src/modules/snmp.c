/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 * Copyright (c) 2010-2015, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name OmniTI Computer Consulting, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <mtev_defines.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <ctype.h>
#include <arpa/inet.h>

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#include <mtev_hash.h>

#include "noit_module.h"
#include "noit_check.h"
#include "noit_check_tools.h"
#include "noit_mtev_bridge.h"

static mtev_log_stream_t nlerr = NULL;
static mtev_log_stream_t nldeb = NULL;
static int __snmp_initialize_once = 0;
static void ensure_usm_user(const char *username, u_char *engineID, size_t engineIDLen);

#define SNMPV2_TRAPS_PREFIX     SNMP_OID_SNMPMODULES,1,1,5
oid trap_prefix[]    = { SNMPV2_TRAPS_PREFIX };
oid cold_start_oid[] = { SNMPV2_TRAPS_PREFIX, 1 };  /* SNMPv2-MIB */
oid warm_start_oid[] = { SNMPV2_TRAPS_PREFIX, 2 };  /* SNMPv2-MIB */
oid link_down_oid[]  = { SNMPV2_TRAPS_PREFIX, 3 };  /* IF-MIB */
oid link_up_oid[]    = { SNMPV2_TRAPS_PREFIX, 4 };  /* IF-MIB */
oid auth_fail_oid[]  = { SNMPV2_TRAPS_PREFIX, 5 };  /* SNMPv2-MIB */
oid egp_xxx_oid[]    = { SNMPV2_TRAPS_PREFIX, 99 }; /* ??? */

#define SNMPV2_TRAP_OBJS_PREFIX SNMP_OID_SNMPMODULES,1,1,4
oid snmptrap_oid[] = { SNMPV2_TRAP_OBJS_PREFIX, 1, 0 };
size_t snmptrap_oid_len = OID_LENGTH(snmptrap_oid);
oid snmptrapenterprise_oid[] = { SNMPV2_TRAP_OBJS_PREFIX, 3, 0 };
size_t snmptrapenterprise_oid_len = OID_LENGTH(snmptrapenterprise_oid);
oid sysuptime_oid[] = { SNMP_OID_MIB2, 1, 3, 0 };
size_t sysuptime_oid_len = OID_LENGTH(sysuptime_oid);

#define SNMPV2_COMM_OBJS_PREFIX SNMP_OID_SNMPMODULES,18,1
oid agentaddr_oid[] = { SNMPV2_COMM_OBJS_PREFIX, 3, 0 };
size_t agentaddr_oid_len = OID_LENGTH(agentaddr_oid);
oid community_oid[] = { SNMPV2_COMM_OBJS_PREFIX, 4, 0 };
size_t community_oid_len = OID_LENGTH(community_oid);

#define RECONNOITER_PREFIX     SNMP_OID_ENTERPRISES,32863,1
oid reconnoiter_oid[] = { RECONNOITER_PREFIX };
size_t reconnoiter_oid_len = OID_LENGTH(reconnoiter_oid);
oid reconnoiter_check_prefix_oid[] = { RECONNOITER_PREFIX,1,1 };
size_t reconnoiter_check_prefix_oid_len =
  OID_LENGTH(reconnoiter_check_prefix_oid);
size_t reconnoiter_check_oid_len = OID_LENGTH(reconnoiter_check_prefix_oid) + 8;
oid reconnoiter_metric_prefix_oid[] = { RECONNOITER_PREFIX,1,2 };
size_t reconnoiter_metric_prefix_oid_len =
  OID_LENGTH(reconnoiter_metric_prefix_oid);

oid reconnoiter_check_status_oid[] = { RECONNOITER_PREFIX,1,3};
size_t reconnoiter_check_status_oid_len =
  OID_LENGTH(reconnoiter_check_status_oid);
oid reconnoiter_check_state_oid[] = { RECONNOITER_PREFIX,1,3,1};
size_t reconnoiter_check_state_oid_len =
  OID_LENGTH(reconnoiter_check_state_oid);
oid reconnoiter_check_state_unknown_oid[] = { RECONNOITER_PREFIX,1,3,1,0};
oid reconnoiter_check_state_good_oid[] = { RECONNOITER_PREFIX,1,3,1,1};
oid reconnoiter_check_state_bad_oid[] = { RECONNOITER_PREFIX,1,3,1,2};
size_t reconnoiter_check_state_val_len =
  OID_LENGTH(reconnoiter_check_state_unknown_oid);
/* Boolean */
oid reconnoiter_check_available_oid[] = { RECONNOITER_PREFIX,1,3,2};
size_t reconnoiter_check_available_oid_len =
  OID_LENGTH(reconnoiter_check_available_oid);
oid reconnoiter_check_available_unknown_oid[] = { RECONNOITER_PREFIX,1,3,2,0};
oid reconnoiter_check_available_yes_oid[] = { RECONNOITER_PREFIX,1,3,2,1};
oid reconnoiter_check_available_no_oid[] = { RECONNOITER_PREFIX,1,3,2,2};
size_t reconnoiter_check_available_val_len =
  OID_LENGTH(reconnoiter_check_available_unknown_oid);
/* timeticks? gauge/unsigned? */
oid reconnoiter_check_duration_oid[] = { RECONNOITER_PREFIX,1,3,3};
size_t reconnoiter_check_duration_oid_len =
  OID_LENGTH(reconnoiter_check_duration_oid);
/* string */
oid reconnoiter_check_status_msg_oid[] = { RECONNOITER_PREFIX,1,3,4};
size_t reconnoiter_check_status_msg_oid_len =
  OID_LENGTH(reconnoiter_check_status_msg_oid);

typedef struct _mod_config {
  mtev_hash_table *options;
  mtev_hash_table target_sessions;
} snmp_mod_config_t;

struct target_session {
  struct synch_state state;
  struct session_list *slp;
  noit_module_t *self;
  char *key;
  char *target;
  eventer_t timeoutevent;
  int version;
  int fd;
  int in_table;
  int refcnt;
  struct timeval last_open;
};

#define sess_handle slp->session

struct snmp_check_closure {
  noit_module_t *self;
  noit_check_t *check;
};

struct v3_probe_magic {
  eventer_t timeoutevent;
  netsnmp_callback cb;
  noit_check_t *check;
  struct target_session *ts; /* for timeout */
  struct snmp_pdu *pdu;
};

struct check_info {
  int timedout;
  struct {
     int reqid;
     char *confname;
     char *oidname;
     oid oid[MAX_OID_LEN];
     size_t oidlen;
     metric_type_t type_override;
     mtev_boolean type_should_override;
     int seen;
  } *oids;
  int noids;
  int noids_seen;
  int nresults;
  eventer_t timeoutevent;
  noit_module_t *self;
  noit_check_t *check;
  struct target_session *ts;
  int version;
};

/* We hold struct check_info's in there key's by their reqid.
 *   If they timeout, we remove them.
 *
 *   When SNMP queries complete, we look them up, if we find them
 *   then we know we can remove the timeout and  complete the check. 
 *   If we don't find them, the timeout fired and removed the check.
 */
mtev_hash_table active_checks = MTEV_HASH_EMPTY;
static void add_check(struct check_info *c) {
  int i;
  for(i=0; i<c->noids; i++)
    mtev_hash_store(&active_checks, (char *)&c->oids[i].reqid, sizeof(c->oids[i].reqid), c);
}
static struct check_info *get_check(int reqid) {
  void *vc;
  if(mtev_hash_retrieve(&active_checks, (char *)&reqid, sizeof(reqid), &vc))
    return (struct check_info *)vc;
  return NULL;
}
static void remove_check_req(struct check_info *c, int reqid) {
  (void)c;
  mtev_hash_delete(&active_checks, (char *)&reqid, sizeof(reqid),
                   NULL, NULL);
}
static void remove_check(struct check_info *c) {
  int i, lastreq = -1;
  for(i=0; i<c->noids; i++) {
    if(c->oids[i].reqid != lastreq) {
      mtev_hash_delete(&active_checks, (char *)&c->oids[i].reqid, sizeof(c->oids[i].reqid),
                       NULL, NULL);
      lastreq = c->oids[i].reqid;
    }
  }
}

struct target_session *
_get_target_session(noit_module_t *self, char *target, int version) {
  char key[128];
  void *vts;
  struct target_session *ts;
  snmp_mod_config_t *conf;
  conf = noit_module_get_userdata(self);
  snprintf(key, sizeof(key), "%s:v%d", target, version);
  if(!mtev_hash_retrieve(&conf->target_sessions,
                         key, strlen(key), &vts)) {
    ts = calloc(1, sizeof(*ts));
    ts->self = self;
    ts->version = version;
    ts->fd = -1;
    ts->refcnt = 0;
    ts->target = strdup(target);
    ts->key = strdup(key);
    ts->in_table = 1;
    mtev_hash_store(&conf->target_sessions,
                    ts->key, strlen(ts->key), ts);
    vts = ts;
  }
  return (struct target_session *)vts;
}

static int noit_snmp_accumulate_results(noit_check_t *check, struct snmp_pdu *pdu) {
  struct check_info *info = check->closure;
  struct variable_list *vars;

  if(pdu)
    for(vars = pdu->variables; vars; vars = vars->next_variable)
      info->nresults++;

  /* manipulate the information ourselves */
  for(vars = pdu->variables; vars; vars = vars->next_variable) {
    char *sp;
    int nresults = 0;
    int oid_idx;
    double float_conv;
    u_int64_t u64;
    int64_t i64;
    char *endptr;
    char varbuff[256];

    snprint_variable(varbuff, sizeof(varbuff),
                     vars->name, vars->name_length, vars);

    /* find the oid to which this is the response */
    oid_idx = nresults; /* our check->stats.inprogress idx is the most likely */
    if(info->oids[oid_idx].oidlen != vars->name_length ||
       memcmp(info->oids[oid_idx].oid, vars->name,
              vars->name_length * sizeof(oid))) {
      /* Not the most obvious guess */
      for(oid_idx = info->noids - 1; oid_idx >= 0; oid_idx--) {
        if(info->oids[oid_idx].oidlen == vars->name_length &&
           !memcmp(info->oids[oid_idx].oid, vars->name,
                  vars->name_length * sizeof(oid))) break;
      }
    }
    if(oid_idx < 0) {
      mtevL(nlerr, "Unexpected oid results to %s`%s`%s: %s\n",
            check->target, check->module, check->name, varbuff);
      nresults++;
      info->nresults++;
      continue;
    }
    if(info->oids[oid_idx].seen == 0) {
      info->oids[oid_idx].seen = 1;
      info->noids_seen++;
    }

#define SETM(a,b) noit_stats_set_metric(check, &check->stats.inprogress, \
                                        info->oids[oid_idx].confname, a, b)
    if(info->oids[oid_idx].type_should_override) {
      sp = strchr(varbuff, ' ');
      if(sp) sp++;
      noit_stats_set_metric_coerce(check, &check->stats.inprogress, info->oids[oid_idx].confname,
                                   info->oids[oid_idx].type_override,
                                   sp);
    }
    else {
      switch(vars->type) {
        case ASN_OCTET_STR:
          sp = malloc(1 + vars->val_len);
          memcpy(sp, vars->val.string, vars->val_len);
          sp[vars->val_len] = '\0';
          SETM(METRIC_STRING, sp);
          free(sp);
          break;
        case ASN_INTEGER:
        case ASN_GAUGE:
          SETM(METRIC_INT32, vars->val.integer);
          break;
        case ASN_TIMETICKS:
        case ASN_COUNTER:
          SETM(METRIC_UINT32, vars->val.integer);
          break;
#ifdef ASN_OPAQUE_I64
        case ASN_OPAQUE_I64:
#endif
        case ASN_INTEGER64:
          printI64(varbuff, vars->val.counter64);
          i64 = strtoll(varbuff, &endptr, 10);
          SETM(METRIC_INT64, (varbuff == endptr) ? NULL : &i64);
          break;
#ifdef ASN_OPAQUE_U64
        case ASN_OPAQUE_U64:
#endif
#ifdef ASN_OPAQUE_COUNTER64
        case ASN_OPAQUE_COUNTER64:
#endif
        case ASN_COUNTER64:
          printU64(varbuff, vars->val.counter64);
          u64 = strtoull(varbuff, &endptr, 10);
          SETM(METRIC_UINT64, (varbuff == endptr) ? NULL : &u64);
          break;
#ifdef ASN_OPAQUE_FLOAT
        case ASN_OPAQUE_FLOAT:
#endif
        case ASN_FLOAT:
          if(vars->val.floatVal) float_conv = *(vars->val.floatVal);
          SETM(METRIC_DOUBLE, vars->val.floatVal ? &float_conv : NULL);
          break;
#ifdef ASN_OPAQUE_DOUBLE
        case ASN_OPAQUE_DOUBLE:
#endif
        case ASN_DOUBLE:
          SETM(METRIC_DOUBLE, vars->val.doubleVal);
          break;
        case ASN_NULL:
          mtevL(nldeb, "snmp[null]: %s\n", varbuff);
        case SNMP_NOSUCHOBJECT:
        case SNMP_NOSUCHINSTANCE:
          SETM(METRIC_STRING, NULL);
          break;
        default:
          /* Advance passed the first space and use that unless there
           * is no space or we have no more string left.
           */
          sp = strchr(varbuff, ' ');
          if(sp) sp++;
          SETM(METRIC_STRING, (sp && *sp) ? sp : NULL);
          mtevL(nlerr, "snmp: unknown type[%d] %s\n", vars->type, varbuff);
      }
    }
    nresults++;
    info->nresults++;
  }
  return (info->noids_seen == info->noids) ? 1 : 0;
}

/* Handling of results */
static void noit_snmp_log_results(noit_module_t *self, noit_check_t *check, const char *err) {
  struct check_info *info = check->closure;
  struct timeval duration;
  char buff[128];

  gettimeofday(&check->stats.inprogress.whence, NULL);
  sub_timeval(check->stats.inprogress.whence, check->last_fire_time, &duration);
  check->stats.inprogress.duration = duration.tv_sec * 1000 + duration.tv_usec / 1000;
  check->stats.inprogress.available = (info->nresults > 0) ? NP_AVAILABLE : NP_UNAVAILABLE;
  check->stats.inprogress.state = (info->noids_seen == info->noids) ? NP_GOOD : NP_BAD;
  if(err) snprintf(buff, sizeof(buff), "%s", err);
  else snprintf(buff, sizeof(buff), "%d/%d gets", info->noids_seen, info->noids);
  check->stats.inprogress.status = buff;

  noit_check_set_stats(check, &check->stats.inprogress);
  noit_check_stats_clear(check, &check->stats.inprogress);
  return;
}

static int noit_snmp_session_cleanse(struct target_session *ts,
                                     int needs_free) {
  if(ts->refcnt == 0 && ts->slp) {
    eventer_t e = eventer_remove_fd(ts->fd);
    if(needs_free) eventer_free(e);
    ts->fd = -1;
    if(ts->timeoutevent) {
      eventer_remove(ts->timeoutevent);
      ts->timeoutevent = NULL;
    }
    snmp_sess_close(ts->slp);
    ts->slp = NULL;
    if(!ts->in_table) {
      free(ts);
    }
    return 1;
  }
  return 0;
}

static int noit_snmp_session_timeout(eventer_t e, int mask, void *closure,
                                     struct timeval *now) {
  struct target_session *ts = closure;
  if(ts->slp) snmp_sess_timeout(ts->slp);
  noit_snmp_session_cleanse(ts, 1);
  if(ts->timeoutevent == e)
    ts->timeoutevent = NULL; /* this will be freed on return */
  return 0;
}

static int noit_snmp_check_timeout(eventer_t e, int mask, void *closure,
                                   struct timeval *now) {
  struct check_info *info = closure;
  info->timeoutevent = NULL;
  info->timedout = 1;
  if(info->ts) {
    info->ts->refcnt--;
    noit_snmp_session_cleanse(info->ts, 1);
    info->ts = NULL;
  }
  remove_check(info);
  /* Log our findings */
  noit_snmp_log_results(info->self, info->check, NULL);
  info->check->flags &= ~NP_RUNNING;
  return 0;
}

static void _set_ts_timeout(struct target_session *ts, struct timeval *t) {
  struct timeval now;
  eventer_t e = NULL;
  if(ts->timeoutevent) {
    e = eventer_remove(ts->timeoutevent);
    ts->timeoutevent = NULL;
  }
  if(!t) return;

  gettimeofday(&now, NULL);
  if(!e) e = eventer_alloc();
  e->callback = noit_snmp_session_timeout;
  e->closure = ts;
  e->mask = EVENTER_TIMER;
  add_timeval(now, *t, &e->whence);
  ts->timeoutevent = e;
  eventer_add(e);
}

static int noit_snmp_handler(eventer_t e, int mask, void *closure,
                             struct timeval *now) {
  int block = 0, rv, liberr, snmperr;
  struct timeval timeout = { 0, 0 };
  struct target_session *ts = closure;
  char *errmsg;

  rv = snmp_sess_read_C1(ts->slp, e->fd);
  if(noit_snmp_session_cleanse(ts, 0))
    return 0;
  snmp_sess_select_info2_flags(ts->slp, NULL, NULL,
                               &timeout, &block, NETSNMP_SELECT_NOFLAGS);
  _set_ts_timeout(ts, block ? &timeout : NULL);

  return EVENTER_READ | EVENTER_EXCEPTION;
}

/* This 'convert_v1pdu_to_v2' was cribbed directly from netsnmp */
static netsnmp_pdu *
convert_v1pdu_to_v2( netsnmp_pdu* template_v1pdu ) {
  netsnmp_pdu *template_v2pdu;
  netsnmp_variable_list *var;
  oid enterprise[MAX_OID_LEN];
  size_t enterprise_len;

  /*
   * Make a copy of the v1 Trap PDU
   *   before starting to convert this
   *   into a v2 Trap PDU.
   */
  template_v2pdu = snmp_clone_pdu( template_v1pdu);
  if(!template_v2pdu) {
    snmp_log(LOG_WARNING,
             "send_trap: failed to copy v2 template PDU\n");
    return NULL;
  }
  template_v2pdu->command = SNMP_MSG_TRAP2;

  /*
   * Insert an snmpTrapOID varbind before the original v1 varbind list
   *   either using one of the standard defined trap OIDs,
   *   or constructing this from the PDU enterprise & specific trap fields
   */
  if(template_v1pdu->trap_type == SNMP_TRAP_ENTERPRISESPECIFIC) {
    if(template_v1pdu->enterprise_length + 2 > MAX_OID_LEN) {
      mtevL(nlerr, "send_trap: enterprise_length too large\n");
      snmp_free_pdu(template_v2pdu);
      return NULL;
    }
    memcpy(enterprise, template_v1pdu->enterprise,
           template_v1pdu->enterprise_length*sizeof(oid));
    enterprise_len = template_v1pdu->enterprise_length;
    enterprise[enterprise_len++] = 0;
    enterprise[enterprise_len++] = template_v1pdu->specific_type;
  } else {
    memcpy(enterprise, cold_start_oid, sizeof(cold_start_oid));
    enterprise[9]  = template_v1pdu->trap_type+1;
    enterprise_len = sizeof(cold_start_oid)/sizeof(oid);
  }

  var = NULL;
  if(!snmp_varlist_add_variable(&var,
                                snmptrap_oid, snmptrap_oid_len,
                                ASN_OBJECT_ID,
                                (u_char*)enterprise,
                                enterprise_len*sizeof(oid))) {
    mtevL(nlerr, "send_trap: failed to insert copied snmpTrapOID varbind\n");
    snmp_free_pdu(template_v2pdu);
    return NULL;
  }
  var->next_variable        = template_v2pdu->variables;
  template_v2pdu->variables = var;

  /*
   * Insert a sysUptime varbind at the head of the v2 varbind list
   */
  var = NULL;
  if(!snmp_varlist_add_variable(&var,
                                sysuptime_oid, sysuptime_oid_len,
                                ASN_TIMETICKS,
                                (u_char*)&(template_v1pdu->time), 
                                sizeof(template_v1pdu->time))) {
    mtevL(nlerr, "send_trap: failed to insert copied sysUptime varbind\n");
    snmp_free_pdu(template_v2pdu);
    return NULL;
  }
  var->next_variable = template_v2pdu->variables;
  template_v2pdu->variables = var;

  /*
   * Append the other three conversion varbinds,
   *  (snmpTrapAgentAddr, snmpTrapCommunity & snmpTrapEnterprise)
   *  if they're not already present.
   *  But don't bomb out completely if there are problems.
   */
  var = find_varbind_in_list(template_v2pdu->variables,
                             agentaddr_oid, agentaddr_oid_len);
  if(!var && (template_v1pdu->agent_addr[0]
              || template_v1pdu->agent_addr[1]
              || template_v1pdu->agent_addr[2]
              || template_v1pdu->agent_addr[3])) {
    if(!snmp_varlist_add_variable(&(template_v2pdu->variables),
                                  agentaddr_oid, agentaddr_oid_len,
                                  ASN_IPADDRESS,
                                  (u_char*)&(template_v1pdu->agent_addr), 
                                  sizeof(template_v1pdu->agent_addr)))
      mtevL(nlerr, "send_trap: failed to append snmpTrapAddr varbind\n");
  }
  var = find_varbind_in_list(template_v2pdu->variables,
                             community_oid, community_oid_len);
  if(!var && template_v1pdu->community) {
    if(!snmp_varlist_add_variable(&(template_v2pdu->variables),
                                  community_oid, community_oid_len,
                                  ASN_OCTET_STR,
                                  template_v1pdu->community, 
                                  template_v1pdu->community_len))
      mtevL(nlerr, "send_trap: failed to append snmpTrapCommunity varbind\n");
  }
  var = find_varbind_in_list(template_v2pdu->variables,
                             snmptrapenterprise_oid,
                             snmptrapenterprise_oid_len);
  if(!var && 
     template_v1pdu->trap_type != SNMP_TRAP_ENTERPRISESPECIFIC) {
    if(!snmp_varlist_add_variable(&(template_v2pdu->variables),
                                  snmptrapenterprise_oid,
                                  snmptrapenterprise_oid_len,
                                  ASN_OBJECT_ID,
                                  (u_char*)template_v1pdu->enterprise, 
                                  template_v1pdu->enterprise_length*sizeof(oid)))
      mtevL(nlerr, "send_trap: failed to append snmpEnterprise varbind\n");
  }
  return template_v2pdu;
}

static int noit_snmp_oid_to_checkid(oid *o, int l, uuid_t checkid, char *out) {
  int i;
  char _uuid_str[UUID_STR_LEN+1], *cp, *uuid_str;

  uuid_str = out ? out : _uuid_str;
  if(l != reconnoiter_check_oid_len) {
    mtevL(nlerr, "unsupported (length) trap recieved\n");
    return -1;
  }
  if(netsnmp_oid_equals(o,
                        reconnoiter_check_prefix_oid_len,
                        reconnoiter_check_prefix_oid,
                        reconnoiter_check_prefix_oid_len) != 0) {
    mtevL(nlerr, "unsupported (wrong namespace) trap recieved\n");
    return -1;
  }
  /* encode this as a uuid */
  cp = uuid_str;
  for(i=0;
      i < reconnoiter_check_oid_len - reconnoiter_check_prefix_oid_len;
      i++) {
    oid v = o[i + reconnoiter_check_prefix_oid_len];
    if(v > 0xffff) {
      mtevL(nlerr, "trap target oid [%ld] out of range\n", (long int)v);
      return -1;
    }
    snprintf(cp, 5, "%04x", (unsigned short)(v & 0xffff));
    cp += 4;
    /* hyphens after index 1,2,3,4 */
    if(i > 0 && i < 5) *cp++ = '-';
  }
  if(uuid_parse(uuid_str, checkid) != 0) {
    mtevL(nlerr, "unexpected error decoding trap uuid '%s'\n", uuid_str);
    return -1;
  }
  return 0;
}

#define isoid(a,b,c,d) (netsnmp_oid_equals(a,b,c,d) == 0)
#define isoidprefix(a,b,c,d) (netsnmp_oid_equals(a,MIN(b,d),c,d) == 0)
#define setstatus(st,soid,sv) \
  if(isoid(o,l,soid,reconnoiter_check_state_val_len)) current->st = sv

static int
noit_snmp_trapvars_to_stats(noit_check_t *check, netsnmp_variable_list *var) {
  stats_t *current = &check->stats.inprogress;
  if(isoidprefix(var->name, var->name_length, reconnoiter_check_status_oid,
                 reconnoiter_check_status_oid_len)) {
    if(var->type == ASN_OBJECT_ID) {
      if(isoid(var->name, var->name_length,
               reconnoiter_check_state_oid, reconnoiter_check_state_oid_len)) {
        oid *o = var->val.objid;
        size_t l = var->val_len / sizeof(*o);
        setstatus(state, reconnoiter_check_state_unknown_oid, NP_UNKNOWN);
        else setstatus(state, reconnoiter_check_state_good_oid, NP_GOOD);
        else setstatus(state, reconnoiter_check_state_bad_oid, NP_BAD);
        else return -1;
      }
      else if(isoid(var->name, var->name_length,
                    reconnoiter_check_available_oid,
                    reconnoiter_check_available_oid_len)) {
        oid *o = var->val.objid;
        size_t l = var->val_len / sizeof(*o);
        setstatus(available, reconnoiter_check_available_unknown_oid, NP_UNKNOWN);
        else setstatus(available, reconnoiter_check_available_yes_oid, NP_AVAILABLE);
        else setstatus(available, reconnoiter_check_available_no_oid, NP_UNAVAILABLE);
        else return -1;
      }
      else {
        /* We don't unerstand any other OBJECT_ID types */
        return -1;
      }
    }
    else if(var->type == ASN_UNSIGNED) {
      /* This is only for the duration (in ms) */
      if(isoid(var->name, var->name_length,
               reconnoiter_check_duration_oid,
               reconnoiter_check_duration_oid_len)) {
        current->duration = *(var->val.integer);
      }
      else
        return -1;
    }
    else if(var->type == ASN_OCTET_STR) {
      /* This is only for the status message */
      if(isoid(var->name, var->name_length,
               reconnoiter_check_status_msg_oid,
               reconnoiter_check_status_msg_oid_len)) {
        current->status = malloc(var->val_len + 1);
        memcpy(current->status, var->val.string, var->val_len);
        current->status[var->val_len] = '\0';
      }
      else
        return -1;
    }
    else {
      /* I don't understand any other type of status message */
      return -1;
    }
  }
  else if(isoidprefix(var->name, var->name_length,
                      reconnoiter_metric_prefix_oid,
                      reconnoiter_metric_prefix_oid_len)) {
    /* decode the metric and store the value */
    int i, len;
    u_int64_t u64;
    double doubleVal;
    char metric_name[128], buff[128], *cp;
    if(var->name_length <= reconnoiter_metric_prefix_oid_len) return -1;
    len = var->name[reconnoiter_metric_prefix_oid_len];
    if(var->name_length != (reconnoiter_metric_prefix_oid_len + 1 + len) ||
       len > sizeof(metric_name) - 1) {
      mtevL(nlerr, "snmp trap, malformed metric name\n");
      return -1;
    }
    for(i=0;i<len;i++) {
      ((unsigned char *)metric_name)[i] =
        (unsigned char)var->name[reconnoiter_metric_prefix_oid_len + 1 + i];
      if(!isprint(metric_name[i])) {
        mtevL(nlerr, "metric_name contains unprintable characters\n");
        return -1;
      }
    }
    metric_name[i] = '\0';
    switch(var->type) {
      case ASN_INTEGER:
      case ASN_UINTEGER:
      case ASN_TIMETICKS:
      case ASN_INTEGER64:
        noit_stats_set_metric(check, current, metric_name,
                              METRIC_INT64, var->val.integer);
        break;
      case ASN_COUNTER64:
        u64 = ((u_int64_t)var->val.counter64->high) << 32;
        u64 |= var->val.counter64->low;
        noit_stats_set_metric(check, current, metric_name,
                              METRIC_UINT64, &u64);
        break;
      case ASN_OPAQUE_FLOAT:
        doubleVal = (double)*var->val.floatVal;
        noit_stats_set_metric(check, current, metric_name,
                              METRIC_DOUBLE, &doubleVal);
        break;
      case ASN_OPAQUE_DOUBLE:
        noit_stats_set_metric(check, current, metric_name,
                              METRIC_DOUBLE, var->val.doubleVal);
        break;
      case ASN_OCTET_STR:
        snprint_value(buff, sizeof(buff), var->name, var->name_length, var);
        /* Advance passed the first space and use that unless there
         * is no space or we have no more string left.
         */
        cp = strchr(buff, ' ');
        if(cp) {
          char *ecp;
          cp++;
          if(*cp == '"') {
            ecp = cp + strlen(cp) - 1;
            if(*ecp == '"') {
              cp++; *ecp = '\0';
            }
          }
        }
        noit_stats_set_metric(check, current, metric_name,
                              METRIC_STRING, (cp && *cp) ? cp : NULL);
        break;
      default:
        mtevL(nlerr, "snmp trap unsupport data type %d\n", var->type);
    }
    mtevL(nldeb, "metric_name -> '%s'\n", metric_name);
  }
  else {
    /* No idea what this is */
    return -1;
  }
  return 0;
}
static int noit_snmp_trapd_response(int operation, struct snmp_session *sp,
                                    int reqid, struct snmp_pdu *pdu,
                                    void *magic) {
  /* the noit pieces */
  noit_check_t *check;
  struct target_session *ts = magic;
  snmp_mod_config_t *conf;
  const char *community = NULL;
  int success = 0;

  /* parsing destination */
  char uuid_str[UUID_STR_LEN + 1];
  uuid_t checkid;

  /* snmp oid parsing helper vars */
  netsnmp_pdu *newpdu = pdu;
  netsnmp_variable_list *var;

  conf = noit_module_get_userdata(ts->self);

  if(pdu->version == SNMP_VERSION_1)
    newpdu = convert_v1pdu_to_v2(pdu);
  if(!newpdu || newpdu->version != SNMP_VERSION_2c) goto cleanup;

  for(var = newpdu->variables; var != NULL; var = var->next_variable) {
    if(netsnmp_oid_equals(var->name, var->name_length,
                          snmptrap_oid, snmptrap_oid_len) == 0)
      break;
  }

  if (!var || var->type != ASN_OBJECT_ID) {
    mtevL(nlerr, "unsupport trap (not a trap?) received\n");
    goto cleanup;
  }

  /* var is the oid on which we are trapping.
   * It should be in the reconnoiter check prefix.
   */
  if(noit_snmp_oid_to_checkid(var->val.objid, var->val_len/sizeof(oid),
                              checkid, uuid_str)) {
    goto cleanup;
  }
  mtevL(nldeb, "recieved trap for %s\n", uuid_str);
  check = noit_poller_lookup(checkid);
  if(!check) {
    mtevL(nlerr, "trap received for non-existent check '%s'\n", uuid_str);
    goto cleanup;
  }
  if(!mtev_hash_retr_str(check->config, "community", strlen("community"),
                         &community) &&
     !mtev_hash_retr_str(conf->options, "community", strlen("community"),
                         &community)) {
    mtevL(nlerr, "No community defined for check, dropping trap\n");
    goto cleanup;
  }

  if(strlen(community) != newpdu->community_len ||
     memcmp(community, newpdu->community, newpdu->community_len)) {
    mtevL(nlerr, "trap attempt with wrong community string\n");
    goto cleanup;
  }

  /* We have a check. The trap is authorized. Now, extract everything. */
  memset(&check->stats.inprogress, 0, sizeof(check->stats.inprogress));
  gettimeofday(&check->stats.inprogress.whence, NULL);
  check->stats.inprogress.available = NP_AVAILABLE;

  /* Rate limit */
  if(((check->stats.inprogress.whence.tv_sec * 1000 +
       check->stats.inprogress.whence.tv_usec / 1000) -
      (check->last_fire_time.tv_sec * 1000 +
       check->last_fire_time.tv_usec / 1000)) < check->period) goto cleanup;

  /* update the last fire time... */
  gettimeofday(&check->last_fire_time, NULL);

  for(; var != NULL; var = var->next_variable)
    if(noit_snmp_trapvars_to_stats(check, var) == 0) success++;
  if(success) {
    char buff[24];
    snprintf(buff, sizeof(buff), "%d datum", success);
    check->stats.inprogress.state = NP_GOOD;
    check->stats.inprogress.status = strdup(buff);
  }
  else {
    check->stats.inprogress.state = NP_BAD;
    check->stats.inprogress.status = strdup("no data");
  }
  noit_check_set_stats(check, &check->stats.inprogress);

 cleanup:
  if(newpdu != pdu)
    snmp_free_pdu(newpdu);
  return 0;
}
static int noit_snmp_asynch_response(int operation, struct snmp_session *sp,
                                     int reqid, struct snmp_pdu *pdu,
                                     void *magic) {
  struct check_info *info;
  /* We don't deal with refcnt hitting zero here.  We could only be hit from
   * the snmp read/timeout stuff.  Handle it there.
   */

  info = get_check(reqid);
  if(!info) return 1;
  remove_check_req(info, reqid);


  if(noit_snmp_accumulate_results(info->check, pdu)) {
    mtevL(nldeb, "snmp %s pdu completed check requirements\n", info->check->name);
    if(info->timeoutevent) {
      eventer_remove(info->timeoutevent);
      eventer_free(info->timeoutevent);
      info->timeoutevent = NULL;
    }
    if(info->ts) {
      info->ts->refcnt--;
      info->ts = NULL;
    }
    noit_snmp_log_results(info->self, info->check, NULL);
    info->check->flags &= ~NP_RUNNING;
  }
  return 1;
}

static void noit_snmp_sess_open(struct target_session *ts,
                                noit_check_t *check) {
  size_t bsize, offset;
  u_char _buf[512];
  u_char *buf = _buf;
  const char *cval;
  struct snmp_session sess;
  netsnmp_transport *transport;
  struct check_info *info = check->closure;
  memset(&sess, 0, sizeof(sess));
  snmp_sess_init(&sess);
  sess.version = info->version;
  sess.peername = ts->target;
  u_char contextEngineID_buf[256];
  u_char securityEngineID_buf[256];

/*
final String walk_base             = config.remove("walk");
*/

#define CONF_GET(name, tgt) \
  mtev_hash_retr_str(check->config, name, strlen(name), tgt)
#define SESS_SET_STRING(value, len, key, default) do {    \
  const char *_cval = NULL;                               \
  if(!CONF_GET(key, &_cval)) {                            \
    _cval = default;                                      \
  }                                                       \
  value = ( __typeof__ (value) )(_cval);                  \
  len = strlen(_cval);                                    \
} while(0)

  SESS_SET_STRING(sess.community, sess.community_len, "community", "public");
  /* TCP/UDP -> session_flags |= SNMP_FLAGS_STREAM_SOCKET */
  /* securityEngineID? */
  /* contextEngineID? */
  SESS_SET_STRING(sess.contextName, sess.contextNameLen, "context_name", "");
  if(CONF_GET("context_engine", &cval)) {
    bsize = sizeof(buf);
    offset = 0;
    if (netsnmp_hex_to_binary(&buf,&bsize,&offset,0,cval,".")) {
	    sess.contextEngineID = contextEngineID_buf;
	    memcpy(sess.contextEngineID, buf, bsize);
	    sess.contextEngineIDLen = bsize;
    }
	}
  SESS_SET_STRING(sess.securityName, sess.securityNameLen, "security_name", "");
  if(sess.securityName) sess.securityLevel = SNMP_SEC_LEVEL_NOAUTH;
  if(CONF_GET("security_engine", &cval)) {
    bsize = sizeof(buf);
    offset = 0;
    if (netsnmp_hex_to_binary(&buf,&bsize,&offset,0,cval,".")) {
	    sess.securityEngineID = securityEngineID_buf;
	    memcpy(sess.securityEngineID, buf, bsize);
	    sess.securityEngineIDLen = bsize;
    }
	}
  if(CONF_GET("security_level", &cval)) {
    if(!strcasecmp(cval, "nanp")) sess.securityLevel = SNMP_SEC_LEVEL_NOAUTH;
    else if(!strcasecmp(cval, "anp")) sess.securityLevel = SNMP_SEC_LEVEL_AUTHNOPRIV;
    else if(!strcasecmp(cval, "ap")) sess.securityLevel = SNMP_SEC_LEVEL_AUTHPRIV;
  }
  if(CONF_GET("auth_protocol", &cval)) {
    if (!strcasecmp(cval,"MD5")) {
      sess.securityAuthProto = usmHMACMD5AuthProtocol;
      sess.securityAuthProtoLen = USM_AUTH_PROTO_MD5_LEN;
    }
    else if (!strcasecmp(cval,"SHA")) {
      sess.securityAuthProto = usmHMACSHA1AuthProtocol;
      sess.securityAuthProtoLen = USM_AUTH_PROTO_SHA_LEN;
    }
  }
  if(CONF_GET("auth_passphrase", &cval)) {
    if(sess.securityAuthProto == NULL) {
      sess.securityAuthProto = usmHMACMD5AuthProtocol;
      sess.securityAuthProtoLen = USM_AUTH_PROTO_MD5_LEN;
    }
    sess.securityAuthKeyLen = USM_AUTH_KU_LEN;
    if(generate_Ku(sess.securityAuthProto, sess.securityAuthProtoLen,
                   (u_char *)cval, strlen(cval),
                   sess.securityAuthKey, &sess.securityAuthKeyLen) != SNMPERR_SUCCESS) {
      /* What do we do? */
      mtevL(nlerr, "auth_passphrase failed to gen master key\n");
    }
  }
  if(CONF_GET("privacy_protocol", &cval)) {
    if (!strcasecmp(cval,"DES")) {
      sess.securityPrivProto = usmDESPrivProtocol;
      sess.securityPrivProtoLen = USM_PRIV_PROTO_DES_LEN;
    }
    else if (!strcasecmp(cval,"AES")) {
      sess.securityPrivProto = usmAESPrivProtocol;
      sess.securityPrivProtoLen = USM_PRIV_PROTO_AES_LEN;
    }
  }
  if(CONF_GET("privacy_passphrase", &cval)) {
    if(sess.securityPrivProto == NULL) {
      sess.securityPrivProto = usmDESPrivProtocol;
      sess.securityPrivProtoLen = USM_PRIV_PROTO_DES_LEN;
    }
    sess.securityPrivKeyLen = USM_PRIV_KU_LEN;
    if(generate_Ku(sess.securityPrivProto, sess.securityPrivProtoLen,
                   (u_char *)cval, strlen(cval),
                   sess.securityPrivKey, &sess.securityPrivKeyLen) != SNMPERR_SUCCESS) {
      /* What do we do? */
    }
  }
  sess.callback = noit_snmp_asynch_response;
  sess.callback_magic = ts;
  sess.flags |= SNMP_FLAGS_DONT_PROBE;
  ts->slp = snmp_sess_open_C1(&sess, &transport);
  ts->sess_handle->flags &= ~SNMP_FLAGS_DONT_PROBE;
  ts->fd = transport->sock;
  gettimeofday(&ts->last_open, NULL);
}

static int noit_snmp_fill_oidinfo(noit_check_t *check) {
  int i, klen;
  mtev_hash_iter iter = MTEV_HASH_ITER_ZERO;
  const char *name, *value;
  struct check_info *info = check->closure;
  mtev_hash_table check_attrs_hash = MTEV_HASH_EMPTY;

  /* Toss the old set and bail if we have zero */
  if(info->oids) {
    for(i=0; i<info->noids;i++) {
      if(info->oids[i].confname) free(info->oids[i].confname);
      if(info->oids[i].oidname) free(info->oids[i].oidname);
    }
    free(info->oids);
  }
  info->noids = 0;
  info->nresults = 0;
  info->noids_seen = 0;
  info->oids = NULL;

  /* Figure our how many. */
  while(mtev_hash_next_str(check->config, &iter, &name, &klen, &value)) {
    if(!strncasecmp(name, "oid_", 4)) {
      info->noids++;
    }
  }

  if(info->noids == 0) return 0;

  /* Create a hash of important check attributes */
  noit_check_make_attrs(check, &check_attrs_hash);

  /* Fill out the new set of required oids */
  info->oids = calloc(info->noids, sizeof(*info->oids));
  memset(&iter, 0, sizeof(iter));
  i = 0;
  while(mtev_hash_next_str(check->config, &iter, &name, &klen, &value)) {
    if(!strncasecmp(name, "oid_", 4)) {
      const char *type_override;
      char oidbuff[2048], typestr[256];
      name += 4;
      info->oids[i].confname = strdup(name);
      noit_check_interpolate(oidbuff, sizeof(oidbuff), value,
                             &check_attrs_hash, check->config);
      info->oids[i].oidname = strdup(oidbuff);
      info->oids[i].oidlen = MAX_OID_LEN;
      if(oidbuff[0] == '.') {
        if(!read_objid(oidbuff, info->oids[i].oid, &info->oids[i].oidlen)) {
          mtevL(nlerr, "Failed to translate oid: %s\n", oidbuff);
          info->noids--;
          continue;
        }
      }
      else {
        if(!get_node(oidbuff, info->oids[i].oid, &info->oids[i].oidlen)) {
          mtevL(nlerr, "Failed to translate oid: %s\n", oidbuff);
          info->noids--;
          continue;
        }
      }
      snprintf(typestr, sizeof(typestr), "type_%s", name);
      if(mtev_hash_retr_str(check->config, typestr, strlen(typestr),
                            &type_override)) {
        int type_enum_fake = *type_override;

        if(!strcasecmp(type_override, "guess"))
          type_enum_fake = METRIC_GUESS;
        else if(!strcasecmp(type_override, "int32"))
          type_enum_fake = METRIC_INT32;
        else if(!strcasecmp(type_override, "uint32"))
          type_enum_fake = METRIC_UINT32;
        else if(!strcasecmp(type_override, "int64"))
          type_enum_fake = METRIC_INT64;
        else if(!strcasecmp(type_override, "uint64"))
          type_enum_fake = METRIC_UINT64;
        else if(!strcasecmp(type_override, "double"))
          type_enum_fake = METRIC_DOUBLE;
        else if(!strcasecmp(type_override, "string"))
          type_enum_fake = METRIC_STRING;

        switch(type_enum_fake) {
          case METRIC_GUESS:
          case METRIC_INT32: case METRIC_UINT32:
          case METRIC_INT64: case METRIC_UINT64:
          case METRIC_DOUBLE: case METRIC_STRING:
            info->oids[i].type_override = *type_override;
            info->oids[i].type_should_override = mtev_true;
          default: break;
        }
      }
      i++;
    }
  }
  assert(info->noids == i);
  mtev_hash_destroy(&check_attrs_hash, NULL, NULL);
  return info->noids;
}
static int noit_snmp_fill_req(struct snmp_pdu *req, noit_check_t *check, int idx) {
  int i;
  struct check_info *info = check->closure;

  if(idx == -1) {
    for(i=0; i<info->noids; i++)
      snmp_add_null_var(req, info->oids[i].oid, info->oids[i].oidlen);
    return info->noids;
  }

  assert(idx >= 0 && idx <info->noids);
  snmp_add_null_var(req, info->oids[idx].oid, info->oids[idx].oidlen);
  return 1;
}

static void
ensure_usm_user(const char *username, u_char *engineID, size_t engineIDLen) {
  struct usmUser *user;
  user = usm_get_user(NULL, 0, (char *)username);
  if (user == NULL) {
    user = (struct usmUser *) calloc(1, sizeof(struct usmUser));
    user->name = strdup(username);
    user->secName = strdup(username);
    user->authProtocolLen = sizeof(usmNoAuthProtocol) / sizeof(oid);
    user->authProtocol =
        snmp_duplicate_objid(usmNoAuthProtocol, user->authProtocolLen);
    user->privProtocolLen = sizeof(usmNoPrivProtocol) / sizeof(oid);
    user->privProtocol =
        snmp_duplicate_objid(usmNoPrivProtocol, user->privProtocolLen);
    if(engineIDLen) {
      user->engineID = malloc(engineIDLen);
      memcpy(user->engineID, engineID, engineIDLen);
      user->engineIDLen = engineIDLen;
    }
    usm_add_user(user);
    mtevL(nldeb, "usm adding user: %s\n", username);
  }
}

/* Shenanigans to work around snmp v3 probes blocking */
static int
snmpv3_build_probe_pdu(netsnmp_pdu **pdu) {
    struct usmUser *user;

    /*
     * create the pdu
     */
    if (!pdu)
        return -1;
    *pdu = snmp_pdu_create(SNMP_MSG_GET);
    if (!(*pdu))
        return -1;
    (*pdu)->version = SNMP_VERSION_3;
    (*pdu)->securityName = strdup("");
    (*pdu)->securityNameLen = strlen((*pdu)->securityName);
    (*pdu)->securityLevel = SNMP_SEC_LEVEL_NOAUTH;
    (*pdu)->securityModel = SNMP_SEC_MODEL_USM;

    /*
     * create the empty user
     */
    ensure_usm_user((*pdu)->securityName, NULL, 0);
    return 0;
}

static int
noit_snmpv3_probe_timeout(eventer_t e, int mask, void *closure,
                          struct timeval *now) {
  struct v3_probe_magic *magic = closure;
  struct check_info *info = magic->check->closure;
  struct target_session *ts = magic->ts;
  if(ts && ts->slp) {
    ts->sess_handle->flags &= ~SNMP_FLAGS_DONT_PROBE;
  }
  if(ts) ts->refcnt--;
  magic->timeoutevent = NULL;
  return 0;
}

static void
copy_auth_to_pdu(struct snmp_session *sp, struct snmp_pdu *pdu) {
#define pdu_copystring(name) do { \
  if(pdu->name) free(pdu->name); \
  pdu->name = NULL; \
  if(sp->name) pdu->name = strdup(sp->name); \
  pdu->name##Len = strlen(pdu->name); \
} while(0)
#define pdu_copyoid(oid) do { \
  if(pdu->oid) free(pdu->oid); \
  pdu->oid = NULL; \
  pdu->oid = snmp_duplicate_objid(sp->oid, sp->oid##Len); \
  pdu->oid##Len = sp->oid##Len; \
} while(0)

  pdu_copystring(securityName);
  pdu->securityModel = sp->securityModel;
  pdu->securityLevel = sp->securityLevel;
}

static int
probe_engine_step1_cb(int operation,
                      struct snmp_session *sp,
                      int reqid,
                      struct snmp_pdu *pdu,
                      void *arg) {
  struct v3_probe_magic *magic = arg;
  struct check_info *info = magic->check->closure;
  struct target_session *ts = info->ts;
  int ret = 1;

  if(magic->timeoutevent) {
    eventer_remove(magic->timeoutevent);
    eventer_free(magic->timeoutevent);
    magic->timeoutevent = NULL;
  }
  /* Did we receive the appropriate Report message? */
  if (operation == NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE &&
      pdu && pdu->command == SNMP_MSG_REPORT) {
    int reqid, i;
    if(pdu->securityEngineIDLen) {
      if(sp->securityEngineID) free(sp->securityEngineID);
      sp->securityEngineID = malloc(sizeof(*sp->securityEngineID)*pdu->securityEngineIDLen);
      memcpy(sp->securityEngineID, pdu->securityEngineID, sizeof(*sp->securityEngineID)*pdu->securityEngineIDLen);
      sp->securityEngineIDLen = pdu->securityEngineIDLen;
    }
    if(pdu->contextEngineIDLen) {
      if(sp->contextEngineID) free(sp->contextEngineID);
      sp->contextEngineID = malloc(sizeof(*sp->contextEngineID)*pdu->contextEngineIDLen);
      memcpy(sp->contextEngineID, pdu->contextEngineID, sizeof(*sp->contextEngineID)*pdu->contextEngineIDLen);
      sp->contextEngineIDLen = pdu->contextEngineIDLen;
    }
    i = usm_create_user_from_session(sp);
    mtevL(nldeb, "usm_create_user_from_session(...) -> %d\n", i);
    copy_auth_to_pdu(sp, magic->pdu);
    reqid = snmp_sess_send(ts->slp, magic->pdu);
    if(reqid == 0) {
      int liberr, snmperr;
      char *errmsg;
      snmp_sess_error(ts->slp, &liberr, &snmperr, &errmsg);
      mtevL(nlerr, "Error sending snmp get request: %s\n", errmsg);
      snmp_free_pdu(magic->pdu);
      magic->pdu = NULL;
      goto probe_failed;
    }
    for(i=0; i<info->noids; i++) info->oids[i].reqid = reqid;
    mtevL(nldeb, "Probe followup sent snmp get[all/%d] -> reqid:%d\n", info->noids, reqid);
    add_check(info);
    ts->refcnt--;
    goto out;
  }

 probe_failed:
  if(magic->pdu) snmp_free_pdu(magic->pdu);
  sp->flags &= ~SNMP_FLAGS_DONT_PROBE;
  ret = magic->cb(NETSNMP_CALLBACK_OP_SEND_FAILED,
                  sp, reqid, pdu, info);

 out:
  return ret;
}

static int noit_snmp_send(noit_module_t *self, noit_check_t *check,
                          noit_check_t *cause) {
  struct timeval when, to;
  struct snmp_pdu *req = NULL;
  struct target_session *ts;
  struct check_info *info = check->closure;
  int port = 161, i;
  mtev_boolean separate_queries = mtev_false;
  const char *portstr, *versstr, *sepstr;
  const char *err = "unknown err";
  char target_port[64];

  info->version = SNMP_VERSION_2c;
  info->self = self;
  info->check = check;
  info->timedout = 0;

  BAIL_ON_RUNNING_CHECK(check);
  check->flags |= NP_RUNNING;

  gettimeofday(&check->last_fire_time, NULL);
  if(mtev_hash_retr_str(check->config, "separate_queries",
                        strlen("separate_queries"), &sepstr)) {
    if(!strcasecmp(sepstr, "on") || !strcasecmp(sepstr, "true"))
      separate_queries = mtev_true;
  }
  if(mtev_hash_retr_str(check->config, "port", strlen("port"),
                        &portstr)) {
    port = atoi(portstr);
  }
  if(mtev_hash_retr_str(check->config, "version", strlen("version"),
                        &versstr)) {
    /* We don't care about 2c or others... as they all default to 2c */
    if(!strcmp(versstr, "1")) info->version = SNMP_VERSION_1;
    if(!strcmp(versstr, "3")) info->version = SNMP_VERSION_3;
  }
  snprintf(target_port, sizeof(target_port), "%s:%d", check->target_ip, port);
  ts = _get_target_session(self, target_port, info->version);
  gettimeofday(&check->last_fire_time, NULL);
  if(!ts->refcnt) {
    eventer_t newe;
    struct timeval timeout;
    netsnmp_session *rsess;
    noit_snmp_sess_open(ts, check);
    newe = eventer_alloc();
    newe->fd = ts->fd;
    newe->callback = noit_snmp_handler;
    newe->closure = ts;
    newe->mask = EVENTER_READ | EVENTER_EXCEPTION;
    eventer_add(newe);
  }
  if(!ts->slp) goto bail;
  ts->refcnt++; /* Increment here, decrement when this check completes */

  noit_snmp_fill_oidinfo(check);
  /* Do we need probing? */
  if (info->version == SNMP_VERSION_3 &&
      ts->sess_handle->securityEngineIDLen == 0 &&
      (0 == (ts->sess_handle->flags & SNMP_FLAGS_DONT_PROBE))) {
    /* Allocate some "magic" structure to remember PDU, callback and argument*/
    struct v3_probe_magic *magic = calloc(1, sizeof(struct v3_probe_magic));
    netsnmp_pdu *probe = NULL;

    mtevL(nldeb, "Probing for v3\n");
    if (!magic) goto bail;
    magic->cb = noit_snmp_asynch_response;
    magic->check = check;
    magic->ts = ts;
  
    if (snmpv3_build_probe_pdu(&probe) != 0) {
      free(magic);
      goto bail;
    }

    /* Send it. */
    ts->sess_handle->flags |= SNMP_FLAGS_DONT_PROBE; /* prevent recursion */
    ts->refcnt++;

    magic->pdu = snmp_pdu_create(SNMP_MSG_GET);
    noit_snmp_fill_req(magic->pdu, check, -1);
    magic->pdu->version = info->version;

    if (!snmp_sess_async_send(ts->slp, probe, probe_engine_step1_cb, magic)) {
      ts->refcnt--;
      snmp_free_pdu(probe);
      free(magic);
      ts->sess_handle->flags &= ~SNMP_FLAGS_DONT_PROBE;
      goto bail;
    }

    magic->timeoutevent = eventer_alloc();
    magic->timeoutevent->callback = noit_snmpv3_probe_timeout;
    magic->timeoutevent->closure = magic;
    magic->timeoutevent->mask = EVENTER_TIMER;
  
    gettimeofday(&when, NULL);
    to.tv_sec = 5;
    to.tv_usec = 0;
    add_timeval(when, to, &magic->timeoutevent->whence);
    eventer_add(magic->timeoutevent);
  }
  else {
    /* Separate queries is not supported on v3... it makes no sense */
    if(separate_queries && info->version != SNMP_VERSION_3) {
      int reqid, i;
      mtevL(nldeb, "Regular old get...\n");
      for(i=0;i<info->noids;i++) {
        req = snmp_pdu_create(SNMP_MSG_GET);
        if(!req) continue;
        noit_snmp_fill_req(req, check, i);
        req->version = info->version;
        reqid = snmp_sess_send(ts->slp, req);
        if(reqid == 0) {
          int liberr, snmperr;
          char *errmsg;
          snmp_sess_error(ts->slp, &liberr, &snmperr, &errmsg);
          mtevL(nlerr, "Error sending snmp get request: %s\n", errmsg);
          snmp_free_pdu(req);
          continue;
        }
        info->oids[i].reqid = reqid;
        mtevL(nldeb, "Sent snmp get[%d/%d] -> reqid:%d\n", i, info->noids, reqid);
      }
    }
    else {
      int reqid, i;
      mtevL(nldeb, "Regular old get...\n");
      req = snmp_pdu_create(SNMP_MSG_GET);
      if(!req) goto bail;
      noit_snmp_fill_req(req, check, -1);
      if(info->version == SNMP_VERSION_3 && ts->sess_handle->securityName) {
        i = usm_create_user_from_session(ts->sess_handle);
        mtevL(nldeb, "usm_create_user_from_session(...) -> %d\n", i);
      }
      req->version = info->version;
      reqid = snmp_sess_send(ts->slp, req);
      if(reqid == 0) {
        int liberr, snmperr;
        char *errmsg;
        snmp_sess_error(ts->slp, &liberr, &snmperr, &errmsg);
        mtevL(nlerr, "Error sending snmp get request: %s\n", errmsg);
        err = errmsg;
        if(reqid == 0) goto bail;
      }
      for(i=0; i<info->noids; i++) info->oids[i].reqid = reqid;
      mtevL(nldeb, "Sent snmp get[all/%d] -> reqid:%d\n", info->noids, reqid);
    }
  }
  info->ts = ts;
  info->timeoutevent = eventer_alloc();
  info->timeoutevent->callback = noit_snmp_check_timeout;
  info->timeoutevent->closure = info;
  info->timeoutevent->mask = EVENTER_TIMER;

  gettimeofday(&when, NULL);
  to.tv_sec = check->timeout / 1000;
  to.tv_usec = (check->timeout % 1000) * 1000;
  add_timeval(when, to, &info->timeoutevent->whence);
  eventer_add(info->timeoutevent);
  add_check(info);
  return 0;

 bail:
  ts->refcnt--;
  noit_snmp_log_results(self, check, err);
  noit_snmp_session_cleanse(ts, 1);
  if(req) snmp_free_pdu(req);
  check->flags &= ~NP_RUNNING;
  return 0;
}

static int noit_snmp_initiate_check(noit_module_t *self, noit_check_t *check,
                                    int once, noit_check_t *cause) {
  if(!check->closure) check->closure = calloc(1, sizeof(struct check_info));
  INITIATE_CHECK(noit_snmp_send, self, check, cause);
  return 0;
}

static int noit_snmptrap_initiate_check(noit_module_t *self,
                                        noit_check_t *check,
                                        int once, noit_check_t *cause) {
  /* We don't do anything for snmptrap checks.  Not intuitive... but they
   * never "run."  We accept input out-of-band via snmp traps.
   */
  check->flags |= NP_PASSIVE_COLLECTION;
  return 0;
}

static int noit_snmp_config(noit_module_t *self, mtev_hash_table *options) {
  snmp_mod_config_t *conf;
  conf = noit_module_get_userdata(self);
  if(conf) {
    if(conf->options) {
      mtev_hash_destroy(conf->options, free, free);
      free(conf->options);
    }
  }
  else
    conf = calloc(1, sizeof(*conf));
  conf->options = options;
  noit_module_set_userdata(self, conf);
  return 1;
}
static int noit_snmp_onload(mtev_image_t *self) {
  if(!nlerr) nlerr = mtev_log_stream_find("error/snmp");
  if(!nldeb) nldeb = mtev_log_stream_find("debug/snmp");
  if(!nlerr) nlerr = noit_stderr;
  if(!nldeb) nldeb = noit_debug;
  eventer_name_callback("noit_snmp/check_timeout", noit_snmp_check_timeout);
  eventer_name_callback("noit_snmp/session_timeout", noit_snmp_session_timeout);
  eventer_name_callback("noit_snmp/handler", noit_snmp_handler);
  return 0;
}

static int noit_snmptrap_onload(mtev_image_t *self) {
  if(!nlerr) nlerr = mtev_log_stream_find("error/snmp");
  if(!nldeb) nldeb = mtev_log_stream_find("debug/snmp");
  if(!nlerr) nlerr = noit_stderr;
  if(!nldeb) nldeb = noit_debug;
  eventer_name_callback("noit_snmp/session_timeout", noit_snmp_session_timeout);
  eventer_name_callback("noit_snmp/handler", noit_snmp_handler);
  return 0;
}

static void
nc_printf_snmpts_brief(mtev_console_closure_t ncct,
                       struct target_session *ts) {
  char fd[32];
  struct timeval now, diff;
  const char *snmpvers = "v(unknown)";
  gettimeofday(&now, NULL);
  sub_timeval(now, ts->last_open, &diff);
  if(ts->fd < 0)
    snprintf(fd, sizeof(fd), "%s", "(closed)");
  else
    snprintf(fd, sizeof(fd), "%d", ts->fd);
  switch(ts->version) {
    case SNMP_VERSION_1: snmpvers = "v1"; break;
    case SNMP_VERSION_2c: snmpvers = "v2c"; break;
    case SNMP_VERSION_3: snmpvers = "v3"; break;
  }
  nc_printf(ncct, "[%s %s]\n\topened: %0.3fs ago\n\tFD: %s\n\trefcnt: %d\n",
            ts->target, snmpvers, diff.tv_sec + (float)diff.tv_usec/1000000,
            fd, ts->refcnt);
}

static int
noit_console_show_snmp(mtev_console_closure_t ncct,
                       int argc, char **argv,
                       mtev_console_state_t *dstate,
                       void *closure) {
  mtev_hash_iter iter = MTEV_HASH_ITER_ZERO;
  uuid_t key_id;
  int klen;
  void *vts;
  snmp_mod_config_t *conf = closure;

  while(mtev_hash_next(&conf->target_sessions, &iter,
                       (const char **)key_id, &klen,
                       &vts)) {
    struct target_session *ts = vts;
    nc_printf_snmpts_brief(ncct, ts);
  }
  return 0;
}

static void
register_console_snmp_commands(snmp_mod_config_t *conf) {
  mtev_console_state_t *tl;
  cmd_info_t *showcmd;

  tl = mtev_console_state_initial();
  showcmd = mtev_console_state_get_cmd(tl, "show");
  assert(showcmd && showcmd->dstate);
  mtev_console_state_add_cmd(showcmd->dstate,
    NCSCMD("snmp", noit_console_show_snmp, NULL, NULL, conf));
}

static __thread char linebuf[1024] = "\0";
static int
_private_snmp_log(int majorID, int minorID, void *serverarg, void *clientarg) {
  struct snmp_log_message *slm;
  snmp_mod_config_t *conf;
  size_t len;
  conf = clientarg;
  slm = serverarg;
  len = strlcat(linebuf, slm->msg, sizeof(linebuf));
  if(len > sizeof(linebuf)-1) {
    linebuf[sizeof(linebuf)-2] = '\n';
    linebuf[sizeof(linebuf)-1] = '\0';
  }
  else if(linebuf[len-1] == '\n') {
  }
  else {
    return 1;
  }
  mtevL(((slm->priority < LOG_NOTICE) ? nlerr : nldeb), "[pri:%d] %s",
        slm->priority, linebuf);
  linebuf[0] = '\0';
  return 1;
}

static int noit_snmp_init(noit_module_t *self) {
  const char *opt;
  snmp_mod_config_t *conf;

  conf = noit_module_get_userdata(self);
  if(mtev_hash_retr_str(conf->options, "debugging", strlen("debugging"), &opt)) {
    snmp_set_do_debugging(atoi(opt));
  }

  if(!__snmp_initialize_once) {
    register_mib_handlers();
    read_premib_configs();
    read_configs();
    init_snmp("noitd");
    snmp_disable_stderrlog();
    snmp_register_callback(SNMP_CALLBACK_LIBRARY, SNMP_CALLBACK_LOGGING,
                           _private_snmp_log, conf);
    netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_EMERG);
    netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_ALERT);
    netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_CRIT);
    netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_ERR);
    netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_WARNING);
    netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_NOTICE);
    netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_INFO);
    netsnmp_register_loghandler(NETSNMP_LOGHANDLER_CALLBACK, LOG_DEBUG);
    __snmp_initialize_once = 1;
  }
  if(strcmp(self->hdr.name, "snmp") == 0) {
    register_console_snmp_commands(conf);
  }

  if(strcmp(self->hdr.name, "snmptrap") == 0) {
    eventer_t newe;
    int i, block = 0, fds = 0;
    fd_set fdset;
    struct timeval timeout = { 0, 0 };
    struct target_session *ts;
    netsnmp_transport *transport;
    netsnmp_session sess, *session = &sess;

    if(!mtev_hash_retrieve(conf->options,
                           "snmptrapd_port", strlen("snmptrapd_port"),
                           (void **)&opt))
      opt = "162";

    transport = netsnmp_transport_open_server("snmptrap", opt);
    if(!transport) {
      mtevL(nlerr, "cannot open netsnmp transport for trap daemon\n");
      return -1;
    }
    ts = _get_target_session(self, "snmptrapd", SNMP_DEFAULT_VERSION);
    snmp_sess_init(session);
    session->peername = SNMP_DEFAULT_PEERNAME;
    session->version = SNMP_DEFAULT_VERSION;
    session->community_len = SNMP_DEFAULT_COMMUNITY_LEN;
    session->retries = SNMP_DEFAULT_RETRIES;
    session->timeout = SNMP_DEFAULT_TIMEOUT;
    session->callback = noit_snmp_trapd_response;
    session->callback_magic = (void *) ts;
    session->authenticator = NULL;
    session->isAuthoritative = SNMP_SESS_UNKNOWNAUTH;
    ts->slp = snmp_sess_add(session, transport, NULL, NULL);

    FD_ZERO(&fdset);
    snmp_sess_select_info(ts->slp, &fds, &fdset, &timeout, &block);
    assert(fds > 0);
    for(i=0; i<fds; i++) {
      if(FD_ISSET(i, &fdset)) {
        ts->refcnt++;
        ts->fd = i;
        newe = eventer_alloc();
        newe->fd = ts->fd;
        newe->callback = noit_snmp_handler;
        newe->closure = ts;
        newe->mask = EVENTER_READ | EVENTER_EXCEPTION;
        eventer_add(newe);
      }
    }
  }
  return 0;
}

#include "snmp.xmlh"
noit_module_t snmp = {
  {
    .magic = NOIT_MODULE_MAGIC,
    .version = NOIT_MODULE_ABI_VERSION,
    .name = "snmp",
    .description = "SNMP collection",
    .xml_description = snmp_xml_description,
    .onload = noit_snmp_onload
  },
  noit_snmp_config,
  noit_snmp_init,
  noit_snmp_initiate_check,
  NULL, /* noit_snmp_cleanup */
  .thread_unsafe = 1
};

#include "snmptrap.xmlh"
noit_module_t snmptrap = {
  {
    .magic = NOIT_MODULE_MAGIC,
    .version = NOIT_MODULE_ABI_VERSION,
    .name = "snmptrap",
    .description = "SNMP trap collection",
    .xml_description = snmptrap_xml_description,
    .onload = noit_snmptrap_onload
  },
  noit_snmp_config,
  noit_snmp_init,
  noit_snmptrap_initiate_check,
  NULL, /* noit_snmp_cleanup */
  .thread_unsafe = 1
};
