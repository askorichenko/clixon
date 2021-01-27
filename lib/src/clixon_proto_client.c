/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * 
 * Client-side functions for clicon_proto protocol
 * Historically this code was part of the clicon_cli application. But
 * it should (is?) be general enough to be used by other applications.
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_yang_module.h"
#include "clixon_plugin.h"
#include "clixon_string.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_proto.h"
#include "clixon_err.h"
#include "clixon_stream.h"
#include "clixon_err_string.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_bind.h"
#include "clixon_xml_sort.h"
#include "clixon_xml_io.h"
#include "clixon_netconf_lib.h"
#include "clixon_proto_client.h"

/*! Connect to internal netconf socket
 */
int
clicon_rpc_connect(clicon_handle h,
		   int          *sockp)
{
    int    retval = -1;
    char  *sockstr = NULL;
    int    port;

    if ((sockstr = clicon_sock_str(h)) == NULL){
	clicon_err(OE_FATAL, 0, "CLICON_SOCK option not set");
	goto done;
    }
    /* What to do if inet socket? */
    switch (clicon_sock_family(h)){
    case AF_UNIX:
	if (clicon_rpc_connect_unix(h, sockstr, sockp) < 0){
#if 0
	    if (errno == ESHUTDOWN)
		/* Maybe could reconnect on a higher layer, but lets fail
		   loud and proud */
		cligen_exiting_set(cli_cligen(h), 1);
#endif
	    goto done;
	}
	break;
    case AF_INET:
	if ((port = clicon_sock_port(h)) < 0){
	    clicon_err(OE_FATAL, 0, "CLICON_SOCK option not set");
	    goto done;
	}
	if (port < 0){
	    clicon_err(OE_FATAL, 0, "CLICON_SOCK_PORT not set");
	    goto done;
	}
	if (clicon_rpc_connect_inet(h, sockstr, port, sockp) < 0)
	    goto done;
	break;
    }
    retval = 0;
 done:
    return retval;
}
    
/*! Send internal netconf rpc from client to backend
 * @param[in]    h      CLICON handle
 * @param[in]    msg    Encoded message. Deallocate with free
 * @param[out]   xret0  Return value from backend as xml tree. Free w xml_free
 * @param[inout] sock0  If pointer exists, do not close socket to backend on success 
 *                      and return it here. For keeping a notify socket open
 * @note sock0 is if connection should be persistent, like a notification/subscribe api
 * @note xret is populated with yangspec according to standard handle yangspec
 */
int
clicon_rpc_msg(clicon_handle      h, 
	       struct clicon_msg *msg, 
	       cxobj            **xret0,
	       int               *sock0)
{
    int     retval = -1;
    char   *retdata = NULL;
    cxobj  *xret = NULL;
    int     s = -1;

#ifdef RPC_USERNAME_ASSERT
    assert(strstr(msg->op_body, "username")!=NULL); /* XXX */
#endif
    clicon_debug(1, "%s request:%s", __FUNCTION__, msg->op_body);
    /* Create a socket and connect to it, either UNIX, IPv4 or IPv6 per config options */
    if ((s = clicon_client_socket_get(h)) < 0){
	if (clicon_rpc_connect(h, &s) < 0)
	    goto done;
	clicon_client_socket_set(h, s);
    }
    if (clicon_rpc(s, msg, &retdata) < 0)
	goto done;

    clicon_debug(1, "%s retdata:%s", __FUNCTION__, retdata);

    if (retdata){
	/* Cannot populate xret here because need to know RPC name (eg "lock") in order to associate yang
	 * to reply.
	 */
	if (clixon_xml_parse_string(retdata, YB_NONE, NULL, &xret, NULL) < 0)
	    goto done;
    }
    if (xret0){
	*xret0 = xret;
	xret = NULL;
    }
    /* If returned, keep socket open, otherwise close it below */
    if (sock0){ 
	*sock0 = s;
	s = -1;
    }
    retval = 0;
 done:
    if (retdata)
	free(retdata);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Check if there is a valid (cached) session-id. If not, send a hello request to backend 
 * Session-ids survive TCP sessions that are created for each message sent to the backend.
 * Clients use two approaches, either:
 * (1) Once at the beginning of the session. Netconf and restconf does this
 * (2) First usage, ie "lazy" evaluation when first needed
 * @param[in]  h           clicon handle
 * @param[out] session_id  Session id
 * @retval     0           OK and session_id set
 * @retval    -1           Error
 * @note This function may send a synchronous(blocking) HELLO request to the backend as a side-effect
 */
static int
session_id_check(clicon_handle h,
		 uint32_t     *session_id)
{
    int      retval = -1;
    uint32_t id;
    
    if (clicon_session_id_get(h, &id) < 0){ /* Not set yet */
	if (clicon_hello_req(h, &id) < 0)
	    goto done;
	clicon_session_id_set(h, id); 
    }
    retval = 0;
    *session_id = id;
 done:
    return retval;
}

/*! Generic xml netconf clicon rpc
 * Want to go over to use netconf directly between client and server,...
 * @param[in]  h       clicon handle
 * @param[in]  xmlstr  XML netconf tree as string
 * @param[out] xret    Return XML netconf tree, error or OK (need to be freed)
 * @param[out] sp      Socket pointer for notification, otherwise NULL
 * @code
 *   cxobj *xret = NULL;
 *   if (clicon_rpc_netconf(h, "<rpc></rpc>", &xret, NULL) < 0)
 *	err;
 *   xml_free(xret);
 * @endcode
 * @see clicon_rpc_netconf_xml xml as tree instead of string
 */
int
clicon_rpc_netconf(clicon_handle  h, 
		   char          *xmlstr,
		   cxobj        **xret,
		   int           *sp)
{
    int                retval = -1;
    uint32_t           session_id;
    struct clicon_msg *msg = NULL;

    if (session_id_check(h, &session_id) < 0)
	goto done;
    if ((msg = clicon_msg_encode(session_id, "%s", xmlstr)) < 0)
	goto done;
    if (clicon_rpc_msg(h, msg, xret, sp) < 0)
	goto done;
    retval = 0;
 done:
    if (msg)
	free(msg);
    return retval;
}

/*! Generic xml netconf clicon rpc
 * Want to go over to use netconf directly between client and server,...
 * @param[in]  h       clicon handle
 * @param[in]  xml     XML netconf tree 
 * @param[out] xret    Return XML netconf tree, error or OK
 * @param[out] sp      Socket pointer for notification, otherwise NULL
 * @code
 *   cxobj *xret = NULL;
 *   int    s; 
 *   if (clicon_rpc_netconf_xml(h, x, &xret, &s) < 0)
 *	err;
 *   xml_free(xret);
 * @endcode

 * @see clicon_rpc_netconf xml as string instead of tree
 */
int
clicon_rpc_netconf_xml(clicon_handle  h, 
		       cxobj         *xml,
		       cxobj        **xret,
		       int           *sp)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cxobj     *xname;
    char      *rpcname;
    cxobj     *xreply;
    yang_stmt *yspec;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if ((xname = xml_child_i_type(xml, 0, 0)) == NULL){
	clicon_err(OE_NETCONF, EINVAL, "Missing rpc name");
	goto done;
    }
    rpcname = xml_name(xname); /* Store rpc name and use in yang binding after reply */
    if (clicon_xml2cbuf(cb, xml, 0, 0, -1) < 0)
	goto done;
    if (clicon_rpc_netconf(h, cbuf_get(cb), xret, sp) < 0)
	goto done;
    if ((xreply = xml_find_type(*xret, NULL, "rpc-reply", CX_ELMNT)) != NULL &&
	xml_find_type(xreply, NULL, "rpc-error", CX_ELMNT) == NULL){
	yspec = clicon_dbspec_yang(h);
	/* Here use rpc name to bind to yang */
	if (xml_bind_yang_rpc_reply(xreply, rpcname, yspec, NULL) < 0) 
	    goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Get database configuration
 * Same as clicon_proto_change just with a cvec instead of lvec
 * @param[in]  h        CLICON handle
 * @param[in]  username If NULL, use default
 * @param[in]  db       Name of database
 * @param[in]  xpath    XPath (or "")
 * @param[in]  nsc       Namespace context for filter
 * @param[out] xt       XML tree. Free with xml_free. 
 *                      Either <config> or <rpc-error>. 
 * @retval    0         OK
 * @retval   -1         Error, fatal or xml
 * @code
 *   cxobj *xt = NULL;
 *   cvec *nsc = NULL;
 *
 *   if ((nsc = xml_nsctx_init(NULL, "urn:example:hello")) == NULL)
 *       err;
 *   if (clicon_rpc_get_config(h, NULL, "running", "/hello/world", nsc, &xt) < 0)
 *       err;
 *   if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
 *	clixon_netconf_error(xerr, "msg", "/hello/world");
 *      err;
 *   }
 *   if (xt)
 *      xml_free(xt);
 *  if (nsc)
 *     xml_nsctx_free(nsc);
 * @endcode
 * @see clicon_rpc_get
 * @see clixon_netconf_error
 * @note the netconf return message is yang populated, as well as the return data
 */
int
clicon_rpc_get_config(clicon_handle h, 
		      char         *username,
		      char         *db, 
		      char         *xpath,
		      cvec         *nsc,
		      cxobj       **xt)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr = NULL;    
    cxobj             *xd;
    uint32_t           session_id;
    int                ret;
    yang_stmt         *yspec;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    if (username == NULL)
	username = clicon_username_get(h);
    if (username != NULL)
	cprintf(cb, " username=\"%s\"", username);
    cprintf(cb, " xmlns:%s=\"%s\"",
	    NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, "><get-config><source><%s/></source>", db);
    if (xpath && strlen(xpath)){
	cprintf(cb, "<%s:filter %s:type=\"xpath\" %s:select=\"%s\"",
		NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX,
		xpath);
	if (xml_nsctx_cbuf(cb, nsc) < 0)
	    goto done;
	cprintf(cb, "/>");
    }
    cprintf(cb, "</get-config></rpc>");
    if ((msg = clicon_msg_encode(session_id, "%s", cbuf_get(cb))) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    /* Send xml error back: first check error, then ok */
    if ((xd = xpath_first(xret, NULL, "/rpc-reply/rpc-error")) != NULL)
	xd = xml_parent(xd); /* point to rpc-reply */
    else if ((xd = xpath_first(xret, NULL, "/rpc-reply/data")) == NULL){
	if ((xd = xml_new("data", NULL, CX_ELMNT)) == NULL)
	    goto done;
    }
    else{
	yspec = clicon_dbspec_yang(h);
	if ((ret = xml_bind_yang(xd, YB_MODULE, yspec, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    if (clixon_netconf_internal_error(xerr,
					      ". Internal error, backend returned invalid XML.",
					      NULL) < 0)
		goto done;
	    if ((xd = xpath_first(xerr, NULL, "rpc-error")) == NULL){
		clicon_err(OE_XML, ENOENT, "Expected rpc-error tag but none found(internal)");
		goto done;
	    }
	}
    }
    if (xt){
	if (xml_rm(xd) < 0)
	    goto done;
	*xt = xd;
    }
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (xerr)
	xml_free(xerr);
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send database entries as XML to backend daemon
 * @param[in] h          CLICON handle
 * @param[in] db         Name of database
 * @param[in] op         Operation on database item: OP_MERGE, OP_REPLACE
 * @param[in] xml        XML string. Ex: <config><a>..</a><b>...</b></config>
 * @retval    0          OK
 * @retval   -1          Error and logged to syslog
 * @note xml arg need to have <config> as top element
 * @code
 * if (clicon_rpc_edit_config(h, "running", OP_MERGE, 
 *                            "<config><a>4</a></config>") < 0)
 *    err;
 * @endcode
 */
int
clicon_rpc_edit_config(clicon_handle       h, 
		       char               *db, 
		       enum operation_type op,
		       char               *xmlstr)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " xmlns:%s=\"%s\"", NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    if ((username = clicon_username_get(h)) != NULL)
	cprintf(cb, " username=\"%s\"", username);
    cprintf(cb, "><edit-config><target><%s/></target>", db);
    cprintf(cb, "<default-operation>%s</default-operation>", 
	    xml_operation2str(op));
    if (xmlstr)
	cprintf(cb, "%s", xmlstr);
    cprintf(cb, "</edit-config></rpc>");
    if ((msg = clicon_msg_encode(session_id, "%s", cbuf_get(cb))) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Editing configuration", NULL);
	goto done;
    }
    retval = 0;
  done:
    if (xret)
	xml_free(xret);
    if (cb)
	cbuf_free(cb);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a request to backend to copy a file from one location to another 
 * Note this assumes the backend can access these files and (usually) assumes
 * clients and servers have the access to the same filesystem.
 * @param[in] h        CLICON handle
 * @param[in] db1      src database, eg "running"
 * @param[in] db2      dst database, eg "startup"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 * @code
 * if (clicon_rpc_copy_config(h, "running", "startup") < 0)
 *    err;
 * @endcode
 */
int
clicon_rpc_copy_config(clicon_handle h, 
		       char         *db1, 
		       char         *db2)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\">"
				 "<copy-config><source><%s/></source><target><%s/></target></copy-config></rpc>",
				 NETCONF_BASE_NAMESPACE,
				 username?username:"",
				 db1, db2)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Copying configuration", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a request to backend to delete a config database
 * @param[in] h        CLICON handle
 * @param[in] db       database, eg "running"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 * @code
 * if (clicon_rpc_delete_config(h, "startup") < 0)
 *    err;
 * @endcode
 */
int
clicon_rpc_delete_config(clicon_handle h, 
			 char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\">"
				 "<edit-config><target><%s/></target><default-operation>none</default-operation><config operation=\"delete\"/></edit-config></rpc>",
				 NETCONF_BASE_NAMESPACE,
				 username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Deleting configuration", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Lock a database
 * @param[in] h        CLICON handle
 * @param[in] db       database, eg "running"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_lock(clicon_handle h, 
		char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\">"
				 "<lock><target><%s/></target></lock></rpc>",
				 NETCONF_BASE_NAMESPACE,
				 username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Locking configuration", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Unlock a database
 * @param[in] h        CLICON handle
 * @param[in] db       database, eg "running"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_unlock(clicon_handle h, 
		  char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\">"
				 "<unlock><target><%s/></target></unlock></rpc>",
				 NETCONF_BASE_NAMESPACE,
				 username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Configuration unlock", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Get database configuration and state data
 * @param[in]  h         Clicon handle
 * @param[in]  xpath     XPath in a filter stmt (or NULL/"" for no filter)
 * @param[in]  namespace Namespace associated w xpath
 * @param[in]  nsc       Namespace context for filter
 * @param[in]  content   Clixon extension: all, config, noconfig. -1 means all
 * @param[in]  depth     Nr of XML levels to get, -1 is all, 0 is none
 * @param[out] xt        XML tree. Free with xml_free. 
 *                       Either <config> or <rpc-error>. 
 * @retval    0          OK
 * @retval   -1          Error, fatal or xml
 * @note if xpath is set but namespace is NULL, the default, netconf base 
 *       namespace will be used which is most probably wrong.
 * @code
 *  cxobj *xt = NULL;
 *  cvec *nsc = NULL;
 *
 *  if ((nsc = xml_nsctx_init(NULL, "urn:example:hello")) == NULL)
 *     err;
 *  if (clicon_rpc_get(h, "/hello/world", nsc, CONTENT_ALL, -1, &xt) < 0)
 *     err;
 *  if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
 *     clixon_netconf_error(xerr, "clicon_rpc_get", NULL);
 *     err;
 *  }
 *  if (xt)
 *     xml_free(xt);
 *  if (nsc)
 *     xml_nsctx_free(nsc);
 * @endcode
 * @see clicon_rpc_get_config which is almost the same as with content=config, but you can also select dbname
 * @see clixon_netconf_error
 * @note the netconf return message is yang populated, as well as the return data
 */
int
clicon_rpc_get(clicon_handle   h, 
	       char           *xpath,
	       cvec           *nsc, /* namespace context for filter */
	       netconf_content content,
	       int32_t         depth,
	       cxobj         **xt)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr = NULL;
    cxobj             *xd;
    char              *username;
    uint32_t           session_id;
    int                ret;
    yang_stmt         *yspec;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "<rpc xmlns=\"%s\" ", NETCONF_BASE_NAMESPACE);
    if ((username = clicon_username_get(h)) != NULL)
	cprintf(cb, " username=\"%s\"", username);
    cprintf(cb, " xmlns:%s=\"%s\"",
	    NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(cb, "><get");
    /* Clixon extension, content=all,config, or nonconfig */
    if ((int)content != -1)
	cprintf(cb, " content=\"%s\"", netconf_content_int2str(content));
    /* Clixon extension, depth=<level> */
    if (depth != -1)
	cprintf(cb, " depth=\"%d\"", depth);
    cprintf(cb, ">");
    if (xpath && strlen(xpath)) {
	cprintf(cb, "<%s:filter %s:type=\"xpath\" %s:select=\"%s\"",
		NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX, NETCONF_BASE_PREFIX,
		xpath);
	if (xml_nsctx_cbuf(cb, nsc) < 0)
	    goto done;
	cprintf(cb, "/>");
    }
    cprintf(cb, "</get></rpc>");
    if ((msg = clicon_msg_encode(session_id,
				 "%s", cbuf_get(cb))) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    /* Send xml error back: first check error, then ok */
    if ((xd = xpath_first(xret, NULL, "/rpc-reply/rpc-error")) != NULL)
	xd = xml_parent(xd); /* point to rpc-reply */
    else if ((xd = xpath_first(xret, NULL, "/rpc-reply/data")) == NULL){
	if ((xd = xml_new("data", NULL, CX_ELMNT)) == NULL)
	    goto done;
    }
    else{
	yspec = clicon_dbspec_yang(h);
	if ((ret = xml_bind_yang(xd, YB_MODULE, yspec, &xerr)) < 0)
	    goto done;
	if (ret == 0){
	    if (clixon_netconf_internal_error(xerr,
					      ". Internal error, backend returned invalid XML.",
					      NULL) < 0)
		goto done;
	    if ((xd = xpath_first(xerr, NULL, "rpc-error")) == NULL){
		clicon_err(OE_XML, ENOENT, "Expected rpc-error tag but none found(internal)");
		goto done;
	    }
	}
    }
    if (xt){
	if (xml_rm(xd) < 0)
	    goto done;
	*xt = xd;
    }
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (xerr)
	xml_free(xerr);
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a close a netconf user session. Socket is also closed if still open
 * @param[in] h        CLICON handle
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 * Session is implicitly created in eg clicon_rpc_netconf
 * @note Maybe separate closing session and closing socket.
 */
int
clicon_rpc_close_session(clicon_handle h)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    int                s;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\" message-id=\"%u\"><close-session/></rpc>",
				 NETCONF_BASE_NAMESPACE, username?username:"", 42)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((s = clicon_client_socket_get(h)) >= 0){
	close(s);
	clicon_client_socket_set(h, -1);
    }
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Close session", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Kill other user sessions
 * @param[in] h           CLICON handle
 * @param[in] session_id  Session id of other user session
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_kill_session(clicon_handle h,
			uint32_t      session_id)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           my_session_id; /* Not the one to kill */
    
    if (session_id_check(h, &my_session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(my_session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\"><kill-session><session-id>%u</session-id></kill-session></rpc>",
				 NETCONF_BASE_NAMESPACE,
				 username?username:"", session_id)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Kill session", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send validate request to backend daemon
 * @param[in] h        CLICON handle
 * @param[in] db       Name of database
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_validate(clicon_handle h, 
		    char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\"><validate><source><%s/></source></validate></rpc>",
				 NETCONF_BASE_NAMESPACE,
				 username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, CLIXON_ERRSTR_VALIDATE_FAILED, NULL);
	goto done;	
    }
    retval = 0;
 done:
    if (msg)
	free(msg);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Commit changes send a commit request to backend daemon
 * @param[in] h          CLICON handle
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_commit(clicon_handle h)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\"><commit/></rpc>",
				 NETCONF_BASE_NAMESPACE,
				 username?username:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, CLIXON_ERRSTR_COMMIT_FAILED, NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Discard all changes in candidate / revert to running
 * @param[in] h        CLICON handle
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_discard_changes(clicon_handle h)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\"  username=\"%s\"><discard-changes/></rpc>",
				 NETCONF_BASE_NAMESPACE,
				 username?username:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Discard changes", NULL);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Create a new notification subscription
 * @param[in]   h        Clicon handle
 * @param{in]   stream   name of notificatio/log stream (CLICON is predefined)
 * @param{in]   filter   message filter, eg xpath for xml notifications
 * @param[out]  s0       socket returned where notification mesages will appear
 * @retval      0        OK
 * @retval      -1       Error and logged to syslog

 * @note When using netconf create-subsrciption,status and format is not supported
 */
int
clicon_rpc_create_subscription(clicon_handle    h,
			       char            *stream, 
			       char            *filter, 
			       int             *s0)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\"><create-subscription xmlns=\"%s\">"
				 "<stream>%s</stream>"
				 "<filter type=\"xpath\" select=\"%s\" />"
				 "</create-subscription></rpc>", 
				 NETCONF_BASE_NAMESPACE,
				 username?username:"",
				 EVENT_RFC5277_NAMESPACE,
				 stream?stream:"", filter?filter:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, s0) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Create subscription", NULL);
	goto done;
    }
    retval = 0;
  done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a debug request to backend server
 * @param[in] h        CLICON handle
 * @param[in] level    Debug level
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_debug(clicon_handle h, 
		int           level)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;
    uint32_t           session_id;
    
    if (session_id_check(h, &session_id) < 0)
	goto done;
    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(session_id,
				 "<rpc xmlns=\"%s\" username=\"%s\"><debug xmlns=\"%s\"><level>%d</level></debug></rpc>",
				 NETCONF_BASE_NAMESPACE,
				 username?username:"",
				 CLIXON_LIB_NS,
				 level)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Debug", NULL);
	goto done;
    }
    if (xpath_first(xret, NULL, "//rpc-reply/ok") == NULL){
	clicon_err(OE_XML, 0, "rpc error"); /* XXX extract info from rpc-error */
	goto done;
    }
    retval = 0;
 done:
    if (msg)
	free(msg);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Send a hello request to the backend server
 * @param[in] h        CLICON handle
 * @param[in] level    Debug level
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 * @note this is internal netconf to backend, not northbound to user client
 * @note this deviates from RFC6241 slightly in that it waits for a reply, the RFC does not
 *       stipulate that.
 */
int
clicon_hello_req(clicon_handle h,
		 uint32_t     *id)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    cxobj             *x;
    char              *username;
    char              *b;
    int                ret;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode(0, "<hello username=\"%s\" xmlns=\"%s\" message-id=\"42\"><capabilities><capability>urn:ietf:params:netconf:base:1.0</capability></capabilities></hello>",
				 username?username:"",
				 NETCONF_BASE_NAMESPACE)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Hello", NULL);
	goto done;
    }
    if ((x = xpath_first(xret, NULL, "hello/session-id")) == NULL){
	clicon_err(OE_XML, 0, "hello session-id");
	goto done;
    }
    b = xml_body(x);
    if ((ret = parse_uint32(b, id, NULL)) <= 0){
	clicon_err(OE_XML, errno, "parse_uint32"); 
	goto done;
    }
    retval = 0;
 done:
    if (msg)
	free(msg);
    if (xret)
	xml_free(xret);
    return retval;
}
