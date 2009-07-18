/*
 *      sqlite_undo.c
 *
 *      Copyright 2009 Simon Naunton <snaunton@gmail.com>
 *
 *      Merge back of some code from a fork of sqlite-undo project by Alexey
 *      Pechnikov <pechnikov@mobigroup.ru>
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

#ifndef SQLITE_CORE
	#include <sqlite3ext.h>
	SQLITE_EXTENSION_INIT1
#else
	#include "sqlite3.h"
#endif

typedef struct {
	char *name;
	int nargs;
	void *func;
} SqliteUndoFunction;

typedef enum {
	SqliteUndoUpdateTypeNone,
	SqliteUndoUpdateTypeTable,
	SqliteUndoUpdateTypeColumn
} SqliteUndoUpdateType;

typedef enum {
	SqliteUndoUndo,
	SqliteUndoRedo
} SqliteUndoOrRedo;

int sqlite_undo_undoable_active_flag = 0;

#define SQLITE_UNDO_ERRMSG_TABLE_MUST_BE_TEXT \
	"Table name must be a text string"
#define SQLITE_UNDO_ERRMSG_INVALID_UPDATE_TYPE	\
	"Invalid update_type. Valid values:\n" \
	"0: None\n" \
	"1: Table\n" \
	"2: Column"
#define SQLITE_UNDO_ERRMSG_CREATE_TRIGGER_FAILED \
	"Failed to create triggers"
#define SQLITE_UNDO_ERRMSG_UNDOABLE_ACTIVE	"Undoable is active"
#define SQLITE_UNDO_ERRMSG_UNDOABLE_NOT_ACTIVE	"Undoable is not active"

#define SQLITE_UNDO_TABLE		"_sqlite_undo"
#define SQLITE_UNDO_SAVEPOINT_UNDOABLE	"_sqlite_undo_undoable"
#define SQLITE_UNDO_SAVEPOINT_UNDO	"_sqlite_undo_undo"

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
		"WHEN (SELECT undoable_active())=1 "  
		"BEGIN "
			"UPDATE " SQLITE_UNDO_TABLE " "
			"SET sql=sql || "
				"'UPDATE %s SET %s "
				"WHERE rowid='||OLD.rowid||';' "
			"WHERE ROWID=("
				"SELECT MAX(ROWID) FROM " SQLITE_UNDO_TABLE
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
		"WHEN (SELECT undoable_active())=1 " 
		"BEGIN "
			"UPDATE " SQLITE_UNDO_TABLE " "
			"SET sql=sql || "
				"'UPDATE %s SET %s "
				"WHERE rowid='||OLD.rowid||';' "
			"WHERE ROWID=("
				"SELECT MAX(ROWID) FROM " SQLITE_UNDO_TABLE
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
		"WHEN (SELECT undoable_active())=1 " 
		"BEGIN "
			"UPDATE " SQLITE_UNDO_TABLE " "
			"SET sql=sql ||"
				"'INSERT INTO %s(rowid,%s) "
				"VALUES('||OLD.rowid||'%s);' "
			"WHERE ROWID=("
				"SELECT MAX(ROWID) FROM " SQLITE_UNDO_TABLE 
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
		"WHEN (SELECT undoable_active())=1 " 
		"BEGIN "
			"UPDATE " SQLITE_UNDO_TABLE " "
			"SET sql=sql || "
				"'DELETE FROM %s "
				"WHERE rowid='||NEW.rowid||';' "
			"WHERE ROWID=("
				"SELECT MAX(ROWID) FROM " SQLITE_UNDO_TABLE
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

		if (rc != SQLITE_ROW) {
			break;
		}

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
	int rc;
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
			"FROM " SQLITE_UNDO_TABLE " "
			"WHERE status=?",
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

static void sqlite_undo_undoable_begin(sqlite3_context *context, int argc,
			sqlite3_value **argv)
{
	int rc;
	sqlite3 *db;

	if (sqlite_undo_undoable_active_flag != 0) {
		sqlite3_result_error(context,
				SQLITE_UNDO_ERRMSG_UNDOABLE_ACTIVE, -1);
		return;
	}

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_exec(db,
			"SAVEPOINT " SQLITE_UNDO_SAVEPOINT_UNDOABLE ";"
			/* Delete redos */
			"DELETE FROM " SQLITE_UNDO_TABLE " WHERE status='R';"
			/* Prepare for an undo entry */
			"INSERT INTO " SQLITE_UNDO_TABLE "(sql, status) "
			"VALUES('','U');",
			NULL, NULL, NULL);

	if (rc != SQLITE_OK) {
		sqlite3_exec(db,
			"ROLLBACK TO SAVEPOINT " SQLITE_UNDO_SAVEPOINT_UNDOABLE,
			NULL, NULL, NULL);
		sqlite3_result_error_code(context, rc);
	}
	else {
		sqlite_undo_undoable_active_flag = 1;
	}

	sqlite3_exec(db,"RELEASE SAVEPOINT " SQLITE_UNDO_SAVEPOINT_UNDOABLE,
			NULL, NULL, NULL);

}

static  sqlite_undo_undoable_active(sqlite3_context *context, int argc,
				sqlite3_value **argv)
{
	sqlite3_result_int(context, sqlite_undo_undoable_active_flag);
}

static void sqlite_undo_undoable_end(sqlite3_context *context, int argc,
						sqlite3_value **argv)
{
	char *result;
	sqlite3 *db;

	if (sqlite_undo_undoable_active_flag) {
		db = sqlite3_context_db_handle(context);

		result = sqlite3_mprintf("UNDO=%lld\nREDO=%lld",
				sqlite_undo_get_buffer_status(db, 'U'),
				sqlite_undo_get_buffer_status(db, 'R'));

		sqlite3_result_text(context, result, -1, sqlite3_free);	

		sqlite_undo_undoable_active_flag = 0;
	}
	else {
		sqlite3_result_error(context,
				SQLITE_UNDO_ERRMSG_UNDOABLE_NOT_ACTIVE, -1);
		return;
	}
}

static int sqlite_undo_step_get_transaction_rowid(sqlite3 *db, char U_R,
						sqlite_int64 *rowid)
{
	int rc;
	sqlite3_stmt *stmt;

	assert(db);
	assert(U_R == 'U' || U_R == 'R');
	assert(rowid);

	rc =  sqlite3_prepare_v2(db, 
			"SELECT max(rowid) FROM " SQLITE_UNDO_TABLE " "
			"WHERE status=?",
			-1, &stmt, NULL);


	sqlite3_bind_text(stmt, 1, &U_R, 1, SQLITE_STATIC);

	if (rc == SQLITE_OK) {
		rc = sqlite3_step(stmt);
	}

	if (rc == SQLITE_ROW) {
		*rowid = sqlite3_column_int64(stmt, 0);
	}

	sqlite3_finalize(stmt);

	return rc;
}

static int sqlite_undo_step_get_transaction_sql(sqlite3_context *context,
						sqlite_int64 rowid,
						char **sql)
{
	int rc;
	sqlite3 *db;
	sqlite3_stmt *stmt;

	assert(context);
	assert(rowid >= 0);

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_prepare_v2(db,
			"SELECT sql FROM " SQLITE_UNDO_TABLE " WHERE rowid=?",
			-1, &stmt, NULL);

	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, rowid);

		rc = sqlite3_step(stmt);

		if (rc == SQLITE_ROW) {
			*sql = sqlite3_mprintf("%s",
					sqlite3_column_text(stmt, 0));
			rc = SQLITE_OK;
		}
		else {
			*sql = NULL;
		}

		sqlite3_finalize(stmt);
	}

	return rc;
}

static int sqlite_undo_step_delete_transaction(sqlite3_context *context,
					sqlite_int64 rowid)
{
	int rc;
	sqlite3 *db;
	sqlite3_stmt *stmt;

	assert(context);
	assert(rowid > 0);

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_prepare_v2(db,
			"DELETE FROM " SQLITE_UNDO_TABLE " WHERE rowid=?",
			-1, &stmt, NULL);

	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, rowid);

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
			"INSERT INTO " SQLITE_UNDO_TABLE "(sql, status) "
			"VALUES('',?)",
			-1, &stmt, NULL);

	if (rc == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, &U_R, 1, NULL);

		rc = sqlite3_step(stmt);

		sqlite3_finalize(stmt);
	}

	return rc;
}

static void sqlite_undo_do(sqlite3_context *context,
			SqliteUndoOrRedo undo_or_redo)
{
	int rc;
	sqlite_int64 rowid = -1LL;
	char *sql = NULL, *result;
	sqlite3 *db;

	assert(context);
	assert(undo_or_redo == SqliteUndoUndo || 
		undo_or_redo == SqliteUndoRedo);

	db = sqlite3_context_db_handle(context);

	rc = sqlite_undo_step_get_transaction_rowid(db, 
			undo_or_redo == SqliteUndoUndo ? 'U' : 'R', &rowid);

	if (rc != SQLITE_ROW) {
		goto result_error;
	}
	
	if (rowid == 0LL) {
		sqlite3_result_null(context);
		return;
	}

	rc = sqlite_undo_step_get_transaction_sql(context, rowid, &sql);
	if (rc != SQLITE_OK) {
		goto result_error;
	}

	if(sql == NULL) {
		sqlite3_result_null(context);
		return;
	}

	rc = sqlite3_exec(db, "SAVEPOINT " SQLITE_UNDO_SAVEPOINT_UNDO,
			NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
		goto result_error;
	}

	rc = sqlite_undo_step_delete_transaction(context, rowid);
	if (rc != SQLITE_DONE) {
		goto rollback;
	}

	rc = sqlite_undo_step_prep_log(context,
				undo_or_redo == SqliteUndoUndo ? 'R' : 'U');
	if (rc != SQLITE_DONE) {
		goto rollback;
	}

	sqlite_undo_undoable_active_flag = 1; /* Capture mirror undo/redo */
	rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
	sqlite_undo_undoable_active_flag = 0;

	if (rc != SQLITE_OK) {
		goto rollback;
	}

	sqlite3_exec(db, "RELEASE SAVEPOINT " SQLITE_UNDO_SAVEPOINT_UNDO,
		NULL, NULL, NULL);

	result = sqlite3_mprintf("UNDO=%lld\nREDO=%lld\nSQL=%s",
			sqlite_undo_get_buffer_status(db, 'U'),
			sqlite_undo_get_buffer_status(db, 'R'),
			sql);

	sqlite3_free(sql);

	sqlite3_result_text(context, result, -1, sqlite3_free);

	return;

rollback:
	sqlite3_exec(db,
		"ROLLBACK TO SAVEPOINT " SQLITE_UNDO_SAVEPOINT_UNDO ";"
		"RELEASE SAVEPOINT " SQLITE_UNDO_SAVEPOINT_UNDO,
		NULL, NULL, NULL);
result_error:
	sqlite3_result_error_code(context, rc);
	sqlite3_free(sql);
}

static void sqlite_undo_undo(sqlite3_context *context, int argc,
			sqlite3_value **argv)
{
	sqlite_undo_do(context, SqliteUndoUndo);
}

static void sqlite_undo_redo(sqlite3_context *context, int argc,
			sqlite3_value **argv)
{
	sqlite_undo_do(context, SqliteUndoRedo);
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
		{"undoable_active", 0, sqlite_undo_undoable_active},
		{"undoable_begin", 0, sqlite_undo_undoable_begin},
		{"undoable_end",   0, sqlite_undo_undoable_end},
		{"undo", 0, sqlite_undo_undo},
		{"redo", 0, sqlite_undo_redo},
		{NULL}
	};

	rc = sqlite3_exec(db,
		"CREATE TEMP TABLE " SQLITE_UNDO_TABLE
			"(sql TEXT, status TEXT)",
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
