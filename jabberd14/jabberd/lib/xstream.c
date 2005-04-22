/* --------------------------------------------------------------------------
 *
 *  jabberd 1.4.4 GPL - XMPP/Jabber server implementation
 *
 *  Copyrights
 *
 *  Portions created by or assigned to Jabber.com, Inc. are
 *  Copyright (C) 1999-2002 Jabber.com, Inc.  All Rights Reserved.  Contact
 *  information for Jabber.com, Inc. is available at http://www.jabber.com/.
 *
 *  Portions Copyright (C) 1998-1999 Jeremie Miller.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 *  Special exception for linking jabberd 1.4.4 GPL with OpenSSL:
 *
 *  In addition, as a special exception, you are allowed to link the code
 *  of jabberd 1.4.4 GPL with the OpenSSL library (or with modified versions
 *  of OpenSSL that use the same license as OpenSSL), and distribute linked
 *  combinations including the two. You must obey the GNU General Public
 *  License in all respects for all of the code used other than OpenSSL.
 *  If you modify this file, you may extend this exception to your version
 *  of the file, but you are not obligated to do so. If you do not wish
 *  to do so, delete this exception statement from your version.
 *
 * --------------------------------------------------------------------------*/

/**
 * @file xstream.c
 * @brief handling of incoming XML stream based events
 *
 * xstream is a way to have a consistent method of handling incoming XML stream based events ...
 * if doesn't handle the generation of an XML stream, but provides some facilities to help doing that
 */

#include <jabberdlib.h>

/* ========== internal expat callbacks =========== */

/**
 * internal expat callback for read start tags of an element
 */
void _xstream_startElement(xstream xs, const char* name, const char** atts) {
    pool p;

    /* if xstream is bad, get outa here */
    if(xs->status > XSTREAM_NODE) return;

    if(xs->node == NULL)
    {
        p = pool_heap(5*1024); /* 5k, typically 1-2k each plus copy of self and workspace */
        xs->node = xmlnode_new_tag_pool(p,name);
        xmlnode_put_expat_attribs(xs->node, atts);

        if(xs->status == XSTREAM_ROOT)
        {
            xs->status = XSTREAM_NODE; /* flag status that we're processing nodes now */
            (xs->f)(XSTREAM_ROOT, xs->node, xs->arg); /* send the root, f must free all nodes */
            xs->node = NULL;
        }
    }else{
        xs->node = xmlnode_insert_tag(xs->node, name);
        xmlnode_put_expat_attribs(xs->node, atts);
    }

    /* depth check */
    xs->depth++;
    if(xs->depth > XSTREAM_MAXDEPTH)
        xs->status = XSTREAM_ERR;
}

/**
 * internal expat callback for read end tags of an element
 */
void _xstream_endElement(xstream xs, const char* name) {
    xmlnode parent;

    /* if xstream is bad, get outa here */
    if(xs->status > XSTREAM_NODE) return;

    /* if it's already NULL we've received </stream>, tell the app and we're outta here */
    if(xs->node == NULL)
    {
        xs->status = XSTREAM_CLOSE;
        (xs->f)(XSTREAM_CLOSE, NULL, xs->arg);
    }else{
        parent = xmlnode_get_parent(xs->node);

        /* we are the top-most node, feed to the app who is responsible to delete it */
        if(parent == NULL)
            (xs->f)(XSTREAM_NODE, xs->node, xs->arg);

        xs->node = parent;
    }
    xs->depth--;
}

/**
 * internal expat callback for read CDATA
 */
void _xstream_charData(xstream xs, const char *str, int len) {
    /* if xstream is bad, get outa here */
    if(xs->status > XSTREAM_NODE) return;

    if(xs->node == NULL)
    {
        /* we must be in the root of the stream where CDATA is irrelevant */
        return;
    }

    xmlnode_insert_cdata(xs->node, str, len);
}

/**
 * internal function to be registered as pool cleaner, frees a stream if the associated memory pool is freed
 *
 * @param pointer to the xstream to free
 */
void _xstream_cleanup(void *arg) {
    xstream xs = (xstream)arg;

    xmlnode_free(xs->node); /* cleanup anything left over */
    XML_ParserFree(xs->parser);
}


/**
 * creates a new xstream with given pool, xstream will be cleaned up w/ pool
 *
 * @param p the memory pool to use for the stream
 * @param f function pointer to the event handler function
 * @param arg parameter to pass to the event handler function
 * @return the created xstream
 */
xstream xstream_new(pool p, xstream_onNode f, void *arg) {
    xstream newx;

    if(p == NULL || f == NULL)
    {
        fprintf(stderr,"Fatal Programming Error: xstream_new() was improperly called with NULL.\n");
        return NULL;
    }

    newx = pmalloco(p, sizeof(_xstream));
    newx->p = p;
    newx->f = f;
    newx->arg = arg;

    /* create expat parser and ensure cleanup */
    newx->parser = XML_ParserCreate(NULL);
    XML_SetUserData(newx->parser, (void *)newx);
    XML_SetElementHandler(newx->parser, (void *)_xstream_startElement, (void *)_xstream_endElement);
    XML_SetCharacterDataHandler(newx->parser, (void *)_xstream_charData);
    pool_cleanup(p, _xstream_cleanup, (void *)newx);

    return newx;
}

/**
 * attempts to parse the buff onto this stream firing events to the handler
 *
 * @param xs the xstream to parse the data on
 * @param buff the new data
 * @param len length of the data
 * @return last known xstream status
 */
int xstream_eat(xstream xs, char *buff, int len)
{
    char *err;
    xmlnode xerr;
    static char maxerr[] = "maximum node size reached";
    static char deeperr[] = "maximum node depth reached";

    if(xs == NULL)
    {
        fprintf(stderr,"Fatal Programming Error: xstream_eat() was improperly called with NULL.\n");
        return XSTREAM_ERR;
    }

    if(len == 0 || buff == NULL)
        return xs->status;

    if(len == -1) /* easy for hand-fed eat calls */
        len = strlen(buff);

    if(!XML_Parse(xs->parser, buff, len, 0))
    {
        err = (char *)XML_ErrorString(XML_GetErrorCode(xs->parser));
        xs->status = XSTREAM_ERR;
    }else if(pool_size(xmlnode_pool(xs->node)) > XSTREAM_MAXNODE || xs->cdata_len > XSTREAM_MAXNODE){
        err = maxerr;
        xs->status = XSTREAM_ERR;
    }else if(xs->status == XSTREAM_ERR){ /* set within expat handlers */
        err = deeperr;
    }

    /* fire parsing error event, make a node containing the error string */
    if(xs->status == XSTREAM_ERR)
    {
        xerr = xmlnode_new_tag("error");
        xmlnode_insert_cdata(xerr,err,-1);
        (xs->f)(XSTREAM_ERR, xerr, xs->arg);
    }

    return xs->status;
}


/* STREAM CREATION UTILITIES */

/** give a standard template xmlnode to work from 
 *
 * @param namespace ("jabber:client", "jabber:server", ...)
 * @param to where the stream is sent to
 * @param from where we are (source of the stream)
 * @return the xmlnode that has been generated as the template
 */
xmlnode xstream_header(char *namespace, char *to, char *from) {
    xmlnode x;
    char id[11];

    sprintf(id,"%X",(int)time(NULL));

    x = xmlnode_new_tag("stream:stream");
    xmlnode_put_attrib(x, "xmlns:stream", "http://etherx.jabber.org/streams");
    xmlnode_put_attrib(x, "id", id);
    if(namespace != NULL)
        xmlnode_put_attrib(x, "xmlns", namespace);
    if(to != NULL)
        xmlnode_put_attrib(x, "to", to);
    if(from != NULL)
        xmlnode_put_attrib(x, "from", from);

    return x;
}

/**
 * trim the xmlnode to only the opening header :)
 *
 * @note NO CHILDREN ALLOWED
 *
 * @param x the xmlnode
 * @return string representation of the start tag
 */
char *xstream_header_char(xmlnode x) {
    spool s;
    char *fixr, *head;

    if(xmlnode_has_children(x)) {
        fprintf(stderr,"Fatal Programming Error: xstream_header_char() was sent a header with children!\n");
        return NULL;
    }

    s = spool_new(xmlnode_pool(x));
    spooler(s,"<?xml version='1.0'?>",xmlnode2str(x),s);
    head = spool_print(s);
    fixr = strstr(head,"/>");
    *fixr = '>';
    ++fixr;
    *fixr = '\0';

    return head;
}

/**
 * format a stream error for logging
 *
 * @param s where to spool the result
 * @param errstruct the information about the error
 */
void xstream_format_error(spool s, streamerr errstruct) {
    /* sanity checks */
    if (s == NULL)
	return;
    if (errstruct == NULL) {
	spool_add(s, "stream:error=(NULL)");
	return;
    }

    switch (errstruct->reason) {
	case unknown_error_type:
	    spool_add(s, "unknown error type / legacy stream error");
	    break;
	case bad_format:
	    spool_add(s, "sent XML that cannot be processed");
	    break;
	case bad_namespace_prefix:
	    spool_add(s, "sent a namespace prefix that is unsupported");
	    break;
	case conflict:
	    spool_add(s, "new stream has been initiated that confilicts with the existing one");
	    break;
	case connection_timeout:
	    spool_add(s, "not generated any traffic over some time");
	    break;
	case host_gone:
	    spool_add(s, "hostname is no longer hosted by the server");
	    break;
	case host_unknown:
	    spool_add(s, "hostname is not hosted by the server");
	    break;
	case improper_addressing:
	    spool_add(s, "stanza lacks a 'to' or 'from' attribute");
	    break;
	case internal_server_error:
	    spool_add(s, "internal server error: maybe missconfiguration");
	    break;
	case invalid_from:
	    spool_add(s, "from address does not match an authorized JID or validated domain");
	    break;
	case invalid_id:
	    spool_add(s, "stream or dialback id is invalid or does not match a previous one");
	    break;
	case invalid_namespace:
	    spool_add(s, "invalid namespace");
	    break;
	case invalid_xml:
	    spool_add(s, "sent invalid XML, did not pass validation");
	    break;
	case not_authorized:
	    spool_add(s, "tried to send data before stream has been authed");
	    break;
	case policy_violation:
	    spool_add(s, "policy violation");
	    break;
	case remote_connection_failed:
	    spool_add(s, "remote connection failed");
	    break;
	case resource_constraint:
	    spool_add(s, "server lacks resources to service the stream");
	    break;
	case restricted_xml:
	    spool_add(s, "sent XML features that are forbidden by RFC3920");
	    break;
	case see_other_host:
	    spool_add(s, "redirected to other host");
	    break;
	case system_shutdown:
	    spool_add(s, "system is being shut down");
	    break;
	case undefined_condition:
	    spool_add(s, "undefined condition");
	    break;
	case unsupported_encoding:
	    spool_add(s, "unsupported encoding");
	    break;
	case unsupported_stanza_type:
	    spool_add(s, "sent a first-level child element (stanza) that is not supported");
	    break;
	case unsupported_version:
	    spool_add(s, "unsupported stream version");
	    break;
	case xml_not_well_formed:
	    spool_add(s, "sent XML that is not well-formed");
	    break;
	default:
	    spool_add(s, "something else (shut not happen)");
	    break;
    }

    if (errstruct->text != NULL) {
	spool_add(s, ": ");
	if (errstruct->lang != NULL) {
	    spool_add(s, "[");
	    spool_add(s, errstruct->lang);
	    spool_add(s, "]");
	}
	spool_add(s, errstruct->text);
    }
}

/**
 * parse a received stream error
 *
 * @param p memory pool used to allocate memory for strings
 * @param errnode the xmlnode containing the stream error
 * @param errstruct where to place the results
 * @return severity of the stream error
 */
streamerr_severity xstream_parse_error(pool p, xmlnode errnode, streamerr errstruct) {
    xmlnode cur = NULL;

    /* sanity checks */
    if (errstruct == NULL || p == NULL || errnode == NULL)
	return error;

    /* init the error structure */
    errstruct->text = NULL;
    errstruct->lang = NULL;
    errstruct->reason = unknown_error_type;
    errstruct->severity = error;

    /* iterate over the nodes in the stream error */
    for (cur = xmlnode_get_firstchild(errnode); cur != NULL; cur = xmlnode_get_nextsibling(cur)) {
	char *ns = NULL;
	char *name = NULL;

	/* direct CDATA? Then it might be a preXMPP stream error */
	if (xmlnode_get_type(cur) == NTYPE_CDATA) {
	    /* only if we did not receive a text element yet */
	    if (errstruct->text == NULL) {
		errstruct->text = pstrdup(p, xmlnode_get_data(cur));
	    }
	    continue;
	}

	/* else we only care about elements */
	if (xmlnode_get_type(cur) != NTYPE_TAG)
	    continue;

	/* only handle the relevant namespace */
	ns = xmlnode_get_attrib(cur, "xmlns");
	if (ns == NULL)
	    continue;
	if (j_strcmp(ns, NS_XMPP_STREAMS) != 0)
	    continue;

	/* check which element it is */
	name = xmlnode_get_name(cur);
	if (j_strcmp(name, "text") == 0) {
	    if (errstruct->text == NULL) {
		errstruct->text = pstrdup(p, xmlnode_get_data(cur));
		errstruct->lang = pstrdup(p, xmlnode_get_attrib(cur, "xml:lang"));
	    }
	} else if (j_strcmp(name, "bad-format") == 0) {
	    errstruct->reason = bad_format;
	    errstruct->severity = error;
	} else if (j_strcmp(name, "bad-namespace-prefix") == 0) {
	    errstruct->reason = bad_namespace_prefix;
	    errstruct->severity = error;
	} else if (j_strcmp(name, "conflict") == 0) {
	    errstruct->reason = conflict;
	    errstruct->severity = configuration;
	} else if (j_strcmp(name, "connection-timeout") == 0) {
	    errstruct->reason = connection_timeout;
	    errstruct->severity = normal;
	} else if (j_strcmp(name, "host-gone") == 0) {
	    errstruct->reason = host_gone;
	    errstruct->severity = configuration;
	} else if (j_strcmp(name, "host-unknown") == 0) {
	    errstruct->reason = host_unknown;
	    errstruct->severity = configuration;
	} else if (j_strcmp(name, "improper-addressing") == 0) {
	    errstruct->reason = improper_addressing;
	    errstruct->severity = error;
	} else if (j_strcmp(name, "internal-server-error") == 0) {
	    errstruct->reason = internal_server_error;
	    errstruct->severity = configuration;
	} else if (j_strcmp(name, "invalid-from") == 0) {
	    errstruct->reason = invalid_from;
	    errstruct->severity = error;
	} else if (j_strcmp(name, "invalid-id") == 0) {
	    errstruct->reason = invalid_id;
	    errstruct->severity = error;
	} else if (j_strcmp(name, "invalid-namespace") == 0) {
	    errstruct->reason = invalid_namespace;
	    errstruct->severity = error;
	} else if (j_strcmp(name, "invalid-xml") == 0) {
	    errstruct->reason = invalid_xml;
	    errstruct->severity = error;
	} else if (j_strcmp(name, "not-authorized") == 0) {
	    errstruct->reason = not_authorized;
	    errstruct->severity = configuration;
	} else if (j_strcmp(name, "policy-violation") == 0) {
	    errstruct->reason = policy_violation;
	    errstruct->severity = configuration;
	} else if (j_strcmp(name, "remote-connection-failed") == 0) {
	    errstruct->reason = remote_connection_failed;
	    errstruct->severity = configuration;
	} else if (j_strcmp(name, "resource-constraint") == 0) {
	    errstruct->reason = resource_constraint;
	    errstruct->severity = normal;
	} else if (j_strcmp(name, "restricted-xml") == 0) {
	    errstruct->reason = restricted_xml;
	    errstruct->severity = error;
	} else if (j_strcmp(name, "see-other-host") == 0) {
	    errstruct->reason = see_other_host;
	    errstruct->severity = configuration;
	} else if (j_strcmp(name, "system-shutdown") == 0) {
	    errstruct->reason = system_shutdown;
	    errstruct->severity = normal;
	} else if (j_strcmp(name, "undefined-condition") == 0) {
	    errstruct->reason = undefined_condition;
	    errstruct->severity = unknown;
	} else if (j_strcmp(name, "unsupported-encoding") == 0) {
	    errstruct->reason = unsupported_encoding;
	    errstruct->severity = feature_lack;
	} else if (j_strcmp(name, "unsupported-stanza-type") == 0) {
	    errstruct->reason = unsupported_stanza_type;
	    errstruct->severity = feature_lack;
	} else if (j_strcmp(name, "unsupported-version") == 0) {
	    errstruct->reason = unsupported_version;
	    errstruct->severity = feature_lack;
	} else if (j_strcmp(name, "xml-not-well-formed") == 0) {
	    errstruct->reason = xml_not_well_formed;
	    errstruct->severity = error;
	}
    }

    return errstruct->severity;
}
