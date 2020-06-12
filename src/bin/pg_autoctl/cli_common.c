/*
 * src/bin/pg_autoctl/cli_common.c
 *     Implementation of a CLI which lets you run individual keeper routines
 *     directly
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */
#include <errno.h>
#include <inttypes.h>
#include <getopt.h>
#include <signal.h>

#include "commandline.h"

#include "cli_common.h"
#include "cli_root.h"
#include "commandline.h"
#include "env_utils.h"
#include "ipaddr.h"
#include "keeper.h"
#include "keeper_config.h"
#include "log.h"
#include "monitor.h"
#include "monitor_config.h"
#include "parsing.h"
#include "pgsetup.h"
#include "pgsql.h"
#include "state.h"
#include "service.h"
#include "string_utils.h"

/* handle command line options for our setup. */
KeeperConfig keeperOptions;
bool allowRemovingPgdata = false;
bool createAndRun = false;
bool outputJSON = false;
int ssl_flag = 0;

static void stop_postgres_and_remove_pgdata_and_config(ConfigFilePaths *pathnames,
													   PostgresSetup *pgSetup);

/*
 * cli_create_node_getopts parses the CLI options for the pg_autoctl create
 * command. An example of a long_options parameter would look like this:
 *
 *	static struct option long_options[] = {
 *		{ "pgctl", required_argument, NULL, 'C' },
 *		{ "pgdata", required_argument, NULL, 'D' },
 *		{ "pghost", required_argument, NULL, 'H' },
 *		{ "pgport", required_argument, NULL, 'p' },
 *		{ "listen", required_argument, NULL, 'l' },
 *		{ "proxyport", required_argument, NULL, 'y' },
 *		{ "username", required_argument, NULL, 'U' },
 *		{ "auth", required_argument, NULL, 'A' },
 *		{ "skip-pg-hba", required_argument, NULL, 'S' },
 *		{ "dbname", required_argument, NULL, 'd' },
 *		{ "nodename", required_argument, NULL, 'n' },
 *		{ "formation", required_argument, NULL, 'f' },
 *		{ "group", required_argument, NULL, 'g' },
 *		{ "monitor", required_argument, NULL, 'm' },
 *		{ "disable-monitor", no_argument, NULL, 'M' },
 *		{ "allow-removing-pgdata", no_argument, NULL, 'R' },
 *		{ "version", no_argument, NULL, 'V' },
 *		{ "verbose", no_argument, NULL, 'v' },
 *		{ "quiet", no_argument, NULL, 'q' },
 *		{ "help", no_argument, NULL, 'h' },
 *		{ "candidate-priority", required_argument, NULL, 'P'},
 *		{ "replication-quorum", required_argument, NULL, 'r'},
 *		{ "help", no_argument, NULL, 0 },
 *		{ "run", no_argument, NULL, 'x' },
 *      { "ssl-self-signed", no_argument, NULL, 's' },
 *      { "no-ssl", no_argument, NULL, 'N' },
 *      { "ssl-ca-file", required_argument, &ssl_flag, SSL_CA_FILE_FLAG },
 *      { "server-cert", required_argument, &ssl_flag, SSL_SERVER_CRT_FLAG },
 *      { "server-key", required_argument, &ssl_flag, SSL_SERVER_KEY_FLAG },
 *      { "ssl-mode", required_argument, &ssl_flag, SSL_MODE_FLAG },
 *		{ NULL, 0, NULL, 0 }
 *	};
 *
 */
int
cli_create_node_getopts(int argc, char **argv,
						struct option *long_options,
						const char *optstring,
						KeeperConfig *options)
{
	KeeperConfig LocalOptionConfig = { 0 };
	int c, option_index = 0, errors = 0;
	int verboseCount = 0;
	SSLCommandLineOptions sslCommandLineOptions = SSL_CLI_UNKNOWN;

	/* force some non-zero default values */
	LocalOptionConfig.monitorDisabled = false;
	LocalOptionConfig.groupId = -1;
	LocalOptionConfig.network_partition_timeout = -1;
	LocalOptionConfig.prepare_promotion_catchup = -1;
	LocalOptionConfig.prepare_promotion_walreceiver = -1;
	LocalOptionConfig.postgresql_restart_failure_timeout = -1;
	LocalOptionConfig.postgresql_restart_failure_max_retries = -1;
	LocalOptionConfig.pgSetup.settings.candidatePriority =
		FAILOVER_NODE_CANDIDATE_PRIORITY;
	LocalOptionConfig.pgSetup.settings.replicationQuorum =
		FAILOVER_NODE_REPLICATION_QUORUM;


	optind = 0;

	while ((c = getopt_long(argc, argv,
							optstring, long_options, &option_index)) != -1)
	{
		/*
		 * The switch statement is ready for all the common letters of the
		 * different nodes that `pg_autoctl create` knows how to deal with. The
		 * parameter optstring restrict which letters we are going to actually
		 * parsed, and there's no command that has all of them.
		 */
		switch (c)
		{
			case 'C':
			{
				/* { "pgctl", required_argument, NULL, 'C' } */
				strlcpy(LocalOptionConfig.pgSetup.pg_ctl, optarg, MAXPGPATH);
				log_trace("--pg_ctl %s", LocalOptionConfig.pgSetup.pg_ctl);
				break;
			}

			case 'D':
			{
				/* { "pgdata", required_argument, NULL, 'D' } */
				strlcpy(LocalOptionConfig.pgSetup.pgdata, optarg, MAXPGPATH);
				log_trace("--pgdata %s", LocalOptionConfig.pgSetup.pgdata);
				break;
			}

			case 'H':
			{
				/* { "pghost", required_argument, NULL, 'h' } */
				strlcpy(LocalOptionConfig.pgSetup.pghost, optarg, _POSIX_HOST_NAME_MAX);
				log_trace("--pghost %s", LocalOptionConfig.pgSetup.pghost);
				break;
			}

			case 'p':
			{
				/* { "pgport", required_argument, NULL, 'p' } */
				if (!stringToInt(optarg, &LocalOptionConfig.pgSetup.pgport))
				{
					log_error("Failed to parse --pgport number \"%s\"",
							  optarg);
					errors++;
				}
				log_trace("--pgport %d", LocalOptionConfig.pgSetup.pgport);
				break;
			}

			case 'l':
			{
				/* { "listen", required_argument, NULL, 'l' } */
				strlcpy(LocalOptionConfig.pgSetup.listen_addresses, optarg, MAXPGPATH);
				log_trace("--listen %s", LocalOptionConfig.pgSetup.listen_addresses);
				break;
			}

			case 'y':
			{
				/* { "proxyport", required_argument, NULL,'y' } */
				if (!stringToInt(optarg, &LocalOptionConfig.pgSetup.proxyport))
				{
					log_error("Failed to parse --proxyport number \"%s\"",
							  optarg);
					errors++;
				}
				log_trace("--proxy %d", LocalOptionConfig.pgSetup.proxyport);
				break;
			}

			case 'U':
			{
				/* { "username", required_argument, NULL, 'U' } */
				strlcpy(LocalOptionConfig.pgSetup.username, optarg, NAMEDATALEN);
				log_trace("--username %s", LocalOptionConfig.pgSetup.username);
				break;
			}

			case 'A':
			{
				/* { "auth", required_argument, NULL, 'A' }, */
				if (!IS_EMPTY_STRING_BUFFER(LocalOptionConfig.pgSetup.authMethod))
				{
					errors++;
					log_error("Please use either --auth or --skip-pg-hba");
				}

				strlcpy(LocalOptionConfig.pgSetup.authMethod, optarg, NAMEDATALEN);
				log_trace("--auth %s", LocalOptionConfig.pgSetup.authMethod);
				break;
			}

			case 'S':
			{
				/* { "skip-pg-hba", required_argument, NULL, 'S' }, */
				if (!IS_EMPTY_STRING_BUFFER(LocalOptionConfig.pgSetup.authMethod))
				{
					errors++;
					log_error("Please use either --auth or --skip-pg-hba");
				}

				strlcpy(LocalOptionConfig.pgSetup.authMethod,
						SKIP_HBA_AUTH_METHOD,
						NAMEDATALEN);
				log_trace("--skip-pg-hba");
				break;
			}

			case 'd':
			{
				/* { "dbname", required_argument, NULL, 'd' } */
				strlcpy(LocalOptionConfig.pgSetup.dbname, optarg, NAMEDATALEN);
				log_trace("--dbname %s", LocalOptionConfig.pgSetup.dbname);
				break;
			}

			case 'n':
			{
				/* { "nodename", required_argument, NULL, 'n' } */
				strlcpy(LocalOptionConfig.nodename, optarg, _POSIX_HOST_NAME_MAX);
				log_trace("--nodename %s", LocalOptionConfig.nodename);
				break;
			}

			case 'f':
			{
				/* { "formation", required_argument, NULL, 'f' } */
				strlcpy(LocalOptionConfig.formation, optarg, NAMEDATALEN);
				log_trace("--formation %s", LocalOptionConfig.formation);
				break;
			}

			case 'g':
			{
				/* { "group", required_argument, NULL, 'g' } */
				if (!stringToInt(optarg, &LocalOptionConfig.groupId))
				{
					log_fatal("--group argument is not a valid group ID: \"%s\"",
							  optarg);
					exit(EXIT_CODE_BAD_ARGS);
				}
				log_trace("--group %d", LocalOptionConfig.groupId);
				break;
			}

			case 'm':
			{
				/* { "monitor", required_argument, NULL, 'm' } */
				if (!validate_connection_string(optarg))
				{
					log_fatal("Failed to parse --monitor connection string, "
							  "see above for details.");
					exit(EXIT_CODE_BAD_ARGS);
				}
				strlcpy(LocalOptionConfig.monitor_pguri, optarg, MAXCONNINFO);
				log_trace("--monitor %s", LocalOptionConfig.monitor_pguri);
				break;
			}

			case 'M':
			{
				/* { "disable-monitor", required_argument, NULL, 'M' }, */
				LocalOptionConfig.monitorDisabled = true;
				log_trace("--disable-monitor");
				break;
			}

			case 'R':
			{
				/* { "allow-removing-pgdata", no_argument, NULL, 'R' } */
				allowRemovingPgdata = true;
				log_trace("--allow-removing-pgdata");
				break;
			}

			case 'P':
			{
				/* { "candidate-priority", required_argument, NULL, 'P'} */
				int candidatePriority = strtol(optarg, NULL, 10);
				if (errno == EINVAL || candidatePriority < 0 || candidatePriority > 100)
				{
					log_fatal("--candidate-priority argument is not valid."
							  " Valid values are integers from 0 to 100. ");
					exit(EXIT_CODE_BAD_ARGS);
				}

				LocalOptionConfig.pgSetup.settings.candidatePriority = candidatePriority;
				log_trace("--candidate-priority %d", candidatePriority);
				break;
			}

			case 'r':
			{
				/* { "replication-quorum", required_argument, NULL, 'r'} */
				bool replicationQuorum = false;

				if (!parse_bool(optarg, &replicationQuorum))
				{
					log_fatal("--replication-quorum argument is not valid."
							  " Valid values are \"true\" or \"false\".");
					exit(EXIT_CODE_BAD_ARGS);
				}

				LocalOptionConfig.pgSetup.settings.replicationQuorum = replicationQuorum;
				log_trace("--replication-quorum %s", boolToString(replicationQuorum));
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

				LocalOptionConfig.pgSetup.ssl.active = 1;
				LocalOptionConfig.pgSetup.ssl.createSelfSignedCert = true;
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

				LocalOptionConfig.pgSetup.ssl.active = 0;
				LocalOptionConfig.pgSetup.ssl.createSelfSignedCert = false;
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
					LocalOptionConfig.pgSetup.ssl.active = 1;
				}

				if (!cli_getopt_ssl_flags(ssl_flag,
										  optarg,
										  &(LocalOptionConfig.pgSetup)))
				{
					errors++;
				}
				break;
			}

			default:
			{
				/* getopt_long already wrote an error message */
				errors++;
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
	 * Now, all commands need PGDATA validation.
	 */
	if (IS_EMPTY_STRING_BUFFER(LocalOptionConfig.pgSetup.pgdata))
	{
		get_env_pgdata_or_exit(LocalOptionConfig.pgSetup.pgdata);
	}

	/*
	 * We require the user to specify an authentication mechanism, or to use
	 * ---skip-pg-hba. Our documentation tutorial will use --auth trust, and we
	 * should make it obvious that this is not the right choice for production.
	 */
	if (IS_EMPTY_STRING_BUFFER(LocalOptionConfig.pgSetup.authMethod))
	{
		log_fatal("Please use either --auth trust|md5|... or --skip-pg-hba");
		log_info("pg_auto_failover can be set to edit Postgres HBA rules "
				 "automatically when needed. For quick testing '--auth trust' "
				 "makes it easy to get started, "
				 "consider another authentication mechanism for production.");
		exit(EXIT_CODE_BAD_ARGS);
	}

	/*
	 * If we have --ssl-self-signed, we don't want to have --ssl-ca-file and
	 * others in use anywhere. If we have --no-ssl, same thing. If we have the
	 * SSL files setup, we want to have neither --ssl-self-signed nor the other
	 * SSL files specified.
	 *
	 * We also need to either use the given sslMode or compute our default.
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

	if (!pgsetup_validate_ssl_settings(&(LocalOptionConfig.pgSetup)))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_ARGS);
	}

	/*
	 * You can't both have a monitor a use --disable-monitor.
	 */
	if (!IS_EMPTY_STRING_BUFFER(LocalOptionConfig.monitor_pguri) &&
		LocalOptionConfig.monitorDisabled)
	{
		log_fatal("Use either --monitor or --disable-monitor, not both.");
		exit(EXIT_CODE_BAD_ARGS);
	}
	else if (IS_EMPTY_STRING_BUFFER(LocalOptionConfig.monitor_pguri) &&
			 !LocalOptionConfig.monitorDisabled)
	{
		log_fatal("Failed to set the monitor URI: "
				  "use either --monitor postgresql://... or --disable-monitor");
		exit(EXIT_CODE_BAD_ARGS);
	}
	else if (LocalOptionConfig.monitorDisabled)
	{
		/*
		 * We must be able to restore this setup from the configuration file,
		 * and for that we set the pg_autoctl.monitor URI in the file to the
		 * "magic" value PG_AUTOCTL_DISABLED.
		 */
		strlcpy(LocalOptionConfig.monitor_pguri,
				PG_AUTOCTL_MONITOR_DISABLED,
				MAXCONNINFO);
	}

	/*
	 * We have a PGDATA setting, prepare our configuration pathnames from it.
	 */
	if (!keeper_config_set_pathnames_from_pgdata(
			&(LocalOptionConfig.pathnames), LocalOptionConfig.pgSetup.pgdata))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_ARGS);
	}

	/* publish our option parsing now */
	*options = LocalOptionConfig;

	return optind;
}


/*
 * cli_getopt_accept_ssl_options compute if we can accept the newSSLoption
 * (such as --no-ssl or --ssl-ca-file) given the previous one we have already
 * accepted.
 */
bool
cli_getopt_accept_ssl_options(SSLCommandLineOptions newSSLOption,
							  SSLCommandLineOptions currentSSLOptions)
{
	if (currentSSLOptions == SSL_CLI_UNKNOWN)
	{
		/* first SSL option being parsed */
		return true;
	}

	if (currentSSLOptions != newSSLOption)
	{
		if (currentSSLOptions == SSL_CLI_USER_PROVIDED ||
			newSSLOption == SSL_CLI_USER_PROVIDED)
		{
			log_error(
				"Using either --no-ssl or --ssl-self-signed "
				"with user-provided SSL certificates "
				"is not supported");
			return false;
		}

		/*
		 * At this point we know that currentSSLOptions and newSSLOption are
		 * different and none of them are SSL_CLI_USER_PROVIDED.
		 */
		log_error("Using both --no-ssl and --ssl-self-signed "
				  "is not supported");
		return false;
	}

	return true;
}


/*
 * cli_getopt_ssl_flags parses the SSL related options from the command line.
 *
 * { "ssl-ca-file", required_argument, &ssl_flag, SSL_CA_FILE_FLAG }
 * { "ssl-crl-file", required_argument, &ssl_flag, SSL_CRL_FILE_FLAG }
 * { "server-cert", required_argument, &ssl_flag, SSL_SERVER_CRT_FLAG }
 * { "server-key", required_argument, &ssl_flag, SSL_SERVER_KEY_FLAG }
 * { "ssl-mode", required_argument, &ssl_flag, SSL_MODE_FLAG },
 *
 * As those options are not using any short option (one-char) variant, they all
 * fall in the case 0, and we can process them thanks to the global variable
 * ssl_flag, an int.
 */
bool
cli_getopt_ssl_flags(int ssl_flag, char *optarg, PostgresSetup *pgSetup)
{
	switch (ssl_flag)
	{
		case SSL_CA_FILE_FLAG:
		{
			strlcpy(pgSetup->ssl.caFile, optarg, MAXPGPATH);
			log_trace("--ssl-ca-file %s", pgSetup->ssl.caFile);
			break;
		}

		case SSL_CRL_FILE_FLAG:
		{
			strlcpy(pgSetup->ssl.crlFile, optarg, MAXPGPATH);
			log_trace("--ssl-crl-file %s", pgSetup->ssl.crlFile);
			break;
		}

		case SSL_SERVER_CRT_FLAG:
		{
			strlcpy(pgSetup->ssl.serverCert, optarg, MAXPGPATH);
			log_trace("--server-cert %s", pgSetup->ssl.serverCert);
			break;
		}

		case SSL_SERVER_KEY_FLAG:
		{
			strlcpy(pgSetup->ssl.serverKey, optarg, MAXPGPATH);
			log_trace("--server-key %s", pgSetup->ssl.serverKey);
			break;
		}

		case SSL_MODE_FLAG:
		{
			strlcpy(pgSetup->ssl.sslModeStr, optarg, SSL_MODE_STRLEN);
			pgSetup->ssl.sslMode = pgsetup_parse_sslmode(optarg);

			log_trace("--ssl-mode %s",
					  pgsetup_sslmode_to_string(pgSetup->ssl.sslMode));

			if (pgSetup->ssl.sslMode == SSL_MODE_UNKNOWN)
			{
				log_fatal("Failed to parse ssl mode \"%s\"", optarg);
				return false;
			}
			break;
		}

		default:
		{
			log_fatal("BUG: unknown ssl flag value: %d", ssl_flag);
			return false;
		}
	}
	return true;
}


/*
 * keeper_cli_getopt_pgdata gets the PGDATA options or environment variable,
 * either of those must be set for all of pg_autoctl's commands. This parameter
 * allows to know which PostgreSQL instance we are the keeper of, and also
 * allows to determine where is our configuration file.
 */
int
cli_getopt_pgdata(int argc, char **argv)
{
	KeeperConfig options = { 0 };
	int c, option_index = 0, errors = 0;
	int verboseCount = 0;
	bool printVersion = false;

	static struct option long_options[] = {
		{ "pgdata", required_argument, NULL, 'D' },
		{ "json", no_argument, NULL, 'J' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	optind = 0;

	/*
	 * The only command lines that are using keeper_cli_getopt_pgdata are
	 * terminal ones: they don't accept subcommands. In that case our option
	 * parsing can happen in any order and we don't need getopt_long to behave
	 * in a POSIXLY_CORRECT way.
	 *
	 * The unsetenv() call allows getopt_long() to reorder arguments for us.
	 */
	unsetenv("POSIXLY_CORRECT");

	while ((c = getopt_long(argc, argv, "+D:JVvqh",
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

			case 'J':
			{
				outputJSON = true;
				log_trace("--json");
				break;
			}

			case 'V':
			{
				/* keeper_cli_print_version prints version and exits. */
				printVersion = true;
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
				errors++;
				break;
			}
		}
	}

	if (errors > 0)
	{
		commandline_help(stderr);
		exit(EXIT_CODE_BAD_ARGS);
	}

	if (printVersion)
	{
		keeper_cli_print_version(argc, argv);
	}

	/* now that we have the command line parameters, prepare the options */
	(void) prepare_keeper_options(&options);

	/* publish our option parsing in the global variable */
	keeperOptions = options;

	return optind;
}


/*
 * prepare_keeper_options finishes the preparation of the keeperOptions that
 * hosts the command line options.
 */
void
prepare_keeper_options(KeeperConfig *options)
{
	if (IS_EMPTY_STRING_BUFFER(options->pgSetup.pgdata))
	{
		get_env_pgdata_or_exit(options->pgSetup.pgdata);
	}

	log_debug("Managing PostgreSQL installation at \"%s\"",
			  options->pgSetup.pgdata);

	if (!keeper_config_set_pathnames_from_pgdata(&options->pathnames,
												 options->pgSetup.pgdata))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_ARGS);
	}

	/*
	 * The function keeper_cli_getopt_pgdata is only used by commands needing a
	 * configuration file to already exists:
	 *
	 * - `pg_autoctl do ...` are coded is a way that they don't need a
	 *    configuration file, instead using their own command line options
	 *    parser, so that test files specify the options on the command line,
	 *    making it easier to maintain,
	 *
	 * - `pg_autoctl config|create|run` are using this function
	 *   keeper_cli_getopt_pgdata and expect the configuration file to exists.
	 *
	 * A typo in PGDATA might be responsible for a failure that is hard to
	 * understand later, because of the way to derive the configuration
	 * filename from the PGDATA value. So we're going to go a little out of our
	 * way and be helpful to the user.
	 */
	if (!file_exists(options->pathnames.config))
	{
		log_fatal("Expected configuration file does not exists: \"%s\"",
				  options->pathnames.config);

		if (!directory_exists(options->pgSetup.pgdata))
		{
			log_warn("HINT: Check your PGDATA setting: \"%s\"",
					 options->pgSetup.pgdata);
		}

		exit(EXIT_CODE_BAD_ARGS);
	}
}


/*
 * set_first_pgctl sets the first pg_ctl found in PATH to given KeeperConfig.
 */
void
set_first_pgctl(PostgresSetup *pgSetup)
{
	char *version = NULL;
	if (!search_path_first("pg_ctl", pgSetup->pg_ctl))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_ARGS);
	}
	version = pg_ctl_version(pgSetup->pg_ctl);

	if (version == NULL)
	{
		/* errors have been logged in pg_ctl_version */
		exit(EXIT_CODE_PGCTL);
	}

	strlcpy(pgSetup->pg_version, version, PG_VERSION_STRING_MAX);

	free(version);
}


/*
 * monitor_init_from_pgsetup might be called either from a monitor or
 * a keeper node.
 *
 * First, see if we are on a keeper node with a configuration file for given
 * PGDATA. If that's the case, then we'll use the pg_autoctl.monitor_pguri
 * setting from there to contact the monitor.
 *
 * Then, if we failed to get the monitor's uri from a keeper's configuration
 * file, probe the given PGDATA to see if there's a running PostgreSQL instance
 * there, and if that's the case consider it's a monitor, and build its
 * connection string from discovered PostgreSQL parameters.
 */
bool
monitor_init_from_pgsetup(Monitor *monitor, PostgresSetup *pgSetup)
{
	ConfigFilePaths pathnames = { 0 };

	if (!keeper_config_set_pathnames_from_pgdata(&pathnames, pgSetup->pgdata))
	{
		/* errors have already been logged */
		return false;
	}

	switch (ProbeConfigurationFileRole(pathnames.config))
	{
		case PG_AUTOCTL_ROLE_MONITOR:
		{
			bool missingPgdataIsOk = false;
			bool pgIsNotRunningIsOk = false;
			char connInfo[MAXCONNINFO];
			MonitorConfig mconfig = { 0 };

			if (!monitor_config_init_from_pgsetup(&mconfig, pgSetup,
												  missingPgdataIsOk,
												  pgIsNotRunningIsOk))
			{
				/* errors have already been logged */
				return false;
			}

			pg_setup_get_local_connection_string(&mconfig.pgSetup, connInfo);
			monitor_init(monitor, connInfo);

			break;
		}

		case PG_AUTOCTL_ROLE_KEEPER:
		{
			KeeperConfig config = { 0 };
			Keeper keeper;
			bool missingPgdataIsOk = true;
			bool pgIsNotRunningIsOk = true;
			bool monitorDisabledIsOk = false;

			log_trace("monitor_init_from_pgsetup: keeper");

			/*
			 * the dereference of pgSetup is safe as it only contains literals,
			 * no pointers. keeper_config_read_file expects pgSetup to be set.
			 */
			config.pgSetup = *pgSetup;
			config.pathnames = pathnames;

			/*
			 * All we need here is a pg_autoctl.monitor URI to connect to. We
			 * don't need that the local PostgreSQL instance has been created
			 * already.
			 */
			if (!keeper_config_read_file(&config,
										 missingPgdataIsOk,
										 pgIsNotRunningIsOk,
										 monitorDisabledIsOk))
			{
				/* errors have already been logged */
				return false;
			}

			if (!monitor_init(&(keeper.monitor), config.monitor_pguri))
			{
				return false;
			}

			*monitor = keeper.monitor;
			break;
		}

		default:
		{
			log_fatal("Unrecognized configuration file \"%s\"", pathnames.config);
			return false;
		}
	}

	return true;
}


/*
 * exit_unless_role_is_keeper exits when the configured role for the local node
 * is not a pg_autoctl keeper, meaning either we fail to parse the
 * configuration file (maybe it doesn't exists), or we parse it correctly and
 * pg_autoctl.role is "monitor".
 */
void
exit_unless_role_is_keeper(KeeperConfig *kconfig)
{
	if (!keeper_config_set_pathnames_from_pgdata(&kconfig->pathnames,
												 kconfig->pgSetup.pgdata))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_CONFIG);
	}

	switch (ProbeConfigurationFileRole(kconfig->pathnames.config))
	{
		case PG_AUTOCTL_ROLE_MONITOR:
		{
			log_fatal("The command `%s` does not apply to a monitor node.",
					  current_command->breadcrumb);
			exit(EXIT_CODE_BAD_CONFIG);
		}

		case PG_AUTOCTL_ROLE_KEEPER:
		{
			/* pg_autoctl.role is as expected, we may continue */
			break;
		}

		default:
		{
			log_fatal("Unrecognized configuration file \"%s\"",
					  kconfig->pathnames.config);
			exit(EXIT_CODE_BAD_CONFIG);
		}
	}
}


/*
 * Provide help.
 */
void
keeper_cli_help(int argc, char **argv)
{
	CommandLine command = root;

	if (env_exists(PG_AUTOCTL_DEBUG))
	{
		command = root_with_debug;
	}

	(void) commandline_print_command_tree(&command, stdout);
}


/*
 * cli_print_version_getopts parses the CLI options for the pg_autoctl version
 * command, which are the usual suspects.
 */
int
cli_print_version_getopts(int argc, char **argv)
{
	int c, option_index = 0;

	static struct option long_options[] = {
		{ "json", no_argument, NULL, 'J' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	optind = 0;

	/*
	 * The only command lines that are using keeper_cli_getopt_pgdata are
	 * terminal ones: they don't accept subcommands. In that case our option
	 * parsing can happen in any order and we don't need getopt_long to behave
	 * in a POSIXLY_CORRECT way.
	 *
	 * The unsetenv() call allows getopt_long() to reorder arguments for us.
	 */
	unsetenv("POSIXLY_CORRECT");

	while ((c = getopt_long(argc, argv, "+JVvqh",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'J':
			{
				outputJSON = true;
				log_trace("--json");
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
				/*
				 * Ignore errors, ignore most of the things, just print the
				 * version and exit(0)
				 */
				break;
			}
		}
	}
	return optind;
}


/*
 * keeper_cli_print_version prints the pg_autoctl version and exits with
 * successful exit code of zero.
 */
void
keeper_cli_print_version(int argc, char **argv)
{
	if (outputJSON)
	{
		JSON_Value *js = json_value_init_object();
		JSON_Object *root = json_value_get_object(js);

		json_object_set_string(root, "pg_autoctl", PG_AUTOCTL_VERSION);
		json_object_set_string(root, "pg_major", PG_MAJORVERSION);
		json_object_set_string(root, "pg_version", PG_VERSION);
		json_object_set_string(root, "pg_version_str", PG_VERSION_STR);
		json_object_set_number(root, "pg_version_num", (double) PG_VERSION_NUM);

		(void) cli_pprint_json(js);
	}
	else
	{
		fformat(stdout, "pg_autoctl version %s\n", PG_AUTOCTL_VERSION);
		fformat(stdout, "compiled with %s\n", PG_VERSION_STR);
		fformat(stdout, "compatible with Postgres 10, 11, and 12\n");
	}

	exit(0);
}


/*
 * cli_pprint_json pretty prints the given JSON value to stdout and frees the
 * JSON related memory.
 */
void
cli_pprint_json(JSON_Value *js)
{
	char *serialized_string;

	/* output our nice JSON object, pretty printed please */
	serialized_string = json_serialize_to_string_pretty(js);
	fformat(stdout, "%s\n", serialized_string);

	/* free intermediate memory */
	json_free_serialized_string(serialized_string);
	json_value_free(js);
}


/*
 * cli_drop_local_node drops the local Postgres node.
 */
void
cli_drop_local_node(KeeperConfig *config, bool dropAndDestroy)
{
	Keeper keeper = { 0 };

	/*
	 * Now also stop the pg_autoctl process.
	 */
	if (file_exists(config->pathnames.pid))
	{
		pid_t pid = 0;

		if (read_pidfile(config->pathnames.pid, &pid))
		{
			log_info("An instance of pg_autoctl is running with PID %d, "
					 "stopping it.", pid);

			if (kill(pid, SIGQUIT) != 0)
			{
				log_error(
					"Failed to send SIGQUIT to the keeper's pid %d: %m", pid);
				exit(EXIT_CODE_INTERNAL_ERROR);
			}
		}
	}

	/* only keeper_remove when we still have a state file around */
	if (file_exists(config->pathnames.state))
	{
		bool ignoreMonitorErrors = true;

		/* keeper_remove uses log_info() to explain what's happening */
		if (!keeper_remove(&keeper, config, ignoreMonitorErrors))
		{
			log_fatal("Failed to remove local node from the pg_auto_failover "
					  "monitor, see above for details");

			exit(EXIT_CODE_BAD_STATE);
		}

		log_info("Removed pg_autoctl node at \"%s\" from the monitor and "
				 "removed the state file \"%s\"",
				 config->pgSetup.pgdata,
				 config->pathnames.state);
	}
	else
	{
		log_warn("Skipping node removal from the monitor: "
				 "state file \"%s\" does not exist",
				 config->pathnames.state);
	}

	/*
	 * Either --destroy the whole Postgres cluster and configuraiton, or leave
	 * enough behind us that it's possible to re-join a formation later.
	 */
	if (dropAndDestroy)
	{
		(void)
		stop_postgres_and_remove_pgdata_and_config(
			&config->pathnames,
			&config->pgSetup);
	}
	else
	{
		/*
		 * We need to stop Postgres now, otherwise we won't be able to drop the
		 * replication slot on the other node, because it's still active.
		 */
		log_info("Stopping PostgreSQL at \"%s\"", config->pgSetup.pgdata);

		if (!pg_ctl_stop(config->pgSetup.pg_ctl, config->pgSetup.pgdata))
		{
			log_error("Failed to stop PostgreSQL at \"%s\"",
					  config->pgSetup.pgdata);
			exit(EXIT_CODE_PGCTL);
		}

		/*
		 * Now give the whole picture to the user, who might have missed our
		 * --destroy option and might want to use it now to start again with a
		 * fresh environment.
		 */
		log_warn("Configuration file \"%s\" has been preserved",
				 config->pathnames.config);

		if (directory_exists(config->pgSetup.pgdata))
		{
			log_warn("Postgres Data Directory \"%s\" has been preserved",
					 config->pgSetup.pgdata);
		}

		log_info("drop node keeps your data and setup safe, you can still run "
				 "Postgres or re-join the pg_auto_failover cluster later");
		log_info("HINT: to completely remove your local Postgres instance and "
				 "setup, consider `pg_autoctl drop node --destroy`");
	}

	keeper_config_destroy(config);
}


/*
 * stop_postgres_and_remove_pgdata_and_config stops PostgreSQL and then removes
 * PGDATA, and then config and state files.
 */
static void
stop_postgres_and_remove_pgdata_and_config(ConfigFilePaths *pathnames,
										   PostgresSetup *pgSetup)
{
	log_info("Stopping PostgreSQL at \"%s\"", pgSetup->pgdata);

	if (!pg_ctl_stop(pgSetup->pg_ctl, pgSetup->pgdata))
	{
		log_error("Failed to stop PostgreSQL at \"%s\"", pgSetup->pgdata);
		log_fatal("Skipping removal of directory \"%s\"", pgSetup->pgdata);
		exit(EXIT_CODE_PGCTL);
	}

	/*
	 * Only try to rm -rf PGDATA if we managed to stop PostgreSQL.
	 */
	if (directory_exists(pgSetup->pgdata))
	{
		log_info("Removing \"%s\"", pgSetup->pgdata);

		if (!rmtree(pgSetup->pgdata, true))
		{
			log_error("Failed to remove directory \"%s\": %m", pgSetup->pgdata);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}
	}
	else
	{
		log_warn("Skipping removal of \"%s\": directory does not exists",
				 pgSetup->pgdata);
	}

	log_info("Removing \"%s\"", pathnames->config);

	if (!unlink_file(pathnames->config))
	{
		/* errors have already been logged. */
		exit(EXIT_CODE_BAD_CONFIG);
	}
}


/*
 * cli_pg_autoctl_reload signals the pg_autoctl process to reload its
 * configuration by sending it the SIGHUP signal.
 */
bool
cli_pg_autoctl_reload(const char *pidfile)
{
	pid_t pid;

	if (read_pidfile(pidfile, &pid))
	{
		if (pid <= 0)
		{
			log_error("Failed to reload pg_autoctl: "
					  "pid file \"%s\" contains negative-or-zero pid %d",
					  pidfile, pid);
			return false;
		}

		if (kill(pid, SIGHUP) != 0)
		{
			log_error("Failed to send SIGHUP to the pg_autoctl's pid %d: %m",
					  pid);
			return false;
		}
	}

	return true;
}
