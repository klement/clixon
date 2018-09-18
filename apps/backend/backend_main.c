/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2018 Olof Hagsand and Benny Holmgren

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

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <ifaddrs.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/types.h>
#include <grp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <libgen.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_handle.h"
#include "backend_socket.h"
#include "backend_client.h"
#include "backend_commit.h"
#include "backend_plugin.h"
#include "backend_handle.h"

/* Command line options to be passed to getopt(3) */
#define BACKEND_OPTS "hD:f:l:d:b:Fza:u:P:1s:c:g:y:x:" /* substitute s: for IRCc:r */

#define BACKEND_LOGFILE "/usr/local/var/clixon_backend.log"

/*! Terminate. Cannot use h after this */
static int
backend_terminate(clicon_handle h)
{
    yang_spec      *yspec;
    char           *pidfile = clicon_backend_pidfile(h);
    char           *sockpath = clicon_sock(h);

    clicon_debug(1, "%s", __FUNCTION__);
    if ((yspec = clicon_dbspec_yang(h)) != NULL)
	yspec_free(yspec);
    clixon_plugin_exit(h);
    /* Delete all backend plugin RPC callbacks */
    rpc_callback_delete_all(); 
    if (pidfile)
	unlink(pidfile);   
    if (sockpath)
	unlink(sockpath);   
    xmldb_plugin_unload(h); /* unload storage plugin */
    backend_handle_exit(h); /* Cannot use h after this */
    event_exit();
    clicon_log_register_callback(NULL, NULL);
    clicon_debug(1, "%s done", __FUNCTION__); 
    clicon_log_exit();
    return 0;
}

/*! Unlink pidfile and quit
 */
static void
backend_sig_term(int arg)
{
    static int i=0;

    if (i++ == 0)
	clicon_log(LOG_NOTICE, "%s: %s: pid: %u Signal %d", 
		   __PROGRAM__, __FUNCTION__, getpid(), arg);
    clicon_exit_set(); /* checked in event_loop() */
}

/*! usage
 */
static void
usage(clicon_handle h,
      char         *argv0)
{
    char *plgdir   = clicon_backend_dir(h);
    char *confsock = clicon_sock(h);
    char *confpid  = clicon_backend_pidfile(h);
    char *group    = clicon_sock_group(h);

    fprintf(stderr, "usage:%s\n"
	    "where options are\n"
            "    -h\t\tHelp\n"
    	    "    -D <level>\tDebug level\n"
    	    "    -f <file>\tCLICON config file\n"
	    "    -l <s|e|o|f<file>> \tLog on (s)yslog, std(e)rr or std(o)ut (stderr is default) Only valid if -F, if background syslog is on syslog.\n"
	    "    -d <dir>\tSpecify backend plugin directory (default: %s)\n"
	    "    -b <dir>\tSpecify XMLDB database directory\n"
    	    "    -F\t\tRun in foreground, do not run as daemon\n"
    	    "    -z\t\tKill other config daemon and exit\n"
    	    "    -a UNIX|IPv4|IPv6\tInternal backend socket family\n"
    	    "    -u <path|addr>\tInternal socket domain path or IP addr (see -a)(default: %s)\n"
    	    "    -P <file>\tPid filename (default: %s)\n"
    	    "    -1\t\tRun once and then quit (dont wait for events)\n"
	    "    -s <mode>\tSpecify backend startup mode: none|startup|running|init (replaces -IRCr\n"
	    "    -c <file>\tLoad extra xml configuration, but don't commit.\n"
	    "    -g <group>\tClient membership required to this group (default: %s)\n"

	    "    -y <file>\tOverride yang spec file (dont include .yang suffix)\n"
	    "    -x <plugin>\tXMLDB plugin\n",
	    argv0,
	    plgdir ? plgdir : "none",
	    confsock ? confsock : "none",
	    confpid ? confpid : "none",
	    group ? group : "none"
	    );
    exit(-1);
}

static int
db_reset(clicon_handle h, 
	 char         *db)
{
    if (xmldb_exists(h, db) == 1 && xmldb_delete(h, db) != 0 && errno != ENOENT) 
	return -1;
    if (xmldb_create(h, db) < 0)
	return -1;
    return 0;
}

/*! Merge db1 into db2 without commit 
 */
static int
db_merge(clicon_handle h,
 	 const char   *db1,
    	 const char   *db2)
{
    int retval = -1;
    cxobj  *xt = NULL;

    /* Get data as xml from db1 */
    if (xmldb_get(h, (char*)db1, NULL, 1, &xt) < 0)
	goto done;
    /* Merge xml into db2. Without commit */
    if (xmldb_put(h, (char*)db2, OP_MERGE, xt, NULL) < 0)
	goto done;
    retval = 0;
 done:
    if (xt)
	xml_free(xt);
    return retval;
}


/*! Create backend server socket and register callback
 */
static int
server_socket(clicon_handle h)
{
    int ss;

    /* Open control socket */
    if ((ss = backend_socket_init(h)) < 0)
	return -1;
    /* ss is a server socket that the clients connect to. The callback
       therefore accepts clients on ss */
    if (event_reg_fd(ss, backend_accept_client, h, "server socket") < 0) {
	close(ss);
	return -1;
    }
    return ss;
}

/*! Callback for CLICON log events
 * If you make a subscription to CLICON stream, this function is called for every
 * log event.
 */
static int
backend_log_cb(int   level, 
	       char *msg, 
	       void *arg)
{
    int    retval = -1;
    size_t n;
    char  *ptr;
    char  *nptr;
    char  *newmsg = NULL;

    /* backend_notify() will go through all clients and see if any has 
       registered "CLICON", and if so make a clicon_proto notify message to
       those clients. 
       Sanitize '%' into "%%" to prevent segvfaults in vsnprintf later.
       At this stage all formatting is already done */
    n = 0;
    for(ptr=msg; *ptr; ptr++)
	if (*ptr == '%')
	    n++;
    if ((newmsg = malloc(strlen(msg) + n + 1)) == NULL) {
	clicon_err(OE_UNIX, errno, "malloc");
	return -1;
    }
    for(ptr=msg, nptr=newmsg; *ptr; ptr++) {
	*nptr++ = *ptr;
	if (*ptr == '%')
	    *nptr++ = '%';
    }
    retval = backend_notify(arg, "CLICON", level, newmsg);
    free(newmsg);

    return retval;
}

/*! Call plugin_start with -- user options */
static int
plugin_start_useroptions(clicon_handle h,
			 char         *argv0,
			 int           argc,
			 char        **argv)
{
    char *tmp;

    tmp = *(argv-1);
    *(argv-1) = argv0;
    if (clixon_plugin_start(h, argc+1, argv-1) < 0) 
	return -1;
    *(argv-1) = tmp;
    return 0;
}

/*! Load external NACM file
 */
static int
nacm_load_external(clicon_handle h)
{
    int         retval = -1;
    char       *filename; /* NACM config file */
    yang_spec  *yspec = NULL;
    cxobj      *xt = NULL;
    struct stat st;
    FILE       *f = NULL;
    int         fd;

    filename = clicon_option_str(h, "CLICON_NACM_FILE");
    if (filename == NULL || strlen(filename)==0){
	clicon_err(OE_UNIX, errno, "CLICON_NACM_FILE not set in NACM external mode");
	goto done;
    }
    if (stat(filename, &st) < 0){
	clicon_err(OE_UNIX, errno, "%s", filename);
	goto done;
    }
    if (!S_ISREG(st.st_mode)){
	clicon_err(OE_UNIX, 0, "%s is not a regular file", filename);
	goto done;
    }
    if ((f = fopen(filename, "r")) == NULL) {
	clicon_err(OE_UNIX, errno, "configure file: %s", filename);
	return -1;
    }
    if ((yspec = yspec_new()) == NULL)
	goto done;
    if (yang_parse(h, CLIXON_DATADIR, "ietf-netconf-acm", NULL, yspec) < 0)
	goto done;
    fd = fileno(f);
    /* Read configfile */
    if (xml_parse_file(fd, "</clicon>", yspec, &xt) < 0)
	goto done;
    if (xt == NULL){
	clicon_err(OE_XML, 0, "No xml tree in %s", filename);
	goto done;
    }
    if (backend_nacm_list_set(h, xt) < 0)
	goto done;
    retval = 0;
 done:
    if (yspec) /* The clixon yang-spec is not used after this */
	yspec_free(yspec);
    if (f)
	fclose(f);
    return retval;
}

/*! Merge xml in filename into database
 */
static int
load_extraxml(clicon_handle h,
	      char         *filename,
	      const char   *db)
{
    int    retval =  -1;
    cxobj *xt = NULL;
    int    fd = -1;
    
    if (filename == NULL)
	return 0;
    if ((fd = open(filename, O_RDONLY)) < 0){
	clicon_err(OE_UNIX, errno, "open(%s)", filename);
	goto done;
    }
    if (xml_parse_file(fd, "</config>", NULL, &xt) < 0)
	goto done;
    /* Replace parent w first child */
    if (xml_rootchild(xt, 0, &xt) < 0)
	goto done;
    /* Merge user reset state */
    if (xmldb_put(h, (char*)db, OP_MERGE, xt, NULL) < 0)
	goto done;
    retval = 0;
 done:
    if (fd != -1)
	close(fd);
    if (xt)
	xml_free(xt);
    return retval;
}

/*! Clixon none startup modes Do not touch running state
 */
static int
startup_mode_none(clicon_handle h)
{
    int retval = -1;

    /* If it is not there, create candidate from running  */
    if (xmldb_exists(h, "candidate") != 1)
	if (xmldb_copy(h, "running", "candidate") < 0)
	    goto done;
    /* Load plugins and call plugin_init() */
    if (backend_plugin_initiate(h) != 0) 
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Clixon init startup modes Start with a completely clean running state
 */
static int
startup_mode_init(clicon_handle h)
{
    int retval = -1;

    /* Reset running, regardless */
    if (db_reset(h, "running") < 0)
	goto done;
    /* If it is not there, create candidate from running  */
    if (xmldb_exists(h, "candidate") != 1)
	if (xmldb_copy(h, "running", "candidate") < 0)
	    goto done;
    /* Load plugins and call plugin_init() */
    if (backend_plugin_initiate(h) != 0) 
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Clixon running startup mode: Commit running db configuration into running 
 *
OK:
        copy   reset              commit   merge
running----+   |--------------------+--------+------>
            \                      /        /
candidate    +--------------------+        /
                                          /
tmp           |-------+-----+------------+---|
             reset   extra  file

COMMIT ERROR:
        copy   reset              copy 
running----+   |--------------------+------> EXIT
            \                      /       
candidate    +--------------------+        

 * @note: if commit fails, copy candidate to running and exit
 */
static int
startup_mode_running(clicon_handle h,
    		     char         *extraxml_file)
{
    int     retval = -1;

    /* Stash original running to candidate for later commit */
    if (xmldb_copy(h, "running", "candidate") < 0)
	goto done;
    /* Load plugins and call plugin_init() */
    if (backend_plugin_initiate(h) != 0) 
	goto done;
    /* Clear tmp db */
    if (db_reset(h, "tmp") < 0)
	goto done;
    /* Application may define extra xml in its reset function*/
    if (clixon_plugin_reset(h, "tmp") < 0)   
	goto done;
    /* Get application extra xml from file */
    if (load_extraxml(h, extraxml_file, "tmp") < 0)   
	goto done;	    
    /* Clear running db */
    if (db_reset(h, "running") < 0)
	goto done;
    /* Commit original running. Assume -1 is validate fail */
    if (candidate_commit(h, "candidate") < 0){
	/*  (1) We cannot differentiate between fatal errors and validation
	 *      failures
	 *  (2) If fatal error, we should exit
	 *  (3) If validation fails we cannot continue. How could we?
	 *  (4) Need to restore the running db since we destroyed it above
	 */
	clicon_log(LOG_NOTICE, "%s: Commit of saved running failed, exiting.", __FUNCTION__);
	/* Reinstate original */
	if (xmldb_copy(h, "candidate", "running") < 0)
	    goto done;
	goto done;
    }
    /* Merge user reset state and extra xml file (no commit) */
    if (db_merge(h, "tmp", "running") < 0)
	goto done;
    retval = 0;
 done:
    if (xmldb_delete(h, "tmp") < 0)
	goto done;
    return retval;
}

/*! Clixon startup startup mode: Commit startup configuration into running state


backup         +--------------------|
         copy / reset              commit merge
running   |-+----|--------------------+-----+------>
                                     /     /
startup    -------------------------+-->  /
                                         /
tmp        -----|-------+-----+---------+--|
             reset   extra  file

COMMIT ERROR:
backup         +------------------------+--|
         copy / reset               copy \
running   |-+----|--------------------+---+------->EXIT
                               error / 
startup    -------------------------+--|    

 * @note: if commit fails, copy backup to commit and exit
 */
static int
startup_mode_startup(clicon_handle h,
		     char *extraxml_file)
{
    int     retval = -1;

    /* Stash original running to backup */
    if (xmldb_copy(h, "running", "backup") < 0)
	goto done;
    /* If startup does not exist, clear it */
    if (xmldb_exists(h, "startup") != 1) /* diff */
	if (xmldb_create(h, "startup") < 0) /* diff */
	    return -1;
    /* Load plugins and call plugin_init() */
    if (backend_plugin_initiate(h) != 0) 
	goto done;
    /* Clear tmp db */
    if (db_reset(h, "tmp") < 0)
	goto done;
    /* Application may define extra xml in its reset function*/
    if (clixon_plugin_reset(h, "tmp") < 0)  
	goto done;
    /* Get application extra xml from file */
    if (load_extraxml(h, extraxml_file, "tmp") < 0)   
	goto done;	    
    /* Clear running db */
    if (db_reset(h, "running") < 0)
	goto done;
    /* Commit startup */
    if (candidate_commit(h, "startup") < 0){ /* diff */
	/*  We cannot differentiate between fatal errors and validation
	 *  failures
	 *  In both cases we copy back the original running and quit
	 */
	clicon_log(LOG_NOTICE, "%s: Commit of startup failed, exiting.", __FUNCTION__);
	if (xmldb_copy(h, "backup", "running") < 0)
	    goto done;
	goto done;
    }
    /* Merge user reset state and extra xml file (no commit) */
    if (db_merge(h, "tmp", "running") < 0)
	goto done;
    retval = 0;
 done:
    if (xmldb_delete(h, "backup") < 0)
	goto done;
    if (xmldb_delete(h, "tmp") < 0)
	goto done;
    return retval;
}

int
main(int    argc,
     char **argv)
{
    int           retval = -1;
    char          c;
    int           zap;
    int           foreground;
    int           once;
    enum startup_mode_t startup_mode;
    char         *extraxml_file;
    char         *config_group;
    char         *argv0 = argv[0];
    struct stat   st;
    clicon_handle h;
    int           help = 0;
    int           pid;
    char         *pidfile;
    char         *sock;
    int           sockfamily;
    char         *xmldb_plugin;
    int           xml_cache;
    int           xml_pretty;
    char         *xml_format;
    char         *nacm_mode;
    int          logdst = CLICON_LOG_SYSLOG|CLICON_LOG_STDERR;
    
    /* In the startup, logs to stderr & syslog and debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst);
    /* Initiate CLICON handle */
    if ((h = backend_handle_init()) == NULL)
	return -1;
    foreground = 0;
    once = 0;
    zap = 0;
    extraxml_file = NULL;

    /*
     * Command-line options for help, debug, and config-file
     */
    opterr = 0;
    optind = 1;
    while ((c = getopt(argc, argv, BACKEND_OPTS)) != -1)
	switch (c) {
	case 'h':
	    /* Defer the call to usage() to later. Reason is that for helpful
	       text messages, default dirs, etc, are not set until later.
	       But this measn that we need to check if 'help' is set before 
	       exiting, and then call usage() before exit.
	    */
	    help = 1; 
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &debug) != 1)
		usage(h, argv[0]);
	    break;
	case 'f': /* config file */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	case 'l': /* Log destination: s|e|o */
	    if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv[0]);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	    break;
	}
    /* 
     * Here we have the debug flag settings, use that.
     * Syslogs also to stderr, but later turn stderr off in daemon mode. 
     * error only to syslog. debug to syslog
     * XXX: if started in a start-daemon script, there will be irritating
     * double syslogs until fork below. 
     */
    clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, logdst); 
    clicon_debug_init(debug, NULL);

    /* Find and read configfile */
    if (clicon_options_main(h) < 0){
	if (help)
	    usage(h, argv[0]);
	return -1;
    }
    /* External NACM file? */
    nacm_mode = clicon_option_str(h, "CLICON_NACM_MODE");
    if (nacm_mode && strcmp(nacm_mode, "external") == 0)
	if (nacm_load_external(h) < 0)
	    goto done;
    
    /* Now run through the operational args */
    opterr = 1;
    optind = 1;
    while ((c = getopt(argc, argv, BACKEND_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f': /* config file */
	case 'l' :
	    break; /* see above */
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_BACKEND_DIR", optarg);
	    break;
	case 'b':  /* XMLDB database directory */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_XMLDB_DIR", optarg);
	    break;
	case 'F' : /* foreground */
	    foreground = 1;
	    break;
	case 'z': /* Zap other process */
	    zap++;
	    break;
	case 'a': /* internal backend socket address family */
	    clicon_option_str_set(h, "CLICON_SOCK_FAMILY", optarg);
	    break;
	case 'u': /* config unix domain path / ip address */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_SOCK", optarg);
	    break;
	case 'P': /* pidfile */
	    clicon_option_str_set(h, "CLICON_BACKEND_PIDFILE", optarg);
	    break;
	case '1' : /* Quit after reading database once - dont wait for events */
	    once = 1;
	    break;
	case 's' : /* startup mode */
	    clicon_option_str_set(h, "CLICON_STARTUP_MODE", optarg);
	    if (clicon_startup_mode(h) < 0){
		fprintf(stderr, "Invalid startup mode: %s\n", optarg);
		usage(h, argv[0]);
	    }
	    break;
	case 'c': /* Load application config */
	    extraxml_file = optarg;
	    break;
	case 'g': /* config socket group */
	    clicon_option_str_set(h, "CLICON_SOCK_GROUP", optarg);
	    break;
	case 'y' :{ /* Override yang module or absolute filename */
	    clicon_option_str_set(h, "CLICON_YANG_MODULE_MAIN", optarg);
	    break;
	}
	case 'x' :{ /* xmldb plugin */
	    clicon_option_str_set(h, "CLICON_XMLDB_PLUGIN", optarg);
	    break;
	}
	default:
	    usage(h, argv[0]);
	    break;
	}

    argc -= optind;
    argv += optind;

    clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, logdst); 

    /* Defer: Wait to the last minute to print help message */
    if (help)
	usage(h, argv[0]);

    /* Check pid-file, if zap kil the old daemon, else return here */
    if ((pidfile = clicon_backend_pidfile(h)) == NULL){
	clicon_err(OE_FATAL, 0, "pidfile not set");
	goto done;
    }
    sockfamily = clicon_sock_family(h);
    if ((sock = clicon_sock(h)) == NULL){
	clicon_err(OE_FATAL, 0, "sock not set");
	goto done;
    }
    if (pidfile_get(pidfile, &pid) < 0)
	return -1;
    if (zap){
	if (pid && pidfile_zapold(pid) < 0)
	    return -1;
	if (lstat(pidfile, &st) == 0)
	    unlink(pidfile);   
	if (sockfamily==AF_UNIX && lstat(sock, &st) == 0)
	    unlink(sock);   
	backend_terminate(h);
	exit(0); /* OK */
    }
    else
	if (pid){
	    clicon_err(OE_DEMON, 0, "Daemon already running with pid %d\n(Try killing it with %s -z)", 
		       pid, argv0);
	    return -1; /* goto done deletes pidfile */
	}

    /* After this point we can goto done on error 
     * Here there is either no old process or we have killed it,.. 
     */
    if (lstat(pidfile, &st) == 0)
	unlink(pidfile);   
    if (sockfamily==AF_UNIX && lstat(sock, &st) == 0)
	unlink(sock);   

    /* Sanity check: config group exists */
    if ((config_group = clicon_sock_group(h)) == NULL){
	clicon_err(OE_FATAL, 0, "clicon_sock_group option not set");
	return -1;
    }

    if (group_name2gid(config_group, NULL) < 0){
	clicon_log(LOG_ERR, "'%s' does not seem to be a valid user group.\n" 
		"The config demon requires a valid group to create a server UNIX socket\n"
		"Define a valid CLICON_SOCK_GROUP in %s or via the -g option\n"
		"or create the group and add the user to it. On linux for example:"
		"  sudo groupadd %s\n" 
		"  sudo usermod -a -G %s user\n", 
		   config_group, clicon_configfile(h),
		   config_group, config_group);
	return -1;
    }

    if (stream_register(h, "NETCONF", "default NETCONF event stream") < 0)
	goto done;
    if (stream_register(h, "CLICON", "Clicon logs") < 0)
	goto done;
	
    if ((xmldb_plugin = clicon_xmldb_plugin(h)) == NULL){
	clicon_log(LOG_ERR, "No xmldb plugin given (specify option CLICON_XMLDB_PLUGIN).\n"); 
	goto done;
    }
    if (xmldb_plugin_load(h, xmldb_plugin) < 0)
	goto done;
    /* Connect to plugin to get a handle */
    if (xmldb_connect(h) < 0)
	goto done;
    /* Read and parse application yang specification */
    if (yang_spec_main(h) == NULL)
	goto done;
    if (yang_spec_append(h, CLIXON_DATADIR, "ietf-restconf-monitoring", NULL)< 0)
	goto done;
    /* Set options: database dir and yangspec (could be hidden in connect?)*/
    if (xmldb_setopt(h, "dbdir", clicon_xmldb_dir(h)) < 0)
	goto done;
    if (xmldb_setopt(h, "yangspec", clicon_dbspec_yang(h)) < 0)
	goto done;
    if ((xml_cache = clicon_option_bool(h, "CLICON_XMLDB_CACHE")) >= 0)
	if (xmldb_setopt(h, "xml_cache", (void*)(intptr_t)xml_cache) < 0)
	    goto done;
    if ((xml_format = clicon_option_str(h, "CLICON_XMLDB_FORMAT")) >= 0)
	if (xmldb_setopt(h, "format", (void*)xml_format) < 0)
	    goto done;
    if ((xml_pretty = clicon_option_bool(h, "CLICON_XMLDB_PRETTY")) >= 0)
	if (xmldb_setopt(h, "pretty", (void*)(intptr_t)xml_pretty) < 0)
	    goto done;
    /* Startup mode needs to be defined,  */
    startup_mode = clicon_startup_mode(h);
    if (startup_mode == -1){ 	
	clicon_log(LOG_ERR, "Startup mode undefined. Specify option CLICON_STARTUP_MODE or specify -s option to clicon_backend.\n"); 
	goto done;
    }
    /* Init running db if it is not there
     */
    if (xmldb_exists(h, "running") != 1)
	if (xmldb_create(h, "running") < 0)
	    return -1;
    switch (startup_mode){
    case SM_NONE:
	if (startup_mode_none(h) < 0)
	    goto done;
	break;
    case SM_INIT: /* -I */
	if (startup_mode_init(h) < 0)
	    goto done;
	break;
    case SM_RUNNING: /* -CIr */
	if (startup_mode_running(h, extraxml_file) < 0)
	    goto done;
	break; 
    case SM_STARTUP: /* startup configuration */
	if (startup_mode_startup(h, extraxml_file) < 0)
	    goto done;
	break;
    }
    /* Initiate the shared candidate. */
    if (xmldb_copy(h, "running", "candidate") < 0)
	goto done;
    /* Call backend plugin_start with user -- options */
    if (plugin_start_useroptions(h, argv0, argc, argv) <0)
	goto done;
    if (once)
	goto done;

    /* Daemonize and initiate logging. Note error is initiated here to make
       demonized errors OK. Before this stage, errors are logged on stderr 
       also */
    if (foreground==0){
	clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, CLICON_LOG_SYSLOG);
	if (daemon(0, 0) < 0){
	    fprintf(stderr, "config: daemon");
	    exit(-1);
	}
    }
    /* Write pid-file */

    if ((pid = pidfile_write(pidfile)) <  0)
	goto done;

    /* Register log notifications */
    if (clicon_log_register_callback(backend_log_cb, h) < 0)
	goto done;
    clicon_log(LOG_NOTICE, "%s: %u Started", __PROGRAM__, getpid());
    if (set_signal(SIGTERM, backend_sig_term, NULL) < 0){
	clicon_err(OE_DEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGINT, backend_sig_term, NULL) < 0){
	clicon_err(OE_DEMON, errno, "Setting signal");
	goto done;
    }
	
    /* Initialize server socket */
    if (server_socket(h) < 0)
	goto done;

    if (debug)
	clicon_option_dump(h, debug);

    if (event_loop() < 0)
	goto done;
    retval = 0;
  done:
    clicon_log(LOG_NOTICE, "%s: %u Terminated retval:%d", __PROGRAM__, getpid(), retval);
    backend_terminate(h); /* Cannot use h after this */

    return retval;
}
