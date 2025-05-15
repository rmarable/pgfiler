/*
 * Copyright (c) 2025 by Paul Vixie
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND MAIL ABUSE PREVENTION SYSTEM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL MAIL ABUSE PREVENTION
 * SYSTEM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <fcntl.h>
#include <paths.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libpq-fe.h>

#define	PG_OID_BYTEA 17
#define	PG_OID_TEXT 25

#define	PG_FMT_TEXT 0
#define	PG_FMT_BINARY 1

#define	CHECK(expr) { if (expr) { perror(#expr); goto done; } }

typedef enum { Select, Upsert, Insert, Replace, Append } Op;

static const char *progname = "pgfiler";

static int	get(PGconn *, const char *, const char *, const char *,
		    const char *, const char *);
static int	put(PGconn *, const char *, const char *, const char *,
		    const char *, const char *, const char *, Op);

static const	char *tmpdir;
static int	tracelevel;
static bool	binary;

static void
usage(const char *msg) __attribute__((noreturn));

void
usage(const char *msg) {
	fprintf(stderr, "%s: usage error (%s)\n", progname, msg);
	fprintf(stderr,
		"usage:\n"
		"\t%s [-bdhMiPpTtux <v>] [-b]"
		" <op> <tbl> <kf> <k> <vf> [<file>]\n"
		"where:\n"
		"\t<op> is select|upsert|insert|replace|append;\n"
		"\t-d dbname; -h dbhost; -p dbport;\n"
		"\t-o pgoptions; -u pguser; -t pgtty; -P pgpasswd;\n"
		"\t-x tracelevel; -T tmpdir;\n"
		"example:\n"
		"\t%s select file_table filename 1.2.3.4 file_data\n",
		progname, progname);
	exit(1);
}

int
main(int argc, char *argv[]) {
	const char	*pghost = NULL, *pgport = NULL, *pgoptions = NULL,
			*pgtty = NULL, *pguser = NULL, *pgpasswd = NULL;
	const char	*opstr, *table, *kf, *k, *vf, *file, *tsf, *t;
	char		*dbname;
	int		status = 0, ch;
	PGconn		*conn;
	Op		op;

	if ((progname = strrchr(argv[0], '/')) != NULL)
		progname++;
	else if (argv[0] != NULL)
		progname = argv[0];

	if ((t = getenv("USER")) == NULL &&
	    (t = getenv("LOGNAME")) == NULL) {
		struct passwd *pw = getpwuid(getuid());

		if (pw == NULL) {
			perror("no default database");
			exit(1);
		}
		t = pw->pw_name;
	}
	dbname = strdup(t);

	if ((tmpdir = getenv("TMPDIR")) == NULL)
		tmpdir = _PATH_TMP;

	tsf = NULL;

	while ((ch = getopt(argc, argv, "bd:h:M:o:P:p:T:t:u:x:")) != -1) {
		switch (ch) {
		case 'b':
			binary = true;
			break;
		case 'd':
			dbname = optarg;
			break;
		case 'h':
			pghost = optarg;
			break;
		case 'M':
			tsf = optarg;
			break;
		case 'o':
			pgoptions = optarg;
			break;
		case 'P':
			pgpasswd = optarg;
			break;
		case 'p':
			pgport = optarg;
			break;
		case 'T':
			tmpdir = optarg;
			break;
		case 't':
			pgtty = optarg;
			break;
		case 'u':
			pguser = optarg;
			break;
		case 'x':
			tracelevel = atoi(optarg);
			break;
		default:
			usage("unrecognized argument");
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 5)
		usage("too few arguments");
	else if (argc > 6)
		usage("too many arguments");
	opstr = argv[0];
	table = argv[1];
	kf = argv[2];
	k = argv[3];
	vf = argv[4];
	file = (argc == 6) ? argv[5] : NULL;

	if (strcmp(opstr, "select") == 0) {
		op = Select;
	} else if (strcmp(opstr, "upsert") == 0) {
		op = Upsert;
	} else if (strcmp(opstr, "insert") == 0) {
		op = Insert;
	} else if (strcmp(opstr, "replace") == 0) {
		op = Replace;
	} else if (strcmp(opstr, "append") == 0) {
		op = Append;
	} else {
		usage("operation must be 'select', "
		      "'upsert', 'insert', "
		      "'replace', or 'append'");
	}

	conn = PQsetdbLogin(pghost, pgport, pgoptions, pgtty,
			    dbname, pguser, pgpasswd);
	if (PQstatus(conn) == CONNECTION_BAD) {
		fprintf(stderr, "%s: \"%s\": %s", progname, dbname,
			PQerrorMessage(conn));
		status = 1;
	} else {
		switch (op) {
		case Select:
			status = get(conn, table, kf, k, vf, file);
			break;
		case Upsert: /*FALLTHROUGH*/
		case Insert: /*FALLTHROUGH*/
		case Replace: /*FALLTHROUGH*/
		case Append:
			status = put(conn, table, kf, k, vf, file, tsf, op);
			break;
		default:
			abort();
		}
	}
	PQfinish(conn);
	free(dbname);
	dbname = NULL;
	return (status);
}

static int
get(PGconn *conn, const char *table, const char *kf, const char *v,
    const char *tf, const char *file)
{
	const Oid	paramTypes[] = { PG_OID_TEXT };
	const char	*paramValues[] = { v };
	int		fd = STDOUT_FILENO;
	char		*cmd = NULL;
	PGresult	*res = NULL;
	int		status = 1;
	char		*ptr;
	int		len;

	/* Open the output file if there is one. */
	if (file != NULL)
		CHECK((fd = open(file, O_WRONLY|O_CREAT)) < 0)

	/* Build the query. */
	CHECK((asprintf(&cmd, "SELECT %s FROM %s WHERE %s = $1",
			tf, table, kf)) < 0)

	/* Send the query. */
	if (tracelevel > 0) {
		fputc('{', stderr);
		fputs(cmd, stderr);
		fputs("}\n", stderr);
	}
	res = PQexecParams(conn, cmd, 1, paramTypes,
			   paramValues, NULL, NULL, binary);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		fprintf(stderr, "%s: \"%s\": %s", progname, cmd,
			PQresultErrorMessage(res));
		goto done;
	}
	if (PQnfields(res) != 1) {
		fprintf(stderr, "%s: \"%s\": %d fields?\n",
			progname, cmd, PQnfields(res));
		goto done;
	}
	if (PQntuples(res) != 1) {
		fprintf(stderr, "%s: \"%s\": %d tuples?\n",
			progname, cmd, PQntuples(res));
		goto done;
	}

	/* Output the result. */
	ptr = PQgetvalue(res, 0, 0);
	len = PQgetlength(res, 0, 0);
	write(fd, ptr, len);
	if (!binary && len > 0 && ptr[len-1] != '\n')
		write(fd, "\n", 1);
	status = 0;
 done:
	if (cmd != NULL)
		free(cmd);
	if (file != NULL)
		close(fd);
	if (res != NULL)
		PQclear(res);
	return (status);
}

static int
put(PGconn *conn, const char *table, const char *kf, const char *k,
    const char *vf, const char *file, const char *tsf, Op op)
{
	char		*cmd = NULL, *tmp, *ts = NULL, *ptr = NULL;
	PGresult	*res = NULL;
	int		fd = STDIN_FILENO;
	int		status = 1;
	struct stat	sb;
	size_t		len;
	Oid		paramTypes[10], *pt = paramTypes;
	const		char *paramValues[10], **pv = paramValues;
	int		paramLengths[10], *pl = paramLengths;
	int		paramFormats[10], *pf = paramFormats;

	/* Open the file if there is one. */
	if (file != NULL)
		CHECK((fd = open(file, O_RDONLY)) < 0)

	/*
	 * If it's not a regular file, make a copy for mmap, then switch.
	 */
	CHECK((fstat(fd, &sb)) < 0)
	if ((sb.st_mode & S_IFMT) != S_IFREG) {
		char buf[65536];
		size_t s;
		int tf;

		CHECK((asprintf(&tmp, "%s/pgfiler.XXXXXX", tmpdir)) < 0)
		CHECK((tf = mkstemp(tmp)) < 0)
		CHECK((unlink(tmp)) < 0)
		ts = strdup("'now'::TIMESTAMP");
		while ((s = read(fd, buf, sizeof buf)) > 0)
			write(tf, buf, s);
		close(fd);
		fd = tf;
		CHECK((fstat(fd, &sb)) < 0)
	} else {
		if (tsf != NULL)
			CHECK((asprintf(&ts, "to_timestamp(%lu)",
					(u_long) sb.st_mtime)) < 0)
	}

	/* Map it into virtual memory. */
	len = sb.st_size;
	CHECK((ptr = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0))
	      == MAP_FAILED)

	// $1 is the key
	*pt++ = PG_OID_TEXT;
	*pv++ = k;
	*pl++ = strlen(k);
	*pf++ = PG_FMT_TEXT;

	// $2 is the value
	*pt++ = binary ? PG_OID_BYTEA : PG_OID_TEXT;
	*pv++ = ptr;
	*pl++ = len;
	*pf++ = binary ? PG_FMT_BINARY : PG_FMT_TEXT;

	/* Construct the INSERT or UPDATE command. */
	switch (op) {
	case Select:
		abort();
	case Upsert: /*FALLTHROUGH*/
	case Insert:
		if (tsf != NULL) {
			CHECK((asprintf(&cmd, "INSERT INTO %s (%s, %s, %s)"
						" VALUES ($1, $2, %s)",
					table, kf, vf, tsf, ts)) < 0)
		} else {
			CHECK((asprintf(&cmd, "INSERT INTO %s (%s, %s)"
						" VALUES ($1, $2)",
					table, kf, vf)) < 0)
		}
		if (op == Upsert) {
			CHECK((asprintf(&tmp, "%s ON CONFLICT (%s) DO UPDATE"
						" SET %s = $2",
					cmd, kf, vf)) < 0)
			free(cmd);
			cmd = tmp;
			tmp = NULL;
			if (tsf != NULL)
				CHECK((asprintf(&tmp, "%s, %s = %s",
						cmd, tsf, ts)) < 0)
			free(cmd);
			cmd = tmp;
			tmp = NULL;
		}
		break;
	case Replace: /*FALLTHROUGH*/
	case Append:
		CHECK((asprintf(&cmd, "UPDATE %s SET", table)) < 0)
		if (tsf != NULL) {
			CHECK((asprintf(&tmp, "%s %s = %s,",
					cmd, tsf, ts)) < 0)
			free(cmd);
			cmd = tmp;
			tmp = NULL;
		}
		if (op == Replace) {
			CHECK((asprintf(&tmp, "%s %s = $2", cmd, vf)) < 0)
			free(cmd);
			cmd = tmp;
			tmp = NULL;
		} else {
			CHECK((asprintf(&tmp, "%s %s = %s || $2",
					cmd, vf, vf)) < 0)
			free(cmd);
			cmd = tmp;
			tmp = NULL;
		}
		CHECK((asprintf(&tmp, "%s WHERE %s = $1", cmd, kf)) < 0)
		free(cmd);
		cmd = tmp;
		tmp = NULL;
		break;
	default:
		abort();
	}

	/* Send the command. */
	if (tracelevel > 0) {
		fputc('{', stderr);
		fputs(cmd, stderr);
		fputs("}\n", stderr);
	}
	res = PQexecParams(conn, cmd, pv - paramValues, paramTypes,
			   paramValues, paramLengths, paramFormats,
			   0);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		fprintf(stderr, "%s: \"%s\": %s (%d) \"%s\" - %s",
			progname, cmd,
			PQresStatus(PQresultStatus(res)),
			PQresultStatus(res), PQcmdStatus(res),
			PQresultErrorMessage(res) != NULL ?
				PQresultErrorMessage(res) : "\n");
		goto done;
	}
	if (strcmp(PQcmdTuples(res), "0") == 0) {
		fprintf(stderr, "%s: \"%s\": '%s' tuples? (%s)\n",
			progname, cmd,
			PQcmdTuples(res), PQcmdStatus(res));
		goto done;
	}
	status = 0;

 done:
	if (ts != NULL)
		free(ts);
	if (ptr != NULL)
		munmap(ptr, len);
	if (fd != STDIN_FILENO)
		close(fd);
	if (cmd != NULL)
		free(cmd);
	if (res != NULL)
		PQclear(res);
	return (status);
}
