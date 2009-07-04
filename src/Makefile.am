AM_CFLAGS = -Wall

INCLUDES = @CFLAGS@ @sqlite_CFLAGS@

lib_LTLIBRARIES = sqlite-undo.la
sqlite_undo_la_SOURCES = sqlite_undo.c
sqlite_undo_la_LDFLAGS = @sqlite_LIBS@ -avoid-version -module -disable-static
