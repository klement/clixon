/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
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

  * libevhtp code  
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

/* The clixon evhtp code can be compiled with or without threading support 
 * The choice is set at libevhtp compile time by cmake. Eg:
 *    cmake -DEVHTP_DISABLE_EVTHR=ON # Disable threads.
 * Default in testing is disabled threads.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <signal.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <assert.h>
#include <sys/stat.h> /* chmod */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* evhtp */
#include <evhtp/evhtp.h>
#include <evhtp/sslutils.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* restconf */

#include "restconf_lib.h"       /* generic shared with plugins */
#include "restconf_handle.h"
#include "restconf_api.h"       /* generic not shared with plugins */
#include "restconf_err.h"
#include "restconf_root.h"

/* Command line options to be passed to getopt(3) */
#define RESTCONF_OPTS "hD:f:E:l:p:d:y:a:u:ro:"

/* See see listen(5) */
#define SOCKET_LISTEN_BACKLOG 16

/* Clixon evhtp handle 
 * Global data about evhtp lib, 
 * See evhtp_request_t *req for per-message state data
 */
typedef struct {
    clicon_handle        eh_h;
    evhtp_t            **eh_htpvec; /* One per socket */
    int                  eh_htplen; /* Number of sockets */
    struct event_base   *eh_evbase; /* Change to list */
    evhtp_ssl_cfg_t     *eh_ssl_config;
} cx_evhtp_handle;

/* Need this global to pass to signal handler 
 * XXX Try to get rid of code in signal handler
 */
static cx_evhtp_handle *_EVHTP_HANDLE = NULL;

/* Need global variable to for signal handler XXX */
static clicon_handle _CLICON_HANDLE = NULL;

static void
evhtp_terminate(cx_evhtp_handle *eh)
{
    evhtp_ssl_cfg_t *sc;
    int              i;
    
    if (eh == NULL)
	return;
    if (eh->eh_htpvec){
	for (i=0; i<eh->eh_htplen; i++){
	    evhtp_unbind_socket(eh->eh_htpvec[i]);
	    evhtp_free(eh->eh_htpvec[i]);
	}
	free(eh->eh_htpvec);
    }
    if (eh->eh_evbase)
	event_base_free(eh->eh_evbase);
    if ((sc = eh->eh_ssl_config) != NULL){
	if (sc->cafile)
	    free(sc->cafile);
	if (sc->pemfile)
	    free(sc->pemfile);
	if (sc->privfile)
	    free(sc->privfile);
	free(sc);
    }
    free(eh);
}

/*! Signall terminates process
 * XXX Try to get rid of code in signal handler -> so we can get rid of global variables
 */
static void
restconf_sig_term(int arg)
{
    static int i=0;

    if (i++ == 0)
	clicon_log(LOG_NOTICE, "%s: %s: pid: %u Signal %d", 
		   __PROGRAM__, __FUNCTION__, getpid(), arg);
    else
	exit(-1);
    if (_EVHTP_HANDLE) /* global */
	evhtp_terminate(_EVHTP_HANDLE);
    if (_CLICON_HANDLE){ /* could be replaced by eh->eh_h */
	//	stream_child_freeall(_CLICON_HANDLE);
	restconf_terminate(_CLICON_HANDLE);
    }
    clicon_exit_set(); /* XXX should rather signal event_base_loop */
    exit(-1);
}

static void
restconf_sig_child(int arg)
{
    int status;
    int pid;

    if ((pid = waitpid(-1, &status, 0)) != -1 && WIFEXITED(status)){
    }
}

static char*
evhtp_method2str(enum htp_method m)
{
    switch (m){
    case htp_method_GET:
	return "GET";
	break;
    case htp_method_HEAD:
	return "HEAD";
	break;
    case htp_method_POST:
	return "POST";
	break;
    case htp_method_PUT:
	return "PUT";
	break;
    case htp_method_DELETE:
	return "DELETE";
	break;
    case htp_method_MKCOL:
	return "MKCOL";
	break;
    case htp_method_COPY:
	return "COPY";
	break;
    case htp_method_MOVE:
	return "MOVE";
	break;
    case htp_method_OPTIONS:
	return "OPTIONS";
	break;
    case htp_method_PROPFIND:
	return "PROPFIND";
	break;
    case htp_method_PROPPATCH:
	return "PROPPATCH";
	break;
    case htp_method_LOCK:
	return "LOCK";
	break;
    case htp_method_UNLOCK:
	return "UNLOCK";
	break;
    case htp_method_TRACE:
	return "TRACE";
	break;
    case htp_method_CONNECT:
	return "CONNECT";
	break;
    case htp_method_PATCH:
	return "PATCH";
	break;
    default:
	return "UNKNOWN";
	break;
    }
}

static int
query_iterator(evhtp_header_t *hdr,
	       void           *arg)
{
    cvec   *qvec = (cvec *)arg;
    char   *key;
    char   *val;
    char   *valu = NULL;    /* unescaped value */
    cg_var *cv;

    key = hdr->key;
    val = hdr->val;
    if (uri_percent_decode(val, &valu) < 0)
	return -1;
    if ((cv = cvec_add(qvec, CGV_STRING)) == NULL){
	clicon_err(OE_UNIX, errno, "cvec_add");
	return -1;
    }
    cv_name_set(cv, key);
    cv_string_set(cv, valu);
    if (valu)
	free(valu); 
    return 0;
}

/*! Translate http header by capitalizing, prepend w HTTP_ and - -> _
 * Example: Host -> HTTP_HOST 
 */
static int
convert_fcgi(evhtp_header_t *hdr,
	     void           *arg)
{
    int           retval = -1;
    clicon_handle h = (clicon_handle)arg;
    cbuf         *cb = NULL;
    int           i;
    char          c;
    
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    /* convert key name */
    cprintf(cb, "HTTP_");
    for (i=0; i<strlen(hdr->key); i++){
	c = hdr->key[i] & 0xff;
	if (islower(c))
	    cprintf(cb, "%c", toupper(c));
	else if (c == '-')
	    cprintf(cb, "_");
	else
	    cprintf(cb, "%c", c);
    }
    if (restconf_param_set(h, cbuf_get(cb), hdr->val) < 0)
	goto done;
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Map from evhtp information to "fcgi" type parameters used in clixon code
 *
 * While all these params come via one call in fcgi, the information must be taken from
 * several different places in evhtp 
 * @param[in]  h    Clicon handle
 * @param[in]  req  Evhtp request struct
 * @param[out] qvec Query parameters, ie the ?<id>=<val>&<id>=<val> stuff
 * @retval     1    OK continue
 * @retval     0    Fail, dont continue
 * @retval    -1    Error
 * The following parameters are set:
 * QUERY_STRING
 * REQUEST_METHOD
 * REQUEST_URI
 * HTTPS
 * HTTP_HOST
 * HTTP_ACCEPT
 * HTTP_CONTENT_TYPE
 * @note there may be more used by an application plugin
 */
static int
evhtp_params_set(clicon_handle    h,
		 evhtp_request_t *req,
    		 cvec            *qvec)
{
    int           retval = -1;
    htp_method    meth;
    evhtp_uri_t  *uri;
    evhtp_path_t *path;
    evhtp_ssl_t  *ssl = NULL;
    char         *subject = NULL;
    cvec         *cvv = NULL;
    char         *cn;

    
    if ((uri = req->uri) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "No uri");
	goto done;
    }
    if ((path = uri->path) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "No path");
	goto done;
    }
    meth = evhtp_request_get_method(req);

    /* QUERY_STRING in fcgi but go direct to the info instead of putting it in a string?
     * This is different from all else: Ie one could have re-created a string here but
     * that would mean double parsing,...
     */
    if (qvec && uri->query)
	if (evhtp_kvs_for_each(uri->query, query_iterator, qvec) < 0){
	    clicon_err(OE_CFG, errno, "evhtp_kvs_for_each");
	    goto done;
	}
    if (restconf_param_set(h, "REQUEST_METHOD", evhtp_method2str(meth)) < 0)
	goto done;
    if (restconf_param_set(h, "REQUEST_URI", path->full) < 0)
	goto done;
    clicon_debug(1, "%s proto:%d", __FUNCTION__, req->proto);
    if (req->proto != EVHTP_PROTO_10 &&
	req->proto != EVHTP_PROTO_11){
	if (restconf_badrequest(h, req)	< 0)
	    goto done;
	goto fail;
    }
    clicon_debug(1, "%s conn->ssl:%d", __FUNCTION__, req->conn->ssl?1:0);
    if ((ssl = req->conn->ssl) != NULL){
	if (restconf_param_set(h, "HTTPS", "https") < 0) /* some string or NULL */
	    goto done;
	/* SSL subject fields, eg CN (Common Name) , can add more here? */
	if ((subject = (char*)htp_sslutil_subject_tostr(req->conn->ssl)) != NULL){
	    if (str2cvec(subject, '/', '=', &cvv) < 0)
		goto done;
	    if ((cn = cvec_find_str(cvv, "CN")) != NULL){
		if (restconf_param_set(h, "SSL_CN", cn) < 0)
		    goto done;
	    }
	}
    }

    /* Translate all http headers by capitalizing, prepend w HTTP_ and - -> _
     * Example: Host -> HTTP_HOST 
     */
    if (evhtp_headers_for_each(req->headers_in, convert_fcgi, h) < 0)
	goto done;
    retval = 1;
 done:
    if (subject)
	free(subject);
    if (cvv)
	cvec_free(cvv);
    return retval;
 fail:
    retval = 0;
    goto done;
}

static int
print_header(evhtp_header_t *header,
	     void           *arg)
{
    //    clicon_handle  h = (clicon_handle)arg;
    
    clicon_debug(1, "%s %s %s",
		 __FUNCTION__, header->key, header->val);
    return 0;
}

static evhtp_res
cx_pre_accept(evhtp_connection_t *conn,
	      void               *arg)
{
    //    clicon_handle  h = (clicon_handle)arg;

    clicon_debug(1, "%s", __FUNCTION__);    
    return EVHTP_RES_OK;
}

static evhtp_res
cx_post_accept(evhtp_connection_t *conn,
	       void               *arg)
{
    //    clicon_handle  h = (clicon_handle)arg;

    clicon_debug(1, "%s", __FUNCTION__);    
    return EVHTP_RES_OK;
}

/*! Generic callback called if no other callbacks are matched
 */
static void
cx_gencb(evhtp_request_t *req,
	 void            *arg)
{
    evhtp_connection_t *conn;
    //    clicon_handle       h = arg;

    clicon_debug(1, "%s", __FUNCTION__);    
    if (req == NULL){
	errno = EINVAL;
	return;
    }
    if ((conn = evhtp_request_get_connection(req)) == NULL)
	goto done;
    htp_sslutil_add_xheaders(
        req->headers_out,
        conn->ssl,
        HTP_SSLUTILS_XHDR_ALL);
    evhtp_send_reply(req, EVHTP_RES_NOTFOUND);
 done:
    return; /* void */
}

/*! /.well-known callback
 * @see cx_genb
 */
static void
cx_path_wellknown(evhtp_request_t *req,
		  void            *arg)
{
    cx_evhtp_handle *eh = (cx_evhtp_handle*)arg;
    clicon_handle    h = eh->eh_h;
    int              ret;

    clicon_debug(1, "------------");
    /* input debug */
    if (clicon_debug_get())
	evhtp_headers_for_each(req->headers_in, print_header, h);
    /* get accepted connection */

    /* set fcgi-like paramaters (ignore query vector) */
    if ((ret = evhtp_params_set(h, req, NULL)) < 0)
	goto done;
    if (ret == 1){
	/* call generic function */
	if (api_well_known(h, req) < 0)
	    goto done;
    }
    /* Clear (fcgi) paramaters from this request */
    if (restconf_param_del_all(h) < 0)
	goto done;
 done:
    return; /* void */
}

/*! /restconf callback
 * @see cx_genb
 */
static void
cx_path_restconf(evhtp_request_t *req,
		 void            *arg)
{
    cx_evhtp_handle *eh = (cx_evhtp_handle*)arg;
    clicon_handle    h = eh->eh_h;
    int              ret;
    cvec            *qvec = NULL;

    clicon_debug(1, "------------");
    /* input debug */
    if (clicon_debug_get())
	evhtp_headers_for_each(req->headers_in, print_header, h);
    
    /* get accepted connection */
    /* Query vector, ie the ?a=x&b=y stuff */
    if ((qvec = cvec_new(0)) ==NULL){
	clicon_err(OE_UNIX, errno, "cvec_new");
	goto done;
    }
    /* set fcgi-like paramaters (ignore query vector) */
    if ((ret = evhtp_params_set(h, req, qvec)) < 0)
	goto done;
    if (ret == 1){
	/* call generic function */
	if (api_root_restconf(h, req, qvec) < 0)
	    goto done;
    }

    /* Clear (fcgi) paramaters from this request */
    if (restconf_param_del_all(h) < 0)
	goto done;
 done:
    if (qvec)
	cvec_free(qvec);
    return; /* void */
}

/*! Get Server cert ssl info
 * @param[in]     h                Clicon handle
 * @param[in]     server_cert_path Path to server ssl cert file
 * @param[in]     server_key_path  Path to server ssl key file
 * @param[in,out] ssl_config       evhtp ssl config struct
 * @retval        0                OK
 * @retval        -1               Error
 */
static int
cx_get_ssl_server_certs(clicon_handle    h,
			const char      *server_cert_path,
			const char      *server_key_path,
			evhtp_ssl_cfg_t *ssl_config)
{
    int         retval = -1;
    struct stat f_stat;

    if (ssl_config == NULL){
	clicon_err(OE_CFG, EINVAL, "ssl_config is NULL");
	goto done;
    }
    if (server_cert_path == NULL){
	clicon_err(OE_CFG, EINVAL, "server_cert_path is not set but is required when ssl is enabled");
	goto done;
    }
    if (server_key_path == NULL){
	clicon_err(OE_CFG, EINVAL, "server_key_path is not set but is required when ssl is enabled");
	goto done;
    }
    if ((ssl_config->pemfile = strdup(server_cert_path)) == NULL){
	clicon_err(OE_CFG, errno, "strdup");
	goto done;
    }
    if (stat(ssl_config->pemfile, &f_stat) != 0) {
	clicon_err(OE_FATAL, errno, "Cannot load SSL cert '%s'", ssl_config->pemfile);
	goto done;
    }
    if ((ssl_config->privfile = strdup(server_key_path)) == NULL){
	clicon_err(OE_CFG, errno, "strdup");
	goto done;
    }
    if (stat(ssl_config->privfile, &f_stat) != 0) {
	clicon_err(OE_FATAL, errno, "Cannot load SSL key '%s'", ssl_config->privfile);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Get client ssl cert info
 * @param[in]     h                   Clicon handle
 * @param[in]     server_ca_cert_path Path to server ssl CA file for client certs
 * @param[in,out] ssl_config          evhtp ssl config struct
 * @retval        0                   OK
 * @retval        -1                  Error
 */
static int
cx_get_ssl_client_ca_certs(clicon_handle    h,
			   const char      *server_ca_cert_path,
			   evhtp_ssl_cfg_t *ssl_config)
{
    int         retval = -1;
    struct stat f_stat;

    if (ssl_config == NULL || server_ca_cert_path == NULL){
	clicon_err(OE_CFG, EINVAL, "Input parameter is NULL");
	goto done;
    }
    if ((ssl_config->cafile = strdup(server_ca_cert_path)) == NULL){
	clicon_err(OE_CFG, errno, "strdup");
	goto done;
    }
    if (stat(ssl_config->cafile, &f_stat) != 0) {
	clicon_err(OE_FATAL, errno, "Cannot load SSL key '%s'", ssl_config->privfile);
	goto done;
    }
    retval = 0;
 done:
    return retval;
}

static int
cx_verify_certs(int pre_verify,
		evhtp_x509_store_ctx_t *store)
{
#if 0 //def NOTYET
    char                 buf[256];
    X509               * err_cert;
    int                  err;
    int                  depth;
    SSL                * ssl;
    evhtp_connection_t * connection;
    evhtp_ssl_cfg_t    * ssl_cfg;
    
    fprintf(stderr, "%s %d\n", __FUNCTION__, pre_verify);

    err_cert   = X509_STORE_CTX_get_current_cert(store);
    err        = X509_STORE_CTX_get_error(store);
    depth      = X509_STORE_CTX_get_error_depth(store);
    ssl        = X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx());

    X509_NAME_oneline(X509_get_subject_name(err_cert), buf, 256);
    
    connection = SSL_get_app_data(ssl);
    ssl_cfg    = connection->htp->ssl_cfg;
#endif
    return pre_verify;
}

/*! Create and bind restconf socket
 * 
 * @param[in]  netns0    Network namespace, special value "default" is same as NULL
 * @param[in]  addr      Address as string, eg "0.0.0.0", "::"
 * @param[in]  addrtype  One of inet:ipv4-address or inet:ipv6-address
 * @param[in]  port      TCP port
 * @param[out] ss        Server socket (bound for accept)
 */
static int
restconf_socket_init(const char   *netns0,
		     const char   *addr,
		     const char   *addrtype,
		     uint16_t      port,
		     int          *ss)
{
    int                 retval = -1;
    struct sockaddr   * sa;
    struct sockaddr_in6 sin6   = { 0 };
    struct sockaddr_in  sin    = { 0 };
    size_t              sin_len;
    const char         *netns;

    clicon_debug(1, "%s %s %s %s", __FUNCTION__, netns0, addrtype, addr);
    /* netns default -> NULL */
    if (netns0 != NULL && strcmp(netns0, "default")==0)
	netns = NULL;
    else
	netns = netns0;
    if (strcmp(addrtype, "inet:ipv6-address") == 0) {
        sin_len          = sizeof(struct sockaddr_in6);
        sin6.sin6_port   = htons(port);
        sin6.sin6_family = AF_INET6;

        inet_pton(AF_INET6, addr, &sin6.sin6_addr);
        sa = (struct sockaddr *)&sin6;
    }
    else if (strcmp(addrtype, "inet:ipv4-address") == 0) {
        sin_len             = sizeof(struct sockaddr_in);
        sin.sin_family      = AF_INET;
        sin.sin_port        = htons(port);
        sin.sin_addr.s_addr = inet_addr(addr);

        sa = (struct sockaddr *)&sin;
    }
    else{
	clicon_err(OE_XML, EINVAL, "Unexpected addrtype: %s", addrtype);
	return -1;
    }
    if (clixon_netns_socket(netns, sa, sin_len, SOCKET_LISTEN_BACKLOG, ss) < 0)
	goto done;
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return retval;
}

/*! Usage help routine
 * @param[in]  argv0  command line
 * @param[in]  h      Clicon handle
 */
static void
usage(clicon_handle h,
      char         *argv0)

{
    fprintf(stderr, "usage:%s [options]\n"
	    "where options are\n"
            "\t-h \t\t  Help\n"
	    "\t-D <level>\t  Debug level\n"
    	    "\t-f <file>\t  Configuration file (mandatory)\n"
	    "\t-E <dir> \t  Extra configuration file directory\n"
	    "\t-l <s|f<file>> \t  Log on (s)yslog, (f)ile (syslog is default)\n"
	    "\t-p <dir>\t  Yang directory path (see CLICON_YANG_DIR)\n"
	    "\t-d <dir>\t  Specify restconf plugin directory dir (default: %s)\n"
	    "\t-y <file>\t  Load yang spec file (override yang main module)\n"
    	    "\t-a UNIX|IPv4|IPv6 Internal backend socket family\n"
    	    "\t-u <path|addr>\t  Internal socket domain path or IP addr (see -a)\n"
	    "\t-r \t\t  Do not drop privileges if run as root\n"
	    "\t-o <option>=<value> Set configuration option overriding config file (see clixon-config.yang)\n"
	    ,
	    argv0,
	    clicon_restconf_dir(h)
	    );
    exit(0);
}

/*! Extract socket info from backend config 
 * @param[in]  h         Clicon handle
 * @param[in]  xs        socket config
 * @param[in]  nsc       Namespace context
 * @param[out] namespace 
 * @param[out] address   Address as string, eg "0.0.0.0", "::"
 * @param[out] addrtype  One of inet:ipv4-address or inet:ipv6-address
 * @param[out] port
 * @param[out] ssl
 */
static int
cx_evhtp_socket_extract(clicon_handle h,
			cxobj        *xs,
			cvec         *nsc,
			char        **namespace,
			char        **address,
			char        **addrtype,
			uint16_t     *port,
			uint16_t     *ssl)
{
    int        retval = -1;
    cxobj     *x;
    char      *str = NULL;
    char      *reason = NULL;
    int        ret;
    char      *body;
    cg_var    *cv = NULL;
    yang_stmt *y;
    yang_stmt *ysub = NULL;

    if ((x = xpath_first(xs, nsc, "namespace")) == NULL){
	clicon_err(OE_XML, EINVAL, "Mandatory namespace not given");
	goto done;
    }
    *namespace = xml_body(x);
    if ((x = xpath_first(xs, nsc, "address")) == NULL){
	clicon_err(OE_XML, EINVAL, "Mandatory address not given");
	goto done;
    }
    /* address is a union type and needs a special investigation to see which type (ipv4 or ipv6)
     * the address is
     */
    body = xml_body(x);
    y = xml_spec(x);
    if ((cv = cv_dup(yang_cv_get(y))) == NULL){
	clicon_err(OE_UNIX, errno, "cv_dup");
	goto done;
    }
    if ((ret = cv_parse1(body, cv, &reason)) < 0){
	clicon_err(OE_XML, errno, "cv_parse1");
	goto done;
    }
    if (ret == 0){
	clicon_err(OE_XML, EFAULT, "%s", reason);
	goto done;
    }
    if ((ret = ys_cv_validate(h, cv, y, &ysub, &reason)) < 0)
	goto done;
    if (ret == 0){
	clicon_err(OE_XML, EFAULT, "Validation os address: %s", reason);
	goto done;
    }
    if (ysub == NULL){
	clicon_err(OE_XML, EFAULT, "No address union type");
	goto done;
    }
    *address = body;
    /* This is YANG type name of ip-address:
     *   typedef ip-address {
     *     type union {
     *       type inet:ipv4-address; <---
     *       type inet:ipv6-address; <---
     *     }
     */
    *addrtype = yang_argument_get(ysub); 
    if ((x = xpath_first(xs, nsc, "port")) != NULL &&
	(str = xml_body(x)) != NULL){
	if ((ret = parse_uint16(str, port, &reason)) < 0){
	    clicon_err(OE_XML, errno, "parse_uint16");
	    goto done;
	}
	if (ret == 0){
	    clicon_err(OE_XML, EINVAL, "Unrecognized value of port: %s", str);
	    goto done;
	}
    }
    if ((x = xpath_first(xs, nsc, "ssl")) != NULL &&
	(str = xml_body(x)) != NULL){
	/* XXX use parse_bool but it is legacy static */
	if (strcmp(str, "false") == 0)
	    *ssl = 0;
	else if (strcmp(str, "true") == 0)
	    *ssl = 1;
	else {
	    clicon_err(OE_XML, EINVAL, "Unrecognized value of ssl: %s", str);
	    goto done;
	}
    }
    retval = 0;
 done:
    if (cv)
        cv_free(cv);
    if (reason)
	free(reason);
    return retval;
}

static int
cx_htp_add(cx_evhtp_handle *eh,
	   evhtp_t         *htp)
{
    eh->eh_htplen++;
    if ((eh->eh_htpvec = realloc(eh->eh_htpvec, eh->eh_htplen*sizeof(htp))) == NULL){
	clicon_err(OE_UNIX, errno, "realloc");
	return -1;
    }
    eh->eh_htpvec[eh->eh_htplen-1] = htp;
    return 0;
}

/*! Phase 2 of backend evhtp init, config single socket
 * @param[in]  h        Clicon handle
 * @param[in]  eh       Evhtp handle
 * @param[in]  ssl_enable Server is SSL enabled
 * @param[in]  xs       XML config of single restconf socket
 * @param[in]  nsc      Namespace context
 */
static int
cx_evhtp_socket(clicon_handle    h,
		cx_evhtp_handle *eh,
		int              ssl_enable,
		cxobj           *xs,
		cvec            *nsc,
		char            *server_cert_path,
		char            *server_key_path,
		char            *server_ca_cert_path)
{
    int          retval = -1;
    char        *netns = NULL;
    char        *address = NULL;
    char        *addrtype = NULL;
    uint16_t     ssl = 0;
    uint16_t     port = 0;
    int          ss = -1;
    evhtp_t     *htp = NULL;

    clicon_debug(1, "%s", __FUNCTION__);
    /* This is socket create a new evhtp_t instance */
    if ((htp = evhtp_new(eh->eh_evbase, NULL)) == NULL){
	clicon_err(OE_UNIX, errno, "evhtp_new");
	goto done;
    }
#ifndef EVHTP_DISABLE_EVTHR /* threads */
    evhtp_use_threads_wexit(htp, NULL, NULL, 4, NULL);
#endif
    /* Callback before the connection is accepted. */
    evhtp_set_pre_accept_cb(htp, cx_pre_accept, h);
    /* Callback right after a connection is accepted. */
    evhtp_set_post_accept_cb(htp, cx_post_accept, h);
    /* Callback to be executed for all /restconf api calls */
    if (evhtp_set_cb(htp, "/" RESTCONF_API, cx_path_restconf, eh) == NULL){
    	clicon_err(OE_EVENTS, errno, "evhtp_set_cb");
    	goto done;
    }
    /* Callback to be executed for all /restconf api calls */
    if (evhtp_set_cb(htp, RESTCONF_WELL_KNOWN, cx_path_wellknown, eh) == NULL){
    	clicon_err(OE_EVENTS, errno, "evhtp_set_cb");
    	goto done;
   }
    /* Generic callback called if no other callbacks are matched */
    evhtp_set_gencb(htp, cx_gencb, h);

    /* Extract socket parameters from single socket config: ns, addr, port, ssl */
    if (cx_evhtp_socket_extract(h, xs, nsc, &netns, &address, &addrtype, &port, &ssl) < 0)
	goto done;
    /* Sanity checks of socket parameters */
    if (ssl){
	if (ssl_enable == 0 || server_cert_path==NULL || server_key_path == NULL){
	    clicon_err(OE_XML, EINVAL, "Enabled SSL server requires server_cert_path and server_key_path"); 
	    goto done;
	}
	//    ssl_verify_mode             = htp_sslutil_verify2opts(optarg);
	if (evhtp_ssl_init(htp, eh->eh_ssl_config) < 0){
	    clicon_err(OE_UNIX, errno, "evhtp_new");
	    goto done;
	}
    }
    /* Open restconf socket and bind */
    if (restconf_socket_init(netns, address, addrtype, port, &ss) < 0)
	goto done;
    /* ss is a server socket that the clients connect to. The callback
       therefore accepts clients on ss */
    if (evhtp_accept_socket(htp, ss, SOCKET_LISTEN_BACKLOG) < 0) {
        /* accept_socket() does not close the descriptor
         * on error, but this function does.
         */
        close(ss);
	goto done;
    }
    if (cx_htp_add(eh, htp) < 0)
	goto done;
    retval = 0;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return retval;
}

/*! Phase 2 of backend evhtp init, config has been retrieved from backend
 * @param[in]  h        Clicon handle
 * @param[in]  xconfig  XML config
 * @param[in]  nsc      Namespace context
 * @param[in]  eh       Evhtp handle
 * @retval     -1       Error
 * @retval     0        OK, but restconf disabled, proceed with other if possible
 * @retval     1        OK
 */
static int
cx_evhtp_init(clicon_handle     h,
	      cxobj            *xrestconf,
	      cvec             *nsc,
	      cx_evhtp_handle  *eh)
{
    int     retval = -1;
    int     ssl_enable = 0;
    int     dbg = 0;
    cxobj **vec = NULL;
    size_t  veclen;
    char   *server_cert_path = NULL;
    char   *server_key_path = NULL;
    char   *server_ca_cert_path = NULL;
    cxobj  *x;
    char   *bstr;
    int     i;
    int     ret;
    clixon_auth_type_t auth_type;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((ret = restconf_config_init(h, xrestconf)) < 0)
	goto done;
    if (ret == 0)
	goto disable;
    auth_type = restconf_auth_type_get(h);
    /* If at least one socket has ssl then enable global ssl_enable */
    ssl_enable = xpath_first(xrestconf, nsc, "socket[ssl='true']") != NULL;

    if ((x = xpath_first(xrestconf, nsc, "server-cert-path")) != NULL)
	server_cert_path = xml_body(x);
    if ((x = xpath_first(xrestconf, nsc, "server-key-path")) != NULL)
	server_key_path = xml_body(x);
    if ((x = xpath_first(xrestconf, nsc, "server-ca-cert-path")) != NULL)
	server_ca_cert_path = xml_body(x);
    if ((x = xpath_first(xrestconf, nsc, "debug")) != NULL &&
	(bstr = xml_body(x)) != NULL){
	dbg = atoi(bstr);
	clicon_debug_init(dbg, NULL); 	
    }

    /* Here the daemon either uses SSL or not, ie you cant seem to mix http and https :-( */
    if (ssl_enable){
	/* Init evhtp ssl config struct */
	if ((eh->eh_ssl_config = malloc(sizeof(evhtp_ssl_cfg_t))) == NULL){
	    clicon_err(OE_UNIX, errno, "malloc");
	    goto done;
	}
	memset(eh->eh_ssl_config, 0, sizeof(evhtp_ssl_cfg_t));
	eh->eh_ssl_config->ssl_opts = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1;

	/* Read server ssl files cert and key */
	if (cx_get_ssl_server_certs(h, server_cert_path, server_key_path, eh->eh_ssl_config) < 0)
	    goto done;
	/* If client auth get client CA cert */
	if (auth_type == CLIXON_AUTH_CLIENT_CERTIFICATE)
	    if (cx_get_ssl_client_ca_certs(h, server_ca_cert_path, eh->eh_ssl_config) < 0)
		goto done;
	eh->eh_ssl_config->x509_verify_cb = cx_verify_certs; /* Is extra verification necessary? */
	if (auth_type == CLIXON_AUTH_CLIENT_CERTIFICATE){
	    eh->eh_ssl_config->verify_peer = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
	    eh->eh_ssl_config->x509_verify_cb = cx_verify_certs;
	    eh->eh_ssl_config->verify_depth = 2;
	}
	//    ssl_verify_mode             = htp_sslutil_verify2opts(optarg);
    }
    /* get the list of socket config-data */
    if (xpath_vec(xrestconf, nsc, "socket", &vec, &veclen) < 0)
	goto done;
    for (i=0; i<veclen; i++){
	if (cx_evhtp_socket(h, eh, ssl_enable, vec[i], nsc,
			    server_cert_path, server_key_path, server_ca_cert_path) < 0)
	    goto done;
    }
    retval = 1;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (vec)
	free(vec);
    return retval;
 disable:
    retval = 0;
    goto done;
}

/*! Read restconf from config 
 * After SEVERAL iterations the code now does as follows:
 * - init clixon
 * - init evhtp
 * - look for local config (in clixon-config file) 
 * - if local config found, open sockets accordingly and exit function
 * - If no local config found, query backend for config and open sockets.
 * That is, EITHER local config OR read config from backend once
 * @param[in]  h     Clicon handle
 * @param[in]  eh    Clixon's evhtp handle
 * @retval     0     OK
 * @retval    -1     Error
 */ 
int
restconf_config(clicon_handle    h,
		cx_evhtp_handle *eh)
{
    int            retval = -1;
    char          *dir;
    yang_stmt     *yspec = NULL;
    char          *str;
    clixon_plugin *cp = NULL;
    cvec          *nsctx_global = NULL; /* Global namespace context */
    size_t         cligen_buflen;
    size_t         cligen_bufthreshold;
    cvec          *nsc = NULL;
    cxobj         *xerr = NULL;
    uint32_t       id = 0; /* Session id, to poll backend up */
    struct passwd *pw;
    cxobj         *xconfig1 = NULL;
    cxobj         *xrestconf1 = NULL;
    cxobj         *xconfig2 = NULL;
    cxobj         *xrestconf2 = NULL;
    int            ret;
    int            backend = 1; /* query backend for config */

    /* Set default namespace according to CLICON_NAMESPACE_NETCONF_DEFAULT */
    xml_nsctx_namespace_netconf_default(h);
    
    assert(SSL_VERIFY_NONE == 0);

    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
	goto done;
    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);
    /* Treat unknown XML as anydata */
    if (clicon_option_bool(h, "CLICON_YANG_UNKNOWN_ANYDATA") == 1)
	xml_bind_yang_unknown_anydata(1);
    
    /* Load restconf plugins before yangs are loaded (eg extension callbacks) */
    if ((dir = clicon_restconf_dir(h)) != NULL)
	if (clixon_plugins_load(h, CLIXON_PLUGIN_INIT, dir, NULL) < 0)
	    return -1;
    /* Create a pseudo-plugin to create extension callback to set the ietf-routing
     * yang-data extension for api-root top-level restconf function.
     */
    if (clixon_pseudo_plugin(h, "pseudo restconf", &cp) < 0)
	goto done;
    cp->cp_api.ca_extension = restconf_main_extension_cb;

    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
	if (yang_spec_parse_file(h, str, yspec) < 0)
	    goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
	if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
				   yspec) < 0)
	    goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
	if (yang_spec_load_dir(h, str, yspec) < 0)
	    goto done;
    }
    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
	goto done;
    /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;

    /* Load yang restconf module */
    if (yang_spec_parse_module(h, "ietf-restconf", NULL, yspec)< 0)
	goto done;
    
    /* Add netconf yang spec, used as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;

    /* Add system modules */
    if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC8040") &&
	yang_spec_parse_module(h, "ietf-restconf-monitoring", NULL, yspec)< 0)
	goto done;
    if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC5277") &&
	yang_spec_parse_module(h, "clixon-rfc5277", NULL, yspec)< 0)
	goto done;

    /* Here all modules are loaded 
     * Compute and set canonical namespace context
     */
    if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
	goto done;
    if (clicon_nsctx_global_set(h, nsctx_global) < 0)
	goto done;

    /* Init evhtp, common stuff */
    if ((eh->eh_evbase = event_base_new()) == NULL){
	clicon_err(OE_UNIX, errno, "event_base_new");
	goto done;
    }

    /* First get local config */
    xconfig1 = clicon_conf_xml(h);
    if ((xrestconf1 = xpath_first(xconfig1, NULL, "restconf")) != NULL){
	/* Initialize evhtp with local config: ret 0 means disabled -> need to query remote */
	if ((ret = cx_evhtp_init(h, xrestconf1, NULL, eh)) < 0)
	    goto done;
	if (ret == 1)
	    backend = 0;
    }
    if (backend){     /* Query backend of config. */
	/* Before evhtp, try again if not done */
	while (1){
	    if (clicon_hello_req(h, &id) < 0){
		if (errno == ENOENT){
		    fprintf(stderr, "waiting");
		    sleep(1);
		    continue;
		}
		clicon_err(OE_UNIX, errno, "clicon_session_id_get");
		goto done;
	    }
	    clicon_session_id_set(h, id);
	    break;
	}
	if ((nsc = xml_nsctx_init(NULL, CLIXON_RESTCONF_NS)) == NULL)
	    goto done;
	if ((pw = getpwuid(getuid())) == NULL){
	    clicon_err(OE_UNIX, errno, "getpwuid");
	    goto done;
	}
	if (clicon_rpc_get_config(h, pw->pw_name, "running", "/restconf", nsc, &xconfig2) < 0)
	    goto done;
	if ((xerr = xpath_first(xconfig2, NULL, "/rpc-error")) != NULL){
	    clixon_netconf_error(xerr, "Get backend restconf config", NULL);
	    goto done;
	}
	/* Extract socket fields from xconfig */
	if ((xrestconf2 = xpath_first(xconfig2, nsc, "restconf")) != NULL){
	    /* Initialize evhtp with config from backend */
	    if (cx_evhtp_init(h, xrestconf2, nsc, eh) < 0)
		goto done;
	}
    }
    retval = 0;
 done:
    if (xconfig2)
	xml_free(xconfig2);
    if (nsc)
	cvec_free(nsc);
    return retval;
}
    
int
main(int    argc,
     char **argv)
{
    int                retval = -1;
    char	      *argv0 = argv[0];
    int                c;
    clicon_handle      h;
    int                logdst = CLICON_LOG_SYSLOG;
    int                dbg = 0;
    cx_evhtp_handle   *eh = NULL;
    int                drop_privileges = 1;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst); 

    /* Create handle */
    if ((h = restconf_handle_init()) == NULL)
	goto done;

    _CLICON_HANDLE = h; /* for termination handling */

    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
	switch (c) {
	case 'h':
	    usage(h, argv0);
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &dbg) != 1)
		usage(h, argv0);
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(h, argv0);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	case 'E': /* extra config directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGDIR", optarg);
	    break;
	 case 'l': /* Log destination: s|e|o */
	     if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv0);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	   break;
	} /* switch getopt */

    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst); 

    clicon_debug_init(dbg, NULL); 
    clicon_log(LOG_NOTICE, "%s: %u Started", __PROGRAM__, getpid());
    if (set_signal(SIGTERM, restconf_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGINT, restconf_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGCHLD, restconf_sig_child, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    
    /* Find and read configfile */
    if (clicon_options_main(h) < 0)
	goto done;
    //    stream_path = clicon_option_str(h, "CLICON_STREAM_PATH");
    
    /* Now rest of options, some overwrite option file */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f':  /* config file */
	case 'E':  /* extra config dir */
	case 'l':  /* log  */
	    break; /* see above */
	case 'p' : /* yang dir path */
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(h, argv0);
	    clicon_option_str_set(h, "CLICON_RESTCONF_DIR", optarg);
	    break;
	case 'y' : /* Load yang spec file (override yang main module) */
	    clicon_option_str_set(h, "CLICON_YANG_MAIN_FILE", optarg);
	    break;
	case 'a': /* internal backend socket address family */
	    clicon_option_str_set(h, "CLICON_SOCK_FAMILY", optarg);
	    break;
	case 'u': /* internal backend socket unix domain path or ip host */
	    if (!strlen(optarg))
		usage(h, argv0);
	    clicon_option_str_set(h, "CLICON_SOCK", optarg);
	    break;
	case 'r':{ /* Do not drop privileges if run as root */
	    drop_privileges = 0;
	    break;
	}
	case 'o':{ /* Configuration option */
	    char          *val;
	    if ((val = index(optarg, '=')) == NULL)
		usage(h, argv0);
	    *val++ = '\0';
	    if (clicon_option_add(h, optarg, val) < 0)
		goto done;
	    break;
	}
        default:
            usage(h, argv0);
            break;
	}
    argc -= optind;
    argv += optind;

    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);

    /* Init restconf auth-type */
    restconf_auth_type_set(h, CLIXON_AUTH_NONE);
    
    /* Dump configuration options on debug */
    if (dbg)      
	clicon_option_dump(h, dbg);

    /* Call start function in all plugins before we go interactive */
     if (clixon_plugin_start_all(h) < 0)
	 goto done;

    if ((eh = malloc(sizeof *eh)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(eh, 0, sizeof *eh);
    eh->eh_h = h;
    _EVHTP_HANDLE = eh; /* global */

    /* Read config */ 
    if (restconf_config(h, eh) < 0)
	goto done;
    /* Drop privileges after evhtp and server key/cert read */
    if (drop_privileges){
	/* Drop privileges to WWWUSER if started as root */
	if (restconf_drop_privileges(h, WWWUSER) < 0)
	    goto done;
    }
    /* libevent main loop */
    event_base_loop(eh->eh_evbase, 0); /* Replace with clixon_event_loop() if libevent is replaced */

    retval = 0;
 done:
    clicon_debug(1, "restconf_main_evhtp done");
    //    stream_child_freeall(h);
    evhtp_terminate(eh);    
    restconf_terminate(h);    
    return retval;
}
