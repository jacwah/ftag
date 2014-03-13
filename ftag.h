/*
 * ftag -- tag your files
 * Copyright 2014 Jacob Wahlgren
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

#define SUCCESS 0
#define ERROR 1

typedef struct sqlite3_stmt step_t;

extern int tag_file(const char *file, const char *tag);
extern const char *step_result(step_t *stmt);
extern void free_step(step_t *stmt);
extern step_t *filter_by_tag(const char *tag);
extern step_t *filter_by_tags(int tagc, char **tagv);
extern step_t *list_by_file(const char *file);
extern int init_db(char *fn, char *dir);
