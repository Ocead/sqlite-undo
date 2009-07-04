/*
 *      sqlite_undo_extension.c
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

#include <assert.h>
#include <sqlite3ext.h>
#include <stdlib.h>

#ifndef NDEBUG
#include <string.h>
#endif

SQLITE_EXTENSION_INIT1

typedef enum {
	UpdateTypeNone,
	UpdateTypeTable,
	UpdateTypeColumn
} UpdateType;

typedef struct {
	char *name;
	int nargs;
	void *func;
} Function;

struct _column {
	int pk;
	char *name;
	struct _column *next;
};

typedef struct _column Column;

static const char *error_message[] = {
	"Table name must be a text string",
	"Invalid update_type. Valid values:\n"
		"0: None\n"
		"1: Table\n"
		"2: Column",
	"Failed to create triggers",
	"SQL must be a text string",
	"A rollback occurred"
};

enum {
	ERRMSG_TABLE_MUST_BE_TEXT,
	ERRMSG_INVALID_UPDATE_TYPE,
	ERRMSG_CREATE_TRIGGER_FAILED,
	ERRMSG_SQL_MUST_BE_TEXT,
	ERRMSG_ROLLBACK_OCCURRED
};

static Column *column_new(int pk, const char *name)
{
	Column *c;

	assert(name);

	c = sqlite3_malloc(sizeof(Column));
	if (c) {
		c->pk = pk;
		c->name = sqlite3_mprintf("%s", name);
		c->next = NULL;
	}

	return c;
}

static void columns_free(Column *head)
{
	Column *c, *n;

	c = head;
	
	while (c) {
		sqlite3_free(c->name);
		n = c->next;
		sqlite3_free(c);
		c = n;
	}
}

static Column *column_get_list(sqlite3 *db, char *table)
{
	int rc;
	char *sql;
	sqlite3_stmt *stmt;
	Column *head = NULL, *c, *p = NULL;

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

	if (rc == SQLITE_OK) {
		while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
			if (rc == SQLITE_ROW) {
				c = column_new(
							sqlite3_column_int(stmt, 5),
							(char*)sqlite3_column_text(stmt, 1));

				if (!head) { 
					head = c;
				}
				if (p) {
					p->next = c;
				}
				p = c;
			}
			else {
				break;
			}
		}
	}

	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		columns_free(head);
		head = NULL;
	}

	return head;
}

static char *add_where_clause(char *column, char *where,
							char *new_or_old)
{
	char *r;

	assert(column);
	assert(new_or_old);

	if (where) {
		r = sqlite3_mprintf("%s AND %s='||quote(%s.%s)||'", where, column,
						new_or_old, column);
		sqlite3_free(where);
	}
	else {
		r = sqlite3_mprintf(" WHERE %s='||quote(%s.%s)||'", column, new_or_old,
						column);
	}

	return r;
}

static char *add_delete_column(char *columns, char *column)
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

static char *add_delete_value(char *values, char *column)
{
	char *r;

	assert(column);

	if (values) {
		r = sqlite3_mprintf("%s,'||quote(OLD.%s)||'", values, column);
		sqlite3_free(values);
	}
	else {
		r = sqlite3_mprintf("'||quote(OLD.%s)||'", column);
	}

	return r;
}

static char *add_update_column(char *columns, char *column)
{
	char *r;

	assert(column);
		
	if (columns) {
		r = sqlite3_mprintf(",%s='||quote(OLD.%s)||'", column, column);
		sqlite3_free(columns);
	}
	else {
		r = sqlite3_mprintf("%s='||quote(OLD.%s)||'", column);
	}

	return r;
}

static char *append_update_column_trigger(char *table_triggers,
								char *table, char *column, char *where)
{
	char *r;

	assert(table);
	assert(column);
	assert(where);

	r = sqlite3_mprintf(
		"%s"
		"CREATE TEMP TRIGGER _u_%s_u_%s AFTER UPDATE OF %s ON %s " 
		"BEGIN "
			"INSERT INTO _undo VALUES("
				"'UPDATE %s SET %s='||quote(OLD.%s)||'%s');"
		"END;",
		table_triggers ? table_triggers : "",
		table, column, column, table,
		table, column, column, where);


	sqlite3_free(table_triggers);

	return r;
}

static char *prepend_update_table_trigger(char *table_triggers,
								char *table, char *columns,
								char *where)
{
	char *r;

	assert(table);
	assert(columns);
	assert(where);

	r = sqlite3_mprintf(
		"%s"
		"CREATE TEMP TRIGGER _u_%s_u AFTER UPDATE ON %s "
		"BEGIN "
			"INSERT INTO _undo VALUES('UPDATE %s SET %s%s');"
		"END;",
		table_triggers ? table_triggers : "",
		table, table,
		table, columns, where);

	sqlite3_free(table_triggers);

	return r;			
}

static char *prepend_delete_trigger(char *table_triggers, char *table,
								char *columns, char *values)
{
	char *r;

	assert(table);
	assert(columns);
	assert(values);

	r = sqlite3_mprintf(
		"CREATE TEMP TRIGGER _u_%s_d BEFORE DELETE ON %s "
		"BEGIN "
			"INSERT INTO _undo VALUES('INSERT INTO %s(%s) VALUES(%s)');"
		"END;"
		"%s",
		table, table,
		table, columns, values,
		table_triggers ? table_triggers : "");

	sqlite3_free(table_triggers);

	return r;
}

static char *prepend_insert_trigger(char *table_triggers, char *table,
								char *where)
{
	char *r;

	assert(table);
	assert(where);

	r = sqlite3_mprintf(
		"CREATE TEMP TRIGGER _u_%s_i AFTER INSERT ON %s "
		"BEGIN "
			"INSERT INTO _undo VALUES('DELETE FROM %s%s');"
		"END;"
		"%s",
		table, table,
		table, where,
		table_triggers ? table_triggers : "");

	sqlite3_free(table_triggers);

	return r;			
}

static char *get_table_undo_triggers(sqlite3 *db, char *table,
								UpdateType update_type)
{
	Column *head, *c;
	char *new_where = NULL, *old_where = NULL, 
		*delete_column_names = NULL, *delete_values = NULL,
		*update_columns = NULL,
		*table_triggers = NULL;

	assert(db);
	assert(table);
	assert((update_type == UpdateTypeNone) ||
		(update_type == UpdateTypeTable) ||
		(update_type == UpdateTypeColumn));

	head = column_get_list(db, table);
	if (!head) {
		return NULL;
	}

	c = head;
	while (c) {
		delete_column_names = add_delete_column(delete_column_names,
											c->name);
		delete_values = add_delete_value(delete_values, c->name);
		if (c->pk) {
			new_where = add_where_clause(c->name, new_where, "NEW");
			if (update_type != UpdateTypeNone) {
				old_where = add_where_clause(c->name, old_where, "OLD");
			}
		}
		c = c->next;
	}

	if (update_type != UpdateTypeNone) {
		c = head;
		while (c) {
			if (!c->pk) {
				if (update_type == UpdateTypeColumn) {
					table_triggers = append_update_column_trigger(
							table_triggers,	table, c->name, old_where);
				}
				else if (update_type == UpdateTypeTable) {
					update_columns = add_update_column(update_columns,
													c->name);
				}
			}
			c = c->next;
		}
	}

	if (update_type == UpdateTypeTable) {
		table_triggers = prepend_update_table_trigger(table_triggers,
								table, update_columns, old_where);
	}

	columns_free(head);

	table_triggers = prepend_delete_trigger(table_triggers, table,
									delete_column_names, delete_values);

	table_triggers = prepend_insert_trigger(table_triggers, table,
										new_where);

	sqlite3_free(new_where);
	sqlite3_free(old_where);
	sqlite3_free(delete_column_names);
	sqlite3_free(delete_values);
	sqlite3_free(update_columns);

	return table_triggers;
}


static void undoable_table(sqlite3_context *context, int argc,
						sqlite3_value **argv)
{
	int rc;;
	UpdateType update_type;
	char *table, *triggers;
	sqlite3 *db;

	if ((sqlite3_value_type(argv[0]) != SQLITE_TEXT)) {
		sqlite3_result_error(context,
						error_message[ERRMSG_TABLE_MUST_BE_TEXT], -1);
		return;		
	}	

	update_type = (UpdateType)sqlite3_value_int(argv[1]);

	if ((update_type != UpdateTypeNone) && 
		(update_type != UpdateTypeTable) &&
		(update_type != UpdateTypeColumn)) {
	
		sqlite3_result_error(context,
						error_message[ERRMSG_INVALID_UPDATE_TYPE], -1);
		return;	
	
	}

	table = (char*)sqlite3_value_text(argv[0]);

	db = sqlite3_context_db_handle(context);

	triggers = get_table_undo_triggers(db, table, update_type);

	if (!triggers) {
		sqlite3_result_error(context,
					error_message[ERRMSG_CREATE_TRIGGER_FAILED], -1);
		return;
	}		

	rc = sqlite3_exec(db, triggers, NULL, NULL, NULL);

	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
	}

	sqlite3_free(triggers);
}

static sqlite_int64 get_buffer_status(sqlite3 *db, char U_R)
{
	int rc;
	sqlite_int64 c = -1LL;
	sqlite3_stmt *stmt;

	assert(db);
	assert(U_R == 'U' || U_R == 'R');

	rc =  sqlite3_prepare_v2(db,
						"SELECT count(*) "
						"FROM _undo "
						"WHERE s=?", -1, &stmt, NULL);

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

static int undoable_begin_do(sqlite3_context *context,
								int argc, sqlite3_value **argv)
{
	int rc;
	sqlite3 *db;

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_exec(db, 
				"BEGIN;"
				/* Delete redos */
				"DELETE FROM _undo "
				"WHERE rowid IN ("
					"SELECT rowid FROM _redo_row_ids"
				");"
				/* Prepare for an undo entry */
				"INSERT INTO _undo(s) VALUES('U');", NULL, NULL, NULL);

	
	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
		rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
		return 0;
	}
	
	return 1;
}

static void undoable_begin(sqlite3_context *context,
						int argc, sqlite3_value **argv)
{
	undoable_begin_do(context, argc, argv);
}

static void undoable_end(sqlite3_context *context, int argc,
						sqlite3_value **argv)
{
	char *result;
	sqlite3 *db;

	db = sqlite3_context_db_handle(context);

	/* 
	 * sqlite3_get_autocommit returns 0 if inside a transaction. The
	 * only way we cannot be in a transaction here is if a commit or
	 * rollback has been issued between undoable_begin() and
	 * undoable_end(). This is not allowed, so assume a commit or 
	 * or rollback has occurred if sqlite_get_autocommit returns 
	 * non-zero
	 */
	if (sqlite3_get_autocommit(db)) {
		sqlite3_result_error(context,
						error_message[ERRMSG_ROLLBACK_OCCURRED], -1);
		return;
	}

	sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

	result = sqlite3_mprintf("UNDO=%lld\nREDO=%lld",
					get_buffer_status(db, 'U'),
					get_buffer_status(db, 'R'));

	sqlite3_result_text(context, result, -1, sqlite3_free);
}

static void undoable(sqlite3_context *context, int argc,
				sqlite3_value **argv)
{
	int rc;
	char *query;
	sqlite3 *db;

	if ((sqlite3_value_type(argv[0]) != SQLITE_TEXT)) {
		sqlite3_result_error(context,
							error_message[ERRMSG_SQL_MUST_BE_TEXT], -1);
		return;		
	}

	query = (char*)sqlite3_value_text(argv[0]);

	if(!undoable_begin_do(context, argc, argv)) { 
		return;
	}

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_exec(db, query, NULL, NULL, NULL);

	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
		return;
	}

	undoable_end(context, argc, argv);
}

static int step_get_transaction_bounds(sqlite3 *db, char *un_re,
							sqlite_int64 *tstart, sqlite_int64 *tend)
{
	int rc;
	char *sql;
	sqlite3_stmt *stmt;

	assert(db);
	assert((strcmp(un_re,"un") == 0) || 
		(strcmp(un_re, "re") == 0));
	assert(tstart);
	assert(tend);

	sql = sqlite3_mprintf(
					"SELECT tstart,tend FROM _%sdo_stack_top", un_re);

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

	if (rc == SQLITE_ROW) {
		return 1;
	}

	return 0;
}

static char *step_get_transaction_sql(sqlite3_context *context,
								sqlite_int64 tstart, sqlite_int64 tend)
{
	int rc;
	char *sql = NULL, *s;
	sqlite3 *db;
	sqlite3_stmt *stmt;

	assert(context);
	assert(tstart >= 0);
	assert(tend >= 0);

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_prepare_v2(db,
			"SELECT s FROM _undo WHERE rowid>? AND rowid<=?", -1,
			&stmt, NULL);

	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
		return NULL;
	}

	sqlite3_bind_int64(stmt, 1, tstart);
	sqlite3_bind_int64(stmt, 2, tend);

	while ((rc = sqlite3_step(stmt)) != SQLITE_DONE) {
		if (rc == SQLITE_ROW) {
			if (!sql) {
				sql = sqlite3_mprintf("%s", 
									sqlite3_column_text(stmt, 0));
			}
			else {
				s = sql;
				sql = sqlite3_mprintf("%s;%s",
									s,sqlite3_column_text(stmt, 0));
				sqlite3_free(s);
			}
		}
		else {
			break;
		}
	}

	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		sqlite3_result_error_code(context, rc);
		sqlite3_free(sql);
		sql = NULL;
	}

	return sql;
}

static int step_delete_transaction(sqlite3_context *context,
								sqlite_int64 tstart,
								sqlite_int64 tend)
{
	int rc;
	char *sql;
	sqlite3 *db;

	assert(context);
	assert(tstart >= 0);
	assert(tend >= 0);

	db = sqlite3_context_db_handle(context);

	sql = sqlite3_mprintf(
				"DELETE FROM _undo WHERE rowid>=%lld AND rowid<=%lld",
				tstart, tend);

	rc = sqlite3_exec(db, sql, NULL, NULL, NULL);

	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
		sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
		return 0;
	}

	return 1;
}

static int step_prep_log(sqlite3_context *context, char U_R)
{
	int rc;
	char *sql;
	sqlite3 *db;

	assert(context);
	assert(U_R == 'U' || U_R == 'R');

	db = sqlite3_context_db_handle(context);

	sql = sqlite3_mprintf(
				"INSERT INTO _undo(s) VALUES('%c')", U_R);

	rc = sqlite3_exec(db, sql, NULL, NULL, NULL);

	sqlite3_free(sql);

	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
		sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);

		return 0;
	}

	return 1;
}

static int step_exec(sqlite3_context *context, char *sql)
{
	int rc;
	sqlite3 *db;

	assert(context);
	assert(sql);

	db = sqlite3_context_db_handle(context);

	rc = sqlite3_exec(db, sql, NULL, NULL, NULL);

	if (rc != SQLITE_OK) {
		sqlite3_result_error_code(context, rc);
		sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);

		return 0;
	}

	return 1;	
}

static void step(sqlite3_context *context, char *un_re, char U_R)
{
	sqlite_int64 tstart = -1LL, tend = -1LL;
	char *sql = NULL, *result;
	sqlite3 *db;

	assert(context);
	assert((strcmp(un_re,"un") == 0) || 
		(strcmp(un_re, "re") == 0));
	assert(U_R == 'U' || U_R == 'R');

	db = sqlite3_context_db_handle(context);

	if (!step_get_transaction_bounds(db, un_re, &tstart, &tend)) {
		return;
	}

	sql = step_get_transaction_sql(context, tstart, tend);
	if (!sql) {
		goto result_error;
	}

	if (!step_delete_transaction(context, tstart, tend)) {
		goto result_error;
	}

	if (!step_prep_log(context, U_R)) {
		goto result_error;
	}

	if (!step_exec(context, sql)) {
		goto result_error;
	}

	result = sqlite3_mprintf("UNDO=%lld\nREDO=%lld\nSQL=%s",
					get_buffer_status(db, 'U'),
					get_buffer_status(db, 'R'),
					sql);

	sqlite3_result_text(context, result, -1, sqlite3_free);

result_error:
	sqlite3_free(sql);
}

static void undo(sqlite3_context *context, int argc,
						sqlite3_value **argv)
{
	step(context, "un", 'R');
}

static void redo(sqlite3_context *context, int argc,
						sqlite3_value **argv)
{
	step(context, "re", 'U');
}

int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg,
						const sqlite3_api_routines *pApi)
{
 	int rc;
	Function *function;
	static Function functions[] = {
		{"undoable_table", 2, undoable_table},
		{"undoable", 1, undoable},
		{"undoable_begin", 0, undoable_begin},
		{"undoable_end", 0, undoable_end},
		{"undo", 0, undo},
		{"redo", 0, redo},
		{NULL}
	};

	SQLITE_EXTENSION_INIT2(pApi)

	rc = sqlite3_exec(db,
				"CREATE TEMP TABLE _undo(s TEXT);"

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

						NULL, NULL, pzErrMsg);

	if (rc != SQLITE_OK) {
		return 1;
	}

	function = functions;

	while (function->name) {
		rc = sqlite3_create_function(db, function->name,
								function->nargs, SQLITE_UTF8, NULL,
								function->func, NULL, NULL);

		if (rc != SQLITE_OK) {
			*pzErrMsg = sqlite3_mprintf("Failed to create %s: %s",
								function->name, sqlite3_errmsg(db));
			return 1;
		}
		function++;
	}

	return 0;
}
