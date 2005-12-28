/* $Id$ */

#include <sys/queue.h>
#include <sys/types.h>

#include <ctype.h>
#include <err.h>
#include <limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum cst_type {
	CST_REXP, CST_LINENO
};

struct csa_rexp {
	regex_t				*csr_preg;
	int				 csr_offset;
	int				 csr_skip;
};

struct csa_lineno {
	int				 csl_lineno;
};

struct cs_arg {
	union {
		struct csa_rexp		 csu_rexp;
		struct csa_lineno	 csu_lineno;
	}				 csa_u;
	int				 csa_repeat;
	enum cst_type			 csa_type;
	SLIST_ENTRY(cs_arg)		 csa_next;
#define csa_rexp	csa_u.csu_rexp
#define csa_lineno	csa_u.csu_lineno
};

void usage(void);
void build_outfn(void);
void compile(char **);
void csplit(const char *);
void fatal(const char *, ...);

int keep = 0;
int silent;
int ndigits = 2;
const char *prefix = "xx";
char *outfn;
int curfileno;
SLIST_HEAD(, cs_arg) cs_args;

int
main(int argc, char *argv[])
{
	const char *errstr;
	int ch;

	while ((ch = getopt(argc, argv, "f:kn:s")) != -1)
		switch (ch) {
		case 'f':
			prefix = optarg;
			break;
		case 'k':
			keep = 1;
			break;
		case 'n':
			/* XXX: NAME_MAX - strlen(prefix) */
			ndigits = strtonum(optarg, 1, INT_MAX, &errstr);
			if (errstr != NULL)
				err(1, "%s: %s", optarg, errstr);
			break;
		case 's':
			silent = 1;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	argc -= optind;
	argv += optind;
	if (argc < 2)
		usage();
	compile(argv + 1);
	csplit(*argv);
	exit(0);
}

void
adjslashes(char *s, char ch)
{
	int saw_slash = 0;
	char *p, *t;

	for (p = s; *p != '\0'; p++) {
		if (*p == ch) {
			if (!saw_slash)
				err(1, "%s: invalid regex", s);
			for (t = p - 1; t[1] != '\0'; t++)
				t[0] = t[1];
		} else {
			switch (*p) {
			case '\\':
				saw_slash = 1;
				break;
			default:
				saw_slash = 0;
				break;
			}
		}
	}
}

void
compile(char **args)
{
	int ec, lineno, offset, repeat;
	char **arg, *s, ebuf[LINE_MAX];
	struct cs_arg *csa, *lastp;
	const char *errstr;
	regex_t *preg;
	int ch;

	csa = lastp = NULL;
	SLIST_INIT(&cs_args);
	for (arg = args; *arg != NULL; arg++) {
		if (isdigit(**arg)) {
			lineno = strtonum(*arg, 0, INT_MAX, &errstr);
			if (errstr != NULL)
				err(1, "%s: %s", *arg, errstr);
			if ((csa = malloc(sizeof(*csa))) == NULL)
				err(1, NULL);
			csa->csa_type = CST_LINENO;
			csa->csa_lineno.csl_lineno = lineno;
			csa->csa_repeat = 1;
			if (lastp == NULL)
				SLIST_INSERT_HEAD(&cs_args, csa, csa_next);
			else
				SLIST_INSERT_AFTER(lastp, csa, csa_next);
			lastp = csa;
		} else {
			ch = **arg;
			switch (ch) {
			case '/':
			case '%':
				if ((s = strrchr(++*arg, ch)) == NULL)
					err(1, "%s: invalid regex", *arg);
				*s++ = '\0';
				offset = strtonum(s, INT_MIN, INT_MAX, &errstr);
				if (errstr != NULL)
					err(1, "%s: %s", s, errstr);
				adjslashes(*arg, ch);
				if ((preg = malloc(sizeof(*preg))) == NULL)
					err(1, NULL);
				if ((ec = regcomp(preg, *arg, 0)) != 0) {
					regerror(ec, preg, ebuf, sizeof(ebuf));
					err(1, "%s: %s", *arg, ebuf);
				}
				if ((csa = malloc(sizeof(*csa))) == NULL)
					err(1, NULL);
				csa->csa_type = CST_REXP;
				csa->csa_rexp.csr_preg = preg;
				csa->csa_rexp.csr_offset = offset;
				csa->csa_repeat = 1;
				csa->csa_rexp.csr_skip = (ch == '%');
				if (lastp == NULL)
					SLIST_INSERT_HEAD(&cs_args, csa, csa_next);
				else
					SLIST_INSERT_AFTER(lastp, csa, csa_next);
				lastp = csa;
				break;
			case '{':
				if ((s = strrchr(++*arg, '}')) == NULL)
					err(1, "%s: repeat missing '}'", *arg);
				*s = '\0';
				repeat = strtonum(*arg, 0, INT_MAX, &errstr);
				if (errstr != NULL)
					errx(1, "%s: %s", *arg, errstr);
				if (repeat == 0)
					repeat = 1;
				if (csa == NULL)
					err(1, "no previous argument "
					    "to repeat");
				csa->csa_repeat = repeat;
				break;
			default:
				errx(1, "%s: invalid argument", *arg);
				/* NOTREACHED */
			}
		}
	}
}

void
csplit(const char *fn)
{
	char outfn[NAME_MAX], ln[LINE_MAX];
	int csalineno, ch;
	struct cs_arg *csa;
	FILE *infp, *outfp;
	off_t siz;

	if (strcmp(fn, "-") == 0)
		infp = stdin;
	else if ((infp = fopen(fn, "r")) == NULL)
		err(1, "%s", fn);

	outfp = NULL; /* gcc */
	curfileno = 0;
	SLIST_FOREACH(csa, &cs_args, csa_next) {
again:
		if (feof(infp))
			break;
		csalineno = 1;
		snprintf(outfn, sizeof(outfn), "%s%0*d", prefix,
		    ndigits, curfileno);
		if ((outfp = fopen(outfn, "w")) == NULL) {
			curfileno--;
			fatal("%s", outfn);
		}
		siz = 0;
		switch (csa->csa_type) {
		case CST_REXP:
/*
			if ()
			while (fgets(ln, sizeof(ln), infp) != NULL) {
				if (regexec())
			}
*/
			break;
		case CST_LINENO:
			while ((ch = fgetc(infp)) != EOF) {
				fputc(ch, outfp);
				siz++;
				if (ch == '\n')
					if (++csalineno ==
					    csa->csa_lineno.csl_lineno)
						break;
			}
			break;
		default:
			fatal("internal error");
			/* NOTREACHED */
		}
		printf("%lld\n", siz);
		curfileno++;
		if (--csa->csa_repeat)
			goto again;
	}
	while ((ch = fgetc(infp)) != EOF)
		fputc(ch, outfp);
	fclose(infp);
}

void
fatal(const char *fmt, ...)
{
	char outfn[NAME_MAX];
	va_list ap;
	int i;

	if (!keep) {
		for (i = 0; i <= curfileno; i++) {
			snprintf(outfn, sizeof(outfn), "%s%0*d", prefix,
			    ndigits, i);
//			unlink(outfn);
printf("unlink %s\n", outfn);
		}
	}
	va_start(ap, fmt);
	verr(1, fmt, ap);
}

void
usage(void)
{
	extern char *__progname;

	fprintf(stderr,
	    "usage: %s [-ks] [-f prefix] [-n number] file arg ...\n",
	    __progname);
	exit(1);
}
