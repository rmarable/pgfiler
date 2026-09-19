#! /bin/sh

set -e
PATH=.:$PATH
tree=${1:-/usr/bin}
dbname=${2:-pgfiler_bench}

if createdb $dbname; then
	psql -d $dbname <<:EOF:
		CREATE TABLE file_table (
			filename text primary key,
			contents bytea not null
		);
:EOF:
else
	echo recommend you "'dropdb $dbname'"
	exit
fi

find $tree -type f -perm -4 -print | while read -r filename; do
	pgfiler -d $dbname -b upsert file_table \
		filename "$filename" \
		contents "$filename"
	echo -n .
done
echo ""

exit
