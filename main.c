/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

/*
 * ipconfig - configure parameters of an ip stack
 */
#include <benchutil/alarm.h>
#include <iplib/iplib.h>
#include <ndblib/ndb.h>
#include <parlib/common.h>
#include <parlib/printf-ext.h>
#include <parlib/uthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dhcp.h"
#include "ipconfig.h"
#include "lib9.h"

#define DEBUG if (debug) warning

/* possible verbs */
enum {
	/* commands */
	Vadd,
	Vremove,
	Vunbind,
	Vaddpref6,
	Vra6,
	/* media */
	Vether,
	Vgbe,
	Vloopback,
	Vtorus,
	Vtree,
	Vpkt,
};

enum {
	Taddr,
	Taddrs,
	Tstr,
	Tbyte,
	Tulong,
	Tvec,
};

typedef struct Option Option;
struct Option {
	char *name;
	int type;
};

/*
 * I was too lazy to look up the types for each of these
 * options.  If someone feels like it, please mail me a
 * corrected array -- presotto
 */
Option option[256] = {
    [OBmask] { "ipmask", Taddr },
    [OBtimeoff] { "timeoff", Tulong },
    [OBrouter] { "ipgw", Taddrs },
    [OBtimeserver] { "time", Taddrs },
    [OBnameserver] { "name", Taddrs },
    [OBdnserver] { "dns", Taddrs },
    [OBlogserver] { "log", Taddrs },
    [OBcookieserver] { "cookie", Taddrs },
    [OBlprserver] { "lpr", Taddrs },
    [OBimpressserver] { "impress", Taddrs },
    [OBrlserver] { "rl", Taddrs },
    [OBhostname] { "sys", Tstr },
    [OBbflen] { "bflen", Tulong },
    [OBdumpfile] { "dumpfile", Tstr },
    [OBdomainname] { "dom", Tstr },
    [OBswapserver] { "swap", Taddrs },
    [OBrootpath] { "rootpath", Tstr },
    [OBextpath] { "extpath", Tstr },
    [OBipforward] { "ipforward", Taddrs },
    [OBnonlocal] { "nonlocal", Taddrs },
    [OBpolicyfilter] { "policyfilter", Taddrs },
    [OBmaxdatagram] { "maxdatagram", Tulong },
    [OBttl] { "ttl", Tulong },
    [OBpathtimeout] { "pathtimeout", Taddrs },
    [OBpathplateau] { "pathplateau", Taddrs },
    [OBmtu] { "mtu", Tulong },
    [OBsubnetslocal] { "subnetslocal", Taddrs },
    [OBbaddr] { "baddr", Taddrs },
    [OBdiscovermask] { "discovermask", Taddrs },
    [OBsupplymask] { "supplymask", Taddrs },
    [OBdiscoverrouter] { "discoverrouter", Taddrs },
    [OBrsserver] { "rs", Taddrs },
    [OBstaticroutes] { "staticroutes", Taddrs },
    [OBtrailerencap] { "trailerencap", Taddrs },
    [OBarptimeout] { "arptimeout", Tulong },
    [OBetherencap] { "etherencap", Taddrs },
    [OBtcpttl] { "tcpttl", Tulong },
    [OBtcpka] { "tcpka", Tulong },
    [OBtcpkag] { "tcpkag", Tulong },
    [OBnisdomain] { "nisdomain", Tstr },
    [OBniserver] { "ni", Taddrs },
    [OBntpserver] { "ntp", Taddrs },
    [OBnetbiosns] { "netbiosns", Taddrs },
    [OBnetbiosdds] { "netbiosdds", Taddrs },
    [OBnetbiostype] { "netbiostype", Taddrs },
    [OBnetbiosscope] { "netbiosscope", Taddrs },
    [OBxfontserver] { "xfont", Taddrs },
    [OBxdispmanager] { "xdispmanager", Taddrs },
    [OBnisplusdomain] { "nisplusdomain", Tstr },
    [OBnisplusserver] { "nisplus", Taddrs },
    [OBhomeagent] { "homeagent", Taddrs },
    [OBsmtpserver] { "smtp", Taddrs },
    [OBpop3server] { "pop3", Taddrs },
    [OBnntpserver] { "nntp", Taddrs },
    [OBwwwserver] { "www", Taddrs },
    [OBfingerserver] { "finger", Taddrs },
    [OBircserver] { "irc", Taddrs },
    [OBstserver] { "st", Taddrs },
    [OBstdaserver] { "stdar", Taddrs },

    [ODipaddr] { "ipaddr", Taddr },
    [ODlease] { "lease", Tulong },
    [ODoverload] { "overload", Taddr },
    [ODtype] { "type", Tbyte },
    [ODserverid] { "serverid", Taddr },
    [ODparams] { "params", Tvec },
    [ODmessage] { "message", Tstr },
    [ODmaxmsg] { "maxmsg", Tulong },
    [ODrenewaltime] { "renewaltime", Tulong },
    [ODrebindingtime] { "rebindingtime", Tulong },
    [ODvendorclass] { "vendorclass", Tvec },
    [ODclientid] { "clientid", Tvec },
    [ODtftpserver] { "tftp", Taddr },
    [ODbootfile] { "bootfile", Tstr },
};

uint8_t defrequested[] = {
    OBmask, OBrouter, OBdnserver, OBhostname, OBdomainname, OBntpserver,
};

uint8_t requested[256];
int nrequested;

char *argv0;
int Oflag;
int beprimary = -1;
Conf conf;
int debug;
int dodhcp;
int dondbconfig = 0;
int dupl_disc = 1; /* flag: V6 duplicate neighbor discovery */
Ctl *firstctl, **ctll;
struct ipifc *ifc;
int ipv6auto = 0;
int myifc = -1;
char *ndboptions;
int nip;
int noconfig;
int nodhcpwatch;
char optmagic[4] = {0x63, 0x82, 0x53, 0x63};
int plan9 = 1;
int sendhostname;

char *verbs[] = {
    [Vadd] "add",
    [Vremove] "remove",
    [Vunbind] "unbind",
    [Vether] "ether",
    [Vgbe] "gbe",
    [Vloopback] "loopback",
    [Vaddpref6] "add6",
    [Vra6] "ra6",
    [Vtorus] "torus",
    [Vtree] "tree",
    [Vpkt] "pkt",
};

void adddefroute(char *, uint8_t *);
int addoption(char *);
void binddevice(void);
void bootprequest(void);
void controldevice(void);
void dhcpquery(int, int);
void dhcprecv(void);
void dhcpsend(int);
int dhcptimer(void);
void dhcpwatch(int);
void doadd(int);
void doremove(void);
void dounbind(void);
int getndb(void);
void getoptions(uint8_t *);
int ip4cfg(void);
int ip6cfg(int a);
void lookforip(char *);
void mkclientid(void);
void ndbconfig(void);
int nipifcs(char *);
int openlisten(void);
uint8_t *optaddaddr(uint8_t *, int, uint8_t *);
uint8_t *optaddbyte(uint8_t *, int, int);
uint8_t *optaddstr(uint8_t *, int, char *);
uint8_t *optadd(uint8_t *, int, void *, int);
uint8_t *optaddulong(uint8_t *, int, uint32_t);
uint8_t *optaddvec(uint8_t *, int, uint8_t *, int);
int optgetaddrs(uint8_t *, int, uint8_t *, int);
int optgetp9addrs(uint8_t *, int, uint8_t *, int);
int optgetaddr(uint8_t *, int, uint8_t *);
int optgetbyte(uint8_t *, int);
int optgetstr(uint8_t *, int, char *, int);
uint8_t *optget(uint8_t *, int, int *);
uint32_t optgetulong(uint8_t *, int);
int optgetvec(uint8_t *, int, uint8_t *, int);
char *optgetx(uint8_t *, uint8_t);
Bootp *parsebootp(uint8_t *, int);
int parseoptions(uint8_t *p, int n);
int parseverb(char *);
void putndb(void);
void tweakservers(void);
void usage(void);
int validip(uint8_t *);
void writendb(char *, int, int);

void
usage(void)
{
	fprintf(stderr,
	        "usage: %s [-6dDGnNOpPruX][-b baud][-c ctl]* [-g gw]"
	        "[-h host][-m mtu]\n"
	        "\t[-x mtpt][-o dhcpopt] type dev [verb] [laddr [mask "
	        "[raddr [fs [auth]]]]]\n",
	        argv0);
	fprintf(stderr, "usage");
	exit(1);
}

void
warning(char *fmt, ...)
{
	char buf[1024];
	va_list arg;

	va_start(arg, fmt);
	vsnprintf(buf, sizeof buf, fmt, arg);
	va_end(arg);
	fprintf(stderr, "%s: %s\n", argv0, buf);
}

size_t
strlcpy(char *dst, const char *src, size_t size)
{
	if (size > 0) {
		while (--size > 0 && *src != '\0')
			*dst++ = *src++;
		*dst = '\0';
	}

	return strlen(src);
}

size_t
strlcat(char *dst, const char *src, size_t size)
{
	size_t rem; /* Buffer space remaining after null in dst. */

	/* We must find the terminating NUL byte in dst, but abort the
	 * search if we go past 'size' bytes.  At the end of this loop,
	 * 'dst' will point to either the NUL byte in the original
	 * destination or to one byte beyond the end of the buffer.
	 *
	 * 'rem' will be the amount of 'size' remaining beyond the NUL byte;
	 * potentially zero. This implies that 'size - rem' is equal to the
	 * distance from the beginning of the destination buffer to 'dst'.
	 *
	 * The return value of strlcat is the sum of the length of the
	 * original destination buffer (size - rem) plus the size of the
	 * src string (the return value of strlcpy).
	 */
	rem = size;
	while ((rem > 0) && (*dst != '\0')) {
		rem--;
		dst++;
	}

	return (size - rem) + strlcpy(dst, src, rem);
}

static char *
sysname(void)
{
	static char sname[256];

	gethostname(sname, sizeof(sname));

	return sname;
}

static void
parsenorm(int argc, char **argv)
{
	switch (argc) {
	case 5:
		if (parseip(conf.auth, argv[4]) == -1)
			usage();
		/* fall through */
	case 4:
		if (parseip(conf.fs, argv[3]) == -1)
			usage();
		/* fall through */
	case 3:
		if (parseip(conf.raddr, argv[2]) == -1)
			usage();
		/* fall through */
	case 2:
		/*
		 * can't test for parseipmask()==-1 cuz 255.255.255.255
		 * looks like that.
		 */
		if (strcmp(argv[1], "0") != 0)
			parseipmask(conf.mask, argv[1]);
		/* fall through */
	case 1:
		if (parseip(conf.laddr, argv[0]) == -1)
			usage();
		/* fall through */
	case 0:
		break;
	default:
		usage();
	}
}

static void
parse6pref(int argc, char **argv)
{
	switch (argc) {
	case 6:
		conf.preflt = strtoul(argv[5], 0, 10);
		/* fall through */
	case 5:
		conf.validlt = strtoul(argv[4], 0, 10);
		/* fall through */
	case 4:
		conf.autoflag = (atoi(argv[3]) != 0);
		/* fall through */
	case 3:
		conf.onlink = (atoi(argv[2]) != 0);
		/* fall through */
	case 2:
		conf.prefixlen = atoi(argv[1]);
		/* fall through */
	case 1:
		if (parseip(conf.v6pref, argv[0]) == -1) {
			fprintf(stderr, "bad address %s", argv[0]);
			exit(-1);
		}
		break;
	}
	DEBUG("parse6pref: pref %R len %d", conf.v6pref, conf.prefixlen);
}

/* parse router advertisement (keyword, value) pairs */
static void
parse6ra(int argc, char **argv)
{
	int i, argsleft;
	char *kw, *val;

	if (argc % 2 != 0)
		usage();

	i = 0;
	for (argsleft = argc; argsleft > 1; argsleft -= 2) {
		kw = argv[i];
		val = argv[i + 1];
		if (strcmp(kw, "recvra") == 0)
			conf.recvra = (atoi(val) != 0);
		else if (strcmp(kw, "sendra") == 0)
			conf.sendra = (atoi(val) != 0);
		else if (strcmp(kw, "mflag") == 0)
			conf.mflag = (atoi(val) != 0);
		else if (strcmp(kw, "oflag") == 0)
			conf.oflag = (atoi(val) != 0);
		else if (strcmp(kw, "maxraint") == 0)
			conf.maxraint = atoi(val);
		else if (strcmp(kw, "minraint") == 0)
			conf.minraint = atoi(val);
		else if (strcmp(kw, "linkmtu") == 0)
			conf.linkmtu = atoi(val);
		else if (strcmp(kw, "reachtime") == 0)
			conf.reachtime = atoi(val);
		else if (strcmp(kw, "rxmitra") == 0)
			conf.rxmitra = atoi(val);
		else if (strcmp(kw, "ttl") == 0)
			conf.ttl = atoi(val);
		else if (strcmp(kw, "routerlt") == 0)
			conf.routerlt = atoi(val);
		else {
			warning("bad ra6 keyword %s", kw);
			usage();
		}
		i += 2;
	}

	/* consistency check */
	if (conf.maxraint < conf.minraint) {
		fprintf(stderr, "maxraint %d < minraint %d",
		        conf.maxraint, conf.minraint);
		exit(-1);
	}
}

static void
init(void)
{
	srand(lrand48());
	if (register_printf_specifier('E', printf_ethaddr, printf_ethaddr_info) != 0)
		fprintf(stderr, "Installing 'E' failed\n");
	if (register_printf_specifier('R', printf_ipaddr, printf_ipaddr_info) != 0)
		fprintf(stderr, "Installing 'i' failed\n");
	if (register_printf_specifier('M', printf_ipmask, printf_ipmask_info) != 0)
		fprintf(stderr, "Installing 'M' failed\n");

	setnetmtpt(conf.mpoint, sizeof conf.mpoint, NULL);
	conf.cputype = getenv("cputype");
	if (conf.cputype == NULL)
		conf.cputype = "386";

	ctll = &firstctl;
	v6paraminit(&conf);

	/* init set of requested dhcp parameters with the default */
	nrequested = sizeof defrequested;
	memcpy(requested, defrequested, nrequested);
}

static int
parseargs(int argc, char **argv)
{
	char *p;
	int action, verb;

	/* default to any host name we already have */
	if (*conf.hostname == 0) {
		p = getenv("sysname");
		if (p == NULL || *p == 0)
			p = sysname();
		if (p != NULL)
			strncpy(conf.hostname, p, sizeof conf.hostname - 1);
	}

	/* defaults */
	conf.type = "ether";
	conf.dev = "/net/ether0";
	action = Vadd;

	/* get optional medium and device */
	if (argc > 0) {
		verb = parseverb(*argv);
		switch (verb) {
		case Vether:
		case Vgbe:
		case Vloopback:
		case Vtorus:
		case Vtree:
		case Vpkt:
			conf.type = *argv++;
			argc--;
			if (argc > 0) {
				conf.dev = *argv++;
				argc--;
			}
			break;
		}
	}

	/* get optional verb */
	if (argc > 0) {
		verb = parseverb(*argv);
		switch (verb) {
		case Vether:
		case Vgbe:
		case Vloopback:
		case Vtorus:
		case Vtree:
		case Vpkt:
			fprintf(stderr, "medium %s already specified", conf.type);
			exit(-1);
		case Vadd:
		case Vremove:
		case Vunbind:
		case Vaddpref6:
		case Vra6:
			argv++;
			argc--;
			action = verb;
			break;
		}
	}

	/* get verb-dependent arguments */
	switch (action) {
	case Vadd:
	case Vremove:
	case Vunbind:
		parsenorm(argc, argv);
		break;
	case Vaddpref6:
		parse6pref(argc, argv);
		break;
	case Vra6:
		parse6ra(argc, argv);
		break;
	}
	return action;
}

int
main(int argc, char **argv)
{
	int retry, action;
	Ctl *cp;

	init();
	retry = 0;
	ARGBEGIN
	{
	case '6': /* IPv6 auto config */
		ipv6auto = 1;
		break;
	case 'b':
		conf.baud = EARGF(usage());
		break;
	case 'c':
		cp = malloc(sizeof *cp);
		if (cp == NULL) {
			fprintf(stderr, "%r");
			exit(1);
		}
		*ctll = cp;
		ctll = &cp->next;
		cp->next = NULL;
		cp->ctl = EARGF(usage());
		break;
	case 'd':
		dodhcp = 1;
		break;
	case 'D':
		debug = 1;
		break;
	case 'g':
		if (parseip(conf.gaddr, EARGF(usage())) == -1)
			usage();
		break;
	case 'G':
		plan9 = 0;
		break;
	case 'h':
		snprintf(conf.hostname, sizeof conf.hostname, "%s", EARGF(usage()));
		sendhostname = 1;
		break;
	case 'm':
		conf.mtu = atoi(EARGF(usage()));
		break;
	case 'n':
		noconfig = 1;
		break;
	case 'N':
		dondbconfig = 1;
		break;
	case 'o':
		if (addoption(EARGF(usage())) < 0)
			usage();
		break;
	case 'O':
		Oflag = 1;
		break;
	case 'p':
		beprimary = 1;
		break;
	case 'P':
		beprimary = 0;
		break;
	case 'r':
		retry = 1;
		break;
	case 'u': /* IPv6: duplicate neighbour disc. off */
		dupl_disc = 0;
		break;
	case 'x':
		setnetmtpt(conf.mpoint, sizeof conf.mpoint, EARGF(usage()));
		break;
	case 'X':
		nodhcpwatch = 1;
		break;
	default:
		usage();
	}
	ARGEND;
	argv0 = "ipconfig"; /* boot invokes us as tcp? */

	action = parseargs(argc, argv);
	switch (action) {
	case Vadd:
		doadd(retry);
		break;
	case Vremove:
		doremove();
		break;
	case Vunbind:
		dounbind();
		break;
	case Vaddpref6:
	case Vra6:
		doipv6(action);
		break;
	}

	return 0;
}

int
havendb(char *net)
{
	struct stat s;
	char buf[128];

	snprintf(buf, sizeof buf, "%s/ndb", net);
	if (stat(buf, &s) < 0)
		return 0;
	if (s.st_size == 0) {
		return 0;
	}
	return 1;
}

void
doadd(int retry)
{
	int tries;

	/* get number of preexisting interfaces */
	nip = nipifcs(conf.mpoint);
	if (beprimary == -1 && (nip == 0 || !havendb(conf.mpoint)))
		beprimary = 1;

	/* get ipifc into name space and condition device for ip */
	if (!noconfig) {
		lookforip(conf.mpoint);
		controldevice();
		binddevice();
	}

	if (ipv6auto) {
		if (ip6cfg(ipv6auto) < 0) {
			fprintf(stderr, "can't automatically start IPv6 on %s",
			        conf.dev);
			exit(-1);
		}
	} else if (validip(conf.laddr) && !isv4(conf.laddr)) {
		if (ip6cfg(0) < 0)
			fprintf(stderr, "can't start IPv6 on %s, address %R",
			        conf.dev, conf.laddr);
			exit(-1);
	}

	if (!validip(conf.laddr)) {
		if (dondbconfig)
			ndbconfig();
		else
			dodhcp = 1;
	}

	/* run dhcp if we need something */
	if (dodhcp) {
		mkclientid();
		for (tries = 0; tries < 30; tries++) {
			dhcpquery(!noconfig, Sselecting);
			if (conf.state == Sbound)
				break;
			sleep(1000);
		}
	}

	if (!validip(conf.laddr)) {
		if (retry && dodhcp && !noconfig) {
			warning("couldn't determine ip address, retrying");
			dhcpwatch(1);
			return;
		}
		fprintf(stderr, "no success with DHCP");
		exit(-1);
	}
			

	if (!noconfig) {
		if (ip4cfg() < 0) {
			fprintf(stderr, "can't start ip");
			exit(-1);
		}
		if (dodhcp && conf.lease != Lforever)
			dhcpwatch(0);
	}

	/* leave everything we've learned somewhere other procs can find it */
	if (beprimary == 1) {
		putndb();
		tweakservers();
	}
}

void
doremove(void)
{
	char file[128];
	char buf[256];
	int cfd;
	struct ipifc *nifc;
	struct iplifc *lifc;

	if (!validip(conf.laddr)) {
		fprintf(stderr, "remove requires an address");
		exit(-1);
	}
	ifc = readipifc(conf.mpoint, ifc, -1);
	for (nifc = ifc; nifc != NULL; nifc = nifc->next) {
		if (strcmp(nifc->dev, conf.dev) != 0)
			continue;
		for (lifc = nifc->lifc; lifc != NULL; lifc = lifc->next) {
			if (ipcmp(conf.laddr, lifc->ip) != 0)
				continue;
			if (validip(conf.mask) && ipcmp(conf.mask, lifc->mask) != 0)
				continue;
			if (validip(conf.raddr) && ipcmp(conf.raddr, lifc->net) != 0)
				continue;

			snprintf(file, sizeof file, "%s/ipifc/%d/ctl",
			         conf.mpoint, nifc->index);
			cfd = open(file, O_RDWR);
			if (cfd < 0) {
				warning("can't open %s: %r", conf.mpoint);
				continue;
			}
			snprintf(buf, sizeof buf, "remove %R %M", lifc->ip, lifc->mask);
			if (write(cfd, buf, strlen(buf)) != strlen(buf))
				warning("can't remove %R %M from %s: %r",
				        lifc->ip, lifc->mask, file);
			close(cfd);
		}
	}
}

void
dounbind(void)
{
	struct ipifc *nifc;
	char file[128];
	int cfd;

	ifc = readipifc(conf.mpoint, ifc, -1);
	for (nifc = ifc; nifc != NULL; nifc = nifc->next) {
		if (strcmp(nifc->dev, conf.dev) == 0) {
			snprintf(file, sizeof file, "%s/ipifc/%d/ctl",
			         conf.mpoint, nifc->index);
			cfd = open(file, O_RDWR);
			if (cfd < 0) {
				warning("can't open %s: %r", conf.mpoint);
				break;
			}
			if (write(cfd, "unbind", strlen("unbind")) < 0)
				warning("can't unbind from %s: %r", file);
			close(cfd);
			break;
		}
	}
}

/* set the default route */
void
adddefroute(char *mpoint, uint8_t *gaddr)
{
	char buf[256];
	int cfd;

	sprintf(buf, "%s/iproute", mpoint);
	cfd = open(buf, O_RDWR);
	if (cfd < 0)
		return;

	if (isv4(gaddr))
		snprintf(buf, sizeof buf, "add 0 0 %R", gaddr);
	else
		snprintf(buf, sizeof buf, "add :: /0 %R", gaddr);
	write(cfd, buf, strlen(buf));
	close(cfd);
}

/* create a client id */
void
mkclientid(void)
{
	if ((strcmp(conf.type, "ether") == 0) || (strcmp(conf.type, "gbe") == 0)) {
		if (myetheraddr(conf.hwa, conf.dev) == 0) {
			conf.hwalen = 6;
			conf.hwatype = 1;
			conf.cid[0] = conf.hwatype;
			memmove(&conf.cid[1], conf.hwa, conf.hwalen);
			conf.cidlen = conf.hwalen + 1;
		} else {
			conf.hwatype = -1;
			snprintf((char *)conf.cid, sizeof conf.cid, "plan9_%ld.%d",
			         lrand48(), getpid());
			conf.cidlen = strlen((char *)conf.cid);
		}
	}
}

/* bind ip into the namespace */
void
lookforip(char *net)
{
	struct stat s;
	char proto[64];

	snprintf(proto, sizeof proto, "%s/ipifc", net);
	if (stat(proto, &s) < 0) {
		fprintf(stderr, "no ip stack bound onto %s", net);
		exit(-1);
	}
}

/* send some ctls to a device */
void
controldevice(void)
{
	char ctlfile[256];
	int fd;
	Ctl *cp;

	if (firstctl == NULL ||
	    (strcmp(conf.type, "ether") != 0 && strcmp(conf.type, "gbe") != 0))
		return;

	snprintf(ctlfile, sizeof ctlfile, "%s/clone", conf.dev);
	fd = open(ctlfile, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "can't open %s", ctlfile);
		exit(-1);
	}

	for (cp = firstctl; cp != NULL; cp = cp->next) {
		if (write(fd, cp->ctl, strlen(cp->ctl)) < 0) {
			fprintf(stderr, "ctl message %s: %r", cp->ctl);
			exit(-1);
		}
		lseek(fd, 0, 0);
	}
}

/* bind an ip stack to a device, leave the control channel open */
void
binddevice(void)
{
	char buf[256];

	if (myifc < 0) {
		/* get a new ip interface */
		snprintf(buf, sizeof buf, "%s/ipifc/clone", conf.mpoint);
		conf.cfd = open(buf, O_RDWR);
		if (conf.cfd < 0) {
			fprintf(stderr, "opening %s/ipifc/clone: %r", conf.mpoint);
			exit(-1);
		}

		/* specify medium as ethernet, bind the interface to it */
		snprintf(buf, sizeof buf, "bind %s %s", conf.type, conf.dev);
		if (write(conf.cfd, buf, strlen(buf)) != strlen(buf)) {
			fprintf(stderr, "%s: bind %s %s: %r", buf, conf.type, conf.dev);
			exit(-1);
		}
	} else {
		/* open the old interface */
		snprintf(buf, sizeof buf, "%s/ipifc/%d/ctl", conf.mpoint, myifc);
		conf.cfd = open(buf, O_RDWR);
		if (conf.cfd < 0) {
			fprintf(stderr, "open %s: %r", buf);
			exit(-1);
		}
	}
}

/* add a logical interface to the ip stack */
int
ip4cfg(void)
{
	char buf[256];
	int n;

	if (!validip(conf.laddr))
		return -1;

	n = snprintf(buf, sizeof buf, "add");
	n += snprintf(buf + n, sizeof buf - n, " %R", conf.laddr);

	if (!validip(conf.mask))
		ipmove(conf.mask, defmask(conf.laddr));
	n += snprintf(buf + n, sizeof buf - n, " %R", conf.mask);

	if (validip(conf.raddr)) {
		n += snprintf(buf + n, sizeof buf - n, " %R", conf.raddr);
		if (conf.mtu != 0)
			n += snprintf(buf + n, sizeof buf - n, " %d", conf.mtu);
	}

	if (write(conf.cfd, buf, n) < 0) {
		warning("write(%s): %r", buf);
		return -1;
	}

	if (beprimary == 1 && validip(conf.gaddr))
		adddefroute(conf.mpoint, conf.gaddr);

	return 0;
}

/* remove a logical interface to the ip stack */
void
ipunconfig(void)
{
	char buf[256];
	int n;

	if (!validip(conf.laddr))
		return;
	DEBUG("couldn't renew IP lease, releasing %R", conf.laddr);
	n = sprintf(buf, "remove");
	n += snprintf(buf + n, sizeof buf - n, " %R", conf.laddr);

	if (!validip(conf.mask))
		ipmove(conf.mask, defmask(conf.laddr));
	n += snprintf(buf + n, sizeof buf - n, " %R", conf.mask);

	write(conf.cfd, buf, n);

	ipmove(conf.laddr, IPnoaddr);
	ipmove(conf.raddr, IPnoaddr);
	ipmove(conf.mask, IPnoaddr);

	/* forget configuration info */
	if (beprimary == 1)
		writendb("", 0, 0);
}

void
dhcpquery(int needconfig, int startstate)
{
	char buf[256];

	if (needconfig) {
		snprintf(buf, sizeof buf, "add %R %R", IPnoaddr, IPnoaddr);
		write(conf.cfd, buf, strlen(buf));
	}

	conf.fd = openlisten();
	if (conf.fd < 0) {
		conf.state = Sinit;
		return;
	}
	// notify(ding);

	/* try dhcp for 10 seconds */
	conf.xid = lrand48();
	conf.starttime = time(0);
	conf.state = startstate;
	switch (startstate) {
	case Sselecting:
		conf.offered = 0;
		dhcpsend(Discover);
		break;
	case Srenewing:
		dhcpsend(Request);
		break;
	default:
		fprintf(stderr, "internal error 0");
		exit(-1);
	}
	conf.resend = 0;
	conf.timeout = time(0) + 4;

	while (conf.state != Sbound) {
		dhcprecv();
		if (dhcptimer() < 0)
			break;
		if (time(0) - conf.starttime > 10)
			break;
	}
	close(conf.fd);

	if (needconfig) {
		snprintf(buf, sizeof buf, "remove %R %R", IPnoaddr, IPnoaddr);
		write(conf.cfd, buf, strlen(buf));
	}
}

enum {
	// This was an hour, but needs to be less for the ARM/GS1 until the timer
	// code has been cleaned up (pb).
	Maxsleep = 450,
};

void
dhcpwatch(int needconfig)
{
	int secs, s;
	uint32_t t;

	if (nodhcpwatch)
		return;

	switch (fork()) {
	default:
		return;
	case 0:
		break;
	}

	// procsetname("dhcpwatch");
	/* keep trying to renew the lease */
	for (;;) {
		if (conf.lease == 0)
			secs = 5;
		else
			secs = conf.lease >> 1;

		/* avoid overflows */
		for (s = secs; s > 0; s -= t) {
			if (s > Maxsleep)
				t = Maxsleep;
			else
				t = s;
			sleep(t * 1000);
		}

		if (conf.lease > 0) {
			/*
			 * during boot, the starttime can be bogus so avoid
			 * spurious ipunconfig's
			 */
			t = time(0) - conf.starttime;
			if (t > (3 * secs) / 2)
				t = secs;
			if (t >= conf.lease) {
				conf.lease = 0;
				if (!noconfig) {
					ipunconfig();
					needconfig = 1;
				}
			} else
				conf.lease -= t;
		}
		dhcpquery(needconfig, needconfig ? Sselecting : Srenewing);

		if (needconfig && conf.state == Sbound) {
			if (ip4cfg() < 0) {
				fprintf(stderr, "can't start ip: %r");
				exit(-1);
			}
			needconfig = 0;
			/*
			 * leave everything we've learned somewhere that
			 * other procs can find it.
			 */
			if (beprimary == 1) {
				putndb();
				tweakservers();
			}
		}
	}
}

int
dhcptimer(void)
{
	uint32_t now;

	now = time(0);
	if (now < conf.timeout)
		return 0;

	switch (conf.state) {
	default:
		fprintf(stderr, "dhcptimer: unknown state %d", conf.state);
		exit(-1);
	case Sinit:
	case Sbound:
		break;
	case Sselecting:
	case Srequesting:
	case Srebinding:
		dhcpsend(conf.state == Sselecting ? Discover : Request);
		conf.timeout = now + 4;
		if (++conf.resend > 5) {
			conf.state = Sinit;
			return -1;
		}
		break;
	case Srenewing:
		dhcpsend(Request);
		conf.timeout = now + 1;
		if (++conf.resend > 3) {
			conf.state = Srebinding;
			conf.resend = 0;
		}
		break;
	}
	return 0;
}

void
dhcpsend(int type)
{
	Bootp bp;
	uint8_t *p;
	int n;
	uint8_t vendor[64];
	struct udphdr *up = (struct udphdr *)bp.udphdr;

	memset(&bp, 0, sizeof bp);

	hnputs(up->rport, 67);
	bp.op = Bootrequest;
	hnputl(bp.xid, conf.xid);
	hnputs(bp.secs, time(0) - conf.starttime);
	hnputs(bp.flags, 0);
	memmove(bp.optmagic, optmagic, 4);
	if (conf.hwatype >= 0 && conf.hwalen < sizeof bp.chaddr) {
		memmove(bp.chaddr, conf.hwa, conf.hwalen);
		bp.hlen = conf.hwalen;
		bp.htype = conf.hwatype;
	}
	p = bp.optdata;
	p = optaddbyte(p, ODtype, type);
	p = optadd(p, ODclientid, conf.cid, conf.cidlen);
	switch (type) {
	default:
		fprintf(stderr, "dhcpsend: unknown message type: %d", type);
		exit(-1);
	case Discover:
		ipmove(up->raddr, IPv4bcast); /* broadcast */
		if (*conf.hostname && sendhostname)
			p = optaddstr(p, OBhostname, conf.hostname);
		if (plan9) {
			n = snprintf((char *)vendor, sizeof vendor, "plan9_%s",
			             conf.cputype);
			p = optaddvec(p, ODvendorclass, vendor, n);
		}
		p = optaddvec(p, ODparams, requested, nrequested);
		if (validip(conf.laddr))
			p = optaddaddr(p, ODipaddr, conf.laddr);
		break;
	case Request:
		switch (conf.state) {
		case Srenewing:
			ipmove(up->raddr, conf.server);
			v6tov4(bp.ciaddr, conf.laddr);
			break;
		case Srebinding:
			ipmove(up->raddr, IPv4bcast); /* broadcast */
			v6tov4(bp.ciaddr, conf.laddr);
			break;
		case Srequesting:
			ipmove(up->raddr, IPv4bcast); /* broadcast */
			p = optaddaddr(p, ODipaddr, conf.laddr);
			p = optaddaddr(p, ODserverid, conf.server);
			break;
		}
		p = optaddulong(p, ODlease, conf.offered);
		if (plan9) {
			n = snprintf((char *)vendor, sizeof vendor, "plan9_%s",
			             conf.cputype);
			p = optaddvec(p, ODvendorclass, vendor, n);
		}
		p = optaddvec(p, ODparams, requested, nrequested);
		if (*conf.hostname && sendhostname)
			p = optaddstr(p, OBhostname, conf.hostname);
		break;
	case Release:
		ipmove(up->raddr, conf.server);
		v6tov4(bp.ciaddr, conf.laddr);
		p = optaddaddr(p, ODipaddr, conf.laddr);
		p = optaddaddr(p, ODserverid, conf.server);
		break;
	}

	*p++ = OBend;

	n = p - (uint8_t *)&bp;

	/*
	 * We use a maximum size DHCP packet to survive the
	 * All_Aboard NAT package from Internet Share.  It
	 * always replies to DHCP requests with a packet of the
	 * same size, so if the request is too short the reply
	 * is truncated.
	 */
	if (write(conf.fd, &bp, sizeof bp) != sizeof bp)
		warning("dhcpsend: write failed: %r");
}

void
rerrstr(char *buf, size_t buflen)
{
	char *e = errstr();
	strlcpy(buf, e, buflen);
}

void
dhcprecv(void)
{
	int i, n, type;
	uint32_t lease;
	char err[256];
	uint8_t buf[8000], vopts[256], taddr[IPaddrlen];
	Bootp *bp;
	struct alarm_waiter waiter;

	init_awaiter(&waiter, alarm_abort_sysc);
	waiter.data = current_uthread;

	memset(buf, 0, sizeof buf);
	set_awaiter_rel(&waiter, 1000 * 1000);
	set_alarm(&waiter);
	n = read(conf.fd, buf, sizeof buf);
	unset_alarm(&waiter);

	if (n < 0) {
		rerrstr(err, sizeof err);
		if (strstr(err, "interrupt") == NULL)
			warning("dhcprecv: bad read: %s", err);
		else
			DEBUG("dhcprecv: read timed out");
		return;
	}
	if (n == 0) {
		warning("dhcprecv: zero-length packet read");
		return;
	}

	bp = parsebootp(buf, n);
	if (bp == 0) {
		DEBUG("parsebootp failed: dropping packet");
		return;
	}

	type = optgetbyte(bp->optdata, ODtype);
	switch (type) {
	default:
		warning("dhcprecv: unknown type: %d", type);
		break;
	case Offer:
		DEBUG("got offer from %R ", bp->siaddr);
		if (conf.state != Sselecting) {
			DEBUG("");
			break;
		}
		lease = optgetulong(bp->optdata, ODlease);
		if (lease == 0) {
			/*
			 * The All_Aboard NAT package from Internet Share
			 * doesn't give a lease time, so we have to assume one.
			 */
			warning("Offer with %lud lease, using %d", lease, MinLease);
			lease = MinLease;
		}
		DEBUG("lease=%lud ", lease);
		if (!optgetaddr(bp->optdata, ODserverid, conf.server)) {
			warning("Offer from server with invalid serverid");
			break;
		}

		v4tov6(conf.laddr, bp->yiaddr);
		memmove(conf.sname, bp->sname, sizeof conf.sname);
		conf.sname[sizeof conf.sname - 1] = 0;
		DEBUG("server=%R sname=%s", conf.server, conf.sname);
		conf.offered = lease;
		conf.state = Srequesting;
		dhcpsend(Request);
		conf.resend = 0;
		conf.timeout = time(0) + 4;
		break;
	case Ack:
		DEBUG("got ack from %R ", bp->siaddr);
		if (conf.state != Srequesting && conf.state != Srenewing &&
		    conf.state != Srebinding)
			break;

		/* ignore a bad lease */
		lease = optgetulong(bp->optdata, ODlease);
		if (lease == 0) {
			/*
			 * The All_Aboard NAT package from Internet Share
			 * doesn't give a lease time, so we have to assume one.
			 */
			warning("Ack with %lud lease, using %d", lease, MinLease);
			lease = MinLease;
		}
		DEBUG("lease=%lud ", lease);

		/* address and mask */
		if (!validip(conf.laddr) || !Oflag)
			v4tov6(conf.laddr, bp->yiaddr);
		if (!validip(conf.mask) || !Oflag) {
			if (!optgetaddr(bp->optdata, OBmask, conf.mask))
				ipmove(conf.mask, IPnoaddr);
		}
		DEBUG("ipaddr=%R ipmask=%M ", conf.laddr, conf.mask);

		/*
		 * get a router address either from the router option
		 * or from the router that forwarded the dhcp packet
		 */
		if (validip(conf.gaddr) && Oflag) {
			DEBUG("ipgw=%R ", conf.gaddr);
		} else if (optgetaddr(bp->optdata, OBrouter, conf.gaddr)) {
			DEBUG("ipgw=%R ", conf.gaddr);
		} else if (memcmp(bp->giaddr, IPnoaddr + IPv4off, IPv4addrlen) != 0) {
			v4tov6(conf.gaddr, bp->giaddr);
			DEBUG("giaddr=%R ", conf.gaddr);
		}

		/* get dns servers */
		memset(conf.dns, 0, sizeof conf.dns);
		n = optgetaddrs(bp->optdata, OBdnserver, conf.dns,
		                sizeof conf.dns / IPaddrlen);
		for (i = 0; i < n; i++)
			DEBUG("dns=%R ", conf.dns + i * IPaddrlen);

		/* get ntp servers */
		memset(conf.ntp, 0, sizeof conf.ntp);
		n = optgetaddrs(bp->optdata, OBntpserver, conf.ntp,
		                sizeof conf.ntp / IPaddrlen);
		for (i = 0; i < n; i++)
			DEBUG("ntp=%R ", conf.ntp + i * IPaddrlen);

		/* get names */
		optgetstr(bp->optdata, OBhostname, conf.hostname, sizeof conf.hostname);
		optgetstr(bp->optdata, OBdomainname, conf.domainname,
		          sizeof conf.domainname);

		/* get anything else we asked for */
		getoptions(bp->optdata);

		/* get plan9-specific options */
		n = optgetvec(bp->optdata, OBvendorinfo, vopts, sizeof vopts - 1);
		if (n > 0 && parseoptions(vopts, n) == 0) {
			if (validip(conf.fs) && Oflag)
				n = 1;
			else {
				n = optgetp9addrs(vopts, OP9fs, conf.fs, 2);
				if (n == 0)
					n = optgetaddrs(vopts, OP9fsv4, conf.fs, 2);
			}
			for (i = 0; i < n; i++)
				DEBUG("fs=%R ", conf.fs + i * IPaddrlen);

			if (validip(conf.auth) && Oflag)
				n = 1;
			else {
				n = optgetp9addrs(vopts, OP9auth, conf.auth, 2);
				if (n == 0)
					n = optgetaddrs(vopts, OP9authv4, conf.auth, 2);
			}
			for (i = 0; i < n; i++)
				DEBUG("auth=%R ", conf.auth + i * IPaddrlen);

			n = optgetp9addrs(vopts, OP9ipaddr, taddr, 1);
			if (n > 0)
				memmove(conf.laddr, taddr, IPaddrlen);
			n = optgetp9addrs(vopts, OP9ipmask, taddr, 1);
			if (n > 0)
				memmove(conf.mask, taddr, IPaddrlen);
			n = optgetp9addrs(vopts, OP9ipgw, taddr, 1);
			if (n > 0)
				memmove(conf.gaddr, taddr, IPaddrlen);
			DEBUG("new ipaddr=%R new ipmask=%M new ipgw=%R",
			      conf.laddr, conf.mask, conf.gaddr);
		}
		conf.lease = lease;
		conf.state = Sbound;
		DEBUG("server=%R sname=%s", conf.server, conf.sname);
		break;
	case Nak:
		conf.state = Sinit;
		warning("recved dhcpnak on %s", conf.mpoint);
		break;
	}
}

/* return pseudo-random integer in range low...(hi-1) */
uint32_t
randint(uint32_t low, uint32_t hi)
{
	if (hi < low)
		return low;
	return low + (lrand48() % hi);
}

long jitter(void) /* compute small pseudo-random delay in ms */
{
	return randint(0, 10 * 1000);
}

int
openlisten(void)
{
	int n, fd, cfd;
	char data[128], devdir[40];

	if (validip(conf.laddr) &&
	    (conf.state == Srenewing || conf.state == Srebinding))
		sprintf(data, "%s/udp!%R!68", conf.mpoint, conf.laddr);
	else
		sprintf(data, "%s/udp!*!68", conf.mpoint);
	for (n = 0; (cfd = announce9(data, devdir, 0)) < 0; n++) {
		if (!noconfig) {
			fprintf(stderr, "can't announce for dhcp: %r");
			exit(-1);
		}

		/* might be another client - wait and try again */
		warning("can't announce %s: %r", data);
		sleep(jitter());
		if (n > 10)
			return -1;
	}

	if (write(cfd, "headers", strlen("headers")) < 0) {
		fprintf(stderr, "can't set header mode: %r");
		exit(-1);
	}

	sprintf(data, "%s/data", devdir);
	fd = open(data, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %r", data);
		exit(-1);
	}
	close(cfd);
	return fd;
}

uint8_t *
optadd(uint8_t *p, int op, void *d, int n)
{
	p[0] = op;
	p[1] = n;
	memmove(p + 2, d, n);
	return p + n + 2;
}

uint8_t *
optaddbyte(uint8_t *p, int op, int b)
{
	p[0] = op;
	p[1] = 1;
	p[2] = b;
	return p + 3;
}

uint8_t *
optaddulong(uint8_t *p, int op, uint32_t x)
{
	p[0] = op;
	p[1] = 4;
	hnputl(p + 2, x);
	return p + 6;
}

uint8_t *
optaddaddr(uint8_t *p, int op, uint8_t *ip)
{
	p[0] = op;
	p[1] = 4;
	v6tov4(p + 2, ip);
	return p + 6;
}

/* add dhcp option op with value v of length n to dhcp option array p */
uint8_t *
optaddvec(uint8_t *p, int op, uint8_t *v, int n)
{
	p[0] = op;
	p[1] = n;
	memmove(p + 2, v, n);
	return p + 2 + n;
}

uint8_t *
optaddstr(uint8_t *p, int op, char *v)
{
	int n;

	n = strlen(v) + 1; /* microsoft leaves on the NUL, so we do too */
	p[0] = op;
	p[1] = n;
	memmove(p + 2, v, n);
	return p + 2 + n;
}

/*
 * parse p, looking for option `op'.  if non-nil, np points to minimum length.
 * return NULL if option is too small, else ptr to opt, and
 * store actual length via np if non-nil.
 */
uint8_t *
optget(uint8_t *p, int op, int *np)
{
	int len, code;

	while ((code = *p++) != OBend) {
		if (code == OBpad)
			continue;
		len = *p++;
		if (code != op) {
			p += len;
			continue;
		}
		if (np != NULL) {
			if (*np > len) {
				return 0;
			}
			*np = len;
		}
		return p;
	}
	return 0;
}

int
optgetbyte(uint8_t *p, int op)
{
	int len;

	len = 1;
	p = optget(p, op, &len);
	if (p == NULL)
		return 0;
	return *p;
}

uint32_t
optgetulong(uint8_t *p, int op)
{
	int len;

	len = 4;
	p = optget(p, op, &len);
	if (p == NULL)
		return 0;
	return nhgetl(p);
}

int
optgetaddr(uint8_t *p, int op, uint8_t *ip)
{
	int len;

	len = 4;
	p = optget(p, op, &len);
	if (p == NULL)
		return 0;
	v4tov6(ip, p);
	return 1;
}

/* expect at most n addresses; ip[] only has room for that many */
int
optgetaddrs(uint8_t *p, int op, uint8_t *ip, int n)
{
	int len, i;

	len = 4;
	p = optget(p, op, &len);
	if (p == NULL)
		return 0;
	len /= IPv4addrlen;
	if (len > n)
		len = n;
	for (i = 0; i < len; i++)
		v4tov6(&ip[i * IPaddrlen], &p[i * IPv4addrlen]);
	return i;
}

/* expect at most n addresses; ip[] only has room for that many */
int
optgetp9addrs(uint8_t *ap, int op, uint8_t *ip, int n)
{
	int len, i, slen, addrs;
	char *p;

	len = 1; /* minimum bytes needed */
	p = (char *)optget(ap, op, &len);
	if (p == NULL)
		return 0;
	addrs = *p++; /* first byte is address count */
	for (i = 0; i < n && i < addrs && len > 0; i++) {
		slen = strlen(p) + 1;
		if (parseip(&ip[i * IPaddrlen], p) == -1)
			fprintf(stderr, "%s: bad address %s\n", argv0, p);
		DEBUG("got plan 9 option %d addr %R (%s)", op, &ip[i * IPaddrlen], p);
		p += slen;
		len -= slen;
	}
	return addrs;
}

int
optgetvec(uint8_t *p, int op, uint8_t *v, int n)
{
	int len;

	len = 1;
	p = optget(p, op, &len);
	if (p == NULL)
		return 0;
	if (len > n)
		len = n;
	memmove(v, p, len);
	return len;
}

int
optgetstr(uint8_t *p, int op, char *s, int n)
{
	int len;

	len = 1;
	p = optget(p, op, &len);
	if (p == NULL)
		return 0;
	if (len >= n)
		len = n - 1;
	memmove(s, p, len);
	s[len] = 0;
	return len;
}

/*
 * sanity check options area
 * 	- options don't overflow packet
 * 	- options end with an OBend
 */
int
parseoptions(uint8_t *p, int n)
{
	int code, len, nin = n;

	while (n > 0) {
		code = *p++;
		n--;
		if (code == OBend)
			return 0;
		if (code == OBpad)
			continue;
		if (n == 0) {
			warning(
			    "parseoptions: bad option: 0x%ux: truncated: "
			    "opt length = %d",
			    code, nin);
			return -1;
		}

		len = *p++;
		n--;
		DEBUG("parseoptions: %s(%d) len %d, bytes left %d",
		      option[code].name, code, len, n);
		if (len > n) {
			warning(
			    "parseoptions: bad option: 0x%ux: %d > %d: "
			    "opt length = %d",
			    code, len, n, nin);
			return -1;
		}
		p += len;
		n -= len;
	}

	/* make sure packet ends with an OBend after all the optget code */
	*p = OBend;
	return 0;
}

/*
 * sanity check received packet:
 * 	- magic is dhcp magic
 * 	- options don't overflow packet
 */
Bootp *
parsebootp(uint8_t *p, int n)
{
	Bootp *bp;

	bp = (Bootp *)p;
	if (n < bp->optmagic - p) {
		warning(
		    "parsebootp: short bootp packet; with options, "
		    "need %d bytes, got %d",
		    bp->optmagic - p, n);
		return NULL;
	}

	if (conf.xid != nhgetl(bp->xid)) /* not meant for us */
		return NULL;

	if (bp->op != Bootreply) {
		warning("parsebootp: bad op %d", bp->op);
		return NULL;
	}

	n -= bp->optmagic - p;
	p = bp->optmagic;

	if (n < 4) {
		warning("parsebootp: no option data");
		return NULL;
	}
	if (memcmp(optmagic, p, 4) != 0) {
		warning("parsebootp: bad opt magic %ux %ux %ux %ux",
		        p[0], p[1], p[2], p[3]);
		return NULL;
	}
	p += 4;
	n -= 4;
	DEBUG("parsebootp: new packet");
	if (parseoptions(p, n) < 0)
		return NULL;
	return bp;
}

/* write out an ndb entry */
void
writendb(char *s, int n, int append)
{
	char file[64];
	int fd;

	snprintf(file, sizeof file, "%s/ndb", conf.mpoint);
	if (append) {
		fd = open(file, O_WRITE);
		lseek(fd, 0, 2);
	} else
		fd = open(file, O_WRITE | O_TRUNC);
	write(fd, s, n);
	close(fd);
}

/* put server addresses into the ndb entry */
void
putaddrs(char *buf, size_t size, char *attr, uint8_t *a, int len)
{
	int i;
	char p[256];

	for (i = 0; i < len && validip(a); i += IPaddrlen, a += IPaddrlen) {
		snprintf(p, sizeof p, "%s=%R\n", attr, a);
		strlcat(buf, p, size);
	}
}

/* make an ndb entry and put it into /net/ndb for the servers to see */
void
putndb(void)
{
	int append;
	char buf[1024], p[256];
	char *np;
	size_t len;

	buf[0] = '\0';
	if (getndb() == 0)
		append = 1;
	else {
		append = 0;
		snprintf(p, sizeof p, "ip=%R ipmask=%M ipgw=%R\n",
		         conf.laddr, conf.mask, conf.gaddr);
		strlcat(buf, p, sizeof buf);
	}
	if ((np = strchr(conf.hostname, '.')) != NULL) {
		if (*conf.domainname == 0)
			strlcpy(conf.domainname, np + 1, sizeof(conf.domainname));
		*np = 0;
	}
	if (*conf.hostname) {
		snprintf(p, sizeof(p), "\tsys=%s\n", conf.hostname);
		strlcat(buf, p, sizeof buf);
	}
	if (*conf.domainname) {
		snprintf(p, sizeof p, "\tdom=%s.%s\n", conf.hostname, conf.domainname);
		strlcat(buf, p, sizeof buf);
	}
	if (validip(conf.fs))
		putaddrs(buf, sizeof buf, "\tfs", conf.fs, sizeof conf.fs);
	if (validip(conf.auth))
		putaddrs(buf, sizeof buf, "\tauth", conf.auth, sizeof conf.auth);
	if (validip(conf.dns))
		putaddrs(buf, sizeof buf, "\tdns", conf.dns, sizeof conf.dns);
	if (validip(conf.ntp))
		putaddrs(buf, sizeof buf, "\tntp", conf.ntp, sizeof conf.ntp);
	if (ndboptions) {
		snprintf(p, sizeof p, "%s\n", ndboptions);
		strlcat(buf, p, sizeof buf);
	}
	len = strlen(buf);
	if (len > 0)
		writendb(buf, len, append);
}

/* get an ndb entry someone else wrote */
int
getndb(void)
{
	char buf[1024];
	int fd, n;
	char *p;

	snprintf(buf, sizeof buf, "%s/ndb", conf.mpoint);
	fd = open(buf, O_RDONLY);
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strstr(buf, "ip=");
	if (p == NULL)
		return -1;
	if (parseip(conf.laddr, p + 3) == -1)
		fprintf(stderr, "%s: bad address %s\n", argv0, p + 3);
	return 0;
}

/* tell a server to refresh */
void
tweakserver(char *server)
{
	int fd;
	char file[64];

	snprintf(file, sizeof file, "%s/%s", conf.mpoint, server);
	fd = open(file, O_RDWR);
	if (fd < 0)
		return;
	write(fd, "refresh", strlen("refresh"));
	close(fd);
}

/* tell all servers to refresh their information */
void
tweakservers(void)
{
	tweakserver("dns");
	tweakserver("cs");
}

/* return number of networks */
int
nipifcs(char *net)
{
	int n;
	struct ipifc *nifc;
	struct iplifc *lifc;

	n = 0;
	ifc = readipifc(net, ifc, -1);
	for (nifc = ifc; nifc != NULL; nifc = nifc->next) {
		/*
		 * ignore loopback devices when trying to
		 * figure out if we're the primary interface.
		 */
		if (strcmp(nifc->dev, "/dev/null") != 0)
			for (lifc = nifc->lifc; lifc != NULL; lifc = lifc->next)
				if (validip(lifc->ip)) {
					n++;
					break;
				}
		if (strcmp(nifc->dev, conf.dev) == 0)
			myifc = nifc->index;
	}
	return n;
}

/* return true if this is a valid v4 address */
int
validip(uint8_t *addr)
{
	return ipcmp(addr, IPnoaddr) != 0 && ipcmp(addr, v4prefix) != 0;
}

/* look for an action */
int
parseverb(char *name)
{
	int i;

	for (i = 0; i < COUNT_OF(verbs); i++)
		if (verbs[i] != NULL && strcmp(name, verbs[i]) == 0)
			return i;
	return -1;
}

/* get everything out of ndb */
void
ndbconfig(void)
{
	int nattr, nauth = 0, ndns = 0, nfs = 0, ok;
	char etheraddr[32];
	char *attrs[10];
	struct ndb *db;
	struct ndbtuple *t, *nt;

	db = ndbopen(0);
	if (db == NULL) {
		fprintf(stderr, "can't open ndb: %r");
		exit(-1);
	}
	if ((strcmp(conf.type, "ether") != 0 && strcmp(conf.type, "gbe") != 0) ||
	    myetheraddr(conf.hwa, conf.dev) != 0) {
		fprintf(stderr, "can't read hardware address");
		exit(-1);
	}
	snprintf(etheraddr, sizeof etheraddr, "%E", conf.hwa);
	nattr = 0;
	attrs[nattr++] = "ip";
	attrs[nattr++] = "ipmask";
	attrs[nattr++] = "ipgw";
	/* the @ triggers resolution to an IP address; see ndb(2) */
	attrs[nattr++] = "@dns";
	attrs[nattr++] = "@ntp";
	attrs[nattr++] = "@fs";
	attrs[nattr++] = "@auth";
	attrs[nattr] = NULL;
	t = ndbipinfo(db, "ether", etheraddr, attrs, nattr);
	for (nt = t; nt != NULL; nt = nt->entry) {
		ok = 1;
		if (strcmp(nt->attr, "ip") == 0)
			ok = parseip(conf.laddr, nt->val);
		else if (strcmp(nt->attr, "ipmask") == 0)
			parseipmask(conf.mask, nt->val); /* could be -1 */
		else if (strcmp(nt->attr, "ipgw") == 0)
			ok = parseip(conf.gaddr, nt->val);
		else if (ndns < 2 && strcmp(nt->attr, "dns") == 0)
			ok = parseip(conf.dns + IPaddrlen * ndns, nt->val);
		else if (strcmp(nt->attr, "ntp") == 0)
			ok = parseip(conf.ntp, nt->val);
		else if (nfs < 2 && strcmp(nt->attr, "fs") == 0)
			ok = parseip(conf.fs + IPaddrlen * nfs, nt->val);
		else if (nauth < 2 && strcmp(nt->attr, "auth") == 0)
			ok = parseip(conf.auth + IPaddrlen * nauth, nt->val);
		if (!ok)
			fprintf(stderr, "%s: bad %s address in ndb: %s\n",
			        argv0, nt->attr, nt->val);
	}
	ndbfree(t);
	if (!validip(conf.laddr)) {
		fprintf(stderr, "address not found in ndb");
		exit(-1);
	}
}

int
addoption(char *opt)
{
	int i;
	Option *o;

	if (opt == NULL)
		return -1;
	for (o = option; o < &option[COUNT_OF(option)]; o++)
		if (o->name && strcmp(opt, o->name) == 0) {
			i = o - option;
			if (memchr(requested, i, nrequested) == 0 &&
			    nrequested < COUNT_OF(requested))
				requested[nrequested++] = i;
			return 0;
		}
	return -1;
}

char *
optgetx(uint8_t *p, uint8_t opt)
{
	int i, n;
	uint32_t x;
	char *s, *ns;
	char str[256];
	uint8_t ip[IPaddrlen], ips[16 * IPaddrlen], vec[256];
	Option *o;

	o = &option[opt];
	if (o->name == NULL)
		return NULL;

	s = NULL;
	switch (o->type) {
	case Taddr:
		if (optgetaddr(p, opt, ip))
			asprintf(&s, "%s=%R", o->name, ip);
		break;
	case Taddrs:
		n = optgetaddrs(p, opt, ips, 16);
		if (n > 0)
			asprintf(&s, "%s=%R", o->name, ips);
		for (i = 1; i < n; i++) {
			asprintf(&ns, "%s %s=%R",
			         s, o->name, &ips[i * IPaddrlen]);
			free(s);
			s = ns;
		}
		break;
	case Tulong:
		x = optgetulong(p, opt);
		if (x != 0)
			asprintf(&s, "%s=%lud", o->name, x);
		break;
	case Tbyte:
		x = optgetbyte(p, opt);
		if (x != 0)
			asprintf(&s, "%s=%lud", o->name, x);
		break;
	case Tstr:
		if (optgetstr(p, opt, str, sizeof str))
			asprintf(&s, "%s=%s", o->name, str);
		break;
	case Tvec:
		n = optgetvec(p, opt, vec, sizeof vec);
		if (n > 0) /* what's %H?  it's not installed */
			asprintf(&s, "%s=%.*H", o->name, n, vec);
		break;
	}
	return s;
}

void
getoptions(uint8_t *p)
{
	int i;
	char *s, *t;

	for (i = COUNT_OF(defrequested); i < nrequested; i++) {
		s = optgetx(p, requested[i]);
		if (s != NULL)
			DEBUG("%s ", s);
		if (ndboptions == NULL)
			asprintf(&ndboptions, "\t%s", s);
		else {
			t = ndboptions;
			asprintf(&ndboptions, "\t%s%s", s, t);
			free(t);
		}
		free(s);
	}
}
