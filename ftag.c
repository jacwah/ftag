/*
 * ftag -- tag and filter your files
 * 
 * Copyright 2014 Jacob Wahlgren
 * 
 * TODO: tag_files: use DISTINCT
 * TODO: Make sql SELECT functions return stuff instead of print
 * TODO: -f force create db
 * 
 */

/*   
 This program is free software: you can redistribute it and/or modify
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <sqlite3.h>

#ifndef DB_FILENAME
#define DB_FILENAME ".ftagdb"
#endif

#define PROGRAM_NAME "ftag"

static enum {
	MODE_NONE,
	MODE_TAG_FILE,
	MODE_FILTER,
	MODE_LIST
};

sqlite3 *dbconn = NULL;

/*** Util ***/

static void help(void)
{
	static const char *str = "Usage: " PROGRAM_NAME " [-dvh] MODE ARG...\n"
	"  " PROGRAM_NAME " [-dv] file FILE TAG...\n"
	"  " PROGRAM_NAME " [-dv] filter TAG...\n"
	"  " PROGRAM_NAME " [-dv] list FILE\n"
	"\n"
	"Options:\n"
	"  -d, --database  specify database to use\n"
	"  -v              increase output verbosity (can be used multiple times)\n"
	"  --help          show this help\n"
	"\n"
	"Report bugs to jacob.wahlgren@gmail.com.\n"
	"This software is licensed under the GNU General public license.\n"
	"Copyright 2014 Jacob Wahlgren.\n";
	fputs(str, stderr);
}

static void usage(void)
{
	static char *str = "Usage: " PROGRAM_NAME " [-dvh] MODE ARG...\n"
	"Use '" PROGRAM_NAME " --help' for more info\n";
	
	fputs(str, stderr);
}

/* On the getcwd hack: returns null if path is too long, and is if not "/" */
static int chdir_to_db(const char *fn)
{
	char buf[2];
	int startdir = open(".", O_RDONLY);
	
	int fchdir(int);
	
	while (1) {
		if (access(fn, R_OK) == 0)
			break;
		
		if (getcwd(buf, 2) != NULL) {
			fchdir(startdir);
			close(startdir);
			
			return 1;
		}
		
		chdir("..");
	}
	close(startdir);
	
	return 0;
}

/*** SQLite wrappers and helpers ***/

static int prepare_or_reset(sqlite3_stmt **prep, const char *str)
{
	if (*prep == NULL) {
		int s = sqlite3_prepare_v2(dbconn, str, -1, prep, NULL);
		
		if (s != SQLITE_OK) {
			fprintf(stderr, "error: failed preparation of SQL statement (%d)\n", s);
			exit(1);
		} else {
			assert(*prep != NULL);
			return 0;
		}
	} else {
		sqlite3_reset(*prep);
		return 1;
	}
}

int tag_file(const char *file, const char *tag)
{
	static sqlite3_stmt *sql_prep = NULL;
	static const char *sql_str = "INSERT OR IGNORE INTO Tag VALUES (?, ?);";
	
	// Only one statement in the sql, only one call to step
	assert(strchr(sql_str, ';') == strrchr(sql_str, ';'));

	prepare_or_reset(&sql_prep, sql_str);
	
	if (sqlite3_bind_text(sql_prep, 1, file, -1, SQLITE_STATIC) != SQLITE_OK ||
		sqlite3_bind_text(sql_prep, 2, tag, -1, SQLITE_STATIC) != SQLITE_OK)
		return 1;
	
	if (sqlite3_step(sql_prep) != SQLITE_DONE)
		return 1;
	
	return 0;
}

int filter_tag(const char *tag)
{
	static sqlite3_stmt *sql_prep = NULL;
	static const char *sql_str = "SELECT DISTINCT file FROM Tag WHERE tag=?;";
	
	// Only one statement in the sql, only one call to step
	assert(strchr(sql_str, ';') == strrchr(sql_str, ';'));
	
	prepare_or_reset(&sql_prep, sql_str);
	
	if (sqlite3_bind_text(sql_prep, 1, tag, -1, SQLITE_STATIC) != SQLITE_OK)
		return 1;
	
	while (sqlite3_step(sql_prep) == SQLITE_ROW)
		puts(sqlite3_column_text(sql_prep, 0));
	
	return 0;
}

int list_tags(const char *file)
{
	static sqlite3_stmt *sql_prep = NULL;
	static const char *sql_str = "SELECT DISTINCT tag FROM Tag WHERE file=?;";
	
	// Only one statement in the sql, only one call to step
	assert(strchr(sql_str, ';') == strrchr(sql_str, ';'));

	prepare_or_reset(&sql_prep, sql_str);
	
	if (sqlite3_bind_text(sql_prep, 1, file, -1, SQLITE_STATIC) != SQLITE_OK)
		return 1;
	
	while (sqlite3_step(sql_prep) == SQLITE_ROW)
		puts(sqlite3_column_text(sql_prep, 0));
	
	return 0;
}

static int init_db(char *fn)
{
	static char *init_sql = "CREATE TABLE IF NOT EXISTS Tag (file varchar(256) NOT NULL, tag varchar(256) NOT NULL);"
							"CREATE UNIQUE INDEX IF NOT EXISTS uq_Tag on Tag (file, tag);";
	chdir_to_db(fn);
	
	if (sqlite3_open(fn, &dbconn) != SQLITE_OK)
		return 1;
	
	if (sqlite3_exec(dbconn, init_sql, NULL, NULL, NULL) != SQLITE_OK)
		return 1;
	else
		return 0;
}

/*** Enter points ***/

int main_tag_file(int argc, char **argv)
{
	assert(argv != NULL);
	
	if (argc < 2) {
		usage();
		return 1;
	}
	
	for (int i = 1; i < argc; i++)
		if (tag_file(argv[0], argv[i]) != 0) {
			fprintf(stderr, PROGRAM_NAME ": error tagging file\n");
			return 1;
		}
	
	return 0;
}

int main_filter(int argc, char **argv)
{
	assert(argv != NULL);
	
	if (argc < 1) {
		usage();
		return 1;
	}
	
	for (int i = 0; i < argc; i++)
		if (filter_tag(argv[i]) != 0) {
			fprintf(stderr, PROGRAM_NAME ": error while filtering tag\n");
			return 1;
		}
		
	return 0;
}

int main_list(int argc, char **argv)
{
	assert(argv != NULL);
	
	if (argc != 1) {
		usage();
		return 1;
	}
	
	if (list_tags(argv[0]) != 0) {
		fprintf(stderr, PROGRAM_NAME ": error while listing tags\n");
		return 1;
	}
	
	return 0;
}
	
int main(int argc, char **argv)
{
	/* Look at git argument parsing for ideas on handling modes etc */
	int chr = 0;
	int mode = MODE_NONE;
	char *dbfilename = DB_FILENAME;
	int verbosity = 0;
	
	static struct option longopts[] = {
		{"database", required_argument, 0, 'd'},
		{"verbose", no_argument, 0, 'v'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};
	
	opterr = 0;
	while ((chr = getopt_long(argc, argv, "d:v", longopts, NULL)) != -1) {
		switch (chr) {
			case 'd':
				dbfilename = optarg;
				break;
			case 'v':
				verbosity++;
				break;
			case 'h':
				help();
				return 0;
			default:
				usage();
				return 1;
		}
	}
	
	if (argc - optind < 1) {
		usage();
		return 1;
	}
	
	if (strcmp(argv[optind], "file") == 0)
		mode = MODE_TAG_FILE;
	else if (strcmp(argv[optind], "filter") == 0)
		mode = MODE_FILTER;
	else if (strcmp(argv[optind], "list") == 0)
		mode = MODE_LIST;
	else {
		usage();
		return 1;
	}
	
	optind++;
	
	if (init_db(dbfilename) != 0) {
		fprintf(stderr, PROGRAM_NAME ": error: failed to initialize database\n");
		return 1;
	}
	
	if (verbosity > 0)
		fprintf(stderr, "choosing db '%s/%s'\n", getcwd(NULL, 0), dbfilename);
	
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
				return 127;
		}
	} else {
		return 127;
	}
}
