/*
 * src/bin/pg_autoctl/cli_create_drop_node.c
 *     Implementation of the pg_autoctl create and pg_autoctl drop CLI for the
 *     pg_auto_failover nodes (monitor, coordinator, worker, postgres).
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>

#include "postgres_fe.h"

#include "cli_common.h"
#include "commandline.h"
#include "env_utils.h"
#include "defaults.h"
#include "fsm.h"
#include "ini_file.h"
#include "ipaddr.h"
#include "keeper_config.h"
#include "keeper_pg_init.h"
#include "keeper.h"
#include "monitor.h"
#include "monitor_config.h"
#include "monitor_pg_init.h"
#include "monitor_service.h"
#include "pgctl.h"
#include "primary_standby.h"
#include "service.h"
#include "string_utils.h"

/*
 * Global variables that we're going to use to "communicate" in between getopts
 * functions and their command implementation. We can't pass parameters around.
 */
MonitorConfig monitorOptions;
static bool dropAndDestroy = false;

static int cli_create_postgres_getopts(int argc, char **argv);
static void cli_create_postgres(int argc, char **argv);

static int cli_create_monitor_getopts(int argc, char **argv);
static void cli_create_monitor(int argc, char **argv);

static int cli_drop_node_getopts(int argc, char **argv);
static void cli_drop_node(int argc, char **argv);
static void cli_drop_monitor(int argc, char **argv);

static void cli_drop_node_from_monitor(KeeperConfig *config,
									   const char *nodename,
									   int port);

static bool discover_nodename(char *nodename, int size,
							  const char *monitorHostname, int monitorPort);
static void check_nodename(const char *nodename);

CommandLine create_monitor_command =
	make_command(
		"monitor",
		"Initialize a pg_auto_failover monitor node",
		" [ --pgdata --pgport --pgctl --nodename ] ",
		"  --pgctl           path to pg_ctl\n"
		"  --pgdata          path to data directory\n"
		"  --pgport          PostgreSQL's port number\n"
		"  --nodename        hostname by which postgres is reachable\n"
		"  --auth            authentication method for connections from data nodes\n"
		"  --skip-pg-hba     skip editing pg_hba.conf rules\n"
		"  --run             create node then run pg_autoctl service\n"
		KEEPER_CLI_SSL_OPTIONS,
		cli_create_monitor_getopts,
		cli_create_monitor);

CommandLine create_postgres_command =
	make_command(
		"postgres",
		"Initialize a pg_auto_failover standalone postgres node",
		"",
		"  --pgctl           path to pg_ctl\n"
		"  --pgdata          path to data director\n"
		"  --pghost          PostgreSQL's hostname\n"
		"  --pgport          PostgreSQL's port number\n"
		"  --listen          PostgreSQL's listen_addresses\n"
		"  --username        PostgreSQL's username\n"
		"  --dbname          PostgreSQL's database name\n"
		"  --nodename        pg_auto_failover node\n"
		"  --formation       pg_auto_failover formation\n"
		"  --monitor         pg_auto_failover Monitor Postgres URL\n"
		"  --auth            authentication method for connections from monitor\n"
		"  --skip-pg-hba     skip editing pg_hba.conf rules\n"
		"  --candidate-priority    priority of the node to be promoted to become primary\n"
		"  --replication-quorum    true if node participates in write quorum\n"
		KEEPER_CLI_SSL_OPTIONS
		KEEPER_CLI_ALLOW_RM_PGDATA_OPTION,
		cli_create_postgres_getopts,
		cli_create_postgres);

CommandLine drop_monitor_command =
	make_command("monitor",
				 "Drop the pg_auto_failover monitor",
				 "[ --pgdata --destroy ]",
				 "  --pgdata      path to data directory\n"
				 "  --destroy     also destroy Postgres database\n",
				 cli_drop_node_getopts,
				 cli_drop_monitor);

CommandLine drop_node_command =
	make_command("node",
				 "Drop a node from the pg_auto_failover monitor",
				 "[ --pgdata --destroy --nodename --pgport ]",
				 "  --pgdata      path to data directory\n"
				 "  --destroy     also destroy Postgres database\n"
				 "  --nodename    nodename to remove from the monitor\n"
				 "  --pgport      Postgres port of the node to remove",
				 cli_drop_node_getopts,
				 cli_drop_node);

/*
 * cli_create_config manages the whole set of configuration parameters that
 * pg_autoctl accepts and deals with either creating a configuration file if
 * necessary, or merges the command line arguments into the pre-existing
 * configuration file.
 */
bool
cli_create_config(Keeper *keeper, KeeperConfig *config)
{
	bool missingPgdataIsOk = true;
	bool pgIsNotRunningIsOk = true;
	bool monitorDisabledIsOk = true;

	/*
	 * We support two modes of operations here:
	 *   - configuration exists already, we need PGDATA
	 *   - configuration doesn't exist already, we need PGDATA, and more
	 */
	if (file_exists(config->pathnames.config))
	{
		KeeperConfig options = *config;

		if (!keeper_config_read_file(config,
									 missingPgdataIsOk,
									 pgIsNotRunningIsOk,
									 monitorDisabledIsOk))
		{
			log_fatal("Failed to read configuration file \"%s\"",
					  config->pathnames.config);
			exit(EXIT_CODE_BAD_CONFIG);
		}

		/*
		 * Now that we have loaded the configuration file, apply the command
		 * line options on top of it, giving them priority over the config.
		 */
		if (!keeper_config_merge_options(config, &options))
		{
			/* errors have been logged already */
			exit(EXIT_CODE_BAD_CONFIG);
		}
	}
	else
	{
		/* set our KeeperConfig from the command line options now. */
		(void) keeper_config_init(config,
								  missingPgdataIsOk,
								  pgIsNotRunningIsOk);

		/* and write our brand new setup to file */
		if (!keeper_config_write_file(config))
		{
			log_fatal("Failed to write the pg_autoctl configuration file, "
					  "see above");
			exit(EXIT_CODE_BAD_CONFIG);
		}
	}

	return true;
}


/*
 * cli_pg_create calls keeper_pg_init and handle errors and warnings, then
 * destroys the extra config structure instance from the command line option
 * handling.
 */
void
cli_create_pg(Keeper *keeper)
{
	KeeperConfig *config = &(keeper->config);

	if (!keeper_pg_init(keeper))
	{
		/* errors have been logged */
		exit(EXIT_CODE_BAD_STATE);
	}

	if (keeperInitWarnings)
	{
		log_info("Keeper has been succesfully initialized, "
				 "please fix above warnings to complete installation.");
	}
	else
	{
		log_info("Keeper has been succesfully initialized.");

		if (createAndRun)
		{
			pid_t pid = 0;

			/* now that keeper_pg_init is done, finish the keeper init */
			if (!keeper_init(keeper, config))
			{
				/* errors have already been logged */
				exit(EXIT_CODE_KEEPER);
			}

			if (!keeper_service_init(keeper, &pid))
			{
				log_fatal("Failed to initialize pg_auto_failover service, "
						  "see above for details");
				exit(EXIT_CODE_KEEPER);
			}

			if (!keeper_check_monitor_extension_version(keeper))
			{
				/* errors have already been logged */
				exit(EXIT_CODE_MONITOR);
			}

			if (!keeper_node_active_loop(keeper, pid))
			{
				exit(EXIT_CODE_KEEPER);
			}
		}
	}

	keeper_config_destroy(config);
}


/*
 * cli_create_postgres_getopts parses command line options and set the global
 * variable keeperOptions from them, without doing any check.
 */
static int
cli_create_postgres_getopts(int argc, char **argv)
{
	KeeperConfig options = { 0 };

	static struct option long_options[] = {
		{ "pgctl", required_argument, NULL, 'C' },
		{ "pgdata", required_argument, NULL, 'D' },
		{ "pghost", required_argument, NULL, 'H' },
		{ "pgport", required_argument, NULL, 'p' },
		{ "listen", required_argument, NULL, 'l' },
		{ "username", required_argument, NULL, 'U' },
		{ "auth", required_argument, NULL, 'A' },
		{ "skip-pg-hba", no_argument, NULL, 'S' },
		{ "dbname", required_argument, NULL, 'd' },
		{ "nodename", required_argument, NULL, 'n' },
		{ "formation", required_argument, NULL, 'f' },
		{ "monitor", required_argument, NULL, 'm' },
		{ "disable-monitor", no_argument, NULL, 'M' },
		{ "allow-removing-pgdata", no_argument, NULL, 'R' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ "candidate-priority", required_argument, NULL, 'P' },
		{ "replication-quorum", required_argument, NULL, 'r' },
		{ "run", no_argument, NULL, 'x' },
		{ "help", no_argument, NULL, 0 },
		{ "no-ssl", no_argument, NULL, 'N' },
		{ "ssl-self-signed", no_argument, NULL, 's' },
		{ "ssl-mode", required_argument, &ssl_flag, SSL_MODE_FLAG },
		{ "ssl-ca-file", required_argument, &ssl_flag, SSL_CA_FILE_FLAG },
		{ "ssl-crl-file", required_argument, &ssl_flag, SSL_CRL_FILE_FLAG },
		{ "server-cert", required_argument, &ssl_flag, SSL_SERVER_CRT_FLAG },
		{ "server-key", required_argument, &ssl_flag, SSL_SERVER_KEY_FLAG },
		{ NULL, 0, NULL, 0 }
	};

	int optind =
		cli_create_node_getopts(argc, argv, long_options,
								"+C:D:H:p:l:U:A:Sd:n:f:m:MRVvqhP:r:xsN",
								&options);

	/* publish our option parsing in the global variable */
	keeperOptions = options;

	return optind;
}


/*
 * cli_create_postgres prepares a local PostgreSQL instance to be used as a
 * standalone Postgres instance, not in a Citus formation.
 */
static void
cli_create_postgres(int argc, char **argv)
{
	Keeper keeper = { 0 };
	KeeperConfig *config = &(keeper.config);

	keeper.config = keeperOptions;

	if (!file_exists(keeper.config.pathnames.config))
	{
		/* pg_autoctl create postgres: mark ourselves as a standalone node */
		keeper.config.pgSetup.pgKind = NODE_KIND_STANDALONE;
		strlcpy(keeper.config.nodeKind, "standalone", NAMEDATALEN);

		if (!check_or_discover_nodename(config))
		{
			/* errors have already been logged */
			exit(EXIT_CODE_BAD_ARGS);
		}
	}

	if (!cli_create_config(&keeper, config))
	{
		log_error("Failed to initialize our configuration, see above.");
		exit(EXIT_CODE_BAD_CONFIG);
	}

	cli_create_pg(&keeper);
}


/*
 * cli_create_monitor_getopts parses the command line options necessary to
 * initialise a PostgreSQL instance as our monitor.
 */
static int
cli_create_monitor_getopts(int argc, char **argv)
{
	MonitorConfig options = { 0 };
	int c, option_index = 0, errors = 0;
	int verboseCount = 0;
	SSLCommandLineOptions sslCommandLineOptions = SSL_CLI_UNKNOWN;

	static struct option long_options[] = {
		{ "pgctl", required_argument, NULL, 'C' },
		{ "pgdata", required_argument, NULL, 'D' },
		{ "pgport", required_argument, NULL, 'p' },
		{ "nodename", required_argument, NULL, 'n' },
		{ "listen", required_argument, NULL, 'l' },
		{ "auth", required_argument, NULL, 'A' },
		{ "skip-pg-hba", no_argument, NULL, 'S' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ "run", no_argument, NULL, 'x' },
		{ "no-ssl", no_argument, NULL, 'N' },
		{ "ssl-self-signed", no_argument, NULL, 's' },
		{ "ssl-mode", required_argument, &ssl_flag, SSL_MODE_FLAG },
		{ "ssl-ca-file", required_argument, &ssl_flag, SSL_CA_FILE_FLAG },
		{ "ssl-crl-file", required_argument, &ssl_flag, SSL_CRL_FILE_FLAG },
		{ "server-cert", required_argument, &ssl_flag, SSL_SERVER_CRT_FLAG },
		{ "server-key", required_argument, &ssl_flag, SSL_SERVER_KEY_FLAG },
		{ NULL, 0, NULL, 0 }
	};

	/* hard-coded defaults */
	options.pgSetup.pgport = pgsetup_get_pgport();

	optind = 0;

	while ((c = getopt_long(argc, argv, "+C:D:p:n:l:A:SVvqhxNs",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'C':
			{
				strlcpy(options.pgSetup.pg_ctl, optarg, MAXPGPATH);
				log_trace("--pg_ctl %s", options.pgSetup.pg_ctl);
				break;
			}

			case 'D':
			{
				strlcpy(options.pgSetup.pgdata, optarg, MAXPGPATH);
				log_trace("--pgdata %s", options.pgSetup.pgdata);
				break;
			}

			case 'p':
			{
				if (!stringToInt(optarg, &options.pgSetup.pgport))
				{
					log_fatal("--pgport argument is a valid port number: \"%s\"",
							  optarg);
					exit(EXIT_CODE_BAD_ARGS);
				}
				log_trace("--pgport %d", options.pgSetup.pgport);
				break;
			}

			case 'l':
			{
				strlcpy(options.pgSetup.listen_addresses, optarg, MAXPGPATH);
				log_trace("--listen %s", options.pgSetup.listen_addresses);
				break;
			}

			case 'n':
			{
				strlcpy(options.nodename, optarg, _POSIX_HOST_NAME_MAX);
				log_trace("--nodename %s", options.nodename);
				break;
			}

			case 'A':
			{
				if (!IS_EMPTY_STRING_BUFFER(options.pgSetup.authMethod))
				{
					errors++;
					log_error("Please use either --auth or --skip-pg-hba");
				}

				strlcpy(options.pgSetup.authMethod, optarg, NAMEDATALEN);
				log_trace("--auth %s", options.pgSetup.authMethod);
				break;
			}

			case 'S':
			{
				if (!IS_EMPTY_STRING_BUFFER(options.pgSetup.authMethod))
				{
					errors++;
					log_error("Please use either --auth or --skip-pg-hba");
				}

				strlcpy(options.pgSetup.authMethod,
						SKIP_HBA_AUTH_METHOD,
						NAMEDATALEN);
				log_trace("--skip-pg-hba");
				break;
			}

			case 'V':
			{
				/* keeper_cli_print_version prints version and exits. */
				keeper_cli_print_version(argc, argv);
				break;
			}

			case 'v':
			{
				++verboseCount;
				switch (verboseCount)
				{
					case 1:
					{
						log_set_level(LOG_INFO);
						break;
					}

					case 2:
					{
						log_set_level(LOG_DEBUG);
						break;
					}

					default:
					{
						log_set_level(LOG_TRACE);
						break;
					}
				}
				break;
			}

			case 'q':
			{
				log_set_level(LOG_ERROR);
				break;
			}

			case 'h':
			{
				commandline_help(stderr);
				exit(EXIT_CODE_QUIT);
				break;
			}

			case 'x':
			{
				/* { "run", no_argument, NULL, 'x' }, */
				createAndRun = true;
				log_trace("--run");
				break;
			}

			case 's':
			{
				/* { "ssl-self-signed", no_argument, NULL, 's' }, */
				if (!cli_getopt_accept_ssl_options(SSL_CLI_SELF_SIGNED,
												   sslCommandLineOptions))
				{
					errors++;
					break;
				}
				sslCommandLineOptions = SSL_CLI_SELF_SIGNED;

				options.pgSetup.ssl.active = 1;
				options.pgSetup.ssl.createSelfSignedCert = true;
				log_trace("--ssl-self-signed");
				break;
			}

			case 'N':
			{
				/* { "no-ssl", no_argument, NULL, 'N' }, */
				if (!cli_getopt_accept_ssl_options(SSL_CLI_NO_SSL,
												   sslCommandLineOptions))
				{
					errors++;
					break;
				}
				sslCommandLineOptions = SSL_CLI_NO_SSL;

				options.pgSetup.ssl.active = 0;
				options.pgSetup.ssl.createSelfSignedCert = false;
				log_trace("--no-ssl");
				break;
			}

			/*
			 * { "ssl-ca-file", required_argument, &ssl_flag, SSL_CA_FILE_FLAG }
			 * { "ssl-crl-file", required_argument, &ssl_flag, SSL_CA_FILE_FLAG }
			 * { "server-cert", required_argument, &ssl_flag, SSL_SERVER_CRT_FLAG }
			 * { "server-key", required_argument, &ssl_flag, SSL_SERVER_KEY_FLAG }
			 * { "ssl-mode", required_argument, &ssl_flag, SSL_MODE_FLAG },
			 */
			case 0:
			{
				if (ssl_flag != SSL_MODE_FLAG)
				{
					if (!cli_getopt_accept_ssl_options(SSL_CLI_USER_PROVIDED,
													   sslCommandLineOptions))
					{
						errors++;
						break;
					}

					sslCommandLineOptions = SSL_CLI_USER_PROVIDED;
					options.pgSetup.ssl.active = 1;
				}

				if (!cli_getopt_ssl_flags(ssl_flag, optarg, &(options.pgSetup)))
				{
					errors++;
				}
				break;
			}

			default:
			{
				/* getopt_long already wrote an error message */
				commandline_help(stderr);
				exit(EXIT_CODE_BAD_ARGS);
				break;
			}
		}
	}

	if (errors > 0)
	{
		commandline_help(stderr);
		exit(EXIT_CODE_BAD_ARGS);
	}

	/*
	 * We're not using pg_setup_init() here: we are following a very different
	 * set of rules. We just want to check:
	 *
	 *   - PGDATA is set and the directory does not exists
	 *   - PGPORT is either set or defaults to 5432
	 *
	 * Also we use the first pg_ctl binary found in the PATH, we're not picky
	 * here, we don't have to manage the whole life-time of that PostgreSQL
	 * instance.
	 */
	if (IS_EMPTY_STRING_BUFFER(options.pgSetup.pgdata))
	{
		get_env_pgdata_or_exit(options.pgSetup.pgdata);
	}

	/*
	 * We require the user to specify an authentication mechanism, or to use
	 * ---skip-pg-hba. Our documentation tutorial will use --auth trust, and we
	 * should make it obvious that this is not the right choice for production.
	 */
	if (IS_EMPTY_STRING_BUFFER(options.pgSetup.authMethod))
	{
		log_fatal("Please use either --auth trust|md5|... or --skip-pg-hba");
		log_info("pg_auto_failover can be set to edit Postgres HBA rules "
				 "automatically when needed. For quick testing '--auth trust' "
				 "makes it easy to get started, "
				 "consider another authentication mechanism for production.");
		exit(EXIT_CODE_BAD_ARGS);
	}

	/*
	 * If any --ssl-* option is provided, either we have a root ca file and a
	 * server.key and a server.crt or none of them. Any other combo is a
	 * mistake.
	 */
	if (sslCommandLineOptions == SSL_CLI_UNKNOWN)
	{
		log_fatal("Explicit SSL choice is required: please use either "
				  "--ssl-self-signed or provide your certificates "
				  "using --ssl-ca-file, --ssl-crl-file, "
				  "--server-key, and --server-cert (or use --no-ssl if you "
				  "are very sure that you do not want encrypted traffic)");
		exit(EXIT_CODE_BAD_ARGS);
	}

	if (!pgsetup_validate_ssl_settings(&(options.pgSetup)))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_ARGS);
	}

	if (IS_EMPTY_STRING_BUFFER(options.pgSetup.pg_ctl))
	{
		set_first_pgctl(&(options.pgSetup));
	}

	if (IS_EMPTY_STRING_BUFFER(options.pgSetup.listen_addresses))
	{
		strlcpy(options.pgSetup.listen_addresses,
				POSTGRES_DEFAULT_LISTEN_ADDRESSES, MAXPGPATH);
	}

	/* publish our option parsing in the global variable */
	monitorOptions = options;

	return optind;
}


/*
 * Initialize the PostgreSQL instance that we're using for the Monitor:
 *
 *  - pg_ctl initdb
 *  - add postgresql-citus.conf to postgresql.conf
 *  - pg_ctl start
 *  - create user autoctl with createdb login;
 *  - create database pg_auto_failover with owner autoctl;
 *  - create extension pgautofailover;
 *
 * When this function is called (from monitor_config_init at the CLI level), we
 * know that PGDATA has been initdb already, and that's about it.
 *
 */
static void
cli_create_monitor(int argc, char **argv)
{
	Monitor monitor = { 0 };
	MonitorConfig *config = &(monitor.config);
	char connInfo[MAXCONNINFO];
	bool missingPgdataIsOk = true;
	bool pgIsNotRunningIsOk = true;

	*config = monitorOptions;

	/*
	 * We support two modes of operations here:
	 *   - configuration exists already, we need PGDATA
	 *   - configuration doesn't exist already, we need PGDATA, and more
	 */
	if (!monitor_config_set_pathnames_from_pgdata(config))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_ARGS);
	}

	if (file_exists(config->pathnames.config))
	{
		MonitorConfig options = *config;

		if (!monitor_config_read_file(config,
									  missingPgdataIsOk,
									  pgIsNotRunningIsOk))
		{
			log_fatal("Failed to read configuration file \"%s\"",
					  config->pathnames.config);
			exit(EXIT_CODE_BAD_CONFIG);
		}

		/*
		 * Now that we have loaded the configuration file, apply the command
		 * line options on top of it, giving them priority over the config.
		 */
		if (!monitor_config_merge_options(config, &options))
		{
			/* errors have been logged already */
			exit(EXIT_CODE_BAD_CONFIG);
		}
	}
	else
	{
		/* Take care of the --nodename */
		if (IS_EMPTY_STRING_BUFFER(config->nodename))
		{
			if (!discover_nodename((char *) (&config->nodename),
								   _POSIX_HOST_NAME_MAX,
								   DEFAULT_INTERFACE_LOOKUP_SERVICE_NAME,
								   DEFAULT_INTERFACE_LOOKUP_SERVICE_PORT))
			{
				log_fatal("Failed to auto-detect the hostname of this machine, "
						  "please provide one via --nodename");
				exit(EXIT_CODE_BAD_ARGS);
			}
		}
		else
		{
			/*
			 * When provided with a --nodename option, we run some checks on
			 * the user provided value based on Postgres usage for the hostname
			 * in its HBA setup. Both forward and reverse DNS needs to return
			 * meaningful values for the connections to be granted when using a
			 * hostname.
			 *
			 * That said network setup is something complex and we don't
			 * pretend we are able to avoid any and all false negatives in our
			 * checks, so we only WARN when finding something that might be
			 * fishy, and proceed with the setup of the local node anyway.
			 */
			(void) check_nodename(config->nodename);
		}

		/* set our MonitorConfig from the command line options now. */
		(void) monitor_config_init(config,
								   missingPgdataIsOk,
								   pgIsNotRunningIsOk);

		/* and write our brand new setup to file */
		if (!monitor_config_write_file(config))
		{
			log_fatal("Failed to write the monitor's configuration file, "
					  "see above");
			exit(EXIT_CODE_BAD_CONFIG);
		}
	}

	pg_setup_get_local_connection_string(&(config->pgSetup), connInfo);
	monitor_init(&monitor, connInfo);

	/* Ok, now we know we have a configuration file, and it's been loaded. */
	if (!monitor_pg_init(&monitor))
	{
		/* errors have been logged */
		exit(EXIT_CODE_BAD_STATE);
	}

	log_info("Monitor has been succesfully initialized.");

	if (createAndRun)
	{
		pid_t pid = 0;

		if (!monitor_service_init(config, &pid))
		{
			log_fatal("Failed to initialize pg_auto_failover service, "
					  "see above for details");
			exit(EXIT_CODE_INTERNAL_ERROR);
		}

		(void) monitor_service_run(&monitor, pid);
	}
	else
	{
		char postgresUri[MAXCONNINFO];

		if (monitor_config_get_postgres_uri(config, postgresUri, MAXCONNINFO))
		{
			log_info("pg_auto_failover monitor is ready at %s", postgresUri);
		}
	}
}


/*
 * cli_drop_node_getopts parses the command line options necessary to drop or
 * destroy a local pg_autoctl node.
 */
static int
cli_drop_node_getopts(int argc, char **argv)
{
	KeeperConfig options = { 0 };
	int c, option_index = 0;
	int verboseCount = 0;

	static struct option long_options[] = {
		{ "pgdata", required_argument, NULL, 'D' },
		{ "destroy", no_argument, NULL, 'd' },
		{ "nodename", required_argument, NULL, 'n' },
		{ "pgport", required_argument, NULL, 'p' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	optind = 0;

	while ((c = getopt_long(argc, argv, "+D:dn:p:Vvqh",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
			{
				strlcpy(options.pgSetup.pgdata, optarg, MAXPGPATH);
				log_trace("--pgdata %s", options.pgSetup.pgdata);
				break;
			}

			case 'd':
			{
				dropAndDestroy = true;
				log_trace("--destroy");
				break;
			}

			case 'n':
			{
				strlcpy(options.nodename, optarg, _POSIX_HOST_NAME_MAX);
				log_trace("--nodename %s", options.nodename);
				break;
			}

			case 'p':
			{
				if (!stringToInt(optarg, &options.pgSetup.pgport))
				{
					log_fatal("--pgport argument is a valid port number: \"%s\"",
							  optarg);
					exit(EXIT_CODE_BAD_ARGS);
				}
				log_trace("--pgport %d", options.pgSetup.pgport);
				break;
			}

			case 'V':
			{
				/* keeper_cli_print_version prints version and exits. */
				keeper_cli_print_version(argc, argv);
				break;
			}

			case 'v':
			{
				++verboseCount;
				switch (verboseCount)
				{
					case 1:
					{
						log_set_level(LOG_INFO);
						break;
					}

					case 2:
					{
						log_set_level(LOG_DEBUG);
						break;
					}

					default:
					{
						log_set_level(LOG_TRACE);
						break;
					}
				}
				break;
			}

			case 'q':
			{
				log_set_level(LOG_ERROR);
				break;
			}

			case 'h':
			{
				commandline_help(stderr);
				exit(EXIT_CODE_QUIT);
				break;
			}

			default:
			{
				/* getopt_long already wrote an error message */
				commandline_help(stderr);
				exit(EXIT_CODE_BAD_ARGS);
				break;
			}
		}
	}

	if (dropAndDestroy &&
		(!IS_EMPTY_STRING_BUFFER(options.nodename) ||
		 options.pgSetup.pgport != 0))
	{
		log_error("Please use either --nodename and --pgport or ---destroy");
		log_info("Destroying a node is not supported from a distance");
		exit(EXIT_CODE_BAD_ARGS);
	}

	/* now that we have the command line parameters, prepare the options */
	(void) prepare_keeper_options(&options);

	/* publish our option parsing in the global variable */
	keeperOptions = options;

	return optind;
}


/*
 * cli_drop_node removes the local PostgreSQL node from the pg_auto_failover
 * monitor, and when it's a worker, from the Citus coordinator too.
 */
static void
cli_drop_node(int argc, char **argv)
{
	KeeperConfig config = keeperOptions;

	bool missingPgdataIsOk = true;
	bool pgIsNotRunningIsOk = true;
	bool monitorDisabledIsOk = false;

	/*
	 * The configuration file is the last bit we remove, so we don't have to
	 * implement "continue from previous failed attempt" when the configuration
	 * file does not exists.
	 */
	if (!file_exists(config.pathnames.config))
	{
		log_error("Failed to find expected configuration file \"%s\"",
				  config.pathnames.config);
		exit(EXIT_CODE_BAD_CONFIG);
	}

	/*
	 * We are going to need to use the right pg_ctl binary to control the
	 * Postgres cluster: pg_ctl stop.
	 */
	switch (ProbeConfigurationFileRole(config.pathnames.config))
	{
		case PG_AUTOCTL_ROLE_MONITOR:
		{
			/*
			 * Now check --nodename and --pgport and remove the entry on the
			 * monitor.
			 */
			if (IS_EMPTY_STRING_BUFFER(config.nodename) ||
				config.pgSetup.pgport == 0)
			{
				log_fatal("To remove a node from the monitor, both the "
						  "--nodename and --pgport options are required");
				exit(EXIT_CODE_BAD_ARGS);
			}

			/* pg_autoctl drop node on the monitor drops another node */
			(void) cli_drop_node_from_monitor(&config,
											  config.nodename,
											  config.pgSetup.pgport);
			return;
		}

		case PG_AUTOCTL_ROLE_KEEPER:
		{
			if (!IS_EMPTY_STRING_BUFFER(config.nodename) ||
				config.pgSetup.pgport != 0)
			{
				log_fatal("Only dropping the local node is supported");
				log_info("To drop another node, please use this command "
						 "from the monitor itself.");
				exit(EXIT_CODE_BAD_ARGS);
			}

			/* just read the keeper file in given KeeperConfig */
			if (!keeper_config_read_file(&config,
										 missingPgdataIsOk,
										 pgIsNotRunningIsOk,
										 monitorDisabledIsOk))
			{
				exit(EXIT_CODE_BAD_CONFIG);
			}

			/* drop the node and maybe destroy its PGDATA entirely. */
			(void) cli_drop_local_node(&config, dropAndDestroy);

			return;
		}

		default:
		{
			log_fatal("Unrecognized configuration file \"%s\"",
					  config.pathnames.config);
			exit(EXIT_CODE_BAD_CONFIG);
		}
	}
}


/*
 * cli_drop_monitor removes the local monitor node.
 */
static void
cli_drop_monitor(int argc, char **argv)
{
	KeeperConfig config = keeperOptions;

	bool missingPgdataIsOk = true;
	bool pgIsNotRunningIsOk = true;

	/*
	 * The configuration file is the last bit we remove, so we don't have to
	 * implement "continue from previous failed attempt" when the configuration
	 * file does not exists.
	 */
	if (!file_exists(config.pathnames.config))
	{
		log_error("Failed to find expected configuration file \"%s\"",
				  config.pathnames.config);
		exit(EXIT_CODE_BAD_CONFIG);
	}

	/*
	 * We are going to need to use the right pg_ctl binary to control the
	 * Postgres cluster: pg_ctl stop.
	 */
	switch (ProbeConfigurationFileRole(config.pathnames.config))
	{
		case PG_AUTOCTL_ROLE_MONITOR:
		{
			MonitorConfig mconfig = { 0 };

			if (!monitor_config_init_from_pgsetup(&mconfig,
												  &(config.pgSetup),
												  missingPgdataIsOk,
												  pgIsNotRunningIsOk))
			{
				/* errors have already been logged */
				exit(EXIT_CODE_BAD_CONFIG);
			}

			/* expose the pgSetup in the given KeeperConfig */
			config.pgSetup = mconfig.pgSetup;

			/* somehow at this point we've lost our pathnames */
			if (!keeper_config_set_pathnames_from_pgdata(
					&(config.pathnames),
					config.pgSetup.pgdata))
			{
				/* errors have already been logged */
				exit(EXIT_CODE_BAD_ARGS);
			}

			/* drop the node and maybe destroy its PGDATA entirely. */
			(void) cli_drop_local_node(&config, dropAndDestroy);
			return;
		}

		case PG_AUTOCTL_ROLE_KEEPER:
		{
			log_fatal("Local node is not a monitor");
			exit(EXIT_CODE_BAD_CONFIG);

			break;
		}

		default:
		{
			log_fatal("Unrecognized configuration file \"%s\"",
					  config.pathnames.config);
			exit(EXIT_CODE_BAD_CONFIG);
		}
	}
}


/*
 * cli_drop_node_from_monitor calls pgautofailover.remove_node() on the
 * monitor for the given --nodename and --pgport.
 */
static void
cli_drop_node_from_monitor(KeeperConfig *config, const char *nodename, int port)
{
	Monitor monitor = { 0 };
	MonitorConfig mconfig = { 0 };
	char connInfo[MAXCONNINFO] = { 0 };

	bool missingPgdataIsOk = true;
	bool pgIsNotRunningIsOk = true;

	if (!monitor_config_init_from_pgsetup(&mconfig,
										  &(config->pgSetup),
										  missingPgdataIsOk,
										  pgIsNotRunningIsOk))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_CONFIG);
	}

	/* expose the pgSetup in the given KeeperConfig */
	config->pgSetup = mconfig.pgSetup;

	/* prepare to connect to the monitor, locally */
	pg_setup_get_local_connection_string(&(mconfig.pgSetup), connInfo);
	monitor_init(&monitor, connInfo);

	if (!monitor_remove(&monitor, (char *) nodename, port))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_MONITOR);
	}
}


/*
 * check_or_discover_nodename checks given --nodename or attempt to discover a
 * suitable default value for the current node when it's not been provided on
 * the command line.
 */
bool
check_or_discover_nodename(KeeperConfig *config)
{
	/* take care of the nodename */
	if (IS_EMPTY_STRING_BUFFER(config->nodename))
	{
		char monitorHostname[_POSIX_HOST_NAME_MAX];
		int monitorPort = 0;

		/*
		 * When --disable-monitor, use the defaults for ipAddr discovery, same
		 * as when creating the monitor node itself.
		 */
		if (config->monitorDisabled)
		{
			strlcpy(monitorHostname,
					DEFAULT_INTERFACE_LOOKUP_SERVICE_NAME,
					_POSIX_HOST_NAME_MAX);

			monitorPort = DEFAULT_INTERFACE_LOOKUP_SERVICE_PORT;
		}
		else if (!hostname_from_uri(config->monitor_pguri,
									monitorHostname, _POSIX_HOST_NAME_MAX,
									&monitorPort))
		{
			log_fatal("Failed to determine monitor hostname when parsing "
					  "Postgres URI \"%s\"", config->monitor_pguri);
			return false;
		}

		if (!discover_nodename((char *) &(config->nodename),
							   _POSIX_HOST_NAME_MAX,
							   monitorHostname,
							   monitorPort))
		{
			log_fatal("Failed to auto-detect the hostname of this machine, "
					  "please provide one via --nodename");
			return false;
		}
	}
	else
	{
		/*
		 * When provided with a --nodename option, we run some checks on the
		 * user provided value based on Postgres usage for the hostname in its
		 * HBA setup. Both forward and reverse DNS needs to return meaningful
		 * values for the connections to be granted when using a hostname.
		 *
		 * That said network setup is something complex and we don't pretend we
		 * are able to avoid any and all false negatives in our checks, so we
		 * only WARN when finding something that might be fishy, and proceed
		 * with the setup of the local node anyway.
		 */
		(void) check_nodename(config->nodename);
	}
	return true;
}


/*
 * discover_nodename discovers a suitable --nodename default value in three
 * steps:
 *
 * 1. First find the local LAN IP address by connecting a socket() to either an
 *    internet service (8.8.8.8:53) or to the monitor's hostname and port, and
 *    then inspecting which local address has been used.
 *
 * 2. Use the local IP address obtained in the first step and do a reverse DNS
 *    lookup for it. The answer is our candidate default --nodename.
 *
 * 3. Do a DNS lookup for the candidate default --nodename. If we get back a IP
 *    address that matches one of the local network interfaces, we keep the
 *    candidate, the DNS lookup that Postgres does at connection time is
 *    expected to then work.
 *
 * All this dansing around DNS lookups is necessary in order to mimic Postgres
 * HBA matching of hostname rules against client IP addresses: the hostname in
 * the HBA rule is resolved and compared to the client IP address. We want the
 * --nodename we use to resolve to an IP address that exists on the local
 * Postgres server.
 *
 * Worst case here is that we fail to discover a --nodename and then ask the
 * user to provide one for us.
 *
 * monitorHostname and monitorPort are used to open a socket to that address,
 * in order to find the right outbound interface. When creating a monitor node,
 * of course, we don't have the monitorHostname yet: we are trying to discover
 * it... in that case we use PG_AUTOCTL_DEFAULT_SERVICE_NAME and PORT, which
 * are the Google DNS service: 8.8.8.8:53, expected to be reachable.
 */
static bool
discover_nodename(char *nodename, int size,
				  const char *monitorHostname, int monitorPort)
{
	/*
	 * Try and find a default --nodename. The --nodename is mandatory, so
	 * when not provided for by the user, then failure to discover a
	 * suitable nodename is a fatal error.
	 */
	char ipAddr[BUFSIZE];
	char localIpAddr[BUFSIZE];
	char hostname[_POSIX_HOST_NAME_MAX];

	/* fetch our local address among the network interfaces */
	if (!fetchLocalIPAddress(ipAddr, BUFSIZE, monitorHostname, monitorPort))
	{
		log_fatal("Failed to find a local IP address, "
				  "please provide --nodename.");
		return false;
	}

	/* from there on we can take the ipAddr as the default --nodename */
	strlcpy(nodename, ipAddr, size);
	log_debug("discover_nodename: local ip %s", ipAddr);

	/* do a reverse DNS lookup from our local LAN ip address */
	if (!findHostnameFromLocalIpAddress(ipAddr,
										hostname, _POSIX_HOST_NAME_MAX))
	{
		/* errors have already been logged */
		log_info("Using local IP address \"%s\" as the --nodename.", ipAddr);
		return true;
	}
	log_debug("discover_nodename: host from ip %s", hostname);

	/* do a DNS lookup of the hostname we got from the IP address */
	if (!findHostnameLocalAddress(hostname, localIpAddr, BUFSIZE))
	{
		/* errors have already been logged */
		log_info("Using local IP address \"%s\" as the --nodename.", ipAddr);
		return true;
	}
	log_debug("discover_nodename: ip from host %s", localIpAddr);

	/*
	 * ok ipAddr resolves to an hostname that resolved back to a local address,
	 * we should be able to use the hostname in pg_hba.conf
	 */
	strlcpy(nodename, hostname, size);
	log_info("Using --nodename \"%s\", which resolves to IP address \"%s\"",
			 nodename, localIpAddr);

	return true;
}


/*
 * check_nodename runs some DNS check against the provided --nodename in order
 * to warn the user in case we might later fail to use it in the Postgres HBA
 * setup.
 *
 * The main trouble we guard against is from HBA authentication. Postgres HBA
 * check_hostname() does a DNS lookup of the hostname found in the pg_hba.conf
 * file and then compares the IP addresses obtained to the client IP address,
 * and refuses the connection where there's no match.
 */
static void
check_nodename(const char *nodename)
{
	char localIpAddress[INET_ADDRSTRLEN];
	IPType ipType = ip_address_type(nodename);

	if (ipType == IPTYPE_NONE)
	{
		if (!findHostnameLocalAddress(nodename,
									  localIpAddress, INET_ADDRSTRLEN))
		{
			log_warn(
				"Failed to resolve nodename \"%s\" to a local IP address, "
				"automated pg_hba.conf setup might fail.", nodename);
		}
	}
	else
	{
		char cidr[BUFSIZE];

		if (!fetchLocalCIDR(nodename, cidr, BUFSIZE))
		{
			log_warn("Failed to find adress \"%s\" in local network "
					 "interfaces, automated pg_hba.conf setup might fail.",
					 nodename);
		}
	}
}
