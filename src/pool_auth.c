/* -*-pgsql-c-*- */
/*
 * $Header: /cvsroot/pgpool/pgpool/pool_auth.c,v 1.12 2007/05/28 01:26:12 y-asaba Exp $
 *
 * pgpool: a language independent connection pool server for PostgreSQL 
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2007	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * pool_auth.c: authenticaton stuff
 *
*/

#include "pool.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_PARAM_H
#include <param.h>
#endif
#include <errno.h>
#include <string.h>

static POOL_STATUS pool_send_auth_ok(POOL_CONNECTION *frontend, int pid, int key, int protoMajor);
static int do_clear_text_password(POOL_CONNECTION *backend, POOL_CONNECTION *frontend, int reauth, int protoMajor);
static int do_crypt(POOL_CONNECTION *backend, POOL_CONNECTION *frontend, int reauth, int protoMajor);
static int do_md5(POOL_CONNECTION *backend, POOL_CONNECTION *frontend, int reauth, int protoMajor);

/*
* do authentication against backend. if success return 0 otherwise non 0.
*/
int pool_do_auth(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *cp)
{
	int status;
	signed char kind;
	int pid, pid1;
	int key, key1;
	int protoMajor;
	int length;

	protoMajor = MAJOR(cp);

        if (protoMajor == PROTO_MAJOR_V3)
        {
                kind = pool_read_kind2(cp);
        }
        else
        {
                kind = pool_read_kind(cp);
        }

	if (kind < 0)
	{
		return -1;
	}

	/* error response? */
	if (kind == 'E')
	{
		/* we assume error response at this stage is likely version
		 * protocol mismatch (v3 frontend vs. v2 backend). So we throw
		 * a V2 protocol error response in the hope that v3 frontend
		 * will negotiate again using v2 protocol.
		 */
		pool_log("pool_do_auth: maybe protocol version mismatch (current version %d)", protoMajor);
		if (protoMajor == PROTO_MAJOR_V3)
		{
			ErrorResponse2(frontend, cp);
		}
		else
		{
			ErrorResponse(frontend, cp);
		}
		return -1;
	}
	else if (kind != 'R')
	{
		pool_error("pool_do_auth: expect \"R\" got %c", kind);
		return -1;
	}

	/*
	 * message length (v3 only) */
	if (protoMajor == PROTO_MAJOR_V3 && pool_read_message_length(cp) < 0)
	{
		return -1;
	}

	/*
	 * read authentication request kind.
	 *
	 * 0: authentication ok
	 * 1: kerberos v4
	 * 2: kerberos v5
	 * 3: clear text password
	 * 4: crypt password
	 * 5: md5 password
	 * 6: scm credential
	 *
	 * in replication mode, we only supports  kind = 0, 3. this is because to "salt"
	 * cannot be replicated among master and secondary.
	 * in non replication mode, we supports  kind = 0, 3, 4, 5
	 */

	status = pool_read(MASTER(cp), &pid, sizeof(pid));
	if (status < 0)
	{
		pool_error("pool_do_auth: read authentication kind failed");
		return -1;
	}

	pid = ntohl(pid);

	if (DUAL_MODE)
	{
		status = pool_read(SECONDARY(cp), &pid1, sizeof(pid1));

		if (status < 0)
		{
			pool_error("pool_do_auth: read authentication kind from secondary failed");
			return -1;
		}

		pid1 = ntohl(pid1);

		if (pid != pid1)
		{
			pool_error("pool_do_auth: authentication kind does not match between master(%d) and secondary(%d)", pid, pid1);
			return -1;
		}
	}

	/* trust? */
	if (pid == 0)
	{
		int msglen;

		pool_write(frontend, "R", 1);

		if (protoMajor == PROTO_MAJOR_V3)
		{
			msglen = htonl(8);
			pool_write(frontend, &msglen, sizeof(msglen));
		}

		msglen = htonl(0);
		if (pool_write_and_flush(frontend, &msglen, sizeof(msglen)) < 0)
		{
			return -1;
		}
		MASTER(cp)->auth_kind = 0;
	}

	/* clear text password authentication? */
	else if (pid == 3)
	{
		pool_debug("trying clear text password authentication");

		pid = do_clear_text_password(MASTER(cp), frontend, 0, protoMajor);

		if (pid >= 0 && DUAL_MODE)
		{
			pid = do_clear_text_password(SECONDARY(cp), frontend, 0, protoMajor);
		}
	}

	/* crypt authentication? */
	else if (pid == 4)
	{
		pool_debug("trying crypt authentication");

		pid = do_crypt(MASTER(cp), frontend, 0, protoMajor);

		if (pid >= 0 && DUAL_MODE)
		{
			pid = do_crypt(SECONDARY(cp), frontend, 0, protoMajor);
		}
	}

	/* md5 authentication? */
	else if (pid == 5)
	{
		pool_debug("trying md5 authentication");

		pid = do_md5(MASTER(cp), frontend, 0, protoMajor);

		if (pid >= 0 && DUAL_MODE)
		{
			pid = do_md5(SECONDARY(cp), frontend, 0, protoMajor);
		}
	}

	if (pid != 0)
	{
		pool_error("pool_do_auth: backend does not return authenticaton ok");
		return -1;
	} 

	/*
	 * authentication ok. now read pid and secret key from the
	 * backend
	 */
	for (;;)
	{
		kind = pool_read_kind(cp);
		if (kind < 0)
		{
			pool_error("pool_do_auth: failed to read kind before backendkeydata");
			return -1;
		}
		else if (kind == 'K')
			break;

		if (protoMajor == PROTO_MAJOR_V3)
		{
			switch (kind)
			{
				case 'S':
					/* process parameter status */
					if (ParameterStatus(frontend, cp) != POOL_CONTINUE)
						return -1;
					pool_flush(frontend);
					break;

				case 'N':
					/* process notice message */
					if (SimpleForwardToFrontend(kind, frontend, cp))
						return -1;
					pool_flush(frontend);
					break;

					/* process error message */
				case 'E':
					SimpleForwardToFrontend(kind, frontend, cp);
					pool_flush(frontend);
					return -1;
					break;

				default:
					pool_error("pool_do_auth: unknown response \"%c\" before processing BackendKeyData",
							   kind);
					return -1;
					break;
			}
		}
		else
		{
			/* V2 case */
			switch (kind)
			{
				case 'N':
					/* process notice message */
					if (NoticeResponse(frontend, cp) != POOL_CONTINUE)
						return -1;
					break;

					/* process error message */
				case 'E':
					ErrorResponse(frontend, cp);
					return -1;
					break;

				default:
					pool_error("pool_do_auth: unknown response \"%c\" before processing V2 BackendKeyData",
							   kind);
					return -1;
					break;
			}
		}
	}

	/*
	 * message length (V3 only)
	 */
	if (protoMajor == PROTO_MAJOR_V3)
	{
		if (kind != 'K')
		{
			pool_error("pool_do_auth: expect \"K\" got %c", kind);
			return -1;
		}

		if ((length = pool_read_message_length(cp)) != 12)
		{
			pool_error("pool_do_auth: invalid messages length(%d) for BackendKeyData", length);
			return -1;
		}
	}

	/*
	 * OK, read pid and secret key
	 */

	/* pid */
	pool_read(MASTER(cp), &pid, sizeof(pid));
	MASTER_CONNECTION(cp)->pid = pid;

	/* key */
	pool_read(MASTER(cp), &key, sizeof(key));
	MASTER_CONNECTION(cp)->key = key;

	if (DUAL_MODE)
	{
		pool_read(SECONDARY(cp), &pid1, sizeof(pid1));
		SECONDARY_CONNECTION(cp)->pid = pid;

		/* key */
		pool_read(SECONDARY(cp), &key1, sizeof(key1));
		SECONDARY_CONNECTION(cp)->key = key;
	}

	return (pool_send_auth_ok(frontend, pid, key, protoMajor));
}

/*
* do re-authentication for reused connection. if success return 0 otherwise non 0.
*/
int pool_do_reauth(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *cp)
{
	int status;
	int protoMajor;

	protoMajor = MAJOR(cp);

	switch(MASTER(cp)->auth_kind)
	{
		case 0:
			/* trust */
			status = 0;
			break;

		case 3:
			/* clear text password */
			status = do_clear_text_password(MASTER(cp), frontend, 1, protoMajor);
			break;
			
		case 4:
			/* crypt password */
			status = do_crypt(MASTER(cp), frontend, 1, protoMajor);
			break;

		case 5:
			/* md5 password */
			status = do_md5(MASTER(cp), frontend, 1, protoMajor);
			break;

		default:
			pool_error("pool_do_reauth: unknown authentication request code %d", 
					   MASTER(cp)->auth_kind);
			return -1;
	}

	if (status == 0)
	{
		int msglen;

		pool_write(frontend, "R", 1);

		if (protoMajor == PROTO_MAJOR_V3)
		{
			msglen = htonl(8);
			pool_write(frontend, &msglen, sizeof(msglen));
		}

		msglen = htonl(0);
		if (pool_write_and_flush(frontend, &msglen, sizeof(msglen)) < 0)
		{
			return -1;
		}
	}
	else
	{
		pool_debug("pool_do_reauth: authentication failed");
		return -1;
	}

	return (pool_send_auth_ok(frontend, MASTER_CONNECTION(cp)->pid, MASTER_CONNECTION(cp)->key, protoMajor) != POOL_CONTINUE);
}

/*
* send authentication ok to frontend. if success return 0 otherwise non 0.
*/
static POOL_STATUS pool_send_auth_ok(POOL_CONNECTION *frontend, int pid, int key, int protoMajor)
{
	char kind;
	int len;

#ifdef NOT_USED
	if (protoMajor == PROTO_MAJOR_V2)
	{
		/* return "Authentication OK" to the frontend */
		kind = 'R';
		pool_write(frontend, &kind, 1);
		len = htonl(0);
		if (pool_write_and_flush(frontend, &len, sizeof(len)) < 0)
		{
			return -1;
		}
	}
#endif

	/* send backend key data */
	kind = 'K';
	pool_write(frontend, &kind, 1);
	if (protoMajor == PROTO_MAJOR_V3)
	{
		len = htonl(12);
		pool_write(frontend, &len, sizeof(len));
	}

	pool_debug("pool_send_auth_ok: send pid %d to frontend", ntohl(pid));

	pool_write(frontend, &pid, sizeof(pid));
	if (pool_write_and_flush(frontend, &key, sizeof(key)) < 0)
	{
		return -1;
	}

	return 0;
}

/*
 * perform clear text password authetication
 */
static int do_clear_text_password(POOL_CONNECTION *backend, POOL_CONNECTION *frontend, int reauth, int protoMajor)
{
	static int size;
	static char password[MAX_PASSWORD_SIZE];
	char response;
	int kind;
	int len;

	/* master? */
	if (!backend->issecondary_backend)
	{
		pool_write(frontend, "R", 1);	/* authenticaton */
		if (protoMajor == PROTO_MAJOR_V3)
		{
			len = htonl(8);
			pool_write(frontend, &len, sizeof(len));
		}
		kind = htonl(3);		/* clear text password authentication */
		pool_write_and_flush(frontend, &kind, sizeof(kind));	/* indicating clear text password authentication */

		/* read password packet */
		if (protoMajor == PROTO_MAJOR_V2)
		{
			if (pool_read(frontend, &size, sizeof(size)))
			{
				pool_error("do_clear_text_password: failed to read password packet size");
				return -1;
			}
		}
		else
		{
			char k;

			if (pool_read(frontend, &k, sizeof(k)))
			{
				pool_error("do_clear_text_password: failed to read password packet \"p\"");
				return -1;
			}
			if (k != 'p')
			{
				pool_error("do_clear_text_password: password packet does not start with \"p\"");
				return -1;
			}
			if (pool_read(frontend, &size, sizeof(size)))
			{
				pool_error("do_clear_text_password: failed to read password packet size");
				return -1;
			}
		}

		if ((ntohl(size) - 4) > sizeof(password))
		{
			pool_error("do_clear_text_password: password is too long (size: %d)", ntohl(size) - 4);
			return -1;
		}

		if (pool_read(frontend, password, ntohl(size) - 4))
		{
			pool_error("do_clear_text_password: failed to read password (size: %d)", ntohl(size) - 4);
			return -1;
		}
	}

	/* connection reusing? */
	if (reauth)
	{
		if ((ntohl(size) - 4) != backend->pwd_size)
		{
			pool_debug("do_clear_text_password; password size does not match in re-authetication");
			return -1;
		}

		if (memcmp(password, backend->password, backend->pwd_size) != 0)
		{
			pool_debug("do_clear_text_password; password does not match in re-authetication");
			return -1;
		}

		return 0;
	}

	/* send password packet to backend */
	if (protoMajor == PROTO_MAJOR_V3)
		pool_write(backend, "p", 1);
	pool_write(backend, &size, sizeof(size));
	pool_write_and_flush(backend, password, ntohl(size) -4);
	if (pool_read(backend, &response, sizeof(response)))
	{
		pool_error("do_clear_text_password: failed to read authentication response");
		return -1;
	}

	if (response != 'R')
	{
		pool_debug("do_clear_text_password: backend does not return R while processing clear text password authentication");
		return -1;
	}

	if (protoMajor == PROTO_MAJOR_V3)
	{
		if (pool_read(backend, &len, sizeof(len)))
		{
			pool_error("do_clear_text_password: failed to read authentication packet size");
			return -1;
		}

		if (ntohl(len) != 8)
		{
			pool_error("do_clear_text_password: incorrect authentication packet size (%d)", ntohl(len));
			return -1;
		}
	}

	/* expect to read "Authentication OK" response. kind should be 0... */
	if (pool_read(backend, &kind, sizeof(kind)))
	{
		pool_debug("do_clear_text_password: failed to read Authentication OK response");
		return -1;
	}

	/* if authenticated, save info */
	if (!reauth && kind == 0)
	{
		if (!backend->issecondary_backend)
		{
			int msglen;

			pool_write(frontend, "R", 1);

			if (protoMajor == PROTO_MAJOR_V3)
			{
				msglen = htonl(8);
				pool_write(frontend, &msglen, sizeof(msglen));
			}

			msglen = htonl(0);
			if (pool_write_and_flush(frontend, &msglen, sizeof(msglen)) < 0)
			{
				return -1;
			}
		}

		backend->auth_kind = 3;
		backend->pwd_size = ntohl(size) - 4;
		memcpy(backend->password, password, backend->pwd_size);
	}
	return kind;
}

/*
 * perform crypt authetication
 */
static int do_crypt(POOL_CONNECTION *backend, POOL_CONNECTION *frontend, int reauth, int protoMajor)
{
	char salt[2];
	static int size;
	static char password[MAX_PASSWORD_SIZE];
	char response;
	int kind;
	int len;

	if (!reauth)
	{
		/* read salt */
		if (pool_read(backend, salt, sizeof(salt)))
		{
			pool_error("do_crypt: failed to read salt");
			return -1;
		}
	}
	else
	{
		memcpy(salt, backend->salt, sizeof(salt));
	}

	/* master? */
	if (!backend->issecondary_backend)
	{
		pool_write(frontend, "R", 1);	/* authenticaton */
		if (protoMajor == PROTO_MAJOR_V3)
		{
			len = htonl(10);
			pool_write(frontend, &len, sizeof(len));
		}
		kind = htonl(4);		/* crypt authentication */
		pool_write(frontend, &kind, sizeof(kind));	/* indicating crypt authentication */
		pool_write_and_flush(frontend, salt, sizeof(salt));		/* salt */

		/* read password packet */
		if (protoMajor == PROTO_MAJOR_V2)
		{
			if (pool_read(frontend, &size, sizeof(size)))
			{
				pool_error("do_crypt: failed to read password packet size");
				return -1;
			}
		}
		else
		{
			char k;

			if (pool_read(frontend, &k, sizeof(k)))
			{
				pool_error("do_crypt_password: failed to read password packet \"p\"");
				return -1;
			}
			if (k != 'p')
			{
				pool_error("do_crypt_password: password packet does not start with \"p\"");
				return -1;
			}
			if (pool_read(frontend, &size, sizeof(size)))
			{
				pool_error("do_crypt_password: failed to read password packet size");
				return -1;
			}
		}

		if ((ntohl(size) - 4) > sizeof(password))
		{
			pool_error("do_crypt: password is too long(size: %d)", ntohl(size) - 4);
			return -1;
		}

		if (pool_read(frontend, password, ntohl(size) - 4))
		{
			pool_error("do_crypt: failed to read password (size: %d)", ntohl(size) - 4);
			return -1;
		}
	}

	/* connection reusing? */
	if (reauth)
	{
		pool_debug("size: %d saved_size: %d", (ntohl(size) - 4), backend->pwd_size);
		if ((ntohl(size) - 4) != backend->pwd_size)
		{
			pool_debug("do_crypt: password size does not match in re-authetication");
			return -1;
		}

		if (memcmp(password, backend->password, backend->pwd_size) != 0)
		{
			pool_debug("do_crypt: password does not match in re-authetication");
			return -1;
		}

		return 0;
	}

	/* send password packet to backend */
	if (protoMajor == PROTO_MAJOR_V3)
		pool_write(backend, "p", 1);
	pool_write(backend, &size, sizeof(size));
	pool_write_and_flush(backend, password, ntohl(size) -4);
	if (pool_read(backend, &response, sizeof(response)))
	{
		pool_error("do_crypt: failed to read authentication response");
		return -1;
	}

	if (response != 'R')
	{
		pool_debug("do_crypt: backend does not return R while processing crypt authentication(%02x) secondary: %d", response, backend->issecondary_backend);
		return -1;
	}

	if (protoMajor == PROTO_MAJOR_V3)
	{
		if (pool_read(backend, &len, sizeof(len)))
		{
			pool_error("do_clear_text_password: failed to read authentication packet size");
			return -1;
		}

		if (ntohl(len) != 8)
		{
			pool_error("do_clear_text_password: incorrect authentication packet size (%d)", ntohl(len));
			return -1;
		}
	}

	/* expect to read "Authentication OK" response. kind should be 0... */
	if (pool_read(backend, &kind, sizeof(kind)))
	{
		pool_debug("do_crypt: failed to read Authentication OK response");
		return -1;
	}

	/* if authenticated, save info */
	if (!reauth && kind == 0)
	{
		int msglen;

		pool_write(frontend, "R", 1);

		if (protoMajor == PROTO_MAJOR_V3)
		{
			msglen = htonl(8);
			pool_write(frontend, &msglen, sizeof(msglen));
		}

		msglen = htonl(0);
		if (pool_write_and_flush(frontend, &msglen, sizeof(msglen)) < 0)
		{
			return -1;
		}

		backend->auth_kind = 4;
		backend->pwd_size = ntohl(size) - 4;
		memcpy(backend->password, password, backend->pwd_size);
		memcpy(backend->salt, salt, sizeof(salt));
	}
	return kind;
}

/*
 * perform MD5 authetication
 */
static int do_md5(POOL_CONNECTION *backend, POOL_CONNECTION *frontend, int reauth, int protoMajor)
{
	char salt[4];
	static int size;
	static char password[MAX_PASSWORD_SIZE];
	char response;
	int kind;
	int len;

	if (!reauth)
	{
		/* read salt */
		if (pool_read(backend, salt, sizeof(salt)))
		{
			pool_error("do_md5: failed to read salt");
			return -1;
		}
		pool_debug("master: %d salt: %hhx%hhx%hhx%hhx", !backend->issecondary_backend,
				   salt[0], salt[1], salt[2], salt[3]);
	}
	else
	{
		memcpy(salt, backend->salt, sizeof(salt));
	}

	/* master? */
	if (!backend->issecondary_backend)
	{
		pool_write(frontend, "R", 1);	/* authenticaton */
		if (protoMajor == PROTO_MAJOR_V3)
		{
			len = htonl(12);
			pool_write(frontend, &len, sizeof(len));
		}
		kind = htonl(5);
		pool_write(frontend, &kind, sizeof(kind));	/* indicating MD5 */
		pool_write_and_flush(frontend, salt, sizeof(salt));		/* salt */

		/* read password packet */
		if (protoMajor == PROTO_MAJOR_V2)
		{
			if (pool_read(frontend, &size, sizeof(size)))
			{
				pool_error("do_md5: failed to read password packet size");
				return -1;
			}
		}
		else
		{
			char k;

			if (pool_read(frontend, &k, sizeof(k)))
			{
				pool_error("do_md5_password: failed to read password packet \"p\"");
				return -1;
			}
			if (k != 'p')
			{
				pool_error("do_md5_password: password packet does not start with \"p\"");
				return -1;
			}
			if (pool_read(frontend, &size, sizeof(size)))
			{
				pool_error("do_md5_password: failed to read password packet size");
				return -1;
			}
		}

		if ((ntohl(size) - 4) > sizeof(password))
		{
			pool_error("do_md5: password is too long(size: %d)", ntohl(size) - 4);
			return -1;
		}

		if (pool_read(frontend, password, ntohl(size) - 4))
		{
			pool_error("do_md5: failed to read password (size: %d)", ntohl(size) - 4);
			return -1;
		}
	}

	/* connection reusing? */
	if (reauth)
	{
		if ((ntohl(size) - 4) != backend->pwd_size)
		{
			pool_debug("do_md5; password size does not match in re-authetication");
			return -1;
		}

		if (memcmp(password, backend->password, backend->pwd_size) != 0)
		{
			pool_debug("do_md5; password does not match in re-authetication");
			return -1;
		}

		return 0;
	}

	/* send password packet to backend */
	if (protoMajor == PROTO_MAJOR_V3)
		pool_write(backend, "p", 1);
	pool_write(backend, &size, sizeof(size));
	pool_write_and_flush(backend, password, ntohl(size) -4);
	if (pool_read(backend, &response, sizeof(response)))
	{
		pool_error("do_md5: failed to read authentication response");
		return -1;
	}

	if (response != 'R')
	{
		pool_debug("do_md5: backend does not return R while processing MD5 authentication %c", response);
		return -1;
	}

	if (protoMajor == PROTO_MAJOR_V3)
	{
		if (pool_read(backend, &len, sizeof(len)))
		{
			pool_error("do_md5: failed to read authentication packet size");
			return -1;
		}

		if (ntohl(len) != 8)
		{
			pool_error("do_clear_text_password: incorrect authentication packet size (%d)", ntohl(len));
			return -1;
		}
	}

	/* expect to read "Authentication OK" response. kind should be 0... */
	if (pool_read(backend, &kind, sizeof(kind)))
	{
		pool_debug("do_md5: failed to read Authentication OK response");
		return -1;
	}

	/* if authenticated, save info */
	if (!reauth && kind == 0)
	{
		int msglen;

		pool_write(frontend, "R", 1);

		if (protoMajor == PROTO_MAJOR_V3)
		{
			msglen = htonl(8);
			pool_write(frontend, &msglen, sizeof(msglen));
		}

		msglen = htonl(0);
		if (pool_write_and_flush(frontend, &msglen, sizeof(msglen)) < 0)
		{
			return -1;
		}

		backend->auth_kind = 5;
		backend->pwd_size = ntohl(size) - 4;
		memcpy(backend->password, password, backend->pwd_size);
		memcpy(backend->salt, salt, sizeof(salt));
	}
	return kind;
}

/*
 * read message length (V3 only)
 */
int pool_read_message_length(POOL_CONNECTION_POOL *cp)
{
	int status;
	int length, length1;

	status = pool_read(MASTER(cp), &length, sizeof(length));
	if (status < 0)
	{
		pool_error("pool_read_message_length: error while reading message length");
		return -1;
	}
	length = ntohl(length);

	pool_debug("pool_read_message_length: lenghth: %d", length);

	if (DUAL_MODE)
	{
		status = pool_read(SECONDARY(cp), &length1, sizeof(length1));
		if (status < 0)
		{
			pool_error("pool_read_message_length: error while reading message length from secondary backend");
			return -1;
		}
		length1 = ntohl(length1);

		if (length != length1)
		{
			pool_error("pool_read_message_length: length does not match between backends master(%d) secondary(%d)",
					   length, length1);
			return -1;
		}
	}

	if (length < 0)
	{
		pool_error("pool_read_message_length: invalid message length (%d)", length);
		return -1;
	}

	return length;
}

/*
 * read message length2 (V3 only)
 * unlike pool_read_message_length, this returns an array of message length.
 * the array is in the static storage, thus it will be destroyed by subsequent calls.
 */
int *pool_read_message_length2(POOL_CONNECTION_POOL *cp)
{
	int status;
	int length, length1;
	static int length_array[MAX_CONNECTION_SLOTS];

	status = pool_read(MASTER(cp), &length, sizeof(length));
	if (status < 0)
	{
		pool_error("pool_read_message_length2: error while reading message length");
		return NULL;
	}
	length = ntohl(length);

	pool_debug("pool_read_message_length2: master lenghth: %d", length);

	if (DUAL_MODE)
	{
		status = pool_read(SECONDARY(cp), &length1, sizeof(length1));
		if (status < 0)
		{
			pool_error("pool_read_message_length2: error while reading message length from secondary backend");
			return NULL;
		}
		length1 = ntohl(length1);

		if (length != length1)
		{
			pool_debug("pool_read_message_length2: length does not match between backends master(%d) secondary(%d)",
					   length, length1);
		}
	}
	else
	{
		length1 = 0;
	}

	if (length < 0 || length1 < 0)
	{
		pool_error("pool_read_message_length2: invalid message length (%d) length1 (%d)", length, length1);
		return NULL;
	}

	length_array[0] = length;
	length_array[1] = length1;
	return &length_array[0];
}

/*
 * return: -2 if kind does not match.
 *         -1 if an error occured.
 */
signed char pool_read_kind(POOL_CONNECTION_POOL *cp)
{
	int status;
	char kind, kind1;

	status = pool_read(MASTER(cp), &kind, sizeof(kind));
	if (status < 0)
	{
		pool_error("read_kind: error while reading message kind");
		return -1;
	}

	if (DUAL_MODE)
	{
		status = pool_read(SECONDARY(cp), &kind1, sizeof(kind1));
		if (status < 0)
		{
			pool_error("read_kind: error while reading message kind from secondary backend");
			return -1;
		}

		if (kind != kind1)
		{
			pool_error("read_kind: kind does not match between backends master(%d) secondary(%d)",
					   kind, kind1);
			return -2;
		}
	}

	return kind;
}

signed char pool_read_kind2(POOL_CONNECTION_POOL *cp)
{
	int status;
	char kind, kind1;
	char *buf;

	buf = pool_read2(MASTER(cp), sizeof(kind));
	if (buf == NULL)
	{
		pool_error("read_kind2: error while reading message kind");
		return -1;
	}

	kind = *buf;

	if (DUAL_MODE)
	{
		status = pool_read(SECONDARY(cp), &kind1, sizeof(kind1));
		if (status < 0)
		{
			pool_error("read_kind2: error while reading message kind from secondary backend");
			return -1;
		}

		if (kind != kind1)
		{
			pool_error("read_kind2: kind does not match between backends master(%d) secondary(%d)",
					   kind, kind1);
			return -1;
		}
	}

	return kind;
}
