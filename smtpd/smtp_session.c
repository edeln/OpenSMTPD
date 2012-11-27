/*	$OpenBSD: smtp_session.c,v 1.175 2012/11/12 14:58:53 eric Exp $	*/

/*
 * Copyright (c) 2008 Gilles Chehade <gilles@openbsd.org>
 * Copyright (c) 2008 Pierre-Yves Ritschard <pyr@openbsd.org>
 * Copyright (c) 2008-2009 Jacek Masiulaniec <jacekm@dobremiasto.net>
 * Copyright (c) 2012 Eric Faurot <eric@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <inttypes.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vis.h>

#include <openssl/ssl.h>

#include "smtpd.h"
#include "log.h"

#define SMTP_KICKTHRESHOLD	50

#define SMTP_MAXMAIL	100
#define SMTP_MAXRCPT	1000

enum {
	CMD_HELO = 0,
	CMD_EHLO,
	CMD_STARTTLS,
	CMD_AUTH,
	CMD_MAIL_FROM,
	CMD_RCPT_TO,
	CMD_DATA,
	CMD_RSET,
	CMD_QUIT,
	CMD_HELP,
	CMD_NOOP,
};

enum smtp_state {
	S_NEW = 0,
	S_CONNECTED,
	S_TLS,
	S_HELO,
	S_AUTH_INIT,
	S_AUTH_USERNAME,
	S_AUTH_PASSWORD,
	S_AUTH_FINALIZE,
	S_BODY,
	S_QUIT,
};

enum session_flags {
	F_EHLO			= 0x01,
	F_8BITMIME		= 0x02,
	F_SECURE		= 0x04,
	F_AUTHENTICATED		= 0x08,

	F_SMTP_MESSAGE_END	= 0x10,
	F_MFA_MESSAGE_END	= 0x20,

	F_KICK			= 0x40,
};

enum {
	PHASE_INIT = 0,
	PHASE_SETUP,
	PHASE_TRANSACTION
};

struct smtp_session {
	uint64_t		 id;
	struct iobuf		 iobuf;
	struct io		 io;
	struct listener		*listener;
	struct sockaddr_storage	 ss;
	char			 hostname[MAXHOSTNAMELEN];

	int			 flags;
	int			 phase;
	enum smtp_state		 state;

	struct auth		 auth;
	struct envelope		 evp;

	char			 cmd[SMTP_LINE_MAX];

	size_t			 kickcount;
	size_t			 mailcount;
	size_t			 rcptcount;
	size_t			 destcount;

	FILE			*ofile;
	size_t			 datalen;

	int			 s_dstatus;
};

#define ADVERTISE_TLS(s) \
	((s)->listener->flags & F_STARTTLS && !((s)->flags & F_SECURE))

#define ADVERTISE_AUTH(s) \
	((s)->listener->flags & F_AUTH && (s)->flags & F_SECURE && \
	 !((s)->flags & F_AUTHENTICATED))

void	 ssl_error(const char *);

static void smtp_io(struct io *, int);
static void smtp_enter_state(struct smtp_session *, int);
static void smtp_reply(struct smtp_session *, char *, ...);
static void smtp_command(struct smtp_session *, char *);
static int smtp_parse_mail_args(struct smtp_session *, char *);
static void smtp_rfc4954_auth_plain(struct smtp_session *, char *);
static void smtp_rfc4954_auth_login(struct smtp_session *, char *);
static void smtp_end_body(struct smtp_session *);
static void smtp_queue_data(struct smtp_session *, char *);
static void smtp_free(struct smtp_session *, const char *);
static const char *smtp_strstate(int);

static int smtp_mailaddr(struct mailaddr *, char *);

static struct { int code; const char *cmd; } commands[] = {
	{ CMD_HELO,		"HELO" },
	{ CMD_EHLO,		"EHLO" },
	{ CMD_STARTTLS,		"STARTTLS" },
	{ CMD_AUTH,		"AUTH" },
	{ CMD_MAIL_FROM,	"MAIL FROM" },
	{ CMD_RCPT_TO,		"RCPT TO" },
	{ CMD_DATA,		"DATA" },
	{ CMD_RSET,		"RSET" },
	{ CMD_QUIT,		"QUIT" },
	{ CMD_HELP,		"HELP" },
	{ CMD_NOOP,		"NOOP" },
	{ -1, NULL },
};

static struct tree wait_lka_ptr;
static struct tree wait_mfa_connect;
static struct tree wait_mfa_data;
static struct tree wait_mfa_helo;
static struct tree wait_mfa_mailfrom;
static struct tree wait_mfa_rcpt;
static struct tree wait_parent_auth;
static struct tree wait_queue_msg;
static struct tree wait_queue_fd;
static struct tree wait_queue_commit;

int
smtp_session(struct listener *listener, int sock,
    const struct sockaddr_storage *ss, const char *hostname)
{
	static int		 init = 0;
	struct smtp_session	*s;

	log_debug("debug: smtp: new client on listener: %p", listener);

	if (!init) {
		tree_init(&wait_lka_ptr);
		tree_init(&wait_mfa_connect);
		tree_init(&wait_mfa_data);
		tree_init(&wait_mfa_helo);
		tree_init(&wait_mfa_mailfrom);
		tree_init(&wait_mfa_rcpt);
		tree_init(&wait_parent_auth);
		tree_init(&wait_queue_msg);
		tree_init(&wait_queue_fd);
		tree_init(&wait_queue_commit);
		init = 1;
	}

	if ((s = calloc(1, sizeof(*s))) == NULL)
		return (-1);
	if (iobuf_init(&s->iobuf, MAX_LINE_SIZE, MAX_LINE_SIZE) == -1) {
		free(s);
		return (-1);
	}
	s->id = generate_uid();
	s->listener = listener;
	memmove(&s->ss, ss, sizeof(*ss));
	io_init(&s->io, sock, s, smtp_io, &s->iobuf);
	io_set_timeout(&s->io, SMTPD_SESSION_TIMEOUT * 1000);
	io_set_write(&s->io);

	s->state = S_NEW;
	s->phase = PHASE_INIT;

	strlcpy(s->evp.tag, listener->tag, sizeof(s->evp.tag));
	s->evp.session_id = s->id;
	s->evp.ss = s->ss;

	/* For local enqueueing, the hostname is already set */
	if (hostname) {
		/* A bit of a hack */
		if (!strcmp(hostname, "localhost"))
			s->evp.flags |= DF_BOUNCE;
		strlcpy(s->hostname, hostname, sizeof(s->hostname));
		strlcpy(s->evp.hostname, hostname, sizeof(s->evp.hostname));
		smtp_enter_state(s, S_CONNECTED);
	} else {
		dns_query_ptr(&s->ss, s->id);
		tree_xset(&wait_lka_ptr, s->id, s);
	}

	return (0);
}

void
smtp_session_imsg(struct imsgev *iev, struct imsg *imsg)
{
	struct queue_resp_msg	*queue_resp;
	struct queue_req_msg	 queue_req;
	struct mfa_resp_msg	*mfa_resp;
	struct smtp_session	*s;
	struct auth		*auth;
	struct dns		*dns;
	void			*ssl;
	char			 user[MAXLOGNAME];

	switch (imsg->hdr.type) {
	case IMSG_DNS_PTR:
		dns = imsg->data;
		s = tree_xpop(&wait_lka_ptr, dns->id);
		if (dns->error)
			strlcpy(s->hostname, "<unknown>", sizeof s->hostname);
		else
			strlcpy(s->hostname, dns->host, sizeof s->hostname);
		strlcpy(s->evp.hostname, s->hostname, sizeof s->evp.hostname);
		smtp_enter_state(s, S_CONNECTED);
		return;

	case IMSG_MFA_CONNECT:
		mfa_resp = imsg->data;
		s = tree_xpop(&wait_mfa_connect, mfa_resp->reqid);
		if (mfa_resp->status != MFA_OK) {
			log_info("smtp-in: Disconnecting session %016" PRIx64
			    ": rejected by filter", s->id);
			smtp_free(s, "rejected by filter");
			return;
		}

		if (s->listener->flags & F_SMTPS) {
			ssl = ssl_smtp_init(s->listener->ssl_ctx);
			io_set_read(&s->io);
			io_start_tls(&s->io, ssl);
			return;
		}
		smtp_reply(s, SMTPD_BANNER, env->sc_hostname);
		smtp_enter_state(s, S_HELO);
		io_reload(&s->io);
		return;

	case IMSG_MFA_HELO:
		mfa_resp = imsg->data;
		s = tree_xpop(&wait_mfa_helo, mfa_resp->reqid);
		if (mfa_resp->status != MFA_OK) {
			smtp_reply(s, "%d Hello rejected", mfa_resp->code);
			io_reload(&s->io);
			return;
		}

		smtp_reply(s, "250%c%s Hello %s [%s], pleased to meet you",
		    (s->flags & F_EHLO) ? '-' : ' ',
		    env->sc_hostname,
		    s->evp.helo,
		    ss_to_text(&s->ss));

		if (s->flags & F_EHLO) {
			smtp_reply(s, "250-8BITMIME");
			smtp_reply(s, "250-ENHANCEDSTATUSCODES");
			smtp_reply(s, "250-SIZE %zu", env->sc_maxsize);
			if (ADVERTISE_TLS(s))
				smtp_reply(s, "250-STARTTLS");
			if (ADVERTISE_AUTH(s))
				smtp_reply(s, "250-AUTH PLAIN LOGIN");
			smtp_reply(s, "250 HELP");
		}
		s->kickcount = 0;
		s->phase = PHASE_SETUP;
		io_reload(&s->io);
		return;

	case IMSG_MFA_MAIL:
		mfa_resp = imsg->data;
		s = tree_xpop(&wait_mfa_mailfrom, mfa_resp->reqid);
		if (mfa_resp->status != MFA_OK) {
			smtp_reply(s, "%d Sender rejected", mfa_resp->code);
			io_reload(&s->io);
			return;
		}
		s->evp.sender = mfa_resp->u.mailaddr;

		queue_req.reqid = s->id;
		queue_req.evpid = 0;
		imsg_compose_event(env->sc_ievs[PROC_QUEUE],
		    IMSG_QUEUE_CREATE_MESSAGE, 0, 0, -1,
		    &queue_req, sizeof(queue_req));
		tree_xset(&wait_queue_msg, s->id, s);
		return;

	case IMSG_MFA_RCPT:
		mfa_resp = imsg->data;
		s = tree_xpop(&wait_mfa_rcpt, mfa_resp->reqid);
		if (mfa_resp->status != MFA_OK) {
			smtp_reply(s, "%d 5.0.0 Recipient rejected: %s@%s",
			    mfa_resp->code,
			    s->evp.rcpt.user,
			    s->evp.rcpt.domain);
			/* s->rcptfailed++; XXX */
		}
		else {
			s->rcptcount++;
			s->kickcount--;
			smtp_reply(s, "%d 2.0.0 Recipient ok", mfa_resp->code);
		}
		io_reload(&s->io);
		return;

	case IMSG_MFA_DATALINE:
		mfa_resp = imsg->data;
		if (!strcmp(mfa_resp->u.buffer, ".")) {
			s = tree_pop(&wait_mfa_data, mfa_resp->reqid);
			if (s == NULL)
				return;	/* dead session */
			s->flags |= F_MFA_MESSAGE_END;
			smtp_end_body(s);
		} else {
			s = tree_get(&wait_mfa_data, mfa_resp->reqid);
			if (s == NULL)
				return;	/* dead session */
			smtp_queue_data(s, mfa_resp->u.buffer);
		}
		return;

	case IMSG_QUEUE_CREATE_MESSAGE:
		queue_resp = imsg->data;
		s = tree_xpop(&wait_queue_msg, queue_resp->reqid);
		if (queue_resp->success) {
			s->evp.id = queue_resp->evpid;
			s->rcptcount = 0;
			s->phase = PHASE_TRANSACTION;
			smtp_reply(s, "250 Ok");
		} else {
			smtp_reply(s, "421 Temporary Error");
		}
		io_reload(&s->io);
		return;

	case IMSG_QUEUE_MESSAGE_FILE:
		queue_resp = imsg->data;
		s = tree_xpop(&wait_queue_fd, queue_resp->reqid);
		if (!queue_resp->success) {
			smtp_reply(s, "421 Temporary Error");
			/* XXX ROLLBACK */
			return;
		}
		if (imsg->fd == -1) {
			smtp_reply(s, "421 Temporary Error");
			/* XXX ROLLBACK */
			return;
		}
		if ((s->ofile = fdopen(imsg->fd, "w")) == NULL) {
			close(imsg->fd);
			smtp_reply(s, "421 Temporary Error");
			/* XXX ROLLBACK */
			return;
		}

		fprintf(s->ofile, "Received: from %s (%s [%s]);\n"
		    "\tby %s (OpenSMTPD) with %sSMTP id %08x;\n",
		    s->evp.helo,
		    s->hostname,
		    ss_to_text(&s->ss),
		    env->sc_hostname,
		    s->flags & F_EHLO ? "E" : "",
		    evpid_to_msgid(s->evp.id));

		if (s->flags & F_SECURE)
			fprintf(s->ofile,
			    "\tTLS version=%s cipher=%s bits=%d;\n",
			    SSL_get_cipher_version(s->io.ssl),
			    SSL_get_cipher_name(s->io.ssl),
			    SSL_get_cipher_bits(s->io.ssl, NULL));

		if (s->rcptcount == 1)
			fprintf(s->ofile, "\tfor <%s@%s>;\n",
			    s->evp.rcpt.user,
			    s->evp.rcpt.domain);

		fprintf(s->ofile, "\t%s\n", time_to_text(time(NULL)));

		/* XXX Test ferror() */
		smtp_reply(s, "354 Enter mail, end with \".\" on a line"
		    "by itself");

		tree_xset(&wait_mfa_data, s->id, s);
		smtp_enter_state(s, S_BODY);
		/* By-pass mfa for message body */
		if (!(env->filtermask & HOOK_DATALINE)) {
			log_debug("disabling mfa for msg body");
			s->flags |= F_MFA_MESSAGE_END;
		}
		io_reload(&s->io);
		return;

	case IMSG_QUEUE_SUBMIT_ENVELOPE:
		queue_resp = imsg->data;
		s = tree_xget(&wait_mfa_rcpt, queue_resp->reqid);
		if (queue_resp->success)
			s->destcount++;
		else
			s->s_dstatus |= DS_TEMPFAILURE;
		return;

	case IMSG_QUEUE_COMMIT_ENVELOPES:
		queue_resp = imsg->data;
		s = tree_xpop(&wait_mfa_rcpt, queue_resp->reqid);
		/* This cannot fail. */
		s->rcptcount++;
		s->kickcount--;
		smtp_reply(s, "250 2.0.0 Recipient ok");
		io_reload(&s->io);
		return;

	case IMSG_QUEUE_COMMIT_MESSAGE:
		queue_resp = imsg->data;
		s = tree_xpop(&wait_queue_commit, queue_resp->reqid);
		if (!queue_resp->success) {
			smtp_reply(s, "421 Temporay failure");
			/* XXX reset */
			io_reload(&s->io);
			return;
		}

		smtp_reply(s, "250 2.0.0 %08x Message accepted for delivery",
		    evpid_to_msgid(s->evp.id));
		log_info("smtp-in: Accepted message %08x on session %016"PRIx64
		    ": from=<%s%s%s>, size=%ld, nrcpts=%zu, proto=%s",
		    evpid_to_msgid(s->evp.id),
		    s->id,
		    s->evp.sender.user,
		    s->evp.sender.user[0] == '\0' ? "" : "@",
		    s->evp.sender.domain,
		    s->datalen,
		    s->rcptcount,
		    s->flags & F_EHLO ? "ESMTP" : "SMTP");

		s->mailcount++;
		s->evp.id = 0;
		s->phase = PHASE_SETUP;
		s->kickcount = 0;
		smtp_enter_state(s, S_HELO);
		io_reload(&s->io);
		return;

	case IMSG_PARENT_AUTHENTICATE:
		auth = imsg->data;
		s = tree_xpop(&wait_parent_auth, auth->id);
		strnvis(user, auth->user, sizeof user, VIS_WHITE | VIS_SAFE);
		if (auth->success) {
			log_info("smtp-in: Accepted authentication for user %s "
			    "on session %016"PRIx64, user, s->id);
			s->kickcount = 0;
			s->flags |= F_AUTHENTICATED;
			smtp_reply(s, "235 Authentication succeeded");
		}
		else {
			log_info("smtp-in: Authentication failed for user %s "
			    "on session %016"PRIx64, user, s->id);
			smtp_reply(s, "535 Authentication failed");
		}
		io_reload(&s->io);
		return;
	}

	log_warnx("smtp_session_imsg: unexpected %s imsg",
	    imsg_to_str(imsg->hdr.type));
	fatalx(NULL);
}

static void
smtp_io(struct io *io, int evt)
{
	struct smtp_session    *s = io->arg;
	struct mfa_req_msg	req;
	void		       *ssl;
	char		       *line;
	size_t			len;

	log_trace(TRACE_IO, "smtp: %p: %s %s", s, io_strevent(evt),
	    io_strio(io));

	switch (evt) {

	case IO_TLSREADY:
		log_info("smtp-in: Started TLS on session %016"PRIx64": %s",
		    s->id, ssl_to_text(s->io.ssl));
		s->flags |= F_SECURE;
		s->kickcount = 0;
		if (s->listener->flags & F_SMTPS) {
			stat_increment("smtp.smtps", 1);
			smtp_reply(s, SMTPD_BANNER, env->sc_hostname);
			io_set_write(&s->io);
		}
		else {
			stat_increment("smtp.tls", 1);
		}
		break;

	case IO_DATAIN:
	    nextline:
		line = iobuf_getline(&s->iobuf, &len);
		if ((line == NULL && iobuf_len(&s->iobuf) >= SMTP_LINE_MAX) ||
		    (line && len >= SMTP_LINE_MAX)) {
			smtp_reply(s, "500 5.0.0 Line too long");
			smtp_enter_state(s, S_QUIT);
			io_set_write(io);
			return;
		}

		/* No complete line received */
		if (line == NULL) {
			iobuf_normalize(&s->iobuf);
			return;
		}

		/* Message body */
		if (s->state == S_BODY && strcmp(line, ".")) {
			if (env->filtermask & HOOK_DATALINE) {
				req.reqid = s->id;
				if (strlcpy(req.u.buffer, line,
				    sizeof req.u.buffer) >= sizeof req.u.buffer)
					fatalx("smtp_io: data truncation");
				imsg_compose_event(env->sc_ievs[PROC_MFA],
				    IMSG_MFA_DATALINE, 0, 0, -1, &req,
				    sizeof(req));
			} else
				smtp_queue_data(s, line);
			goto nextline;
		}

		/* Pipelining not supported */
		if (iobuf_len(&s->iobuf)) {
			smtp_reply(s, "500 5.0.0 Pipelining not supported");
			smtp_enter_state(s, S_QUIT);
			io_set_write(io);
			return;
		}

		/* End of body */
		if (s->state == S_BODY) {
			s->flags |= F_SMTP_MESSAGE_END;
			iobuf_normalize(&s->iobuf);
			io_set_write(io);
			smtp_end_body(s);
			return;
		}

		/* Must be a command */
		strlcpy(s->cmd, line, sizeof s->cmd);
		iobuf_normalize(&s->iobuf);
		io_set_write(io);
		smtp_command(s, s->cmd);
		if (s->flags & F_KICK) {
			smtp_free(s, "kick");
			return;
		}
		break;

	case IO_LOWAT:
		if (s->state == S_QUIT) {
			log_info("smtp-in: Closing session %016" PRIx64, s->id);
			smtp_free(s, "done");
			break;
		}

		io_set_read(io);

		/* Wait for the client to start tls */
		if (s->state == S_TLS) {
			ssl = ssl_smtp_init(s->listener->ssl_ctx);
			io_start_tls(io, ssl);
		}
		break;

	case IO_TIMEOUT:
		log_info("smtp-in: Disconnecting session %016"PRIx64": "
		    "session timeout", s->id);
		smtp_free(s, "timeout");
		break;

	case IO_DISCONNECTED:
		log_info("smtp-in: Received disconnect from session %016"PRIx64,
		    s->id);
		smtp_free(s, "disconnected");
		break;

	case IO_ERROR:
		log_info("smtp-in: Disconnecting session %016"PRIx64": "
		    "IO error: %s", s->id, io->error);
		smtp_free(s, "IO error");
		break;

	default:
		fatalx("smtp_io()");
	}
}

static void
smtp_command(struct smtp_session *s, char *line)
{
	struct queue_req_msg	 queue_req;
	struct mfa_req_msg	 mfa_req;
	char			*args, *eom, *method;
	int			 cmd, i;

	log_trace(TRACE_SMTP, "smtp: %p: <<< %s", s, line);

	if (++s->kickcount >= SMTP_KICKTHRESHOLD) {
		log_info("smtp-in: Disconnecting session %016" PRIx64
		    ": session not moving forward", s->id);
		s->flags |= F_KICK;
		stat_increment("smtp.kick", 1);
		return;
	}

	/*
	 * These states are special
	 */
	if (s->state == S_AUTH_INIT) {
		smtp_rfc4954_auth_plain(s, line);
		return;
	}
	if (s->state == S_AUTH_USERNAME || s->state == S_AUTH_PASSWORD) {
		smtp_rfc4954_auth_login(s, line);
		return;
	}

	/*
	 * Unlike other commands, "mail from" and "rcpt to" contain a
	 * space in the command name.
	 */
	if (strncasecmp("mail from:", line, 10) == 0 ||
	    strncasecmp("rcpt to:", line, 8) == 0)
		args = strchr(line, ':');
	else
		args = strchr(line, ' ');

	if (args) {
		*args++ = '\0';
		while (isspace((int)*args))
			args++;
	}

	cmd = -1;
	for (i = 0; commands[i].code != -1; i++)
		if (!strcasecmp(line, commands[i].cmd)) {
			cmd = commands[i].code;
			break;
		}

	switch (cmd) {
	/*
	 * INIT
	 */
	case CMD_HELO:
	case CMD_EHLO:
		if (s->phase != PHASE_INIT) {
			smtp_reply(s, "XXX Already indentified");
			break;
		}

		if (args == NULL) {
			smtp_reply(s, "501 %s requires domain address",
			    (cmd == CMD_HELO) ? "HELO" : "EHLO");
			break;
		}

		if (!valid_domainpart(args)) {
			smtp_reply(s, "501 Invalid domain name");
			break;
		}
		strlcpy(s->evp.helo, args, sizeof(s->evp.helo));
		s->evp.session_id = s->id;
		s->flags &= F_SECURE|F_AUTHENTICATED;
		if (cmd == CMD_EHLO) {
			s->flags |= F_EHLO;
			s->flags |= F_8BITMIME;
		}
		mfa_req.reqid = s->id;
		mfa_req.u.evp = s->evp;
		imsg_compose_event(env->sc_ievs[PROC_MFA], IMSG_MFA_HELO,
		    0, 0, -1, &mfa_req, sizeof(mfa_req));
		tree_xset(&wait_mfa_helo, s->id, s);
		break;
	/*
	 * SETUP
	 */
	case CMD_STARTTLS:
		if (s->phase != PHASE_SETUP) {
			smtp_reply(s, "XXX Command not allowed at this point.");
			break;
		}

		if (s->flags & F_SECURE) {
			smtp_reply(s, "501 Channel already secured");
			break;
		}
		if (args != NULL) {
			smtp_reply(s, "501 No parameters allowed");
			break;
		}
		smtp_reply(s, "220 Ready to start TLS");
		smtp_enter_state(s, S_TLS);
		break;

	case CMD_AUTH:
		if (s->phase != PHASE_SETUP) {
			smtp_reply(s, "XXX Command not allowed at this point.");
			break;
		}

		if (s->flags & F_AUTHENTICATED) {
			smtp_reply(s, "503 Already authenticated");
			break;
		}

		if (!ADVERTISE_AUTH(s)) {
			smtp_reply(s, "503 Command not supported");
			break;
		}

		if (args == NULL) {
			smtp_reply(s, "501 No parameters given");
			break;
		}

		method = args;
		eom = strchr(args, ' ');
		if (eom == NULL)
			eom = strchr(args, '\t');
		if (eom != NULL)
			*eom++ = '\0';
		if (strcasecmp(method, "PLAIN") == 0)
			smtp_rfc4954_auth_plain(s, eom);
		else if (strcasecmp(method, "LOGIN") == 0)
			smtp_rfc4954_auth_login(s, eom);
		else
			smtp_reply(s, "504 AUTH method \"%s\" not supported",
			    method);
		break;

	case CMD_MAIL_FROM:
		if (s->phase != PHASE_SETUP) {
			smtp_reply(s, "XXX Command not allowed at this point.");
			break;
		}

		if (s->listener->flags & F_STARTTLS_REQUIRE &&
		    !(s->flags & F_SECURE)) {
			smtp_reply(s,
			    "530 5.7.0 Must issue a STARTTLS command first");
			break;
		}

		if (s->listener->flags & F_AUTH_REQUIRE &&
		    !(s->flags & F_AUTHENTICATED)) {
			smtp_reply(s,
			    "530 5.7.0 Must issue an AUTH command first");
			break;
		}

		if (s->mailcount >= SMTP_MAXMAIL) {
			smtp_reply(s, "452 Too many messages sent");
			break;
		}

		if (smtp_mailaddr(&s->evp.sender, args) == 0) {
			smtp_reply(s, "553 5.1.7 Sender address syntax error");
			break;
		}

		if (s->flags & F_EHLO && smtp_parse_mail_args(s, args) == -1)
			break;

		mfa_req.reqid = s->id;
		mfa_req.u.evp = s->evp;
		imsg_compose_event(env->sc_ievs[PROC_MFA], IMSG_MFA_MAIL,
		    0, 0, -1, &mfa_req, sizeof(mfa_req));
		tree_xset(&wait_mfa_mailfrom, s->id, s);
		break;
	/*
	 * TRANSACTION
	 */
	case CMD_RCPT_TO:
		if (s->phase != PHASE_TRANSACTION) {
			smtp_reply(s, "XXX Command not allowed at this point.");
			break;
		}

		if (s->rcptcount >= SMTP_MAXRCPT) {
			smtp_reply(s, "452 Too many recipients");
			break;
		}

		if (smtp_mailaddr(&s->evp.rcpt, args) == 0) {
			smtp_reply(s,
			    "553 5.1.3 Recipient address syntax error");
			break;
		}
		mfa_req.reqid = s->id;
		mfa_req.u.evp = s->evp;
		imsg_compose_event(env->sc_ievs[PROC_MFA], IMSG_MFA_RCPT,
		    0, 0, -1, &mfa_req, sizeof(mfa_req));
		tree_xset(&wait_mfa_rcpt, s->id, s);
		break;

	case CMD_RSET:
		if (s->phase != PHASE_TRANSACTION) {
			smtp_reply(s, "XXX Command not allowed at this point.");
			break;
		}
		mfa_req.reqid = s->id;
		mfa_req.u.evp = s->evp;
		imsg_compose_event(env->sc_ievs[PROC_MFA], IMSG_MFA_RSET,
		    0, 0, -1, &mfa_req, sizeof(mfa_req));
		smtp_reply(s, "250 2.0.0 Reset state");
		s->phase = PHASE_SETUP;
		s->evp.id = 0;
		break;

	case CMD_DATA:
		if (s->phase != PHASE_TRANSACTION) {
			smtp_reply(s, "XXX Command not allowed at this point.");
			break;
		}
		if (s->rcptcount == 0) {
			smtp_reply(s, "503 5.5.1 No recipient specified");
			break;
		}
		queue_req.reqid = s->id;
		queue_req.evpid = s->evp.id;
		imsg_compose_event(env->sc_ievs[PROC_QUEUE],
		    IMSG_QUEUE_MESSAGE_FILE, 0, 0, -1, &queue_req,
		    sizeof(queue_req));
		tree_xset(&wait_queue_fd, s->id, s);
		break;
	/*
	 * ANY
	 */
	case CMD_QUIT:
		smtp_reply(s, "221 2.0.0 Bye");
		smtp_enter_state(s, S_QUIT);
		break;

	case CMD_NOOP:
		smtp_reply(s, "250 2.0.0 Ok");
		break;

	case CMD_HELP:
		smtp_reply(s, "214- This is OpenSMTPD");
		smtp_reply(s, "214- To report bugs in the implementation, "
		    "please contact bugs@openbsd.org");
		smtp_reply(s, "214- with full details");
		smtp_reply(s, "214 End of HELP info");
		break;

	default:
		smtp_reply(s, "500 Command unrecognized");
		break;
	}
}

static void
smtp_rfc4954_auth_plain(struct smtp_session *s, char *arg)
{
	struct auth	*a = &s->auth;
	char		 buf[1024], *user, *pass;
	int		 len;

	switch (s->state) {
	case S_HELO:
		if (arg == NULL) {
			smtp_enter_state(s, S_AUTH_INIT);
			smtp_reply(s, "334 ");
			return;
		}
		smtp_enter_state(s, S_AUTH_INIT);
		/* FALLTHROUGH */

	case S_AUTH_INIT:
		/* String is not NUL terminated, leave room. */
		if ((len = __b64_pton(arg, (unsigned char *)buf,
			    sizeof(buf) - 1)) == -1)
			goto abort;
		/* buf is a byte string, NUL terminate. */
		buf[len] = '\0';

		/*
		 * Skip "foo" in "foo\0user\0pass", if present.
		 */
		user = memchr(buf, '\0', len);
		if (user == NULL || user >= buf + len - 2)
			goto abort;
		user++; /* skip NUL */
		if (strlcpy(a->user, user, sizeof(a->user)) >= sizeof(a->user))
			goto abort;

		pass = memchr(user, '\0', len - (user - buf));
		if (pass == NULL || pass >= buf + len - 2)
			goto abort;
		pass++; /* skip NUL */
		if (strlcpy(a->pass, pass, sizeof(a->pass)) >= sizeof(a->pass))
			goto abort;

		a->id = s->id;
		imsg_compose_event(env->sc_ievs[PROC_PARENT],
		    IMSG_PARENT_AUTHENTICATE, 0, 0, -1, a, sizeof(*a));
		bzero(a->pass, sizeof(a->pass));
		tree_xset(&wait_parent_auth, s->id, s);
		return;

	default:
		fatal("smtp_rfc4954_auth_plain: unknown state");
	}

abort:
	smtp_reply(s, "501 Syntax error");
	smtp_enter_state(s, S_HELO);
}

static void
smtp_rfc4954_auth_login(struct smtp_session *s, char *arg)
{
	struct auth	*a = &s->auth;

	switch (s->state) {
	case S_HELO:
		smtp_enter_state(s, S_AUTH_USERNAME);
		smtp_reply(s, "334 VXNlcm5hbWU6");
		return;

	case S_AUTH_USERNAME:
		bzero(a->user, sizeof(a->user));
		if (__b64_pton(arg, (unsigned char *)a->user,
			sizeof(a->user) - 1) == -1)
			goto abort;

		smtp_enter_state(s, S_AUTH_PASSWORD);
		smtp_reply(s, "334 UGFzc3dvcmQ6");
		return;

	case S_AUTH_PASSWORD:
		bzero(a->pass, sizeof(a->pass));
		if (__b64_pton(arg, (unsigned char *)a->pass,
			sizeof(a->pass) - 1) == -1)
			goto abort;

		a->id = s->id;
		imsg_compose_event(env->sc_ievs[PROC_PARENT],
		    IMSG_PARENT_AUTHENTICATE, 0, 0, -1, a, sizeof(*a));
		bzero(a->pass, sizeof(a->pass));
		tree_xset(&wait_parent_auth, s->id, s);
		return;

	default:
		fatal("smtp_rfc4954_auth_login: unknown state");
	}

abort:
	smtp_reply(s, "501 Syntax error");
	smtp_enter_state(s, S_HELO);
}

static int
smtp_parse_mail_args(struct smtp_session *s, char *args)
{
	char *b;

	for (b = strrchr(args, ' '); b != NULL; b = strrchr(args, ' ')) {
		*b++ = '\0';
		if (strncasecmp(b, "AUTH=", 5) == 0)
			log_debug("debug: smtp: AUTH in MAIL FROM command");
		else if (!strcasecmp(b, "BODY=7BIT"))
			/* XXX only for this transaction */
			s->flags &= ~F_8BITMIME;
		else if (strcasecmp(b, "BODY=8BITMIME"))
			;
		else {
			smtp_reply(s, "503 5.5.4 Unsupported option %s", b);
			return (-1);
		}
	}

	return (0);
}

void
smtp_enter_state(struct smtp_session *s, int newstate)
{
	struct mfa_req_msg	mfa_req;
	int			oldstate;

	oldstate = s->state;

	log_trace(TRACE_SMTP, "smtp: %p: %s -> %s", s,
	    smtp_strstate(s->state),
	    smtp_strstate(newstate));

	s->state = newstate;

	/* don't try this at home! */
#define smtp_enter_state(_s, _st) do { newstate = _st; goto again; } while(0)

	switch (s->state) {

	case S_CONNECTED:
		log_info("smtp-in: New session %016"PRIx64" from host %s [%s]",
		    s->id, s->hostname, ss_to_text(&s->ss));
		mfa_req.reqid = s->id;
		mfa_req.u.evp = s->evp;
		imsg_compose_event(env->sc_ievs[PROC_MFA], IMSG_MFA_CONNECT,
		    0, 0, -1, &mfa_req, sizeof(mfa_req));
		tree_xset(&wait_mfa_connect, s->id, s);
		break;

	default:
		break;
	}

#undef smtp_enter_state
}

static void
smtp_end_body(struct smtp_session *s)
{
	struct queue_req_msg	queue_req;

	log_trace(TRACE_SMTP, "[EOM] 0x%04x", s->flags);

	if (!(s->flags & F_SMTP_MESSAGE_END && s->flags & F_MFA_MESSAGE_END))
		return;

	log_trace(TRACE_SMTP, "[GO]");

	s->phase = PHASE_SETUP;

	s->datalen = ftell(s->ofile);
	if (! safe_fclose(s->ofile))
		s->s_dstatus |= DS_TEMPFAILURE;
	s->ofile = NULL;

	if (s->s_dstatus & DS_PERMFAILURE) {
		smtp_reply(s, "554 5.0.0 Transaction failed");
		smtp_enter_state(s, S_HELO);
		return;
	}

	if (s->s_dstatus & DS_TEMPFAILURE) {
		smtp_reply(s, "421 4.0.0 Temporary failure");
		smtp_enter_state(s, S_QUIT);
		stat_increment("smtp.tempfail", 1);
		return;
	}

	queue_req.reqid = s->id;
	queue_req.evpid = s->evp.id;
	imsg_compose_event(env->sc_ievs[PROC_QUEUE], IMSG_QUEUE_COMMIT_MESSAGE,
	    0, 0, -1, &queue_req, sizeof(queue_req));
	tree_xset(&wait_queue_commit, s->id, s);
}

static void
smtp_queue_data(struct smtp_session *s, char *line)
{
	size_t datalen, i, len;


	log_trace(TRACE_SMTP, "[BODY] %s", line);

	/* Don't waste resources on message if it's going to bin anyway. */
	if (s->s_dstatus & (DS_PERMFAILURE|DS_TEMPFAILURE))
		return;

	/*
	 * "If the first character is a period and there are other characters
	 *  on the line, the first character is deleted." [4.5.2]
	 */
	if (*line == '.')
		line++;

	len = strlen(line);

	/*
	 * If size of data overflows a size_t or exceeds max size allowed
	 * for a message, set permanent failure.
	 */
	datalen = ftell(s->ofile);
	if (SIZE_MAX - datalen < len + 1 ||
	    datalen + len + 1 > env->sc_maxsize) {
		s->s_dstatus |= DS_PERMFAILURE;
		return;
	}

	if (!(s->flags & F_8BITMIME))
		for (i = 0; i < len; ++i)
			if (line[i] & 0x80)
				line[i] = line[i] & 0x7f;

	if (fprintf(s->ofile, "%s\n", line) != (int)len + 1)
		s->s_dstatus |= DS_TEMPFAILURE;
}

static void
smtp_reply(struct smtp_session *s, char *fmt, ...)
{
	va_list	 ap;
	int	 n;
	char	 buf[SMTP_LINE_MAX], tmp[SMTP_LINE_MAX];

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n == -1 || n >= SMTP_LINE_MAX)
		fatalx("smtp_reply: line too long");
	if (n < 4)
		fatalx("smtp_reply: response too short");

	log_trace(TRACE_SMTP, "smtp: %p: >>> %s", s, buf);

	iobuf_xfqueue(&s->iobuf, "smtp_reply", "%s\r\n", buf);

	switch (buf[0]) {
	case '5':
	case '4':
		strnvis(tmp, s->cmd, sizeof tmp, VIS_SAFE | VIS_CSTYLE);
		log_info("smtp-in: Failed command on session %016" PRIx64
		    ": \"%s\" => %.*s", s->id, tmp, n, buf);
		break;
	}
}

static void
smtp_free(struct smtp_session *s, const char * reason)
{
	uint32_t msgid;

	log_debug("debug: smtp: %p: deleting session: %s", s, reason);

	tree_pop(&wait_mfa_data, s->id);

	if (s->evp.id != 0) {
		msgid = evpid_to_msgid(s->evp.id);
		imsg_compose_event(env->sc_ievs[PROC_QUEUE],
		    IMSG_QUEUE_REMOVE_MESSAGE, 0, 0, -1, &msgid, sizeof(msgid));
	}

	if (s->ofile != NULL)
		fclose(s->ofile);

	if (s->flags & F_SECURE && s->listener->flags & F_SMTPS)
		stat_decrement("smtp.smtps", 1);
	if (s->flags & F_SECURE && s->listener->flags & F_STARTTLS)
		stat_decrement("smtp.tls", 1);

	io_clear(&s->io);
	iobuf_clear(&s->iobuf);
	free(s);

	smtp_collect();
}

static int
smtp_mailaddr(struct mailaddr *maddr, char *line)
{
	size_t len;

	len = strlen(line);
	if (*line != '<' || line[len - 1] != '>')
		return (0);
	line[len - 1] = '\0';

	return (email_to_mailaddr(maddr, line + 1));
}

#define CASE(x) case x : return #x

const char *
smtp_strstate(int state)
{
	static char	buf[32];

	switch (state) {
	CASE(S_NEW);
	CASE(S_CONNECTED);
	CASE(S_TLS);
	CASE(S_HELO);
	CASE(S_AUTH_INIT);
	CASE(S_AUTH_USERNAME);
	CASE(S_AUTH_PASSWORD);
	CASE(S_AUTH_FINALIZE);
	CASE(S_BODY);
	CASE(S_QUIT);
	default:
		snprintf(buf, sizeof(buf), "??? (%d)", state);
		return (buf);
	}
}
