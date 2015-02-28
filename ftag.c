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

#define FILTER_ANY_TAG  (1<<0)
#define FILTER_ALL_TAGS (1<<1)
#define FILTER_ALL      (1<<2)

enum mode {
	MODE_NONE,
	MODE_TAG_FILE,
	MODE_FILTER,
	MODE_LIST
};

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
    int startdir;

    if (fn == NULL)
        return ERROR;

    startdir = open(".", O_RDONLY);

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

int tag_file(const char *file, const char *tag)
{
	static const char *sql_str =
    "BEGIN;"
    "INSERT OR IGNORE INTO tag (name) VALUES (:tag);"
    "INSERT OR IGNORE INTO file (relative_path) VALUES (:file);"
    "INSERT INTO file_tag (file_id, tag_id) SELECT file.id, tag.id FROM "
    "file, tag WHERE file.relative_path = :file AND tag.name = :tag;"
    "COMMIT;"
    ;

	sqlite3_stmt *sql_prep = NULL;
    const char *sql_unread = sql_str;

    if (file == NULL || tag == NULL)
        return ERROR;

    // Prepare, bind and execute one statement at a time
    while (sql_unread < sql_str + strlen(sql_str)) {
        int status = sqlite3_prepare_v2(dbconn, sql_unread, -1, &sql_prep, &sql_unread);
        if (status != SQLITE_OK)
            return ERROR;

        // Is 0 if parameter doesn't exist in this statement
        int file_index = sqlite3_bind_parameter_index(sql_prep, ":file");
        int tag_index = sqlite3_bind_parameter_index(sql_prep, ":tag");

        if (file_index > 0)
            if (sqlite3_bind_text(sql_prep, file_index, file, -1, SQLITE_STATIC)
                != SQLITE_OK)
                return ERROR;

        if (tag_index > 0)
            if (sqlite3_bind_text(sql_prep, tag_index, tag, -1, SQLITE_STATIC)
                != SQLITE_OK)
                return ERROR;

        if (sqlite3_step(sql_prep) != SQLITE_DONE)
            return ERROR;

        sqlite3_finalize(sql_prep);
        sql_prep = NULL;
    }

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

// Get id of all tags. If tag doesn't exist give value -1 (doesn't return
// any rows)
int *get_tag_ids(int tagc, const char **tagv)
{
	static const char *sql = "SELECT id FROM tag WHERE name=?;";
	int *buf;

	if (tagv == NULL)
		return NULL;

	buf = malloc(sizeof(int) * tagc);
	if (buf == NULL)
		return NULL;

	for (int i = 0; i < tagc; i++) {
		sqlite3_stmt *prep;

		sqlite3_prepare_v2(dbconn, sql, -1, &prep, NULL);

		if (prep == NULL)
			return NULL;

		if (sqlite3_bind_text(prep, 1, tagv[i], -1, SQLITE_STATIC) != SQLITE_OK)
			goto error;

		switch (sqlite3_step(prep)) {
			case SQLITE_DONE:
				buf[i] = -1;
				continue;
			case SQLITE_ROW:
				break;
			default:
				goto error;
		}

		int count = sqlite3_column_count(prep);

		if (count == 0) {
			goto error;
		} if (sqlite3_column_count(prep) > 1) {
			fprintf(stderr, PROGRAM_NAME ": database corrupted\n");
			goto error;
		} else {
			buf[i] = sqlite3_column_int(prep, 0);
		}

		continue;

		error:
		if (prep != NULL)
			sqlite3_finalize(prep);
		if (buf != NULL)
			free(buf);
		return NULL;
	}

	return buf;
}

step_t *filter_ids_any_tag(int tagc, int *tagv)
{
	static const char *sql_base =
	"SELECT DISTINCT f.relative_path FROM file AS f, file_tag AS x "
	"WHERE f.id = x.file_id AND x.tag_id = ?";
	char *sql_union = NULL;
	sqlite3_stmt *prep = NULL;

	sql_union = malloc(strlen(sql_base) * tagc +
					   strlen(" UNION ") * (tagc - 1) + 1 + 1);
	if (sql_union == NULL)
		return NULL;

	strcpy(sql_union, sql_base);
	for (int i = 1; i < tagc; i++) {
		strcat(sql_union, " UNION ");
		strcat(sql_union, sql_base);
	}
	strcat(sql_union, ";");


	if (sqlite3_prepare_v2(dbconn, sql_union, -1, &prep, NULL) != SQLITE_OK)
		prep = NULL;

	for (int i = 0; i < tagc; i++) {
		if (sqlite3_bind_int(prep, i+1, tagv[i]) != SQLITE_OK) {
			sqlite3_finalize(prep);
			prep = NULL;
			break;
		}
	}

	free(sql_union);

	return prep;
}

step_t *filter_all(void)
{
	static const char *sql = "SELECT DISTINCT relative_path FROM file;";
	sqlite3_stmt *prep = NULL;

	if (sqlite3_prepare_v2(dbconn, sql, -1, &prep, NULL) != SQLITE_OK)
		return NULL;
	else
		return prep;
}

step_t *filter_strs(int tagc, const char **tagv, int flags)
{
	step_t *step = NULL;

	if (flags == 0)
		return NULL;

	if (flags & FILTER_ALL) {
		step = filter_all();
	} else {
		int *ids = get_tag_ids(tagc, tagv);
		if (ids == NULL)
			return NULL;

		if (flags & FILTER_ANY_TAG)
			step = filter_ids_any_tag(tagc, ids);

		free(ids);
	}

	return step;
}

step_t *list_by_file(const char *path)
{
	static const char *sql = "SELECT DISTINCT t.name FROM tag AS t, file AS f, "
	"file_tag AS x WHERE t.id = x.tag_id AND x.file_id = f.id AND "
	"f.relative_path = ?;";
	sqlite3_stmt *prep = NULL;

	if (sqlite3_prepare_v2(dbconn, sql, -1, &prep, NULL) != SQLITE_OK)
		return NULL;

	if (sqlite3_bind_text(prep, 1, path, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
		sqlite3_finalize(prep);
		prep = NULL;
	}

	return prep;
}

step_t *list_all_tags(void)
{
	static const char *sql = "SELECT DISTINCT name FROM tag;";
	sqlite3_stmt *prep = NULL;

	if (sqlite3_prepare_v2(dbconn, sql, -1, &prep, NULL) != SQLITE_OK)
		return NULL;

	return prep;
}

static void close_db(void)
{
	if (dbconn != NULL) {
		sqlite3_close(dbconn);
		dbconn = NULL;
	}
}

static int run_init_db_sql() {
    static char *init_sql =
    "BEGIN IMMEDIATE;"
    "CREATE TABLE file ( id INTEGER PRIMARY KEY, relative_path TEXT );"
    "CREATE TABLE tag ( id INTEGER PRIMARY KEY, name TEXT );"
    "CREATE TABLE file_tag ( file_id INTEGER, tag_id INTEGER );"

    "CREATE UNIQUE INDEX file_path_uq ON file (relative_path);"
    "CREATE UNIQUE INDEX tag_name_uq ON tag (name);"
    "CREATE UNIQUE INDEX file_tag_uq ON file_tag (file_id, tag_id);"
    "COMMIT;"
    ;

    return sqlite3_exec(dbconn, init_sql, NULL, NULL, NULL);
}

/* Open and init database, or search for DB_FILENAME if (fn == NULL) and with chdir_to_db if (dir == NULL)
 * The database is freed atexit
 * If fn is :memory: and dir is NULL it will open an in-memory database
 */
int init_db(char *fn, char *dir)
{
	if (dbconn != NULL)
		return ERROR;

    if (fn == NULL) {
		fn = DB_FILENAME;
    // Do not accidentaly open memory db
    } else if (strcmp(":memory:", fn) == 0) {
        fn = "./:memory:";
    }

    if (dir == NULL) {
        chdir_to_db(fn);
    } else {
        if (chdir(dir) != 0) {
            fprintf(stderr, PROGRAM_NAME ": failed to change to dir '%s'\n", dir);
            exit(ERROR);
        }
    }

    // Return error if database doesn't already exist
    int status = sqlite3_open_v2(fn, &dbconn, SQLITE_OPEN_READWRITE, NULL);

    if (status != SQLITE_OK) {
        status = sqlite3_open_v2(fn, &dbconn, SQLITE_OPEN_READWRITE |
                                 SQLITE_OPEN_CREATE, NULL);
        if (status != SQLITE_OK)
            return ERROR;
        else {
            status = run_init_db_sql();
            if (status != SQLITE_OK)
                return ERROR;
        }
    }

	atexit(close_db);

    return SUCCESS;
}

/* Same as init_db, but use a volatile in-memory database instead of on disk */
int init_memory_db(void) {
    if (dbconn != NULL)
        return ERROR;

    int status = sqlite3_open_v2(":memory:", &dbconn,
                                 SQLITE_OPEN_READWRITE, NULL);
    if (status != SQLITE_OK)
        return ERROR;
    else if (run_init_db_sql() != SQLITE_OK)
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
	int flags = 0;

	assert(argv != NULL);

	if (argc == 0) {
		flags |= FILTER_ALL;
	} else {
		flags |= FILTER_ANY_TAG;
	}

	step = filter_strs(argc, (const char **) argv, flags);

	if (step == NULL) {
		fprintf(stderr, PROGRAM_NAME ": error while filtering\n");
		return ERROR;
	} else {
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
		step = list_all_tags();
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

static void setup_test_db(CuTest *tc) {
    // When tests fail, they won't be able to close_db
    if (dbconn != NULL)
        close_db();
    CuAssertIntEquals(tc, SUCCESS, init_memory_db());
}

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

static void test_tag_file_null_file(CuTest *tc)
{
    setup_test_db(tc);
    CuAssertIntEquals(tc, ERROR, tag_file(NULL, "tag"));
    close_db();
}

static void test_tag_file_null_tag(CuTest *tc)
{
    setup_test_db(tc);
    CuAssertIntEquals(tc, ERROR, tag_file("file", NULL));
    close_db();
}

static void test_tag_file_tag_exits(CuTest *tc)
{
    sqlite3_stmt *prep = NULL;

    setup_test_db(tc);

    CuAssertIntEquals(tc, SUCCESS, tag_file("file", "tag"));
    CuAssertIntEquals(tc, SQLITE_OK,
                      sqlite3_prepare_v2(dbconn, "SELECT name FROM tag;", -1,
                                         &prep, NULL)
                      );
    CuAssertIntEquals(tc, SQLITE_ROW, sqlite3_step(prep));
    CuAssertStrEquals(tc, "tag",
                      (const char *) sqlite3_column_text(prep, 0));
    sqlite3_finalize(prep);
    close_db();

}

static void test_tag_file_file_exits(CuTest *tc)
{
    sqlite3_stmt *prep = NULL;

    setup_test_db(tc);

    CuAssertIntEquals(tc, SUCCESS, tag_file("file", "tag"));
    CuAssertIntEquals(tc, SQLITE_OK,
                      sqlite3_prepare_v2(dbconn,
                                         "SELECT relative_path FROM file;", -1,
                                         &prep, NULL)
                      );
    CuAssertIntEquals(tc, SQLITE_ROW, sqlite3_step(prep));
    CuAssertStrEquals(tc, "file",
                      (const char *)sqlite3_column_text(prep, 0));
    // no more rows should be returned!!
    sqlite3_finalize(prep);
    close_db();
}

static void test_tag_file_xref_exits(CuTest *tc)
{
    sqlite3_stmt *prep = NULL;

    setup_test_db(tc);

    CuAssertIntEquals(tc, SUCCESS, tag_file("file", "tag"));
    sqlite3_prepare_v2(dbconn, "SELECT file_id, tag_id FROM file_tag;", -1,
                       &prep, NULL);
    CuAssertIntEquals(tc, SQLITE_ROW, sqlite3_step(prep));

    // Since the in-memory database should be empty, both file and
    // tag should have been assigned id 1
    CuAssertIntEquals(tc, 1, sqlite3_column_int(prep, 0));
    CuAssertIntEquals(tc, 1, sqlite3_column_int(prep, 1));

    sqlite3_finalize(prep);

    close_db();
}

static CuSuite *tag_file_get_suite()
{
    CuSuite *suite = CuSuiteNew();

    SUITE_ADD_TEST(suite, test_tag_file_null_file);
    SUITE_ADD_TEST(suite, test_tag_file_null_tag);
    SUITE_ADD_TEST(suite, test_tag_file_tag_exits);
    SUITE_ADD_TEST(suite, test_tag_file_file_exits);
    SUITE_ADD_TEST(suite, test_tag_file_xref_exits);

    return suite;
}

static void test_init_db_fn_memory_with_dir(CuTest *tc)
{
    char dir[5 + 6 + 1];

    if (dbconn != NULL) {
        close_db();
        dbconn = NULL;
    }

    strncpy(dir, "ftag-XXXXXX", sizeof(dir));
    char *status = mkdtemp(dir);
    if (status == NULL)
        CuFail(tc, "Failed mkdtemp");

    CuAssertIntEquals(tc, SUCCESS, init_db(":memory:", dir));
    chdir(dir);

    int exists = access(":memory:", F_OK);

	close_db();
    unlink(":memory:");
    chdir("..");
    rmdir(dir);

    CuAssertIntEquals(tc, 0, exists);
}

static void test_init_db_fn_memory_null_dir(CuTest *tc)
{
    char dir[5 + 6 + 1];

    if (dbconn != NULL) {
        close_db();
        dbconn = NULL;
    }

    strncpy(dir, "ftag-XXXXXX", sizeof(dir));
    char *status = mkdtemp(dir);
    if (status == NULL)
        CuFail(tc, "Failed mkdtemp");

    chdir(dir);
    CuAssertIntEquals(tc, SUCCESS, init_db(":memory:", NULL));

    int exists = access(":memory:", F_OK);

	close_db();
    unlink(":memory:");
    chdir("..");
    rmdir(dir);

    CuAssertIntEquals(tc, 0, exists);
}

static CuSuite *init_db_get_suite()
{
    CuSuite *suite = CuSuiteNew();

    SUITE_ADD_TEST(suite, test_init_db_fn_memory_with_dir);
    SUITE_ADD_TEST(suite, test_init_db_fn_memory_null_dir);

    return suite;
}

static void test_get_tag_ids(CuTest *tc)
{
	static const char *insert_sql =
	"BEGIN;"
	"INSERT INTO tag (name) VALUES ('tag1');"
	"INSERT INTO tag (name) VALUES ('tag2');"
	"INSERT INTO tag (name) VALUES ('tag3');"
	"COMMIT;"
	;

	setup_test_db(tc);

	CuAssertIntEquals(tc, SQLITE_OK,
					  sqlite3_exec(dbconn, insert_sql, NULL, NULL, NULL));

	int *idv = get_tag_ids(3, (const char *[]) {"tag1", "tag2", "tag3"} );
	CuAssertPtrNotNull(tc, idv);

	for (int i = 0; i < 3; i++)
		CuAssertIntEquals(tc, i+1, idv[i]);

	close_db();
}

static CuSuite *get_tag_ids_get_suite()
{
	CuSuite *suite = CuSuiteNew();

	SUITE_ADD_TEST(suite, test_get_tag_ids);

	return suite;
}

static void filter_setup_test_db(CuTest *tc)
{
	static char *insert_sql =
	"BEGIN;"
	"INSERT INTO tag (id, name) VALUES ('1', 'tag1');"
	"INSERT INTO tag (id, name) VALUES ('2', 'tag2');"
	"INSERT INTO file (id, relative_path) VALUES ('1', 'file1');"
	"INSERT INTO file (id, relative_path) VALUES ('2', 'file2');"
	"INSERT INTO file_tag (file_id, tag_id) VALUES ('1', '1');"
	"INSERT INTO file_tag (file_id, tag_id) VALUES ('2', '1');"
	"INSERT INTO file_tag (file_id, tag_id) VALUES ('2', '2');"
	"COMMIT;"
	;

	setup_test_db(tc);

	CuAssertIntEquals(tc, SQLITE_OK,
					  sqlite3_exec(dbconn, insert_sql, NULL, NULL, NULL));
}

static void test_filter_ids_any_tag_one(CuTest *tc)
{
	int id2 = 2;
	step_t *step = NULL;

	filter_setup_test_db(tc);

	step = filter_ids_any_tag(1, &id2);
	CuAssertPtrNotNull(tc, step);
	CuAssertStrEquals(tc, "file2", step_result(step));
	CuAssertPtrEquals(tc, NULL, (void *) step_result(step));

	free_step(step);
	close_db();
}

static void test_filter_ids_any_tag_two(CuTest *tc)
{
	int ids[2] = { 1, 2 };
	step_t *step = NULL;

	filter_setup_test_db(tc);

	step = filter_ids_any_tag(2, ids);
	CuAssertPtrNotNull(tc, step);
	// Counts on results being ordered
	CuAssertStrEquals(tc, "file1", step_result(step));
	CuAssertStrEquals(tc, "file2", step_result(step));
	CuAssertPtrEquals(tc, NULL, (void *) step_result(step));

	free_step(step);
	close_db();
}

static CuSuite *filter_ids_any_tag_get_suite()
{
	CuSuite *suite = CuSuiteNew();

	SUITE_ADD_TEST(suite, test_filter_ids_any_tag_one);
	SUITE_ADD_TEST(suite, test_filter_ids_any_tag_two);

	return suite;
}

static int run_tests(void)
{
    CuString *output = CuStringNew();
    CuSuite *suite = CuSuiteNew();

    CuSuiteConsume(suite, chdir_to_db_get_suite());
    CuSuiteConsume(suite, tag_file_get_suite());
    CuSuiteConsume(suite, init_db_get_suite());
	CuSuiteConsume(suite, get_tag_ids_get_suite());
	CuSuiteConsume(suite, filter_ids_any_tag_get_suite());
   
    CuSuiteRun(suite);
    CuSuiteSummary(suite, output);
    CuSuiteDetails(suite, output);
    fprintf(stderr, "%s\n", output->buffer);

    CuStringDelete(output);
    CuSuiteDelete(suite);
	// In case the last test failed
	close_db();
    
    return SUCCESS;
}
