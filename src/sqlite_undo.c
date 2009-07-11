/*
 *      sqlite_undo.c
 *      
 *      Copyright 2009 Simon Naunton <snaunton@gmail.com>
 *      
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 3 of the License, or
 *      (at your option) any later version.
 *      
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *      
 *      You should have received a copy of the GNU General Public License
 *      along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_UNDO)

#include <assert.h>
#include <stdlib.h>

#ifndef NDEBUG
	#include <string.h>
#endif 

#ifndef SQLITE_CORE
	#include <sqlite3ext.h>
	SQLITE_EXTENSION_INIT1
#else
	#include "sqlite3.h"
#endif

typedef enum {
	SqliteUndoUpdateTypeNone,
	SqliteUndoUpdateTypeTable,
	SqliteUndoUpdateTypeColumn
} SqliteUndoUpdateType;

typedef struct {
	char *name;
	int nargs;
	void *func;
} SqliteUndoFunction;

#define SQLITE_UNDO_ERRMSG_TABLE_MUST_BE_TEXT \
	"Table name must be a text string"
#define SQLITE_UNDO_ERRMSG_INVALID_UPDATE_TYPE	\
	"Invalid update_type. Valid values:\n" \
	"0: None\n" \
	"1: Table\n" \
	"2: Column"
#define SQLITE_UNDO_ERRMSG_CREATE_TRIGGER_FAILED "Failed to create triggers"
#define SQLITE_UNDO_ERRMSG_SQL_MUST_BE_TEXT 	 "SQL must be a text string"
#define SQLITE_UNDO_ERRMSG_ROLLBACK_OCCURRED     "A ROLLBACK occurred"
#define SQLITE_UNDO_ERRMSG_COMMIT_FAILED	 "COMMIT failed"

static char *sqlite_undo_add_delete_column(char *columns, char *column)
{
	char *r;

	assert(column);

	if (columns) {
		r = sqlite3_mprintf("%s,%s", columns, column);
		sqlite3_free(columns);
	}
	else {
		r =sqlite3_mprintf("%s", column);
	}

	return r;
}

static char *sqlite_undo_add_delete_value(char *values, char *column)
{
	char *r;

	assert(column);

	if (values) {
		r = sqlite3_mprintf("%s,'||quote(OLD.%s)||'", values, column);
		sqlite3_free(values);
	}
	else {
		r = sqlite3_mprintf(",'||quote(OLD.%s)||'", column);
	}

	return r;
}

static char *sqlite_undo_add_update_column(char *columns, char *column)
{
	char *r;

	assert(column);
		
	if (columns) {
		r = sqlite3_mprintf(",%s='||quote(OLD.%s)||'", column, column);
		sqlite3_free(columns);
	}
	else {
		r = sqlite3_mprintf("%s='||quote(OLD.%s)||'", column, column);
	}

	return r;
}

static char *sqlite_undo_append_update_column_trigger(char *triggers,
					char *table, char *column)
{
	char *r;

	assert(table);
	assert(column);

	r = sqlite3_mprintf(
		"%s"
		"CREATE TEMP TRIGGER _u_%s_u_%s AFTER UPDATE OF %s ON %s "
		"WHEN (SELECT active FROM _undo_active) IS NOT NULL "  
		"BEGIN "
			"INSERT INTO _undo "
			"VALUES("
				"'UPDATE %s SET %s='||quote(OLD.%s)||'"
				"WHERE rowid='||OLD.rowid"
			");"
		"END;",
		triggers ? triggers : "",
		table, column, column, table,
		table, column, column);


	sqlite3_free(triggers);

	return r;
}

static char *sqlite_undo_prepend_update_table_trigger(char *triggers,
						char *table, char *columns)
{
	char *r;

	assert(table);
	assert(columns);

	r = sqlite3_mprintf(
		"%s"
		"CREATE TEMP TRIGGER _u_%s_u AFTER UPDATE ON %s "
		"WHEN (SELECT active FROM _undo_active) IS NOT NULL " 
		"BEGIN "
			"INSERT INTO _undo "
			"VALUES("
				"'UPDATE %s SET %s "
				"WHERE rowid='||OLD.rowid"
			");"
		"END;",
		triggers ? triggers : "",
		table, table,
		table, columns);

	sqlite3_free(triggers);

	return r;			
}

static char *sqlite_undo_prepend_delete_trigger(char *triggers, char *table,
					char *columns, char *values)
{
	char *r;

	assert(table);
	assert(columns);
	assert(values);

	r = sqlite3_mprintf(
		"CREATE TEMP TRIGGER _u_%s_d BEFORE DELETE ON %s "
		"WHEN (SELECT active FROM _undo_active) IS NOT NULL " 
		"BEGIN "
			"INSERT INTO _undo "
			"VALUES("
				"'INSERT INTO %s(rowid,%s) "
				"VALUES('||OLD.rowid||'%s)'"
			");"
		"END;"
		"%s",
		table, table,
		table, columns, values,
		triggers ? triggers : "");

	sqlite3_free(triggers);

	return r;
}

static char *sqlite_undo_prepend_insert_trigger(char *triggers, char *table)
{
	char *r;

	assert(table);

	r = sqlite3_mprintf(
		"CREATE TEMP TRIGGER _u_%s_i AFTER INSERT ON %s "
		"WHEN (SELECT active FROM _undo_active) IS NOT NULL " 
		"BEGIN "
			"INSERT INTO _undo "
			"VALUES("
				"'DELETE FROM %s "
				"WHERE rowid='||NEW.rowid"
			");"
		"END;"
		"%s",
		table, table,
		table,
		triggers ? triggers : "");

	sqlite3_free(triggers);

	return r;			
}

static char *sqlite_undo_get_table_undo_triggers(sqlite3 *db, char *table,
				SqliteUndoUpdateType update_type)
{
	int rc, pk;
	char *sql, *name, *delete_names = NULL, *delete_values = NULL,
		*update_columns = NULL, *triggers = NULL;
	sqlite3_stmt *stmt;

	assert(db);
	assert(table);
	assert((update_type == SqliteUndoUpdateTypeNone) ||
		(update_type == SqliteUndoUpdateTypeTable) ||
		(update_type == SqliteUndoUpdateTypeColumn));

/*
	PRAGMA table_info([table]) returns:

	[0] = column number
	[1] = column name
	[2] = column type
	[3] = flag: is the column NOT NULL?
	[4] = default value of the column
	[5] = flag: is this column part of the table's PRIMARY KEY?
*/

	sql = sqlite3_mprintf("PRAGMA table_info(%s)", table);
	rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	sqlite3_free(sql);

	if (rc != SQLITE_OK) {
		return NULL;
	}
		
	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_ROW) {
			pk = sqlite3_column_int(stmt, 5);
			name = (char*)sqlite3_column_text(stmt, 1);
			
			delete_names = sqlite_undo_add_delete_column(
								delete_names,
								name);
			delete_values = sqlite_undo_add_delete_value(
								delete_values,
								name);

			if (pk) {
				continue;
			}

			switch (update_type) {
			case SqliteUndoUpdateTypeColumn :
				triggers =
					sqlite_undo_append_update_column_trigger(
								triggers,
								table,
								name);
				break;
			case SqliteUndoUpdateTypeTable :
				update_columns = sqlite_undo_add_update_column(
								update_columns,
								name);
				break;
			case SqliteUndoUpdateTypeNone :
				break;
			}
		}
		else {
			break;
		}
	}


	sqlite3_finalize(stmt);

	if (rc == SQLITE_DONE) {
		if (update_type == SqliteUndoUpdateTypeTable) {
			triggers = sqlite_undo_prepend_update_table_trigger(
								triggers,
								table,
								update_columns);
		}

		triggers = sqlite_undo_prepend_delete_trigger(triggers, table,
							delete_names,
							delete_values);

		triggers = sqlite_undo_prepend_insert_trigger(triggers, table);
	}
	else {
		sqlite3_free(triggers);
		triggers = NULL;
	}

	sqlite3_free(delete_names);
	sqlite3_free(delete_values);
	sqlite3_free(update_columns);

	return triggers;
}

static void sqlite_undo_undoable_table(sqlite3_context *context, int argc,
			sqlite3_value **argv)
{
	int rc;;
	SqliteUndoUpdateType update_type;
	char *table, *triggers;
	sqlite3 *db;

	if ((sqlite3_value_type(argv[0]) != SQLITE_TEXT)) {
		sqlite3_result_error(context,
				SQLITE_UNDO_ERRMSG_TABLE_MUST_BE_TEXT, -1);
		return;		
	}	

	update_type = (SqliteUndoUpdateType)sqlite3_value_int(argv[1]);

	if ((update_type != SqliteUndoUpdateTypeNone) && 
		(update_type != SqliteUndoUpdateTypeTable) &&
		(update_type != SqliteUndoUpdateTypeColumn)) {
	
		sqlite3_result_error(context,
				SQLITE_UNDO_ERRMSG_INVALID_UPDATE_TYPE, -1);
		return;	
	
	}

	table = (char*)sqlite3_value_text(argv[0]);

	db = sqlite3_context_db_handle(context);

	triggers = sqlite_undo_get_table_undo_triggers(db, table, update_type);

	if (!triggers) {
		sqlite3_result_error(context,
				SQLITE_UNDO_ERRMSG_CREATE_TRIGGER_FAILED, -1);
		return;
	}		

	rc = sqlite3_exec(db, triggers, NULL, NULL, NULL);

	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
	}

	sqlite3_free(triggers);
}

static sqlite_int64 sqlite_undo_get_buffer_status(sqlite3 *db, char U_R)
{
	int rc;
	sqlite_int64 c = -1LL;
	sqlite3_stmt *stmt;

	assert(db);
	assert(U_R == 'U' || U_R == 'R');

	rc =  sqlite3_prepare_v2(db,
			"SELECT count(*) "
			"FROM _undo "
			"WHERE s=?",
			-1, &stmt, NULL);

	sqlite3_bind_text(stmt, 1, &U_R, 1, NULL);

	if (rc == SQLITE_OK) {
		rc = sqlite3_step(stmt);
	}

	if (rc == SQLITE_ROW) {
		c = sqlite3_column_int64(stmt, 0);
	}

	sqlite3_finalize(stmt);

	return c;
}

static int sqlite_undo_undoable_begin_do(sqlite3_context *context, int argc,
			sqlite3_value **argv)
{
	int rc;
	sqlite3 *db;

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_exec(db, 
			"BEGIN;"
			/* Delete redos */
			"DELETE FROM _undo "
			"WHERE rowid IN ("
				"SELECT rowid "
				"FROM _redo_row_ids"
			");"
			/* Prepare for an undo entry */
			"INSERT INTO _undo(s) "
			"VALUES('U');"
			/* Undoable transaction is active */
			"UPDATE _undo_active SET active=1;",
			NULL, NULL, NULL);

	
	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
		sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
		return 0;
	}
	
	return 1;
}

static void sqlite_undo_undoable_begin(sqlite3_context *context, int argc,
				sqlite3_value **argv)
{
	sqlite_undo_undoable_begin_do(context, argc, argv);
}

static void sqlite_undo_undoable_end(sqlite3_context *context, int argc,
						sqlite3_value **argv)
{
	char *result;
	sqlite3 *db;

	db = sqlite3_context_db_handle(context);

	sqlite3_exec(db, "UPDATE _undo_active SET active=NULL",
		NULL, NULL, NULL);

	/* 
	 * sqlite3_get_autocommit returns 0 if inside a transaction. The
	 * only way we cannot be in a transaction here is if a commit or
	 * rollback has been issued between sqlite_undo_undoable_begin() and
	 * sqlite_undo_undoable_end(). This is not allowed, so assume a commit 
	 * or rollback has occurred if sqlite_get_autocommit returns 
	 * non-zero
	 */
	if (sqlite3_get_autocommit(db)) {
		sqlite3_result_error(context,
				SQLITE_UNDO_ERRMSG_ROLLBACK_OCCURRED, -1);
		return;
	}

	if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
		sqlite3_result_error(context,
				SQLITE_UNDO_ERRMSG_COMMIT_FAILED, -1);
		return;
	}

	result = sqlite3_mprintf("UNDO=%lld\nREDO=%lld",
			sqlite_undo_get_buffer_status(db, 'U'),
			sqlite_undo_get_buffer_status(db, 'R'));

	sqlite3_result_text(context, result, -1, sqlite3_free);
}

static void sqlite_undo_undoable(sqlite3_context *context, int argc,
			sqlite3_value **argv)
{
	int rc;
	char *query;
	sqlite3 *db;

	if ((sqlite3_value_type(argv[0]) != SQLITE_TEXT)) {
		sqlite3_result_error(context,
				SQLITE_UNDO_ERRMSG_SQL_MUST_BE_TEXT, -1);
		return;		
	}

	query = (char*)sqlite3_value_text(argv[0]);

	if(!sqlite_undo_undoable_begin_do(context, argc, argv)) { 
		return;
	}

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_exec(db, query, NULL, NULL, NULL);

	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
		return;
	}

	sqlite_undo_undoable_end(context, argc, argv);
}

static int sqlite_undo_step_get_transaction_bounds(sqlite3 *db, char *un_re,
						sqlite_int64 *tstart,
						sqlite_int64 *tend)
{
	int rc;
	char *sql;
	sqlite3_stmt *stmt;

	assert(db);
	assert((strcmp(un_re,"un") == 0) || 
		(strcmp(un_re, "re") == 0));
	assert(tstart);
	assert(tend);

	sql = sqlite3_mprintf("SELECT tstart,tend FROM _%sdo_stack_top",
			un_re);

	rc =  sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

	sqlite3_free(sql);

	if (rc == SQLITE_OK) {
		rc = sqlite3_step(stmt);
	}

	if (rc == SQLITE_ROW) {
		*tstart = sqlite3_column_int64(stmt, 0);
		*tend = sqlite3_column_int64(stmt, 1);
	}

	sqlite3_finalize(stmt);

	return rc;
}

static int sqlite_undo_step_get_transaction_sql(sqlite3_context *context,
						sqlite_int64 tstart,
						sqlite_int64 tend,
						char **sql)
{
	int rc;
	char *_sql = NULL, *s;
	sqlite3 *db;
	sqlite3_stmt *stmt;

	assert(context);
	assert(tstart >= 0);
	assert(tend >= 0);

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_prepare_v2(db,
			"SELECT s FROM _undo WHERE rowid>? AND rowid<=?",
			-1, &stmt, NULL);

	if (rc != SQLITE_OK) {
		return rc;
	}

	sqlite3_bind_int64(stmt, 1, tstart);
	sqlite3_bind_int64(stmt, 2, tend);

	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_ROW) {
			if (!_sql) {
				_sql = sqlite3_mprintf("%s", 
						sqlite3_column_text(stmt, 0));
			}
			else {
				s = _sql;
				_sql = sqlite3_mprintf("%s;%s", s,
						sqlite3_column_text(stmt, 0));
				sqlite3_free(s);
			}
		}
		else {
			break;
		}
	}

	sqlite3_finalize(stmt);

	if (rc == SQLITE_DONE) {
		*sql = _sql;
	}
	else {
		sqlite3_free(_sql);
	}

	return rc;
}

static int sqlite_undo_step_delete_transaction(sqlite3_context *context,
					sqlite_int64 tstart,
					sqlite_int64 tend)
{
	int rc;
	sqlite3 *db;
	sqlite3_stmt *stmt;

	assert(context);
	assert(tstart >= 0);
	assert(tend >= 0);

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_prepare_v2(db,
			"DELETE FROM _undo "
			"WHERE rowid>=? AND rowid<=?",
			-1, &stmt, NULL);

	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, tstart);
		sqlite3_bind_int64(stmt, 2, tend);

		rc = sqlite3_step(stmt);

		sqlite3_finalize(stmt);
	}

	return rc;
}

static int sqlite_undo_step_prep_log(sqlite3_context *context, char U_R)
{
	int rc;
	sqlite3 *db;
	sqlite3_stmt *stmt;

	assert(context);
	assert(U_R == 'U' || U_R == 'R');

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_prepare_v2(db,
			"INSERT INTO _undo(s) VALUES(?)",
			-1, &stmt, NULL);

	if (rc == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, &U_R, 1, NULL);

		rc = sqlite3_step(stmt);

		sqlite3_finalize(stmt);
	}

	return rc;
}

static void sqlite_undo_step(sqlite3_context *context, char *un_re, char U_R)
{
	int rc;
	sqlite_int64 tstart = -1LL, tend = -1LL;
	char *sql = NULL, *result;
	sqlite3 *db;

	assert(context);
	assert((strcmp(un_re,"un") == 0) || 
		(strcmp(un_re, "re") == 0));
	assert(U_R == 'U' || U_R == 'R');

	db = sqlite3_context_db_handle(context);

	rc = sqlite_undo_step_get_transaction_bounds(db, un_re, &tstart, &tend);;

	switch (rc) {
	case SQLITE_DONE :
		sqlite3_result_null(context);
		return;
	case SQLITE_ROW :
		break;
	default :
		goto result_error;
	}

	rc = sqlite_undo_step_get_transaction_sql(context, tstart, tend, &sql);
	if (rc != SQLITE_DONE) {
		goto result_error;
	}

	sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

	rc = sqlite_undo_step_delete_transaction(context, tstart, tend);
	if (rc != SQLITE_DONE) {
		goto rollback;
	}

	rc = sqlite_undo_step_prep_log(context, U_R);
	if (rc != SQLITE_DONE) {
		goto rollback;
	}

	sqlite3_exec(db, "UPDATE _undo_active SET active=1",
		NULL, NULL, NULL);

	rc = sqlite3_exec(db, sql, NULL, NULL, NULL);

	sqlite3_exec(db, "UPDATE _undo_active SET active=NULL",
		NULL, NULL, NULL);

	if (rc != SQLITE_OK) {
		goto rollback;
	}

	sqlite3_exec(db, "END", NULL, NULL, NULL);
	
	result = sqlite3_mprintf("UNDO=%lld\nREDO=%lld\nSQL=%s",
			sqlite_undo_get_buffer_status(db, 'U'),
			sqlite_undo_get_buffer_status(db, 'R'),
			sql);

	sqlite3_free(sql);

	sqlite3_result_text(context, result, -1, sqlite3_free);

	return;

rollback:
	sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
result_error:
	sqlite3_result_error_code(context, rc);
	sqlite3_free(sql);
}

static void sqlite_undo_undo(sqlite3_context *context, int argc,
			sqlite3_value **argv)
{
	sqlite_undo_step(context, "un", 'R');
}

static void sqlite_undo_redo(sqlite3_context *context, int argc,
			sqlite3_value **argv)
{
	sqlite_undo_step(context, "re", 'U');
}

#if !SQLITE_CORE
static
#endif
int sqlite3UndoInit(sqlite3 *db)
{
 	int rc;
	SqliteUndoFunction *function;
	static SqliteUndoFunction functions[] = {
		{"undoable_table", 2, sqlite_undo_undoable_table},
		{"undoable", 1, sqlite_undo_undoable},
		{"undoable_begin", 0, sqlite_undo_undoable_begin},
		{"undoable_end", 0, sqlite_undo_undoable_end},
		{"undo", 0, sqlite_undo_undo},
		{"redo", 0, sqlite_undo_redo},
		{NULL}
	};

	rc = sqlite3_exec(db,
		"CREATE TEMP TABLE _undo(s TEXT);"

		"CREATE TEMP TABLE _undo_active(active INTEGER);"
		"INSERT INTO _undo_active(active) VALUES(NULL);"

		"CREATE TEMP VIEW _undo_stack AS "
			"SELECT T1.rowid AS tstart,"
				"coalesce("
					"("
						"SELECT T2.rowid "
						"FROM _undo T2 "
						"WHERE T2.rowid>T1.rowid "
						"AND (T2.s='U' OR T2.s='R') "
						"LIMIT 1"
					")-1,"
					"("
						"SELECT max(rowid) "
						"FROM _undo"
					")"
				") AS tend "
			"FROM _undo T1 "
			"WHERE T1.s='U' "
			"ORDER BY rowid DESC;"

		"CREATE TEMP VIEW _undo_stack_top AS "
				"SELECT tstart,tend FROM _undo_stack LIMIT 1;"

		"CREATE TEMP VIEW _redo_stack AS "
			"SELECT T1.rowid AS tstart,"
				"coalesce("
					"("
						"SELECT T2.rowid "
						"FROM _undo T2 "
						"WHERE T2.rowid>T1.rowid "
						"AND (T2.s='U' OR T2.s='R') "
						"LIMIT 1"
					")-1,"
					"("
						"SELECT max(rowid) "
						"FROM _undo"
					")"
				") AS tend "
				"FROM _undo T1 "
				"WHERE T1.s='R' "
				"ORDER BY rowid DESC;"

			"CREATE TEMP VIEW _redo_stack_top AS "
				"SELECT tstart,tend FROM _redo_stack LIMIT 1;"

			"CREATE TEMP VIEW _redo_row_ids AS "
				"SELECT T2.rowid "
				"FROM _redo_stack T1 "
					"LEFT JOIN _undo T2 "
					"ON T2.rowid "
					"BETWEEN T1.tstart AND T1.tend "
				"ORDER BY T2.rowid DESC;",

				NULL, NULL, NULL);

	if (rc != SQLITE_OK) {
		return rc;
	}

	function = functions;

	while (function->name) {
		rc = sqlite3_create_function(db, function->name,
					function->nargs, SQLITE_UTF8, NULL,
					function->func, NULL, NULL);

		if (rc != SQLITE_OK) {
			break;
		}

		function++;
	}

	return rc;
}

#if !SQLITE_CORE
int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg,
                        const sqlite3_api_routines *pApi)
{
	SQLITE_EXTENSION_INIT2(pApi)
	return sqlite3UndoInit(db);
}
#endif

#endif
