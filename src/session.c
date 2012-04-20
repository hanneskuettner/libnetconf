/**
 * \file session.c
 * \author Radek Krejci <rkrejci@cesnet.cz>
 * \brief Implementation of functions to handle NETCONF sessions.
 *
 * Copyright (C) 2012 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is, and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include <libxml/tree.h>
#include <libxml/parser.h>

#include "netconf_internal.h"
#include "messages.h"
#include "messages_internal.h"
#include "session.h"

/**
 * Sleep time in microseconds to wait between unsuccessful reading due to EAGAIN or EWOULDBLOCK
 */
#define NC_READ_SLEEP 100
/*
 #define NC_WRITE(session,buf,c) \
	if(session->fd_output == STDOUT_FILENO){ \
		c += write (STDOUT_FILENO, (buf), strlen(buf)); \
	} else {\
		c += libssh2_channel_write (session->ssh_channel, (buf), strlen(buf));}
 */
#define NC_WRITE(session,buf,c) \
	if(session->ssh_channel){ \
		c += libssh2_channel_write (session->ssh_channel, (buf), strlen(buf)); \
	} else if (session->fd_output != -1) { \
		c += write (session->fd_output, (buf), strlen(buf));}

char* nc_session_get_id (const struct nc_session *session)
{
	if (session == NULL) {
		return (NULL);
	}
	return (strdup (session->session_id));
}

int nc_session_get_version (const struct nc_session *session)
{
	if (session == NULL) {
		return (-1);
	}
	return (session->version);
}

int nc_session_get_eventfd (const struct nc_session *session)
{
	if (session == NULL) {
		return -1;
	}

	if (session->libssh2_socket != -1) {
		return (session->libssh2_socket);
	} else if (session->fd_input != -1) {
		return (session->fd_input);
	} else {
		return (-1);
	}
}

void nc_cpblts_free(struct nc_cpblts *c)
{
	int i;

	if (c == NULL) {
		return;
	}

	if (c->list != NULL) {
		if (c->items <= c->list_size) {
			for(i = 0; i < c->items; i++) {
				if (c->list[i] != NULL) {
					free(c->list[i]);
				}
			}
		} else {
			WARN("nc_cpblts_free: invalid capabilities structure, some memory may not be freed.");
		}
		free(c->list);
	}
	free(c);
}

struct nc_cpblts *nc_cpblts_new(char **list)
{
	struct nc_cpblts *retval;
	char* item;
	int i;

	retval = calloc(1, sizeof(struct nc_cpblts));
	if (retval == NULL) {
		ERROR("Memory allocation failed: %s (%s:%d).", strerror (errno), __FILE__, __LINE__);
		return (NULL);
	}

	retval->list_size = 10; /* initial value */
	retval->list = malloc(retval->list_size * sizeof(char*));
	if (retval->list == NULL) {
		ERROR("Memory allocation failed: %s (%s:%d).", strerror (errno), __FILE__, __LINE__);
		free(retval);
		return (NULL);
	}
	retval->list[0] = NULL;

	if (list != NULL) {
		for (i = 0, item = list[i]; item != NULL; item = list[i++]) {
			retval->list[i] = strdup (item);
			retval->items++;
			if (retval->items == retval->list_size) {
				/* resize the capacity of the capabilities list */
				errno = 0;
				retval->list = realloc (retval->list, retval->list_size * 2);
				if (errno != 0) {
					nc_cpblts_free (retval);
					return (NULL);
				}
				retval->list_size *= 2;
			}
			retval->list[i + 1] = NULL;
		}
	}

	return (retval);
}

int nc_cpblts_add (struct nc_cpblts *capabilities, const char* capability_string)
{
	if (capabilities == NULL || capability_string == NULL) {
		return (EXIT_FAILURE);
	}

	if (capabilities->items > capabilities->list_size) {
		WARN("nc_cpblts_add: structure inconsistency! Some data may be lost.");
		return (EXIT_FAILURE);
	}

	capabilities->list[capabilities->items] = strdup(capability_string);
	capabilities->items++;
	if (capabilities->items == capabilities->list_size) {
		/* resize the capacity of the capabilities list */
		errno = 0;
		capabilities->list = realloc(capabilities->list, capabilities->list_size * 2);
		if (errno != 0) {
			return (EXIT_FAILURE);
		}
		capabilities->list_size *= 2;
	}
	capabilities->list[capabilities->items + 1] = NULL;

	return (EXIT_SUCCESS);
}

int nc_cpblts_remove (struct nc_cpblts *capabilities, const char* capability_string)
{
	int i;

	if (capabilities == NULL || capability_string == NULL) {
		return (EXIT_FAILURE);
	}

	if (capabilities->items > capabilities->list_size) {
		WARN("nc_cpblts_add: structure inconsistency! Some data may be lost.");
		return (EXIT_FAILURE);
	}

	for (i = 0; i < capabilities->items; i++) {
		if (capabilities->list[i] != NULL && strcmp(capabilities->list[i], capability_string) == 0) {
			break;
		}
	}
	if (i < capabilities->items) {
		free(capabilities->list[i]);
		/* move here the last item from the list */
		capabilities->list[i] = capabilities->list[capabilities->items - 1];
		/* and then set the last item in the list to NULL */
		capabilities->list[capabilities->items - 1] = NULL;
		capabilities->list_size--;
	}

	return (EXIT_SUCCESS);
}


void nc_cpblts_iter_start(struct nc_cpblts *c)
{
	if (c == NULL) {
		return;
	}
	c->iter = 0;
}

char *nc_cpblts_iter_next(struct nc_cpblts *c)
{
	if (c == NULL || c->list == NULL) {
		return (NULL);
	}

	if (c->iter > (c->items - 1)) {
		return NULL;
	}

	return (strdup(c->list[c->iter++]));
}

struct nc_cpblts *nc_session_get_cpblts_default ()
{
	struct nc_cpblts *retval;

	retval = nc_cpblts_new(NULL);
	if (retval == NULL) {
		return (NULL);
	}

	nc_cpblts_add(retval, "urn:ietf:params:netconf:base:1.0");
	nc_cpblts_add(retval, "urn:ietf:params:netconf:base:1.0");
	nc_cpblts_add(retval, "urn:ietf:params:netconf:base:1.1");
	nc_cpblts_add(retval, "urn:ietf:params:netconf:capability:writable-running:1.0");
	nc_cpblts_add(retval, "urn:ietf:params:netconf:capability:candidate:1.0");
	nc_cpblts_add(retval, "urn:ietf:params:netconf:capability:startup:1.0");

	return (retval);
}

struct nc_cpblts* nc_session_get_cpblts (const struct nc_session* session)
{
	if (session == NULL) {
		return (NULL);
	}

	return (session->capabilities);
}

void nc_session_close (struct nc_session* session)
{
	nc_rpc *rpc_close;
	nc_reply *reply;


	/* close the SSH session */
	if (session != NULL) {
		if (session->ssh_channel != NULL) {

			/* close NETCONF session */
			rpc_close = nc_rpc_closesession ();
			if (rpc_close != NULL) {
				if (nc_session_send_rpc (session, rpc_close) != 0) {
					nc_session_recv_reply (session, &reply);
					nc_reply_free (reply);
				}
				nc_rpc_free (rpc_close);
			}

			libssh2_channel_free (session->ssh_channel);
		}
		if (session->ssh_session != NULL) {
			libssh2_session_disconnect(session->ssh_session, "Initializing NETCONF session failed");
			libssh2_session_free (session->ssh_session);
		}
		if (session->hostname != NULL) {
			free (session->hostname);
		}
		if (session->username != NULL) {
			free (session->username);
		}
		close (session->libssh2_socket);

		if (session->capabilities != NULL) {
			nc_cpblts_free(session->capabilities);
		}
		memset (session, 0, sizeof(struct nc_session));
		free (session);
	}
}

int nc_session_send (struct nc_session* session, struct nc_msg *msg)
{
	ssize_t c = 0;
	int len;
	char *text;
	char buf[1024];

	if ((session->ssh_channel == NULL) && (session->fd_output == -1)) {
		return (EXIT_FAILURE);
	}

	xmlDocDumpFormatMemory (msg->doc, (xmlChar**) (&text), &len, 1);
	DBG("Writing message: %s", text);

	/* if v1.1 send chunk information before message */
	if (session->version == NETCONFV11) {
		snprintf (buf, 1024, "\n#%d\n", (int) strlen (text));
		c = 0;
		do {
			NC_WRITE(session, &(buf[c]), c);
		} while (c != strlen (buf));
	}

	/* write the message */
	c = 0;
	do {
		NC_WRITE(session, &(text[c]), c);
	} while (c != strlen (text));
	free (text);

	/* close message */
	if (session->version == NETCONFV11) {
		text = NC_V11_END_MSG;
	} else { /* NETCONFV10 */
		text = NC_V10_END_MSG;
	}
	c = 0;
	do {
		NC_WRITE(session, &(text[c]), c);
	} while (c != strlen (text));

	return (EXIT_SUCCESS);
}

int nc_session_read_len (struct nc_session* session, size_t chunk_length, char **text, size_t *len)
{
	char *buf, *err_msg;
	ssize_t c;
	size_t rd = 0;

	buf = malloc ((chunk_length + 1) * sizeof(char));
	if (buf == NULL) {
		ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
		*len = 0;
		*text = NULL;
		return (EXIT_FAILURE);
	}

	while (rd < chunk_length) {
		if (session->ssh_channel) {
			/* read via libssh2 */
			c = libssh2_channel_read(session->ssh_channel, &(buf[rd]), chunk_length - rd);
			if (c == LIBSSH2_ERROR_EAGAIN) {
				usleep (NC_READ_SLEEP);
				continue;
			} else if (c < 0) {
				libssh2_session_last_error (session->ssh_session, &err_msg, NULL, 0);
				ERROR("Reading from SSH channel failed (%s)", err_msg);
				free (buf);
				*len = 0;
				*text = NULL;
				return (EXIT_FAILURE);
			} else if (c == 0) {
				if (libssh2_channel_eof (session->ssh_channel)) {
					ERROR("Server has closed the communication socket");
					free (buf);
					*len = 0;
					*text = NULL;
					return (EXIT_FAILURE);
				}
				usleep (NC_READ_SLEEP);
				continue;
			}
		} else if (session->fd_input != -1) {
			/* read via file descriptor */
			c = read (session->fd_input, &(buf[rd]), chunk_length - rd);
			if (c == -1) {
				if (errno == EAGAIN) {
					usleep (NC_READ_SLEEP);
					continue;
				} else {
					ERROR("Reading from input file descriptor failed (%s)", strerror(errno));
					free (buf);
					*len = 0;
					*text = NULL;
					return (EXIT_FAILURE);
				}
			}
		} else {
			ERROR("No way to read input, fatal error.");
			free (buf);
			*len = 0;
			*text = NULL;
			return (EXIT_FAILURE);
		}

		rd += c;
	}

	/* add terminating null byte */
	buf[rd] = 0;

	*len = rd;
	*text = buf;
	return (EXIT_SUCCESS);
}

int nc_session_read_until (struct nc_session* session, const char* endtag, char **text, size_t *len)
{
	char *err_msg;
	size_t rd = 0;
	ssize_t c;
	static char *buf = NULL;
	static int buflen = 0;

	/* allocate memory according to so far maximum buffer size */
	if (buflen == 0) {
		/* set starting buffer size */
		buflen = 1024;
		buf = (char*) malloc (buflen * sizeof(char));
		if (buf == NULL) {
			ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
			return (EXIT_FAILURE);
		}
	}
	memset (buf, 0, buflen);

	for (rd = 0;;) {
		if (session->ssh_channel) {
			/* read via libssh2 */
			c = libssh2_channel_read(session->ssh_channel, &(buf[rd]), 1);
			if (c == LIBSSH2_ERROR_EAGAIN) {
				usleep (NC_READ_SLEEP);
				continue;
			} else if (c < 0) {
				libssh2_session_last_error (session->ssh_session, &err_msg, NULL, 0);
				ERROR("Reading from SSH channel failed (%s)", err_msg);
				free (buf);
				if (len != NULL) {
					*len = 0;
				}
				if (text != NULL) {
					*text = NULL;
				}
				return (EXIT_FAILURE);
			} else if (c == 0) {
				if (libssh2_channel_eof (session->ssh_channel)) {
					ERROR("Server has closed the communication socket");
					free (buf);
					if (len != NULL) {
						*len = 0;
					}
					if (text != NULL) {
						*text = NULL;
					}
					return (EXIT_FAILURE);
				}
				usleep (NC_READ_SLEEP);
				continue;
			}
		} else if (session->fd_input != -1) {
			/* read via file descriptor */
			c = read (session->fd_input, &(buf[rd]), 1);
			if (c == -1) {
				if (errno == EAGAIN) {
					usleep (NC_READ_SLEEP);
					continue;
				} else {
					ERROR("Reading from input file descriptor failed (%s)", strerror(errno));
					free (buf);
					if (len != NULL) {
						*len = 0;
					}
					if (text != NULL) {
						*text = NULL;
					}
					return (EXIT_FAILURE);
				}
			}
		} else {
			ERROR("No way to read input, fatal error.");
			free (buf);
			if (len != NULL) {
				*len = 0;
			}
			if (text != NULL) {
				*text = NULL;
			}
			return (EXIT_FAILURE);
		}

		rd += c; /* should be rd++ */

		if ((rd) < strlen (endtag)) {
			/* not enough data to compare with endtag */
			continue;
		} else {
			/* compare with endtag */
			if (strcmp (endtag, &(buf[rd - strlen (endtag)])) == 0) {
				/* end tag found */
				if (len != NULL) {
					*len = rd;
				}
				if (text != NULL) {
					*text = strdup (buf);
				}
				return (EXIT_SUCCESS);
			}
		}

		/* resize buffer if needed */
		if (rd == buflen) {
			/* get more memory for the text */
			buf = (char*) realloc (buf, (2 * buflen) * sizeof(char));
			if (buf == NULL) {
				ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
				if (len != NULL) {
					*len = 0;
				}
				if (text != NULL) {
					*text = NULL;
				}
				return (EXIT_FAILURE);
			}
			buflen = 2 * buflen;
		}
	}

	/* no one could be here */
	ERROR("Reading NETCONF message fatal failure");
	if (buf != NULL) {
		free (buf);
	}
	if (len != NULL) {
		*len = 0;
	}
	if (text != NULL) {
		*text = NULL;
	}
	return (EXIT_FAILURE);
}

int nc_session_receive (struct nc_session* session, struct nc_msg** msg)
{
	struct nc_msg *retval;
	char *text = NULL, *chunk = NULL;
	size_t len;
	unsigned long long int text_size = 0, total_len = 0;
	size_t chunk_length;
	xmlChar *msgid;

	if (session == NULL || (session->hostname == NULL)) {
		ERROR("Invalid session to receive data.");
		return (EXIT_FAILURE);
	}

	switch (session->version) {
	case NETCONFV10:
		if (nc_session_read_until (session, NC_V10_END_MSG, &text, &len) != 0) {
			return (EXIT_FAILURE);
		}
		text[len - 1 - strlen (NC_V10_END_MSG)] = 0;
		DBG("Received message: %s", text);
		break;
	case NETCONFV11:
		do {
			if (nc_session_read_until (session, "\n#", NULL, NULL) != 0) {
				if (total_len > 0) {
					free (text);
				}
				return (EXIT_FAILURE);
			}
			if (nc_session_read_until (session, "\n", &chunk, &len) != 0) {
				if (total_len > 0) {
					free (text);
				}
				return (EXIT_FAILURE);
			}
			if (strcmp (chunk, "#\n") == 0) {
				/* end of chunked framing message */
				free (chunk);
				break;
			}

			/* convert string to the size of the following chunk */
			chunk_length = strtoul (chunk, (char **) NULL, 10);
			if (chunk_length == 0) {
				ERROR("Invalid frame chunk size detected, fatal error.");
				return (EXIT_FAILURE);
			}
			free (chunk);
			chunk = NULL;

			/* now we have size of next chunk, so read the chunk */
			if (nc_session_read_len (session, chunk_length, &chunk, &len) != 0) {
				if (total_len > 0) {
					free (text);
				}
				return (EXIT_FAILURE);
			}

			/* realloc resulting text buffer if needed (always needed now) */
			if (text_size < (total_len + len + 1)) {
				text = realloc (text, total_len + len + 1);
				if (text == NULL) {
					ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
					return (EXIT_FAILURE);
				}
				text[total_len] = '\0';
				text_size = total_len + len + 1;
			}
			strcat (text, chunk);
			total_len = strlen (text); /* don't forget count terminating null byte */
			free (chunk);
			chunk = NULL;

		} while (1);
		DBG("Received message: %s", text);
		break;
	default:
		ERROR("Unsupported NETCONF protocol version (%d)", session->version);
		return (EXIT_FAILURE);
		break;
	}

	retval = malloc (sizeof(struct nc_msg));
	if (retval == NULL) {
		ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
		free (text);
		return (EXIT_FAILURE);
	}

	/* store the received message in libxml2 format */
	retval->doc = xmlReadDoc (BAD_CAST text, NULL, NULL, XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (retval->doc == NULL) {
		free (retval);
		free (text);
	}
	free (text);

	/* parse and store message-id */
	msgid = xmlGetProp (retval->doc->children, BAD_CAST "message-id");
	if (msgid != NULL) {
		retval->msgid = strtoull ((char*) msgid, NULL, 10);
		xmlFree (msgid);
	} else {
		if (xmlStrcmp (retval->doc->children->name, BAD_CAST "rpc-reply") == 0) {
			WARN("Missing message-id in rpc-reply.");
		}
		retval->msgid = 0;
	}

	/* parse and store rpc-reply type */
	if (xmlStrcmp (retval->doc->children->name, BAD_CAST "rpc-reply") == 0) {
		if (xmlStrcmp (retval->doc->children->children->name, BAD_CAST "ok") == 0) {
			retval->type = NC_REPLY_OK;
		} else if (xmlStrcmp (retval->doc->children->children->name, BAD_CAST "rpc-error") == 0) {
			retval->type = NC_REPLY_ERROR;
		} else if (xmlStrcmp (retval->doc->children->children->name, BAD_CAST "data") == 0) {
			retval->type = NC_REPLY_DATA;
		} else {
			retval->type = NC_REPLY_UNKNOWN;
			WARN("Unknown type of received <rpc-reply> detected.");
		}
	} else {
		retval->type = NC_REPLY_UNKNOWN;
	}

	/* return the result */
	*msg = retval;
	return (EXIT_SUCCESS);
}

nc_msgid nc_session_recv_reply (struct nc_session* session, nc_reply** reply)
{
	int ret;

	ret = nc_session_receive (session, (struct nc_msg**) reply);
	if (ret != EXIT_SUCCESS) {
		return (0);
	} else {
		return (nc_reply_get_msgid (*reply));
	}
}

nc_msgid nc_session_send_rpc (struct nc_session* session, nc_rpc *rpc)
{
	int ret;
	char msg_id_str[16];
	struct nc_msg *msg;

	if (session == NULL || (session->hostname == NULL)) {
		ERROR("Invalid session to send <rpc>.");
		return (0); /* failure */
	}

	/* TODO: lock for threads */
	msg = nc_msg_dup ((struct nc_msg*) rpc);

	/* set message id */
	if (xmlStrcmp (rpc->doc->children->name, BAD_CAST "rpc") == 0) {
		sprintf (msg_id_str, "%llu", session->msgid++);
		xmlSetProp (msg->doc->children, BAD_CAST "message-id", BAD_CAST msg_id_str);
	}

	/* set proper namespace according to NETCONF version */
	xmlNewNs (msg->doc->children, (xmlChar *) (
	        (session->version == NETCONFV10) ? NC_NS_BASE10 : NC_NS_BASE11), NULL);

	/* send message */
	ret = nc_session_send (session, msg);

	nc_msg_free (msg);

	if (ret != EXIT_SUCCESS) {
		session->msgid--;
		return (0);
	} else {
		return (session->msgid);
	}
}