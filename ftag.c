/*
 * ftag -- tag your files
 * Copyright 2014, 2015 Jacob Wahlgren
 * jacob.wahlgren@gmail.com
 * 
 */

/*
 This is a part of ftag.

 ftag is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/***--- Includes ---***/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <sqlite3.h>
#include "CuTest.h"
#include "ftag.h"

/***--- Constants and globals ---***/

#ifndef DB_FILENAME
#define DB_FILENAME ".ftagdb"
#endif

#define PROGRAM_NAME "ftag"

enum mode {
	MODE_NONE,
	MODE_TAG_FILE,
	MODE_FILTER,
	MODE_LIST
};

/* The prepcache is a linked list of prepared statements freed atexit by close_db */
struct prepcache_node {
	sqlite3_stmt *stmt;
	struct prepcache_node *next;
};

struct prepcache_node *prepcache = NULL;

static sqlite3 *dbconn = NULL;

int showhidden = 0;

/***--- Util ---***/

static void help(void)
{
	static const char *str = "Usage: " PROGRAM_NAME " [OPTIONS] MODE ARG...\n"
	"  " PROGRAM_NAME " [OPTIONS] file FILE TAG...\n"
	"  " PROGRAM_NAME " [OPTIONS] filter [TAG...]\n"
	"  " PROGRAM_NAME " [OPTIONS] list [FILE]\n"
	"\n"
	"Options:\n"
	"  -a, --show-hidden    show all files/tags, even those beginning with a .\n"
	"  -d, --database-name  specify database name\n"
	"  -p, --database-dir   force database directory\n"
	"  -v                   increase output verbosity (can be used multiple times)\n"
    "  -t, --test           run unit tests and exit\n"
	"  --help               show this help\n"
	"\n"
	"Report bugs to jacob.wahlgren@gmail.com.\n"
	"This software is licensed under the GNU General public license.\n"
	"Copyright 2014, 2015 Jacob Wahlgren.\n";
	fputs(str, stderr);
}

static void usage(void)
{
	static char *str = "Usage: " PROGRAM_NAME " [-dpvh] MODE ARG...\n"
	"Use '" PROGRAM_NAME " --help' for more info\n";

	fputs(str, stderr);
}

/* Ascend to the first directory containing a file fn, or stay at the current dir */
static int chdir_to_db(const char *fn)
{
	// getcwd returns null if path is too long, the only one char path is /
	char buf[2];
	int startdir = open(".", O_RDONLY);

	int fchdir(int);

	while (1) {
		if (access(fn, R_OK) == 0)
			break;

		if (getcwd(buf, 2) != NULL) {
			fchdir(startdir);
			close(startdir);
			
			return ERROR;
		}

		chdir("..");
	}
	close(startdir);

	return SUCCESS;
}

/***--- SQLite wrappers and helpers ---***/

static int prepcache_add(sqlite3_stmt **stmt)
{
    struct prepcache_node *node = malloc(sizeof(*node));

    if (node == NULL || stmt == NULL)
        return ERROR;

    node->next = NULL;
    node->stmt = *stmt;

    if (prepcache != NULL) {
        struct prepcache_node *walk = prepcache;

        while (walk->next != NULL)
            walk = walk->next;

        walk->next = node;
    } else
        prepcache = node;

    return SUCCESS;
}

static void prepcache_free(void)
{
    while (prepcache != NULL) {
        struct prepcache_node *tmp = prepcache->next;

        sqlite3_finalize(prepcache->stmt);
        prepcache->stmt = NULL;
        free(prepcache);
        prepcache = tmp;
    }
}

/* If the stmt is null, prepare it with str, otherwise call reset on it. The stmt is freed atexit. */
static int prepare_or_reset(sqlite3_stmt **prep, const char *str)
{
	if (*prep == NULL) {
		int status = sqlite3_prepare_v2(dbconn, str, -1, prep, NULL);

		if (status != SQLITE_OK) {
			fprintf(stderr, PROGRAM_NAME ": error: failed preparation of SQL"
                    "statement (%d)\n\"%s\"\n", status, str);

			exit(1);
		} else {
            prepcache_add(prep);

			assert(*prep != NULL);

			return SUCCESS;
		}
	} else {
		sqlite3_reset(*prep);

		return SUCCESS;
	}
}

/* Prepare a statement, must be finalized. */
static int prepare(sqlite3_stmt **prep, const char *str)
{
	int status = sqlite3_prepare_v2(dbconn, str, -1, prep, NULL);

	if (status != SQLITE_OK) {
		fprintf(stderr, "error: failed preparation of SQL statement (%d)\n", status);

		exit(1);
	} else {
		return SUCCESS;
	}
}

int tag_file(const char *file, const char *tag)
{
	static const char *sql_str = "INSERT OR IGNORE INTO Tag VALUES (?, ?);";
	static sqlite3_stmt *sql_prep = NULL;

	// Only one statement in the sql, only one call to step
	assert(strchr(sql_str, ';') == strrchr(sql_str, ';'));

	prepare_or_reset(&sql_prep, sql_str);

	if (sqlite3_bind_text(sql_prep, 1, file, -1, SQLITE_STATIC) != SQLITE_OK ||
		sqlite3_bind_text(sql_prep, 2, tag, -1, SQLITE_STATIC) != SQLITE_OK)
		return ERROR;

	if (sqlite3_step(sql_prep) != SQLITE_DONE)
		return ERROR;

	return SUCCESS;
}

const char *step_result(step_t *stmt)
{
	int status;

	top:
	status = sqlite3_step(stmt);

	if (status == SQLITE_ROW) {
		const char *str = (char *) sqlite3_column_text(stmt, 0);

		if (!showhidden && *str == '.')
			goto top;

		return str;
	} else if (status == SQLITE_DONE)
		return NULL;
	else {
		fprintf(stderr, PROGRAM_NAME ": error stepping result\n");
		exit(ERROR);
	}
}

void free_step(step_t *stmt)
{
#ifndef NDEBUG
	int status =
#endif
	sqlite3_finalize(stmt);

	assert(status == SQLITE_OK);
}

step_t *filter_by_tag(const char *tag)
{
	static const char *sql_str = "SELECT DISTINCT file FROM Tag WHERE tag=? ORDER BY file;";
	static const char *sql_str_all = "SELECT DISTINCT file FROM Tag ORDER BY file;";
	sqlite3_stmt *sql_prep = NULL;

	// Only one statement in the sql, only one call to step
	assert(strchr(sql_str, ';') == strrchr(sql_str, ';'));

	if (tag == NULL)
		prepare(&sql_prep, sql_str_all);
	else {
		prepare(&sql_prep, sql_str);

		if (sqlite3_bind_text(sql_prep, 1, tag, -1, SQLITE_STATIC) != SQLITE_OK)
			return NULL;
	}

	return (step_t *) sql_prep;
}

step_t *filter_by_tags(int tagc, char **tagv)
{
	static const char *sql_base_str = "SELECT DISTINCT file FROM Tag WHERE tag IN (?";
	static const char *sql_end_str = ") ORDER BY file DESC;";
	sqlite3_stmt *prep = NULL;
	size_t sql_len = 0;
	char *sql_str = NULL;

	assert(tagc > 0);

	sql_len = strlen(sql_base_str) + strlen(sql_end_str) + 1;
	sql_str = malloc(sql_len);

	if (sql_str == NULL)
		goto mem_err;

	strcpy(sql_str, sql_base_str);

	for (int i = 1; i < tagc; i++) {
		sql_str = realloc(sql_str, sql_len + 2);
		sql_len += 2;

		if (sql_str == NULL)
			goto mem_err;

		strcat(sql_str, ",?");
	}

	strcat(sql_str, sql_end_str);
	prepare(&prep, sql_str);

	if (sql_str != NULL)
		free(sql_str);

	for (int i = 0; i < tagc; i++)
		if (sqlite3_bind_text(prep, i + 1, tagv[i], -1, SQLITE_TRANSIENT) != SQLITE_OK) {
			fprintf(stderr, PROGRAM_NAME ": error binding SQLITE values\n");
			exit(ERROR);
		}

	return (step_t *) prep;

	mem_err:
	fprintf(stderr, PROGRAM_NAME ": failed memory allocation, exiting\n");
	exit(ERROR);
}

step_t *list_by_file(const char *file)
{
	static const char *sql_str = "SELECT DISTINCT tag FROM Tag WHERE file=? ORDER BY tag;";
	static const char *sql_str_all = "SELECT DISTINCT tag FROM Tag ORDER BY tag;";
	sqlite3_stmt *sql_prep = NULL;

	// Only one statement in the sql, only one call to step
	assert(strchr(sql_str, ';') == strrchr(sql_str, ';'));

	if (file == NULL)
		prepare(&sql_prep, sql_str_all);
	else {
		prepare(&sql_prep, sql_str);

		if (sqlite3_bind_text(sql_prep, 1, file, -1, SQLITE_STATIC) != SQLITE_OK)
			return NULL;
	}

	return (step_t *) sql_prep;
}

static void close_db(void)
{
    prepcache_free();

	if (dbconn != NULL) {
		sqlite3_close(dbconn);
		dbconn = NULL;
	}
}

/* Open and init database, or search for DB_FILENAME if (fn == NULL) and with chdir_to_db if (dir == NULL)
 * The database is freed atexit
 */
int init_db(char *fn, char *dir)
{
	static char *init_sql = "CREATE TABLE IF NOT EXISTS Tag (file varchar(256) NOT NULL, tag varchar(256) NOT NULL);"
	"CREATE UNIQUE INDEX IF NOT EXISTS uq_Tag on Tag (file, tag);";

	if (dbconn != NULL)
		return ERROR;

	if (fn == NULL)
		fn = DB_FILENAME;

	if (dir == NULL)
		chdir_to_db(fn);
	else
		if (chdir(dir) != 0) {
			fprintf(stderr, PROGRAM_NAME ": failed to change to dir '%s'\n", dir);
			exit(ERROR);
		}

	if (sqlite3_open(fn, &dbconn) != SQLITE_OK)
		return ERROR;

	atexit(close_db);

	if (sqlite3_exec(dbconn, init_sql, NULL, NULL, NULL) != SQLITE_OK)
		return ERROR;
	else
		return SUCCESS;
}

/***--- Entry points ---***/

static int main_tag_file(int argc, char **argv)
{
	assert(argv != NULL);

	if (argc < 2) {
		usage();
		return ERROR;
	}

	for (int i = 1; i < argc; i++)
		if (tag_file(argv[0], argv[i]) == ERROR) {
			fprintf(stderr, PROGRAM_NAME ": error tagging file\n");

			return ERROR;
		}

	return SUCCESS;
}

static int main_filter(int argc, char **argv)
{
	step_t *step = NULL;

	assert(argv != NULL);

	if (argc == 0) {
		if ((step = filter_by_tag(NULL)) == NULL) {
			fprintf(stderr, PROGRAM_NAME ": error while filtering tag\n");
			return ERROR;
		}
	} else if (argc == 1) {
		if ((step = filter_by_tag(argv[0])) == NULL) {
			fprintf(stderr, PROGRAM_NAME ": error while filtering tag \n");
			return ERROR;
		}
	} else {
		if ((step = filter_by_tags(argc, argv)) == NULL) {
			fprintf(stderr, PROGRAM_NAME ": error while filtering tag\n");
			return ERROR;
		}
	}

	if (step != NULL) {
		const char *str = NULL;

		while ((str = step_result(step)) != NULL)
			puts(str);
	}

	free_step(step);

	return SUCCESS;
}

static int main_list(int argc, char **argv)
{
	step_t *step = NULL;

	assert(argv != NULL);

	if (argc == 0)
		step = list_by_file(NULL);
	else if (argc == 1)
		step = list_by_file(argv[0]);
	else {
		usage();
		return ERROR;
	}

	if (step == NULL) {
		fprintf(stderr, PROGRAM_NAME ": error while listing tags\n");
		return ERROR;
	} else {
		const char *str = NULL;
		while((str = step_result(step)) != NULL)
			puts(str);
	}

	free_step(step);

	return SUCCESS;
}

// Forward declartion to make it run in main
static int run_tests(void);

int main(int argc, char **argv)
{
	/* Look at git argument parsing for ideas on handling modes etc */
	int chr = 0;
	enum mode mode = MODE_NONE;
	char *dbfilename = NULL;
	int verbosity = 0;
	char *dbpath = NULL;

	static struct option longopts[] = {
		{"show-hidden", no_argument, 0, 'a'},
		{"database-name", required_argument, 0, 'd'},
		{"database-dir", required_argument, 0, 'p'},
		{"verbose", no_argument, 0, 'v'},
		{"help", no_argument, 0, 'h'},
        {"test", no_argument, 0, 't'},
		{0, 0, 0, 0}
	};

	opterr = 0;
	while ((chr = getopt_long(argc, argv, "ad:p:vt", longopts, NULL)) != -1) {
		switch (chr) {
			case 'a':
				showhidden = 1;
				break;
			case 'd':
				dbfilename = optarg;
				break;
			case 'v':
				verbosity++;
				break;
			case 'h':
				help();
				return 0;
			case 'p':
				dbpath = optarg;
				break;
            case 't':
               return run_tests();
			default:
				usage();
				return ERROR;
		}
	}

	if (argc - optind < 1) {
		usage();
		return ERROR;
	}

	if (strcmp(argv[optind], "file") == 0)
		mode = MODE_TAG_FILE;
	else if (strcmp(argv[optind], "filter") == 0)
		mode = MODE_FILTER;
	else if (strcmp(argv[optind], "list") == 0)
		mode = MODE_LIST;
	else {
		usage();
		return ERROR;
	}

	optind++;

	if (dbfilename == NULL) {
		static char *filename_static = DB_FILENAME;
		dbfilename = filename_static;
	}

	if (init_db(dbfilename, dbpath) != 0) {
		fprintf(stderr, PROGRAM_NAME ": error: failed to initialize database\n");
		return ERROR;
	}

	if (verbosity > 0) {
		char *cdir = getcwd(NULL, 0);
		
		if (cdir != NULL) {
			fprintf(stderr, "choosing db '%s/%s'\n", cdir, dbfilename);
			free(cdir);
		} else {
			close_db();

			return ERROR;
		}
	}

	if (mode != MODE_NONE) {
		int margc = argc - optind;
		char **margv = argv + optind;
	
		switch (mode) {
			case MODE_TAG_FILE:
				return main_tag_file(margc, margv);
			case MODE_FILTER:
				return main_filter(margc, margv);
			case MODE_LIST:
				return main_list(margc, margv);
			default:
				assert(0);
				return ERROR;
		}
	} else
		return ERROR;
}

/***--- Tests ---***/

static void test_chdir_to_db_null_return(CuTest *tc)
{
    CuAssertIntEquals(tc, ERROR, chdir_to_db(NULL));
}

static void test_chdir_to_db_null_cwd(CuTest *tc)
{
    char *beforedir = getcwd(NULL, 0);
    char *afterdir = NULL;

    chdir_to_db(NULL);
    afterdir = getcwd(NULL, 0);
    CuAssertStrEquals(tc, beforedir, afterdir);
    free(beforedir);
    free(afterdir);
}

static CuSuite *chdir_to_db_get_suite()
{
    CuSuite *suite = CuSuiteNew();

    SUITE_ADD_TEST(suite, test_chdir_to_db_null_return);
    SUITE_ADD_TEST(suite, test_chdir_to_db_null_cwd);
 
    return suite;
}

static void test_prepcache_add_null_stmt_empty(CuTest *tc)
{
    // empty prepcache
    CuAssertIntEquals(tc, ERROR, prepcache_add(NULL));
}

static void test_prepcache_add_null_stmt(CuTest *tc)
{
    // non empty prepcache
    prepcache = malloc(sizeof(*prepcache));

    if (prepcache == NULL) {
        fprintf(stderr, PROGRAM_NAME ": failed malloc call\n");
        exit(1);
    }

    prepcache->stmt = NULL;
    prepcache->next = NULL;

    CuAssertIntEquals(tc, ERROR, prepcache_add(NULL));

    free(prepcache);
    prepcache = NULL;
}

static CuSuite *prepcache_add_get_suite()
{
    CuSuite *suite = CuSuiteNew();

    SUITE_ADD_TEST(suite, test_prepcache_add_null_stmt_empty);
    SUITE_ADD_TEST(suite, test_prepcache_add_null_stmt);

    return suite;
}

static void test_prepcache_free_when_null(CuTest *tc)
{
    assert(prepcache == NULL);
    prepcache_free();
    CuAssertPtrEquals(tc, NULL, prepcache);
}

static void test_prepcache_free_one_node(CuTest *tc)
{
    sqlite3 *db;
    const char *str = "CREATE TABLE IF NOT EXISTS Tag (file varchar(256) NOT NULL, tag varchar(256) NOT NULL);";

    CuAssertIntEquals(tc, SQLITE_OK, sqlite3_open(":memory:", &db));
    prepcache = malloc(sizeof(*prepcache));

    sqlite3_prepare_v2(db, str, -1, &prepcache->stmt, NULL);
    prepcache->next = NULL;

    prepcache_free();

    CuAssertPtrEquals(tc, NULL, prepcache);

    sqlite3_close(db);
}

static void test_prepcache_free_multiple(CuTest *tc)
{
    sqlite3 *db;

    CuAssertIntEquals(tc, SQLITE_OK, sqlite3_open(":memory:", &db));

    prepcache = malloc(sizeof(*prepcache));
    sqlite3_prepare_v2(db, "CREATE TABLE IF NOT EXISTS Tag (file varchar(256) NOT NULL, tag varchar(256) NOT NULL);", -1, &prepcache->stmt, NULL);
    if (prepcache == NULL) goto mem_err;

    prepcache->next = malloc(sizeof(*prepcache));
    sqlite3_prepare_v2(db, "CREATE UNIQUE INDEX IF NOT EXISTS uq_Tag on Tag (file, tag);", -1, &prepcache->next->stmt, NULL);
    if (prepcache->next == NULL) goto mem_err;

    prepcache->next->next = malloc(sizeof(*prepcache));
    sqlite3_prepare_v2(db, "SELECT DISTINCT tag FROM Tag ORDER BY tag;", -1,
        &prepcache->next->next->stmt, NULL);
    if (prepcache->next->next == NULL) goto mem_err;
    prepcache->next->next->next = NULL;

    prepcache_free();

    CuAssertPtrEquals(tc, NULL, prepcache);

    sqlite3_close(db);

    return;

mem_err:
    fprintf(stderr, PROGRAM_NAME ": failed malloc call\n");
    exit(1);
}

static CuSuite *prepcache_free_get_suite()
{
    CuSuite *suite = CuSuiteNew();

    SUITE_ADD_TEST(suite, test_prepcache_free_when_null);
    SUITE_ADD_TEST(suite, test_prepcache_free_one_node);
    SUITE_ADD_TEST(suite, test_prepcache_free_multiple);

    return suite;
}

static int run_tests(void)
{
    CuString *output = CuStringNew();
    CuSuite *suite = CuSuiteNew();

    CuSuiteAddSuite(suite, chdir_to_db_get_suite());
    CuSuiteAddSuite(suite, prepcache_add_get_suite());
    CuSuiteAddSuite(suite, prepcache_free_get_suite());
   
    CuSuiteRun(suite);
    CuSuiteSummary(suite, output);
    CuSuiteDetails(suite, output);
    fprintf(stderr, "%s\n", output->buffer);
    
    return SUCCESS;
}
