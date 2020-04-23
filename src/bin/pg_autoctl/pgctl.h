/*
 * src/bin/pg_autoctl/pgctl.h
 *   API for controling PostgreSQL, using its binary tooling (pg_ctl,
 *   pg_controldata, pg_basebackup and such).
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#ifndef PGCTL_H
#define PGCTL_H

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "postgres_fe.h"
#include "utils/pidfile.h"

#include "defaults.h"
#include "file_utils.h"
#include "pgsetup.h"
#include "pgsql.h"

#define PG_CTL_STATUS_NOT_RUNNING 3

bool pg_controldata(PostgresSetup *pgSetup, bool missing_ok);
int config_find_pg_ctl(PostgresSetup *pgSetup);
char * pg_ctl_version(const char *pg_ctl_path);

bool pg_add_auto_failover_default_settings(PostgresSetup *pgSetup,
										   char *configFilePath,
										   GUC *settings);
bool pg_basebackup(const char *pgdata,
				   const char *pg_ctl,
				   ReplicationSource *replicationSource);
bool pg_rewind(const char *pgdata,
			   const char *pg_ctl,
			   ReplicationSource *replicationSource);

bool pg_ctl_initdb(const char *pg_ctl, const char *pgdata);
bool pg_ctl_start(const char *pg_ctl,
				  const char *pgdata, int pgport, char *listen_addresses);
bool pg_log_startup(const char *pgdata, int logLevel);
bool pg_ctl_stop(const char *pg_ctl, const char *pgdata);
int pg_ctl_status(const char *pg_ctl, const char *pgdata, bool log_output);
bool pg_ctl_restart(const char *pg_ctl, const char *pgdata);
bool pg_ctl_promote(const char *pg_ctl, const char *pgdata);

bool pg_setup_standby_mode(uint32_t pg_control_version,
						   const char *pgdata,
						   ReplicationSource *replicationSource);

bool pg_cleanup_standby_mode(uint32_t pg_control_version,
							 const char *pg_ctl,
							 const char *pgdata,
							 PGSQL *pgsql);

bool pg_is_running(const char *pg_ctl, const char *pgdata);
bool pg_create_self_signed_cert(PostgresSetup *pgSetup, const char *nodename);

#endif /* PGCTL_H */
