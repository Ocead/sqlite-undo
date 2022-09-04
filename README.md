# sqlite-undo

This is an extension for SQLite3 implementing support for Undo/Redo operatations on databases.
This project is based on [https://sourceforge.net/projects/sqlite-undo/](https://sourceforge.net/projects/sqlite-undo/) and extends it by `UPDATE` support. The original README of the project follows:

				sqlite-undo

	Authors
	-----------
	Simon Naunton		<snaunton@gmail.com>
	Alexey Pechnikov	<pechnikov@mobigroup.ru>


	What is it?
	-----------

	sqlite-undo is an extension for sqlite with the aim of providing "drop in"
	undo/redo functionality for databases used by single user applications or
	possibly multi-user applications where users use separate data sets.

	Many of the design ideas were cribbed from:

	http://www.sqlite.org/cvstrac/wiki?p=UndoRedo

	The incredibly awesome sqlite database lives here:

	http://www.sqlite.org


	The Latest Version
	------------------

	Details of the latest version can be found on the sqlite-undo sourceforge
	project page:

	https://sourceforge.net/projects/sqlite-undo/


	Documentation
	-------------

	sqlite-undo is an extension to sqlite3 which allows transactional undo/redo
	functionality to be either compiled directly into sqlite3 or to be loaded at
	runtime using the load_extension functionality of sqlite3. Note that
	sqlite-undo is licensed under the GPL v3 while sqlite is in the public domain.
	This will affect how you are able to distribute a package where sqlite-undo
	is statically linked to sqlite. Please see the file called LICENSE or refer
	to another copy of the GPL v3 license for further information.

	If using the loadable extension (sqlite-undo.so), the first step in enabling 
	undo functionality is to load the extension. Currently, this is done like so:

	SELECT load_extension('/path/to/sqlite-undo.so');

	See the sqlite documentation at http://www.sqlite.org/lang_corefunc.html for
	further information on using the load_extension functionality. It uses the 
	default entry point so does not need an entry point specified explicitly.

	The second step, or first step if statically linked is to make all tables that
	require undo functionality undoable. This is done using the undoable() function
	which has the following syntax:

	undoable_table('x', y)

	x: The name of the table to make undoable. This is a text parameter, so must be 
	enclosed in quotes.
	y: This is an integer parameter which must be either 0, 1 or 2. It defines how
	the triggers used to undo UPDATEs to the table are defined.
	- 0: Use this if no UPDATEs are going to be made against the table.
	- 1: Use this if UPDATEs are usually made on the whole table or several
			columns at a time.
	- 2: Use this if UPDATEs are usually made one column at a time.

	1 and 2 above will work for all UPDATEs against the table, but are more
	effecient for their respective usages noted above.

	NOTE: As primary keys should not be UPDATEed, all columns defined as 
	primary keys will be ignored and will not be undoable for UPDATEs.

	Example:

	> CREATE TABLE Test(id INTEGER PRIMARY KEY AUTOINCREMENT, data TEXT);
	> SELECT undoable_table('Test', 2);

	Initialization is now complete.

	To make a transaction undoable all sql statements need to be executed between
	calls to undoable_begin() and undoable_end().

	These functions signify the beginning and end of an undoable transaction. All
	SQL statements executed between them will be considered part of the same
	transaction. undoable_end() returns a text string containing the number of
	transactions the can be undone and the number of transactions that can be redone
	as a text string or an sqlite error code if an error occurred while executing
	SQL internally. In addition if undoable_begin() is called while an undoable
	transaction is already active or undoable_end() is called when no undoable
	transaction is active, SQLITE_ERROR and a textual error message is
	returned.

	Example:

	> SELECT undoable_begin();
	> INSERT INTO Test(data) VALUES('Hello');
	> INSERT INTO Test(data) VALUES('Goodbye');
	> SELECT undoable_end();
	UNDO=1
	REDO=0

	This executes the two INSERT statements making them undoable.
	SELECT undoable_end() returns a string showing that there is one transaction
	that can be undone and no transactions that can be redone.

	To execute an undo or redo the following functions are used:

	undo()
	redo()

	On success, both functions return a text string containing the number of
	transactions the can be undone, the number of transactions that can be redone
	and the SQL used to undo or redo the transaction as a text string.

	If there is nothing to redo or undo NULL is returned.

	If an error occurs an sqlite error code is returned.

	Example:

	> SELECT undoable_begin();
	> INSERT INTO Test(data) VALUES('Hello');
	> INSERT INTO Test(data) VALUES('Goodbye');
	> SELECT undoable_end();
	UNDO=1
	REDO=0
	> SELECT * FROM Test;
	Hello
	Goodbye
	> SELECT undo();
	UNDO=0
	REDO=1
	SQL=DELETE FROM test WHERE rowid=1;DELETE FROM test WHERE rowid=2
	> SELECT * FROM Test;
	> SELECT redo();
	UNDO=1
	REDO=0
	SQL=INSERT INTO test(rowid,data) VALUES(1,'Hello');\
	INSERT INTO test(rowid,data) VALUES(2,'Goodbye')
	> SELECT * FROM Test;
	Hello
	Goodbye

	There is also a function undoable_active(). This is used intternally to
	determine whether an undoable transaction is currently active, or not, and is 
	not intended to be used directly. It returns an integer value of 1 if an undo
	transaction is currently active or 0 if not.

	When loaded or enabled sqlite undo creates a temporary table _sqlite_undo. This
	table should not be used directly.


	Installation
	------------

	sqlite-undo has only been tested on Linux against sqlite versions 3.6.10 and 
	3.6.16. These instructions are therefore for linux only.

	sqlite-undo should compile and run against any version of sqlite with support
	for SAVEPOINTs and loadable extensions.

	To compile sqlite-undo as a loadable module, use:

	gcc -DNDEBUG -shared -o sqlite-undo.so sqlite-undo.c

	If gcc cannot find sqlite3ext.h, either make sure that you have this file 
	installed in one of the usual locations, usually /usr/include or 
	/usr/local/include, or if sqlite3ext.h is installed in a different location,
	you can use the following replacing /path/to with the directory where 
	sqlite3ext.h is located:

	gcc -DNDEBUG -I/path/to -shared -o sqlite-undo.so sqlite-undo.c

	After compilation sqlite_undo.so can be copied to wherever it is needed.

	sqlite-undo can be easily compiled statically into an sqlite amalgamation.

	Edit sqlite3.c from the amalgamation and enter two lines at the end of the file:

	/************** Begin file sqlite_undo.c *************************************/
	/************** End of sqlite_undo.c *****************************************/

	Copy the ENTIRE contents of sqlite_undo.c and paste it in between these lines.

	Find the following code in sqlite3.c:

	#ifdef SQLITE_ENABLE_FTS3
	if( !db->mallocFailed && rc==SQLITE_OK ){
		rc = sqlite3Fts3Init(db);
	}
	#endif

	And add the following code directly below it:

	#ifdef SQLITE_ENABLE_UNDO
	if( !db->mallocFailed && rc==SQLITE_OK){
		rc = sqlite3UndoInit(db);
	}
	#endif

	SQLITE_ENABLE_UNDO must be defined at compile time for sqlite-undo to be
	compiled. There are several ways to do this, but the easiest (though least
	sophisticated) way of doing this is to find the following line at the top of 
	sqlite3.c:

	#define SQLITE_CORE 1

	And add the following line directly above or below it:

	#define SQLITE_ENABLE_UNDO 1


	Licensing
	---------

	sqlite-undo is licensed under the GPL V.3. Please see the file called LICENSE.
