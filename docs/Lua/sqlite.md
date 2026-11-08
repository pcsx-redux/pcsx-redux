# SQLite

`PCSX.SQLite` is a binding to [SQLite](https://sqlite.org/), built into PCSX-Redux. It is meant for scripts that need to keep structured data around between sessions, such as notes, symbol tables, or translation work, without writing their own file format.

```lua
local db = PCSX.SQLite.open('notes.db')
db:exec 'CREATE TABLE IF NOT EXISTS notes (address INTEGER PRIMARY KEY, text TEXT)'
db:run('INSERT OR REPLACE INTO notes VALUES (?, ?)', 0x80010000, 'entry point')
for row in db:rows('SELECT address, text FROM notes WHERE address >= ?', 0x80000000) do
    print(string.format('%08x %s', row.address, row.text))
end
db:close()
```

Every failure raises a Lua error. The message is SQLite's own, followed by its extended result code in parentheses, for example `sqlite: UNIQUE constraint failed: notes.address (2067)`.

## Opening a database

`PCSX.SQLite.open([path [, options]])` opens or creates the database at `path` and returns a database object. Without a path, or with `':memory:'`, the database lives in memory and disappears when it is closed. `path` may also be a [URI filename](https://sqlite.org/uri.html). `options` is a table; `readOnly = true` opens an existing database without write access.

`PCSX.SQLite.version()` returns the SQLite version string.

## Database objects

- `db:exec(sql)` runs one or more statements separated by semicolons. It takes no parameters and discards any rows.
- `db:run(sql, ...)` prepares one statement, binds the extra arguments to its parameters in order, runs it to completion, and returns the number of rows it changed.
- `db:rows(sql, ...)` prepares one statement, binds the extra arguments, and returns an iterator over its result rows. Each row is a table keyed by column name. A loop that exits early keeps the statement, and on a file its read lock, until the garbage collector finalizes it.
- `db:first(sql, ...)` returns the first result row as a table keyed by column name, or `nil` if there is none, and finalizes the statement.
- `db:prepare(sql)` returns a statement object, described below.
- `db:transaction(fn, ...)` calls `fn(...)` inside a transaction. If `fn` returns, the transaction is committed and its return values are returned. If `fn` raises an error, the transaction is rolled back and the error is raised again. If the commit itself fails, for example on a deferred foreign key, the transaction is rolled back and the error is raised. Called inside another transaction, it uses a savepoint, so only the inner part is rolled back. If SQLite ends the transaction on its own, as `INSERT OR ROLLBACK` does, every statement until `transaction()` returns raises an error, and so does `transaction()`.
- `db:changes()` is the number of rows changed by the most recent statement.
- `db:lastInsertRowId()` is the rowid of the most recent insertion.
- `db:busyTimeout(ms)` sets how long a statement waits for a lock held by another connection before failing.
- `db:close()` closes the database. A database that is garbage collected is closed too, but closing it explicitly releases the file at a known point.

## Statements

- `stmt:bind(index, value)` binds one parameter. `index` is the 1-based position of the parameter, or its name including the prefix, such as `':name'`. It returns the statement, so calls can be chained.
- `stmt:bindAll(...)` binds every parameter in order. The number of values must match the number of parameters.
- `stmt:step()` returns `true` when a row is available and `false` when the statement is done.
- `stmt:column(index)`, `stmt:columnType(index)` and `stmt:columnName(index)` read the current row. Column indices are 1-based. `columnType` returns one of `'integer'`, `'float'`, `'text'`, `'blob'` or `'null'`.
- `stmt:row()` returns the current row as a table keyed by column name, and `stmt:values()` as an array.
- `stmt:columnCount()` is the number of columns in the result.
- `stmt:reset()` rewinds the statement so it can run again, and `stmt:clearBindings()` sets every parameter back to `NULL`.
- `stmt:finalize()` releases the statement. Once its database is closed, finalizing is the only thing a statement can still do.

## Values

When binding, `nil` becomes `NULL`, a boolean becomes the integer 0 or 1, a number with no fractional part that fits in 64 bits becomes an integer, any other number becomes a float, and a string becomes text. A 64-bit integer cdata is bound as an integer. To store a string as a blob, wrap it with `PCSX.SQLite.blob(string)`.

When reading, integers come back as Lua numbers when they fit exactly in a double, and as 64-bit integer cdata otherwise. Text and blobs both come back as Lua strings, which may contain any byte; `columnType` tells them apart. `NULL` comes back as `nil`.
