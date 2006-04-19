/*
 * rlm_detail.c	accounting:    Write the "detail" files.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000  The FreeRADIUS server project
 */

static const char rcsid[] = "$Id$";

#include	<freeradius-devel/autoconf.h>

#include	<sys/stat.h>
#include	<sys/select.h>

#include	<stdlib.h>
#include	<string.h>
#include	<ctype.h>
#include	<fcntl.h>

#include	<freeradius-devel/radiusd.h>
#include	<freeradius-devel/modules.h>
#include	<freeradius-devel/rad_assert.h>

#define 	DIRLEN	8192

static const char *packet_codes[] = {
  "",
  "Access-Request",
  "Access-Accept",
  "Access-Reject",
  "Accounting-Request",
  "Accounting-Response",
  "Accounting-Status",
  "Password-Request",
  "Password-Accept",
  "Password-Reject",
  "Accounting-Message",
  "Access-Challenge"
};


struct detail_instance {
	/* detail file */
	char *detailfile;

	/* detail file permissions */
	int detailperm;

	/* directory permissions */
	int dirperm;

	/* last made directory */
	char *last_made_directory;

	/* timestamp & stuff */
	char *header;

	/* if we want file locking */
	int locking;

	lrad_hash_table_t *ht;
};

static const CONF_PARSER module_config[] = {
	{ "detailfile",    PW_TYPE_STRING_PTR,
	  offsetof(struct detail_instance,detailfile), NULL, "%A/%{Client-IP-Address}/detail" },
	{ "header",    PW_TYPE_STRING_PTR,
	  offsetof(struct detail_instance,header), NULL, "%t" },
	{ "detailperm",    PW_TYPE_INTEGER,
	  offsetof(struct detail_instance,detailperm), NULL, "0600" },
	{ "dirperm",       PW_TYPE_INTEGER,
	  offsetof(struct detail_instance,dirperm),    NULL, "0755" },
	{ "locking",       PW_TYPE_BOOLEAN,
	  offsetof(struct detail_instance,locking),    NULL, "no" },
	{ NULL, -1, 0, NULL, NULL }
};


/*
 *	Clean up.
 */
static int detail_detach(void *instance)
{
        struct detail_instance *inst = instance;
	free((char *) inst->detailfile);

	free((char*) inst->last_made_directory);

	if (inst->ht) lrad_hash_table_free(inst->ht);

        free(inst);
	return 0;
}


/*
 *	(Re-)read radiusd.conf into memory.
 */
static int detail_instantiate(CONF_SECTION *conf, void **instance)
{
	struct detail_instance *inst;
	CONF_SECTION	*cs;

	inst = rad_malloc(sizeof(*inst));
	if (!inst) {
		return -1;
	}
	memset(inst, 0, sizeof(*inst));

	if (cf_section_parse(conf, inst, module_config) < 0) {
		detail_detach(inst);
		return -1;
	}

	inst->last_made_directory = NULL;

	/*
	 *	Suppress certain attributes.
	 */
	cs = cf_section_sub_find(conf, "suppress");
	if (cs) {
		CONF_ITEM	*ci;

		inst->ht = lrad_hash_table_create(NULL);

		for (ci = cf_item_find_next(cs, NULL);
		     ci != NULL;
		     ci = cf_item_find_next(cs, ci)) {
			const char	*attr;
			DICT_ATTR	*da;

			if (!cf_item_is_pair(ci)) continue;
						
			attr = cf_pair_attr(cf_itemtopair(ci));
			if (!attr) continue; /* pair-anoia */

			da = dict_attrbyname(attr);
			if (!da) {
				radlog(L_INFO, "rlm_detail: WARNING: No such attribute %s: Cannot suppress printing it.", attr);
				continue;
			}

			/*
			 *	For better distribution we should really
			 *	hash the attribute number or name.  But
			 *	since the suppression list will usually
			 *	be small, it doesn't matter.
			 */
			if (!lrad_hash_table_insert(inst->ht, da->attr, da)) {
				radlog(L_ERR, "rlm_detail: Failed trying to remember %s", attr);
				detail_detach(inst);
				return -1;
			}
		}
	}


	*instance = inst;
	return 0;
}

/*
 *	Do detail, compatible with old accounting
 */
static int do_detail(void *instance, REQUEST *request, RADIUS_PACKET *packet,
		     int compat)
{
	int		outfd;
	FILE		*outfp;
	char		timestamp[256];
	char		buffer[DIRLEN];
	char		*p;
	struct stat	st;
	int		locked;
	int		lock_count;
	struct timeval	tv;
	REALM		*proxy_realm;
	char		proxy_buffer[16];
	VALUE_PAIR	*pair = packet->vps;

	struct detail_instance *inst = instance;

	/*
	 *	Nothing to log: don't do anything.
	 */
	if (!packet) {
		return RLM_MODULE_NOOP;
	}

	/*
	 *	Create a directory for this nas.
	 *
	 *	Generate the path for the detail file.  Use the
	 *	same format, but truncate at the last /.  Then
	 *	feed it through radius_xlat() to expand the
	 *	variables.
	 */
	radius_xlat(buffer, sizeof(buffer), inst->detailfile, request, NULL);
	DEBUG2("rlm_detail: %s expands to %s", inst->detailfile, buffer);

	/*
	 *	Grab the last directory delimiter.
	 */
	p = strrchr(buffer,'/');

	/*
	 *	There WAS a directory delimiter there, and
	 *	the file doesn't exist, so
	 *	we prolly must create it the dir(s)
	 */
	if ((p) && (stat(buffer, &st) < 0)) {
		*p = '\0';
		/*
		 *	NO previously cached directory name, so we've
		 *	got to create a new one.
		 *
		 *	OR the new directory name is different than the old,
		 *	so we've got to create a new one.
		 *
		 *	OR the cached directory has somehow gotten removed,
		 *	so we've got to create a new one.
		 */
		if ((inst->last_made_directory == NULL) ||
		    (strcmp(inst->last_made_directory, buffer) != 0)) {
			free((char *) inst->last_made_directory);
			inst->last_made_directory = strdup(buffer);
		}

		/*
		 *	stat the directory, and don't do anything if
		 *	it exists.  If it doesn't exist, create it.
		 *
		 *	This also catches the case where some idiot
		 *	deleted a directory that the server was using.
		 */
		if (rad_mkdir(inst->last_made_directory, inst->dirperm) < 0) {
			radlog(L_ERR, "rlm_detail: Failed to create directory %s: %s", inst->last_made_directory, strerror(errno));
			return RLM_MODULE_FAIL;
		}

		*p = '/';
	} /* else there was no directory delimiter. */

	locked = 0;
	lock_count = 0;
	do {
		/*
		 *	Open & create the file, with the given
		 *	permissions.
		 */
		if ((outfd = open(buffer, O_WRONLY | O_APPEND | O_CREAT,
				  inst->detailperm)) < 0) {
			radlog(L_ERR, "rlm_detail: Couldn't open file %s: %s",
			       buffer, strerror(errno));
			return RLM_MODULE_FAIL;
		}

		/*
		 *	If we fail to aquire the filelock in 80 tries
		 *	(approximately two seconds) we bail out.
		 */
		if (inst->locking) {
			lseek(outfd, 0L, SEEK_SET);
			if (rad_lockfd_nonblock(outfd, 0) < 0) {
				close(outfd);
				tv.tv_sec = 0;
				tv.tv_usec = 25000;
				select(0, NULL, NULL, NULL, &tv);
				lock_count++;
				continue;
			}

			/*
			 *	The file might have been deleted by
			 *	radrelay while we tried to acquire
			 *	the lock (race condition)
			 */
			if (fstat(outfd, &st) != 0) {
				radlog(L_ERR, "rlm_detail: Couldn't stat file %s: %s",
				       buffer, strerror(errno));
				close(outfd);
				return RLM_MODULE_FAIL;
			}
			if (st.st_nlink == 0) {
				DEBUG("rlm_detail: File %s removed by another program, retrying",
				      buffer);
				close(outfd);
				lock_count = 0;
				continue;
			}

			DEBUG("rlm_detail: Acquired filelock, tried %d time(s)",
			      lock_count + 1);
			locked = 1;
		}
	} while (inst->locking && !locked && lock_count < 80);

	if (inst->locking && !locked) {
		close(outfd);
		radlog(L_ERR, "rlm_detail: Failed to aquire filelock for %s, giving up",
		       buffer);
		return RLM_MODULE_FAIL;
	}

	/*
	 *	Convert the FD to FP.  The FD is no longer valid
	 *	after this operation.
	 */
	if ((outfp = fdopen(outfd, "a")) == NULL) {
		radlog(L_ERR, "rlm_detail: Couldn't open file %s: %s",
		       buffer, strerror(errno));
		if (inst->locking) {
			lseek(outfd, 0L, SEEK_SET);
			rad_unlockfd(outfd, 0);
			DEBUG("rlm_detail: Released filelock");
		}
		close(outfd);	/* automatically releases the lock */

		return RLM_MODULE_FAIL;
	}

	/*
	 *	Write the information to the file.
	 */
	if (!compat) {
		/*
		 *	Print out names, if they're OK.
		 *	Numbers, if not.
		 */
		if ((packet->code > 0) &&
		    (packet->code <= PW_ACCESS_CHALLENGE)) {
			fprintf(outfp, "Packet-Type = %s\n",
				packet_codes[packet->code]);
		} else {
			fprintf(outfp, "Packet-Type = %d\n", packet->code);
		}
	}

	/*
	 *	Post a timestamp
	 */
	fseek(outfp, 0L, SEEK_END);
	radius_xlat(timestamp, sizeof(timestamp), inst->header, request, NULL);
	fprintf(outfp, "%s\n", timestamp);

	/* Write each attribute/value to the log file */
	for (; pair != NULL; pair = pair->next) {
		if (inst->ht &&
		    lrad_hash_table_finddata(inst->ht, pair->attribute)) continue;

		/*
		 *	Don't print passwords in old format...
		 */
		if (compat && (pair->attribute == PW_PASSWORD)) continue;

		/*
		 *	Print all of the attributes.
		 */
		fputs("\t", outfp);
		vp_print(outfp, pair);
		fputs("\n", outfp);
	}

	/*
	 *	Add non-protocol attibutes.
	 */
	if (compat) {
		if ((pair = pairfind(request->config_items,
				     PW_PROXY_TO_REALM)) != NULL) {
			proxy_realm = realm_find(pair->vp_strvalue, TRUE);
			if (proxy_realm) {
				memset((char *) proxy_buffer, 0, 16);

				rad_assert(proxy_realm->acct_ipaddr.af == AF_INET);

				inet_ntop(proxy_realm->acct_ipaddr.af,
					  &proxy_realm->acct_ipaddr.ipaddr,
					  proxy_buffer, sizeof(proxy_buffer));
				fprintf(outfp, "\tFreeradius-Proxied-To = %s\n",
					proxy_buffer);
				DEBUG("rlm_detail: Freeradius-Proxied-To set to %s",
				      proxy_buffer);
			}
		}
		fprintf(outfp, "\tTimestamp = %ld\n",
			(unsigned long) request->timestamp);

		if (request->packet->verified == 2)
			fputs("\tRequest-Authenticator = Verified\n", outfp);
		else if (request->packet->verified == 1)
			fputs("\tRequest-Authenticator = None\n", outfp);
	}

	fputs("\n", outfp);

	if (inst->locking) {
		fflush(outfp);
		lseek(outfd, 0L, SEEK_SET);
		rad_unlockfd(outfd, 0);
		DEBUG("rlm_detail: Released filelock");
	}

	fclose(outfp);

	/*
	 *	And everything is fine.
	 */
	return RLM_MODULE_OK;
}

/*
 *	Accounting - write the detail files.
 */
static int detail_accounting(void *instance, REQUEST *request)
{

	return do_detail(instance,request,request->packet, TRUE);
}

/*
 *	Incoming Access Request - write the detail files.
 */
static int detail_authorize(void *instance, REQUEST *request)
{
	return do_detail(instance,request,request->packet, FALSE);
}

/*
 *	Outgoing Access-Request Reply - write the detail files.
 */
static int detail_postauth(void *instance, REQUEST *request)
{
	return do_detail(instance,request,request->reply, FALSE);
}


/*
 *	Outgoing Access-Request to home server - write the detail files.
 */
static int detail_pre_proxy(void *instance, REQUEST *request)
{
	if (request->proxy &&
	    request->proxy->vps) {
		return do_detail(instance,request,request->proxy, FALSE);
	}

	return RLM_MODULE_NOOP;
}


/*
 *	Outgoing Access-Request Reply - write the detail files.
 */
static int detail_post_proxy(void *instance, REQUEST *request)
{
	if (request->proxy_reply &&
	    request->proxy_reply->vps) {
		return do_detail(instance,request,request->proxy_reply, FALSE);
	}

	return RLM_MODULE_NOOP;
}


/* globally exported name */
module_t rlm_detail = {
	RLM_MODULE_INIT,
	"detail",
	RLM_TYPE_THREAD_UNSAFE,        /* type: reserved */
	detail_instantiate,		/* instantiation */
	detail_detach,			/* detach */
	{
		NULL,			/* authentication */
		detail_authorize, 	/* authorization */
		NULL,			/* preaccounting */
		detail_accounting,	/* accounting */
		NULL,			/* checksimul */
		detail_pre_proxy,      	/* pre-proxy */
		detail_post_proxy,	/* post-proxy */
		detail_postauth		/* post-auth */
	},
};

