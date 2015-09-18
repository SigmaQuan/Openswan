/* information about connections between hosts and clients
 *
 * A policy is always on the IPhost_pair list; as all connections
 * (even right=%any), always have some kind of IP address, even if unknown.
 * A policy is almost always on the IDhost_pair list: all connections with
 * known identities are there; the exception is policies which have an ID
 * wildcard (rightca=) which are not on this list, but their INSTANCEs will be.
 *
 * Copyright (C) 1998-2002  D. Hugh Redelmeier.
 * Copyright (C) 2007 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2007 Ken Bantoft <ken@xelerance.com>
 * Copyright (C) 2008-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <resolv.h>

#include <openswan.h>
#include <openswan/ipsec_policy.h>
#include "openswan/pfkeyv2.h"
#include "kameipsec.h"

#include "sysdep.h"
#include "constants.h"
#include "oswalloc.h"
#include "oswtime.h"
#include "id.h"
#include "x509.h"
#include "pgp.h"
#include "certs.h"
#include "secrets.h"

#include "defs.h"
#include "ac.h"
#ifdef XAUTH_USEPAM
#include <security/pam_appl.h>
#endif
#include "pluto/connections.h"	/* needs id.h */
#include "pending.h"
#include "foodgroups.h"
#include "packet.h"
#include "demux.h"	/* needs packet.h */
#include "state.h"
#include "timer.h"
#include "ipsec_doi.h"	/* needs demux.h and state.h */
#include "pluto/server.h"
#include "kernel.h"	/* needs connections.h */
#include "log.h"
#include "keys.h"
#include "adns.h"	/* needs <resolv.h> */
#include "dnskey.h"	/* needs keys.h and adns.h */
#include "whack.h"
#include "alg_info.h"
#include "spdb.h"
#include "ike_alg.h"
#include "plutocerts.h"
#include "kernel_alg.h"
#include "plutoalg.h"
#include "xauth.h"
#ifdef NAT_TRAVERSAL
#include "nat_traversal.h"
#endif

#include "pluto/virtual.h"

#include "hostpair.h"

/* struct host_pair: a nexus of information about a pair of hosts.
 *
 * An IPhostpair is an IP address, UDP port pair.
 *   - this is the primary index, because that is all we have
 *     from looking at a packet.
 *
 * An IDhostpair is a pair of IDs, and has a seperate index/chain.
 *
 * Only oriented connections are registered.
 *
 * Unoriented connections are kept on the unoriented_connections
 * linked list (using hp_next).  For them, host_pair is NULL.
 */

struct IPhost_pair *IPhost_pairs = NULL;
struct IDhost_pair *IDhost_pairs = NULL;

/*
 * the pending list is a set of related connections which
 * depend upon this conn to be activated before they can
 * be kicked off.
 */
void host_pair_enqueue_pending(const struct connection *c
			       , struct pending *p
			       , struct pending **pnext)
{
    *pnext = c->IPhost_pair->pending;
    c->IPhost_pair->pending = p;
}

struct pending **host_pair_first_pending(const struct connection *c)
{
    if(c->IPhost_pair == NULL) return NULL;

    return &c->IPhost_pair->pending;
}


/* check to see that Ids of peers match */
bool
same_peer_ids(const struct connection *c, const struct connection *d
              , const struct id *his_id)
{
    return same_id(&c->spd.this.id, &d->spd.this.id)
	&& same_id(his_id == NULL? &c->spd.that.id : his_id, &d->spd.that.id);
}

/** returns a host pair based upon addresses.
 *
 * find_host_pair is given a pair of addresses, plus UDP ports, and
 * returns a host_pair entry that covers it. It also moves the relevant
 * pair description to the beginning of the list, so that it can be
 * found faster next time.
 *
 */
struct IPhost_pair *
find_host_pair(bool exact
               , const ip_address *myaddr
	       , u_int16_t myport
               , enum keyword_host histype
	       , const ip_address *hisaddr
	       , u_int16_t hisport)
{
    struct IPhost_pair *p, *prev;

    /*
     * look for a host-pair that has the right set of ports/address,
     *   but the histype could also be %any.
     */

    /*
     * for the purposes of comparison, port 500 and 4500 are identical,
     * but other ports are not.
     * So if any port==4500, then set it to 500.
     */
    if(myport == pluto_port4500)   myport=pluto_port500;
    if(hisport== pluto_port4500)   hisport=pluto_port500;

    DBG(DBG_CONTROLMORE,
        char b1[ADDRTOT_BUF];
        char b2[ADDRTOT_BUF];
        char himtypebuf[KEYWORD_NAME_BUFLEN];
        DBG_log("find_host_pair: looking for me=%s:%d %s him=%s:%d %s\n"
                , myaddr ? (addrtot(myaddr, 0, b1, sizeof(b1)), b1) : "<none>"
                , myport
                , keyword_name(&kw_host_list, histype, himtypebuf)
                , hisaddr ? (addrtot(hisaddr, 0, b2, sizeof(b2)), b2) : "<none>"
                , hisport, exact ? "exact-match" : "any-match"));

    for (prev = NULL, p = IPhost_pairs; p != NULL; prev = p, p = p->next)
    {
	DBG(DBG_CONTROLMORE,
	    char b1[ADDRTOT_BUF];
	    char b2[ADDRTOT_BUF];
            char himtypebuf[KEYWORD_NAME_BUFLEN];
	    DBG_log("find_host_pair: comparing to me=%s:%d %s him=%s:%d\n"
                    , (addrtot(&p->me.addr, 0, b1, sizeof(b1)), b1)
                    , p->me.host_port
                    , keyword_name(&kw_host_list, p->him.host_type, himtypebuf)
                    , (addrtot(&p->him.addr, 0, b2, sizeof(b2)), b2)
                    , p->him.host_port));

        /* kick out if it does not match: easier to understand than positive/convuluted logic */
	if (!sameaddr(&p->me.addr, myaddr))  continue;
        if(p->me.host_port_specific && p->me.host_port != myport) continue;

        /* if we are looking for %any, then it *MUST* match that */
        if(!exact && histype == KH_ANY && p->him.host_type != KH_ANY) continue;

        /* if hisport is specific, then it must match */
        if(p->him.host_port_specific && p->him.host_port != hisport) continue;

        /* finally, it must either match address, or conn is %any (if !exact) */
        if(histype == KH_ANY || !sameaddr(&p->him.addr, hisaddr)) {
            if(exact) continue;
            if(p->him.host_type != KH_ANY) continue;
        }

	/* now it matches: but a future version might want to try for bestfit */
        if (prev != NULL)
	    {
		prev->next = p->next;	/* remove p from list */
		p->next = IPhost_pairs;	/* and stick it on front */
		IPhost_pairs = p;
	    }
        break;
    }
    DBG(DBG_CONTROLMORE,
        DBG_log("find_host_pair: concluded with %s", p && p->connections ? p->connections->name : "<none>"));
    return p;
}

void remove_IPhost_pair(struct IPhost_pair *hp)
{
    list_rm(struct IPhost_pair, next, hp, IPhost_pairs);
}

void remove_IDhost_pair(struct IDhost_pair *hp)
{
    list_rm(struct IDhost_pair, next, hp, IDhost_pairs);
    if(hp->connections == NULL) {
        free_id_content(&hp->me_who);
        free_id_content(&hp->him_who);
    }
}

/* find head of list of connections with this pair of hosts */
struct connection *
find_host_pair_connections(const char *func, bool exact
			   , const ip_address *myaddr, u_int16_t myport
                           , enum keyword_host histype
			   , const ip_address *hisaddr, u_int16_t hisport)
{
    struct IPhost_pair *hp = find_host_pair(exact, myaddr, myport, histype, hisaddr, hisport);

    DBG(DBG_CONTROLMORE,
	char b1[ADDRTOT_BUF];
	char b2[ADDRTOT_BUF];
        char himtypebuf[KEYWORD_NAME_BUFLEN];
	DBG_log("found_host_pair_conn (%s): %s:%d %s/%s:%d -> hp:%s\n"
		  , func
		  , (addrtot(myaddr,  0, b1, sizeof(b1)), b1)
		  , myport
                  , keyword_name(&kw_host_list, histype, himtypebuf)
		  , hisaddr ? (addrtot(hisaddr, 0, b2, sizeof(b2)), b2) : "%any"
		  , hisport
		  , (hp && hp->connections) ? hp->connections->name : "none"));

    return hp == NULL? NULL : hp->connections;
}

void
connect_to_IPhost_pair(struct connection *c)
{
    if (oriented(*c))
    {
	struct IPhost_pair *hp= find_host_pair(EXACT_MATCH, &c->spd.this.host_addr
					      , c->spd.this.host_port
                                              , c->spd.that.host_type
					      , &c->spd.that.host_addr
					      , c->spd.that.host_port);


	DBG(DBG_CONTROLMORE,
	    char b1[ADDRTOT_BUF];
	    char b2[ADDRTOT_BUF];
            char himtypebuf[KEYWORD_NAME_BUFLEN];
	    DBG_log("connect_to_host_pair: %s:%d %s %s:%d -> hp:%s\n"
		      , (addrtot(&c->spd.this.host_addr, 0, b1,sizeof(b1)), b1)
		      , c->spd.this.host_port
                    , keyword_name(&kw_host_list, c->spd.that.host_type, himtypebuf)
		      , (addrtot(&c->spd.that.host_addr, 0, b2,sizeof(b2)), b2)
		      , c->spd.that.host_port
		      , (hp && hp->connections) ? hp->connections->name : "none"));

	if (hp == NULL)
	{
	    /* no suitable host_pair -- build one */
	    hp = alloc_thing(struct IPhost_pair, "host_pair");
	    hp->me.addr = c->spd.this.host_addr;
	    hp->him.addr = c->spd.that.host_addr;
	    hp->him.host_type = c->spd.that.host_type;
#ifdef NAT_TRAVERSAL
	    hp->me.host_port = nat_traversal_enabled ? pluto_port500 : c->spd.this.host_port;
	    hp->him.host_port = nat_traversal_enabled ? pluto_port500 : c->spd.that.host_port;
#else
	    hp->me.host_port = c->spd.this.host_port;
 	    hp->him.host_port = c->spd.that.host_port;
#endif
	    hp->connections = NULL;
	    hp->pending = NULL;
	    hp->next = IPhost_pairs;
	    IPhost_pairs = hp;
	}
	c->IPhost_pair = hp;
	c->IPhp_next = hp->connections;
	hp->connections = c;
    }
    else
    {
	/* since this connection isn't oriented, we place it
	 * in the unoriented_connections list instead.
	 */
	c->IPhost_pair = NULL;
	c->IPhp_next = unoriented_connections;
	unoriented_connections = c;
    }
}

/** returns a host pair based upon addresses.
 *
 * find_host_pair is given a pair of addresses, plus UDP ports, and
 * returns a host_pair entry that covers it. It also moves the relevant
 * pair description to the beginning of the list, so that it can be
 * found faster next time.
 *
 * if exact=FALSE, then me does not have to match.
 *
 *
 */
struct IDhost_pair *
find_ID_host_pair(bool exact
                  , const struct id me
                  , const struct id him)
{
    struct IDhost_pair *p, *pbest = NULL;
    bool exactmatch = FALSE;
    char mebuf[IDTOA_BUF], himbuf[IDTOA_BUF];
    char thisid[IDTOA_BUF], thatid[IDTOA_BUF];

    /*
     * look for a host-pair that has the right set of local ID/remote ID.
     * It is not legal to get him=ID_NONE, but it is legal for me=ID_MYID.
     *
     */
    if(DBGP(DBG_CONTROLMORE)) {
        strcpy(mebuf,  "<any>");
        strcpy(himbuf, "<any>");
        if(me.has_wildcards == 0) {
            idtoa(&me,  mebuf,  sizeof(mebuf));
        }
        if(him.has_wildcards == 0) {
            idtoa(&him, himbuf, sizeof(himbuf));
        }
    }

    DBG(DBG_CONTROLMORE
        , DBG_log("find_ID_host_pair: looking for me=%s him=%s\n"
                  , mebuf, himbuf));

    for (p = IDhost_pairs; p != NULL; p = p->next)
    {
	DBG(DBG_CONTROLMORE,
            idtoa(&p->me_who, thisid, sizeof(thisid));
            idtoa(&p->him_who,thatid, sizeof(thatid));
            DBG_log("                  comparing to me=%s him=%s\n"
                    , thisid, thatid));

        /* kick out if it does not match:
         * easier to understand than positive/convuluted logic
         */
        if(!same_id(&him, &p->him_who)) continue;
        if(exact) {
            if(!same_id(&me,  &p->me_who))  continue;
        } else {
            if(same_id(&me,  &p->me_who)) {
                pbest = p;
                exactmatch = TRUE;
                break;
            } else {
                pbest = p;
                exactmatch = FALSE;
            }
        }
        if(!exact && !exactmatch) {
            pbest = p;
            break;
        }
        /* loop looking for better matches */
    }
    DBG(DBG_CONTROLMORE,
        DBG_log("  concluded with %s", pbest && pbest->connections ? pbest->connections->name : "<none>"));
    return pbest;
}

void
connect_to_IDhost_pair(struct connection *c)
{
    struct IDhost_pair *hp= find_ID_host_pair(EXACT_MATCH
                                              , c->spd.this.id
					      , c->spd.that.id);

    if (hp == NULL) {
        /* no suitable host_pair -- build one */
        hp = alloc_thing(struct IDhost_pair, "ID host_pair");
        hp->me_who   = c->spd.this.id;
        hp->him_who  = c->spd.that.id;
        unshare_id_content(&hp->me_who);
        unshare_id_content(&hp->him_who);

        hp->connections = NULL;
        hp->next = IDhost_pairs;
        IDhost_pairs = hp;
    }

    /* add this connection to front of ID host pair connection list */
    c->IDhost_pair = hp;
    c->IDhp_next = hp->connections;
    hp->connections = c;
}

void
release_dead_interfaces(void)
{
    struct IPhost_pair *hp;

    for (hp = IPhost_pairs; hp != NULL; hp = hp->next)
    {
	struct connection **pp
	    , *p;

	for (pp = &hp->connections; (p = *pp) != NULL; )
	{
	    if (p->interface->change == IFN_DELETE)
	    {
		/* this connection's interface is going away */
		enum connection_kind k = p->kind;

		release_connection(p, TRUE);

		if (k <= CK_PERMANENT)
		{
		    /* The connection should have survived release:
		     * move it to the unoriented_connections list.
		     */
		    passert(p == *pp);

		    terminate_connection(p->name);
		    p->interface = NULL;

		    *pp = p->IPhp_next;	/* advance *pp */
		    p->IPhost_pair = NULL;
		    p->IPhp_next = unoriented_connections;
		    unoriented_connections = p;
		}
		else
		{
		    /* The connection should have vanished,
		     * but the previous connection remains.
		     */
		    passert(p != *pp);
		}
	    }
	    else
	    {
		pp = &p->IPhp_next;	/* advance pp */
	    }
	}
    }
}

/*
 * dump list of hostpairs to whacklog
 */
void
hostpair_list(void)
{
    struct IPhost_pair *pi;
    struct IDhost_pair *pd;

    whack_log(RC_LOG, "IP hostpairs: ");
    for (pi = IPhost_pairs; pi != NULL; pi = pi->next)
    {
        char b1[ADDRTOT_BUF];
        char b2[ADDRTOT_BUF];
        char instance[1 + 10 + 1];
        char himtypebuf[KEYWORD_NAME_BUFLEN];
        struct connection *c = pi->connections;

        addrtot(&c->spd.this.host_addr, 0, b1,sizeof(b1));
        addrtot(&c->spd.that.host_addr, 0, b2,sizeof(b2));
        keyword_name(&kw_host_list, c->spd.that.host_type, himtypebuf);

        whack_log(RC_LOG, "  hostpair: %s:%d %s %s:%d"
                  , b1, c->spd.this.host_port, himtypebuf
                  , b2, c->spd.that.host_port);
        while(c != NULL) {
            fmt_connection_inst_name(c, instance, sizeof(instance));
            whack_log(RC_LOG, "     %s[%s]\n", c->name, instance);

            c = c->IPhp_next;
        }
    }

    whack_log(RC_LOG, "ID hostpairs: ");
    for (pd = IDhost_pairs; pd != NULL; pd = pd->next)
    {
        char b1[IDTOA_BUF];
        char b2[IDTOA_BUF];
        char instance[1 + 10 + 1];
        struct connection *c = pd->connections;

        idtoa(&c->spd.this.id, b1,sizeof(b1));
        idtoa(&c->spd.that.id, b2,sizeof(b2));

        whack_log(RC_LOG, "  hostpair: %s %s"
                  , b1, b2);
        while(c != NULL) {
            fmt_connection_inst_name(c, instance, sizeof(instance));
            whack_log(RC_LOG, "     %s[%s]\n", c->name, instance);

            c = c->IDhp_next;
        }
    }
}

/*
 * Local Variables:
 * c-basic-offset:4
 * c-style: pluto
 * End:
 */
