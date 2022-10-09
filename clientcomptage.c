/*
 * clientcomptage, a PostgreSQL fun and quick tool.
 *
 * This software is released under the PostgreSQL Licence.
 *
 * Guillaume Lelarge, guillaume@lelarge.info, 2022.
 */


/*
 * Headers
 */
#include "postgres_fe.h"
#include "common/string.h"

#include <err.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/stat.h>

#include <unistd.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "postgres_fe.h"
#include "common/username.h"
#include "common/logging.h"
#include "fe_utils/cancel.h"
#include "fe_utils/connect_utils.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/query_utils.h"
#include "fe_utils/simple_list.h"
#include "fe_utils/string_utils.h"

#include "fe_utils/print.h"
#include "libpq-fe.h"
#include "libpq/pqsignal.h"


/*
 * Defines
 */
#define CLIENTCOMPTAGE_VERSION "0.0.1"
#define CLIENTCOMPTAGE_DEFAULT_LINES 20
#define CLIENTCOMPTAGE_DEFAULT_STRING_SIZE 2048


/*
 * Enums and structs
 */

typedef enum
{
  NONE = 0,
  AJOUT,
  JOURS,
  MOIS,
  SEMAINES
} actions_t;

/* these are the options structure for command line parameters */
struct options
{
  /* misc */
  char      *script;
  bool      verbose;
  actions_t action;
  char      *heures;

  /* connection parameters */
  char      *dsn;

  /* version number */
  int       major;
  int       minor;
};


/*
 * Global variables
 */
PGconn         *conn;
struct options *opts;
extern char    *optarg;


/*
 * Function prototypes
 */
static void help(const char *progname);
void        get_opts(int, char **);
#ifndef FE_MEMUTILS_H
void        *pg_malloc(size_t size);
char        *pg_strdup(const char *in);
#endif
void        fetch_table(char *label, char *query);
bool        backend_minimum_version(int major, int minor);
void        execute(char *query);
void        exec_command(char *cmd);
static void quit_properly(SIGNAL_ARGS);


/*
 * Print help message
 */
static void
help(const char *progname)
{
  printf("%s does some stuff :)\n\n"
       "Usage:\n"
       "  %s [OPTIONS]\n"
       "\nGeneral options:\n"
       "  -a            ajout d'heures réalisées\n"
       "  -j|--jour     décompte par jour\n"
       "  -m|--mois     décompte par mois\n"
       "  -s|--semaines décompte par semaine\n"
       "  -v            verbose\n"
       "  -?|--help     show this help, then exit\n"
       "  -V|--version  output version information, then exit\n"
       "\n"
       "Report bugs to <guillaume@lelarge.info>.\n",
       progname, progname);
}


/*
 * Parse command line options and check for some usage errors
 */
void
get_opts(int argc, char **argv)
{
  int        c;
  const char *progname;

  progname = get_progname(argv[0]);

  /* set the defaults */
  opts->script = NULL;
  opts->verbose = false;

  /* we should deal quickly with help and version */
  if (argc > 1)
  {
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
    {
      help(progname);
      exit(0);
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
    {
      puts("pgreport " CLIENTCOMPTAGE_VERSION " (compiled with PostgreSQL " PG_VERSION ")");
      exit(0);
    }
  }

  /* get options */
  while ((c = getopt(argc, argv, "a:jmsv")) != -1)
  {
    switch (c)
    {
      case 'a':
        opts->action = AJOUT;
        opts->heures = pg_strdup(optarg);
        break;
      case 'j':
        opts->action = JOURS;
        break;
      case 'm':
        opts->action = MOIS;
        break;
      case 's':
        opts->action = SEMAINES;
        break;
      default:
        pg_log_error("Try \"%s --help\" for more information.\n", progname);
        exit(EXIT_FAILURE);
    }
  }
}


#ifndef FE_MEMUTILS_H
/*
 * "Safe" wrapper around malloc().
 */
void *
pg_malloc(size_t size)
{
  void *tmp;

  /* Avoid unportable behavior of malloc(0) */
  if (size == 0)
    size = 1;
  tmp = malloc(size);
  if (!tmp)
  {
    pg_log_error("out of memory (pg_malloc)\n");
    exit(EXIT_FAILURE);
  }
  return tmp;
}


/*
 * "Safe" wrapper around strdup().
 */
char *
pg_strdup(const char *in)
{
  char *tmp;

  if (!in)
  {
    pg_log_error("cannot duplicate null pointer (internal error)\n");
    exit(EXIT_FAILURE);
  }
  tmp = strdup(in);
  if (!tmp)
  {
    pg_log_error("out of memory (pg_strdup)\n");
    exit(EXIT_FAILURE);
  }
  return tmp;
}
#endif


/*
 * Compare given major and minor numbers to the one of the connected server
 */
bool
backend_minimum_version(int major, int minor)
{
  return opts->major > major || (opts->major == major && opts->minor >= minor);
}


/*
 * Execute query
 */
void
execute(char *query)
{
  PGresult *results;

  if (opts->script)
  {
    printf("%s;\n", query);
  }
  else
  {
    /* make the call */
    results = PQexec(conn, query);

    /* check and deal with errors */
    if (!results)
    {
      pg_log_error("query failed: %s", PQerrorMessage(conn));
      pg_log_info("query was: %s", query);
      PQclear(results);
      PQfinish(conn);
      exit(EXIT_FAILURE);
    }

    /* cleanup */
    PQclear(results);
  }
}


/*
 * Handle query
 */
void
fetch_table(char *label, char *query)
{
  PGresult      *res;
  printQueryOpt myopt;

  if (opts->script)
  {
    printf("\\echo %s\n",label);
    printf("%s;\n",query);
  }
  else
  {
    myopt.nullPrint = NULL;
    myopt.title = pstrdup(label);
    myopt.translate_header = false;
    myopt.n_translate_columns = 0;
    myopt.translate_columns = NULL;
    myopt.footers = NULL;
    myopt.topt.format = PRINT_ALIGNED;
    myopt.topt.expanded = 0;
    myopt.topt.border = 2;
    myopt.topt.pager = 0;
    myopt.topt.tuples_only = false;
    myopt.topt.start_table = true;
    myopt.topt.stop_table = true;
    myopt.topt.default_footer = false;
    myopt.topt.line_style = NULL;
    //myopt.topt.fieldSep = NULL;
    //myopt.topt.recordSep = NULL;
    myopt.topt.numericLocale = false;
    myopt.topt.tableAttr = NULL;
    myopt.topt.encoding = PQenv2encoding();
    myopt.topt.env_columns = 0;
    //myopt.topt.columns = 3;
    myopt.topt.unicode_border_linestyle = UNICODE_LINESTYLE_SINGLE;
    myopt.topt.unicode_column_linestyle = UNICODE_LINESTYLE_SINGLE;
    myopt.topt.unicode_header_linestyle = UNICODE_LINESTYLE_SINGLE;

    /* execute it */
    res = PQexec(conn, query);

    /* check and deal with errors */
    if (!res || PQresultStatus(res) > 2)
    {
      pg_log_error("query failed: %s", PQerrorMessage(conn));
      pg_log_info("query was: %s", query);
      PQclear(res);
      PQfinish(conn);
      exit(EXIT_FAILURE);
    }

    /* print results */
    printQuery(res, &myopt, stdout, false, NULL);

    /* cleanup */
    PQclear(res);
  }
}


/*
 * Close the PostgreSQL connection, and quit
 */
static void
quit_properly(SIGNAL_ARGS)
{
  PQfinish(conn);
  exit(EXIT_FAILURE);
}


/*
 * Main function
 */
int
main(int argc, char **argv)
{
  const char *progname;
  ConnParams cparams;
  char       sql[CLIENTCOMPTAGE_DEFAULT_STRING_SIZE];

  /*
   * If the user stops the program,
   * quit nicely.
   */
  pqsignal(SIGINT, quit_properly);

  /* Initialize the logging interface */
  pg_logging_init(argv[0]);

  /* Get the program name */
  progname = get_progname(argv[0]);

  /* Allocate the options struct */
  opts = (struct options *) pg_malloc(sizeof(struct options));

  /* Parse the options */
  get_opts(argc, argv);

  /* Set the connection struct */
  cparams.pghost = "localhost";
  cparams.pgport = "5414";
  cparams.dbname = "dalibo";
  cparams.pguser = "postgres";
  cparams.prompt_password = TRI_DEFAULT;
  cparams.override_dbname = NULL;

  /* Connect to the database */
  conn = connectDatabase(&cparams, progname, false, false, false);

  switch (opts->action)
  {
    case AJOUT:
      snprintf(sql, sizeof(sql),
        "INSERT INTO public.comptage (deb,fin) VALUES (%s)", opts->heures);
      execute(sql);
      break;
    case JOURS:
      fetch_table("Jours", "SELECT * FROM public.jours_v");
      break;
    case MOIS:
      fetch_table("Mois", "SELECT * FROM public.mois");
      break;
    case SEMAINES:
      fetch_table("Semaines", "SELECT * FROM public.semaines");
      break;
    default:
      pg_log_error("No action defined");
  }

  PQfinish(conn);

  pg_free(opts);

  return 0;
}
