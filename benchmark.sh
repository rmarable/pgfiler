#! /bin/sh

tree=${1:-/usr/bin}
dbname=${2:-pgfiler_bench}

if createdb $dbname; then
	psql -d $dbname <<:EOF:
		CREATE TABLE file_table (
			filename text primary key,
			contents binary not null
		);
:EOF:
else
	echo recommend you "'dropdb $dbname'"
	exit
fi

find $tree -type f -print | while read filename; do
	pgfiler -d $dbname insert file_table filename "$filename" contents $filename
	echo -n .
done

exit
