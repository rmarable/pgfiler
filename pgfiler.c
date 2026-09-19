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

#define	_GNU_SOURCE	/* for vasprintf() */

#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <paths.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libpq-fe.h>

#define	PG_OID_INFER 0	/* let the server deduce it from context */
#define	PG_OID_BYTEA 17
#define	PG_OID_TEXT 25

#define	PG_FMT_TEXT 0
#define	PG_FMT_BINARY 1

/*
 * The server refuses a protocol message larger than MaxAllocSize - 1, and
 * the Bind message carrying our value also carries the key and a few
 * dozen bytes of framing.
 */
#define	PG_MAX_MESSAGE 0x3ffffffe
#define	PG_BIND_OVERHEAD 64

#define	CHECK(expr) { if (expr) { perror(#expr); goto done; } }

typedef enum { Select, Upsert, Insert, Replace, Append } Op;

static const char *progname = "pgfiler";

static int	get(PGconn *, const char *, const char *, const char *,
		    const char *, const char *);
static int	put(PGconn *, const char *, const char *, const char *,
		    const char *, const char *, const char *, Op);
static char	*quote(PGconn *, const char *);
static bool	writeall(int, const void *, size_t);
static char	*xasprintf(const char *, ...)
		    __attribute__((format(printf, 1, 2)));
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
		"\t-P is visible in ps(1); PGPASSWORD or ~/.pgpass is not;\n"
		"\ta <k> beginning with '-' needs a '--' before <op>;\n"
		"example:\n"
		"\t%s select file_table filename 1.2.3.4 file_data\n",
		progname, progname);
	exit(1);
}

int
main(int argc, char *argv[]) {
	const char	*pghost = NULL, *pgport = NULL, *pgoptions = NULL,
			*pgtty = NULL, *pguser = NULL;
	const char	*opstr, *table, *kf, *k, *vf, *file, *tsf, *t;
	char		*dbname, *pgpasswd = NULL;
	int		status = 0, ch;
	PGconn		*conn;
	Op		op;

	if (argv[0] != NULL) {
		if ((progname = strrchr(argv[0], '/')) != NULL)
			progname++;
		else
			progname = argv[0];
	}

	if ((t = getenv("USER")) == NULL || *t == '\0')
		t = getenv("LOGNAME");
	if (t == NULL || *t == '\0') {
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
			/* Keep a copy, then hide the original from ps(1). */
			free(pgpasswd);
			pgpasswd = xstrdup(optarg);
			explicit_bzero(optarg, strlen(optarg));
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

	if (op == Select && tsf != NULL)
		usage("-M is meaningless with 'select'");

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
	if (pgpasswd != NULL) {
		explicit_bzero(pgpasswd, strlen(pgpasswd));
		free(pgpasswd);
		pgpasswd = NULL;
	}
	return (status);
}

static int
get(PGconn *conn, const char *table, const char *kf, const char *v,
    const char *tf, const char *file)
{
	/*
	 * $1 is the key, whose type is whatever the key column is -- saying
	 * "text" here would mean no integer or inet key column could ever
	 * be matched.
	 */
	const Oid	paramTypes[] = { PG_OID_INFER };
	const char	*paramValues[] = { v };
	int		fd = STDOUT_FILENO;
	char		*cmd = NULL, *tmp = NULL;
	PGresult	*res = NULL;
	char		*qtable = NULL, *qkf = NULL, *qtf = NULL;
	int		status = 1;
	bool		opened = false;
	char		*ptr;
	int		len;

	/* Build the query. */
	if ((qtable = quote(conn, table)) == NULL ||
	    (qkf = quote(conn, kf)) == NULL ||
	    (qtf = quote(conn, tf)) == NULL)
		goto done;
	cmd = xasprintf("SELECT %s FROM %s WHERE %s = $1", qtf, qtable, qkf);

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

	/*
	 * Without -b, a bytea column arrives in its text representation
	 * ("\x48690a"), which is not the file the caller asked for.
	 */
	if (!binary && PQftype(res, 0) == PG_OID_BYTEA) {
		fprintf(stderr, "%s: \"%s\": column is bytea, needs -b\n",
			progname, cmd);
		goto done;
	}

	if (PQgetisnull(res, 0, 0)) {
		fprintf(stderr, "%s: \"%s\": null value?\n", progname, cmd);
		goto done;
	}

	/*
	 * Write to a temporary file beside the real one, and rename it into
	 * place only once it's complete, so that neither a failed query nor
	 * a failed write leaves a partial or empty file where a good one was.
	 */
	if (file != NULL) {
		tmp = xasprintf("%s.XXXXXX", file);
		if ((fd = mkstemp(tmp)) < 0) {
			perror(tmp);
			goto done;
		}
		opened = true;
	}

	/*
	 * Output the result.  The courtesy newline is only for a human
	 * looking at a terminal; adding it to a file would mean a store,
	 * fetch and store again did not round trip.
	 */
	ptr = PQgetvalue(res, 0, 0);
	len = PQgetlength(res, 0, 0);
	CHECK(!writeall(fd, ptr, (size_t) len))
	if (!binary && len > 0 && ptr[len-1] != '\n' && isatty(fd))
		CHECK(!writeall(fd, "\n", 1))

	if (file != NULL) {
		struct stat sb;
		mode_t mode;

		/* Keep the old file's mode, or give a new one the usual. */
		if (stat(file, &sb) == 0) {
			mode = sb.st_mode & 07777;
		} else {
			mode = umask(0);
			umask(mode);
			mode = 0666 & ~mode;
		}
		if (fchmod(fd, mode) < 0) {
			perror(file);
			goto done;
		}
		/* Some filesystems only report a failed write at close. */
		opened = false;
		if (close(fd) < 0 || rename(tmp, file) < 0) {
			perror(file);
			goto done;
		}
		free(tmp);
		tmp = NULL;
	}
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
	if (opened)
		close(fd);
	if (tmp != NULL) {
		unlink(tmp);
		free(tmp);
	}
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
	int		fd = STDIN_FILENO, tf = -1;
	int		status = 1;
	bool		fdmine = false;
	struct stat	sb;
	size_t		len = 0;
	off_t		maxval = PG_MAX_MESSAGE - PG_BIND_OVERHEAD
				 - (off_t) strlen(k);
	Oid		paramTypes[10], *pt = paramTypes;
	const		char *paramValues[10], **pv = paramValues;
	int		paramLengths[10], *pl = paramLengths;
	int		paramFormats[10], *pf = paramFormats;

	/* Open the file if there is one. */
	if (file != NULL) {
		if ((fd = open(file, O_RDONLY)) < 0) {
			perror(file);
			goto done;
		}
		fdmine = true;
	}

	/*
	 * The mtime is only meaningful for a regular file; anything else,
	 * a pipe say, is being written right now.
	 */
	CHECK((fstat(fd, &sb)) < 0)
	if (tsf != NULL) {
		if (S_ISREG(sb.st_mode))
			ts = xasprintf("to_timestamp(%jd)",
				       (intmax_t) sb.st_mtime);
		else
			ts = xstrdup("'now'::TIMESTAMP");
	}

	/*
	 * First choice is to map the file directly, which wants a regular
	 * file of known size.
	 */
	if (S_ISREG(sb.st_mode) && sb.st_size > 0) {
		void *p;

		if (sb.st_size > maxval) {
			fprintf(stderr, "%s: %jd bytes is larger than %jd\n",
				progname, (intmax_t) sb.st_size,
				(intmax_t) maxval);
			goto done;
		}
		len = (size_t) sb.st_size;
		p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
		if (p != MAP_FAILED)
			map = p;
	}

	/*
	 * Otherwise copy it to a temporary file and map that.  This is for
	 * pipes, but also for /proc, where stat reports st_size 0 -- or a
	 * wrong nonzero count, as for /proc/mounts -- for files full of
	 * content, and where mmap refuses the file anyway.
	 */
	if (map == NULL) {
		char buf[65536];
		off_t total = 0;
		ssize_t s;

		tmp = xasprintf("%s%spgfiler.XXXXXX", tmpdir,
				tmpdir[strlen(tmpdir)-1] == '/' ? "" : "/");
		CHECK((tf = mkstemp(tmp)) < 0)
		CHECK((unlink(tmp)) < 0)
		free(tmp);
		tmp = NULL;
		while ((s = read(fd, buf, sizeof buf)) > 0) {
			/* Stop before filling tmpdir with unusable data. */
			if ((total += s) > maxval) {
				fprintf(stderr, "%s: input is larger than"
					" %jd bytes\n", progname,
					(intmax_t) maxval);
				goto done;
			}
			CHECK(!writeall(tf, buf, (size_t) s))
		}
		CHECK(s < 0)
		close(fd);
		fd = tf;
		tf = -1;
		fdmine = true;
		len = (size_t) total;
		if (len != 0) {
			void *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);

			CHECK(p == MAP_FAILED)
			map = p;
		}
	}
	if (map != NULL)
		val = map;

	// $1 is the key; its type is the key column's, which only the
	// server knows, so that an integer or inet key column still works.
	*pt++ = PG_OID_INFER;
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
			cmd = xasprintf("INSERT INTO %s (%s, %s, %s)"
					" VALUES ($1, $2, %s)",
					qtable, qkf, qvf, qtsf, ts);
		} else {
			cmd = xasprintf("INSERT INTO %s (%s, %s)"
					" VALUES ($1, $2)",
					qtable, qkf, qvf);
		}
		if (op == Upsert) {
			tmp = xasprintf("%s ON CONFLICT (%s) DO UPDATE"
					" SET %s = $2",
					cmd, qkf, qvf);
			free(cmd);
			cmd = tmp;
			tmp = NULL;
			if (tsf != NULL) {
				tmp = xasprintf("%s, %s = %s",
						cmd, qtsf, ts);
				free(cmd);
				cmd = tmp;
				tmp = NULL;
			}
		}
		break;
	case Replace: /*FALLTHROUGH*/
	case Append:
		cmd = xasprintf("UPDATE %s SET", qtable);
		if (tsf != NULL) {
			tmp = xasprintf("%s %s = %s,", cmd, qtsf, ts);
			free(cmd);
			cmd = tmp;
			tmp = NULL;
		}
		if (op == Replace) {
			tmp = xasprintf("%s %s = $2", cmd, qvf);
		} else {
			/* COALESCE, since NULL || anything is NULL. */
			tmp = xasprintf("%s %s = COALESCE(%s, '') || $2",
					cmd, qvf, qvf);
		}
		free(cmd);
		cmd = tmp;
		tmp = NULL;
		tmp = xasprintf("%s WHERE %s = $1", cmd, qkf);
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
	if (tmp != NULL)
		free(tmp);
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
	if (tf >= 0)
		close(tf);
	if (fdmine && fd >= 0)
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
		size_t plen = (dot != NULL) ? (size_t) (dot - part)
					    : strlen(part);
		char *q;

		if (plen == 0) {
			fprintf(stderr, "%s: %s: empty identifier\n",
				progname, name);
			free(res);
			return (NULL);
		}
		if ((q = PQescapeIdentifier(conn, part, plen)) == NULL) {
			fprintf(stderr, "%s: %s: %s", progname, name,
				PQerrorMessage(conn));
			free(res);
			return (NULL);
		}
		if (res == NULL) {
			tmp = xstrdup(q);
		} else {
			tmp = xasprintf("%s.%s", res, q);
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

/*
 * write(2) is allowed to do less than it's told, and says nothing about
 * errno when it does, so a partial write must be retried rather than
 * reported with perror().
 */
static bool
writeall(int fd, const void *buf, size_t len) {
	const char *ptr = buf;

	while (len != 0) {
		ssize_t n = write(fd, ptr, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return (false);
		}
		if (n == 0) {
			errno = EIO;
			return (false);
		}
		ptr += n;
		len -= (size_t) n;
	}
	return (true);
}

static char *
xasprintf(const char *fmt, ...) {
	char *res;
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vasprintf(&res, fmt, ap);
	va_end(ap);
	if (n < 0) {
		perror("asprintf");
		exit(1);
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
