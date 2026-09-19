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

#define	_GNU_SOURCE	/* for asprintf() */

#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
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
static char	*quote(PGconn *, const char *);
static char	*xstrdup(const char *);

static const	char *tmpdir;
static int	tracelevel;
static bool	binary;

static void
usage(const char *msg) __attribute__((noreturn));

static void
usage(const char *msg) {
	fprintf(stderr, "%s: usage error (%s)\n", progname, msg);
	fprintf(stderr,
		"usage:\n"
		"\t%s [-b] [-M tsf] [-T tmpdir] [-x tracelevel]\n"
		"\t\t[-d dbname] [-h dbhost] [-p dbport] [-o pgoptions]\n"
		"\t\t[-u pguser] [-t pgtty] [-P pgpasswd]\n"
		"\t\t<op> <tbl> <kf> <k> <vf> [<file>]\n"
		"where:\n"
		"\t<op> is select|upsert|insert|replace|append;\n"
		"\t<file> defaults to stdin (put) or stdout (select);\n"
		"\t-b means <vf> is bytea rather than text;\n"
		"\t-M names a column to set to the file's mtime;\n"
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

	if (argv[0] != NULL) {
		if ((progname = strrchr(argv[0], '/')) != NULL)
			progname++;
		else
			progname = argv[0];
	}

	if ((t = getenv("USER")) == NULL &&
	    (t = getenv("LOGNAME")) == NULL) {
		struct passwd *pw = getpwuid(getuid());

		if (pw == NULL) {
			perror("no default database");
			exit(1);
		}
		t = pw->pw_name;
	}
	dbname = xstrdup(t);

	if ((tmpdir = getenv("TMPDIR")) == NULL || *tmpdir == '\0')
		tmpdir = _PATH_TMP;

	tsf = NULL;

	while ((ch = getopt(argc, argv, "bd:h:M:o:P:p:T:t:u:x:")) != -1) {
		switch (ch) {
		case 'b':
			binary = true;
			break;
		case 'd':
			free(dbname);
			dbname = xstrdup(optarg);
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
			if (*optarg == '\0')
				usage("tmpdir must not be empty");
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
		fprintf(stderr, "%s: %s: %s\n", progname, dbname,
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
	char		*qtable = NULL, *qkf = NULL, *qtf = NULL;
	int		status = 1;
	char		*ptr;
	int		len;

	/* Open the output file if there is one. */
	if (file != NULL)
		if ((fd = open(file, O_WRONLY|O_CREAT|O_TRUNC, 0666)) < 0) {
			perror(file);
			goto done;
		}

	/* Build the query. */
	if ((qtable = quote(conn, table)) == NULL ||
	    (qkf = quote(conn, kf)) == NULL ||
	    (qtf = quote(conn, tf)) == NULL)
		goto done;
	CHECK((asprintf(&cmd, "SELECT %s FROM %s WHERE %s = $1",
			qtf, qtable, qkf)) < 0)

	/* Send the query. */
	if (tracelevel > 0) {
		fputc('{', stderr);
		fputs(cmd, stderr);
		fputs("}\n", stderr);
	}
	res = PQexecParams(conn, cmd, 1, paramTypes,
			   paramValues, NULL, NULL, binary);
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		fprintf(stderr, "%s: \"%s\": %s\n", progname, cmd,
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

	if (PQgetisnull(res, 0, 0)) {
		fprintf(stderr, "%s: \"%s\": null value?\n", progname, cmd);
		goto done;
	}

	/* Output the result. */
	ptr = PQgetvalue(res, 0, 0);
	len = PQgetlength(res, 0, 0);
	CHECK((write(fd, ptr, (size_t) len)) != len)
	if (!binary && len > 0 && ptr[len-1] != '\n')
		CHECK((write(fd, "\n", 1)) != 1)
	status = 0;
 done:
	if (qtable != NULL)
		free(qtable);
	if (qkf != NULL)
		free(qkf);
	if (qtf != NULL)
		free(qtf);
	if (cmd != NULL)
		free(cmd);
	if (file != NULL && fd >= 0)
		close(fd);
	if (res != NULL)
		PQclear(res);
	return (status);
}

static int
put(PGconn *conn, const char *table, const char *kf, const char *k,
    const char *vf, const char *file, const char *tsf, Op op)
{
	char		*cmd = NULL, *tmp = NULL, *ts = NULL, *map = NULL;
	char		*qtable = NULL, *qkf = NULL, *qvf = NULL, *qtsf = NULL;
	const char	*val = "";
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
		if ((fd = open(file, O_RDONLY)) < 0) {
			perror(file);
			goto done;
		}

	/*
	 * If it's not a regular file, make a copy for mmap, then switch.
	 */
	CHECK((fstat(fd, &sb)) < 0)
	if ((sb.st_mode & S_IFMT) != S_IFREG) {
		char buf[65536];
		ssize_t s;
		int tf;

		CHECK((asprintf(&tmp, "%s%spgfiler.XXXXXX", tmpdir,
				tmpdir[strlen(tmpdir)-1] == '/' ? "" : "/"))
		      < 0)
		CHECK((tf = mkstemp(tmp)) < 0)
		CHECK((unlink(tmp)) < 0)
		free(tmp);
		tmp = NULL;
		ts = xstrdup("'now'::TIMESTAMP");
		while ((s = read(fd, buf, sizeof buf)) > 0)
			CHECK((write(tf, buf, (size_t) s)) != s)
		CHECK(s < 0)
		close(fd);
		fd = tf;
		CHECK((fstat(fd, &sb)) < 0)
	} else {
		if (tsf != NULL)
			CHECK((asprintf(&ts, "to_timestamp(%lu)",
					(u_long) sb.st_mtime)) < 0)
	}

	/* Map it into virtual memory, unless it's empty. */
	len = sb.st_size;
	if (len > INT_MAX) {
		fprintf(stderr, "%s: %zu bytes is too large\n", progname, len);
		goto done;
	}
	if (len != 0) {
		void *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);

		CHECK(p == MAP_FAILED)
		map = p;
		val = p;
	}

	// $1 is the key
	*pt++ = PG_OID_TEXT;
	*pv++ = k;
	*pl++ = strlen(k);
	*pf++ = PG_FMT_TEXT;

	// $2 is the value; binary format, since libpq derives the length of
	// a text format parameter with strlen(), and this one isn't a string.
	*pt++ = binary ? PG_OID_BYTEA : PG_OID_TEXT;
	*pv++ = val;
	*pl++ = len;
	*pf++ = PG_FMT_BINARY;

	if ((qtable = quote(conn, table)) == NULL ||
	    (qkf = quote(conn, kf)) == NULL ||
	    (qvf = quote(conn, vf)) == NULL ||
	    (tsf != NULL && (qtsf = quote(conn, tsf)) == NULL))
		goto done;

	/* Construct the INSERT or UPDATE command. */
	switch (op) {
	case Select:
		abort();
	case Upsert: /*FALLTHROUGH*/
	case Insert:
		if (tsf != NULL) {
			CHECK((asprintf(&cmd, "INSERT INTO %s (%s, %s, %s)"
						" VALUES ($1, $2, %s)",
					qtable, qkf, qvf, qtsf, ts)) < 0)
		} else {
			CHECK((asprintf(&cmd, "INSERT INTO %s (%s, %s)"
						" VALUES ($1, $2)",
					qtable, qkf, qvf)) < 0)
		}
		if (op == Upsert) {
			CHECK((asprintf(&tmp, "%s ON CONFLICT (%s) DO UPDATE"
						" SET %s = $2",
					cmd, qkf, qvf)) < 0)
			free(cmd);
			cmd = tmp;
			tmp = NULL;
			if (tsf != NULL) {
				CHECK((asprintf(&tmp, "%s, %s = %s",
						cmd, qtsf, ts)) < 0)
				free(cmd);
				cmd = tmp;
				tmp = NULL;
			}
		}
		break;
	case Replace: /*FALLTHROUGH*/
	case Append:
		CHECK((asprintf(&cmd, "UPDATE %s SET", qtable)) < 0)
		if (tsf != NULL) {
			CHECK((asprintf(&tmp, "%s %s = %s,",
					cmd, qtsf, ts)) < 0)
			free(cmd);
			cmd = tmp;
			tmp = NULL;
		}
		if (op == Replace) {
			CHECK((asprintf(&tmp, "%s %s = $2",
					cmd, qvf)) < 0)
			free(cmd);
			cmd = tmp;
			tmp = NULL;
		} else {
			CHECK((asprintf(&tmp, "%s %s = %s || $2",
					cmd, qvf, qvf)) < 0)
			free(cmd);
			cmd = tmp;
			tmp = NULL;
		}
		CHECK((asprintf(&tmp, "%s WHERE %s = $1", cmd, qkf)) < 0)
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
		fprintf(stderr, "%s: \"%s\": %s (%d) \"%s\" - %s\n",
			progname, cmd,
			PQresStatus(PQresultStatus(res)),
			PQresultStatus(res), PQcmdStatus(res),
			PQresultErrorMessage(res) != NULL ?
				PQresultErrorMessage(res) : "");
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
	if (qtable != NULL)
		free(qtable);
	if (qkf != NULL)
		free(qkf);
	if (qvf != NULL)
		free(qvf);
	if (qtsf != NULL)
		free(qtsf);
	if (map != NULL)
		munmap(map, len);
	if (fd != STDIN_FILENO && fd >= 0)
		close(fd);
	if (cmd != NULL)
		free(cmd);
	if (res != NULL)
		PQclear(res);
	return (status);
}

/*
 * Quote a possibly qualified SQL identifier, one dotted part at a time,
 * so that "schema.table" still means what it says.
 */
static char *
quote(PGconn *conn, const char *name) {
	const char	*part = name;
	char		*res = NULL, *tmp;
	bool		more = true;

	while (more) {
		const char *dot = strchr(part, '.');
		char *q = PQescapeIdentifier(conn, part,
					     dot != NULL ? (size_t) (dot - part)
							 : strlen(part));

		if (q == NULL) {
			fprintf(stderr, "%s: %s: %s", progname, name,
				PQerrorMessage(conn));
			free(res);
			return (NULL);
		}
		if (res == NULL) {
			tmp = xstrdup(q);
		} else {
			if (asprintf(&tmp, "%s.%s", res, q) < 0) {
				perror("asprintf");
				exit(1);
			}
			free(res);
		}
		res = tmp;
		PQfreemem(q);
		if (dot == NULL)
			more = false;
		else
			part = dot + 1;
	}
	return (res);
}

static char *
xstrdup(const char *str) {
	char *res = strdup(str);

	if (res == NULL) {
		perror("strdup");
		exit(1);
	}
	return (res);
}
