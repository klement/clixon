/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <assert.h>
#include <dlfcn.h>
#include <dirent.h>
#include <libgen.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* clicon_cli */
#include "clixon_cli_api.h"
#include "cli_plugin.h"
#include "cli_handle.h"

/*
 *
 * CLI PLUGIN INTERFACE, INTERNAL SECTION
 *
 */

/*! Find syntax mode named 'mode'. Create if specified
 */
static cli_syntaxmode_t *
syntax_mode_find(cli_syntax_t *stx,
		 const char   *mode,
		 int           create)
{
    cli_syntaxmode_t *m;

    m = stx->stx_modes;
    if (m) {
	do {
	    if (strcmp(m->csm_name, mode) == 0)
		return m;
	    m = NEXTQ(cli_syntaxmode_t *, m);
	} while (m && m != stx->stx_modes);
    }
    
    if (create == 0)
	return  NULL;

   if ((m = malloc(sizeof(cli_syntaxmode_t))) == NULL) {
	perror("malloc");
	return NULL;
    }
    memset(m, 0, sizeof(*m));
    strncpy(m->csm_name, mode, sizeof(m->csm_name)-1);
    strncpy(m->csm_prompt, CLI_DEFAULT_PROMPT, sizeof(m->csm_prompt)-1);
    INSQ(m, stx->stx_modes);
    stx->stx_nmodes++;

    return m;
}

/*! Generate parse tree for syntax mode 
 * @param[in]   h     Clicon handle
 * @param[in]   m     Syntax mode struct
 */
static int
gen_parse_tree(clicon_handle     h,
	       cli_syntaxmode_t *m)
{
    cligen_tree_add(cli_cligen(h), m->csm_name, m->csm_pt);
    return 0;
}

/*! Append syntax
 * @param[in]     h       Clicon handle
 */
static int
syntax_append(clicon_handle h,
	      cli_syntax_t *stx,
	      const char   *name, 
	      parse_tree    pt)
{
    cli_syntaxmode_t *m;

    if ((m = syntax_mode_find(stx, name, 1)) == NULL) 
	return -1;

    if (cligen_parsetree_merge(&m->csm_pt, NULL, pt) < 0)
	return -1;
    
    return 0;
}

/*! Remove all cligen syntax modes
 * @param[in]     h       Clicon handle
 */
static int
cli_syntax_unload(clicon_handle h)
{
    cli_syntax_t            *stx = cli_syntax(h);
    cli_syntaxmode_t        *m;

    if (stx == NULL)
	return 0;

    while (stx->stx_nmodes > 0) {
	m = stx->stx_modes;
	DELQ(m, stx->stx_modes, cli_syntaxmode_t *);
	if (m)
	    free(m);
	stx->stx_nmodes--;
    }
    return 0;
}

/*! Dynamic linking loader string to function mapper
 *
 * Maps strings from the CLI specification file to real funtions using dlopen 
 * mapping. 
 * First look for function name in local namespace if handle given (given plugin)
 * Then check global namespace, i.e.m lib*.so
 * 
 * @param[in]  name    Name of function
 * @param[in]  handle  Handle to plugin .so module  as returned by dlopen
 * @param[out] error   Static error string, if set indicates error
 * @retval     fn      Function pointer
 * @retval     NULL    FUnction not found or symbol NULL (check error for proper handling)
 * @see see cli_plugin_load where (optional) handle opened
 * @note the returned function is not type-checked which may result in segv at runtime
 */
void *
clixon_str2fn(char  *name, 
	      void  *handle, 
	      char **error)
{
    void *fn = NULL;

    /* Reset error */
    *error = NULL;

    /* First check given plugin if any */
    if (handle) {
	dlerror();	/* Clear any existing error */
	fn = dlsym(handle, name);
	if ((*error = (char*)dlerror()) == NULL)
	    return fn;  /* If no error we found the address of the callback */
    }

    /* Now check global namespace which includes any shared object loaded
     * into the global namespace. I.e. all lib*.so as well as the 
     * master plugin if it exists 
     */
    dlerror();	/* Clear any existing error */
    fn = dlsym(NULL, name);
    if ((*error = (char*)dlerror()) == NULL)
	return fn;  /* If no error we found the address of the callback */

    /* Return value not really relevant here as the error string is set to
     * signal an error. However, just checking the function pointer for NULL
     * should work in most cases, although it's not 100% correct. 
     */
   return NULL; 
}

/*! Append to syntax mode from file
 * @param[in]  h         Clixon handle
 * @param[in]  filename	 Name of file where syntax is specified (in syntax-group dir)
 * @param[in]  dir	 Name of dir, or NULL
 */
static int
cli_load_syntax(clicon_handle h,
		const char   *filename,
		const char   *dir)
{
    void      *handle = NULL;  /* Handle to plugin .so module */
    char      *mode = NULL;    /* Name of syntax mode to append new syntax */
    parse_tree pt = {0,};
    int        retval = -1;
    FILE      *f;
    char       filepath[MAXPATHLEN];
    cvec      *cvv = NULL;
    char      *prompt = NULL;
    char     **vec = NULL;
    int        i, nvec;
    char      *plgnam;
    clixon_plugin *cp;

    if (dir)
	snprintf(filepath, MAXPATHLEN-1, "%s/%s", dir, filename);
    else
	snprintf(filepath, MAXPATHLEN-1, "%s", filename);
    if ((cvv = cvec_new(0)) == NULL){
	clicon_err(OE_PLUGIN, errno, "cvec_new");
	goto done;
    }
    /* Build parse tree from syntax spec. */
    if ((f = fopen(filepath, "r")) == NULL){
	clicon_err(OE_PLUGIN, errno, "fopen %s", filepath);
	goto done;
    }

    /* Assuming this plugin is first in queue */
    if (cli_parse_file(h, f, filepath, &pt, cvv) < 0){
	clicon_err(OE_PLUGIN, 0, "failed to parse cli file %s", filepath);
	fclose(f);
	goto done;
    }
    fclose(f);
    /* Get CLICON specific global variables */
    prompt = cvec_find_str(cvv, "CLICON_PROMPT");
    plgnam = cvec_find_str(cvv, "CLICON_PLUGIN");
    mode = cvec_find_str(cvv, "CLICON_MODE");

    if (plgnam != NULL) { /* Find plugin for callback resolving */
	if ((cp = clixon_plugin_find(h, plgnam)) != NULL)
	    handle = cp->cp_handle;
	if (handle == NULL){
	    clicon_err(OE_PLUGIN, 0, "CLICON_PLUGIN set to '%s' in %s but plugin %s.so not found in %s", 
		       plgnam, filename, plgnam, 
		       clicon_cli_dir(h));
	    goto done;
	}
    }
    /* Resolve callback names to function pointers. */
    if (cligen_callbackv_str2fn(pt, (cgv_str2fn_t*)clixon_str2fn, handle) < 0){     
	clicon_err(OE_PLUGIN, 0, "Mismatch between CLIgen file '%s' and CLI plugin file '%s'. Some possible errors:\n\t1. A function given in the CLIgen file does not exist in the plugin (ie link error)\n\t2. The CLIgen spec does not point to the correct plugin .so file (CLICON_PLUGIN=\"%s\" is wrong)", 
		   filename, plgnam, plgnam);
	goto done;
    }
     if (cligen_expandv_str2fn(pt, (expandv_str2fn_t*)clixon_str2fn, handle) < 0)     
	 goto done;
     /* Variable translation functions */
     if (cligen_translate_str2fn(pt, (translate_str2fn_t*)clixon_str2fn, handle) < 0)     
	 goto done;

    /* Make sure we have a syntax mode specified */
    if (mode == NULL || strlen(mode) < 1) { /* may be null if not given in file */
	clicon_err(OE_PLUGIN, 0, "No syntax mode specified in %s", filepath);
	goto done;
    }
    if ((vec = clicon_strsep(mode, ":", &nvec)) == NULL) 
	goto done;
    for (i = 0; i < nvec; i++) {
	if (syntax_append(h,
			  cli_syntax(h),
			  vec[i],
			  pt) < 0) { 
	    goto done;
	}
	if (prompt)
	    cli_set_prompt(h, vec[i], prompt);
    }

    cligen_parsetree_free(pt, 1);
    retval = 0;
    
done:
    if (cvv)
	cvec_free(cvv);
    if (vec)
	free(vec);
    return retval;
}

/*! Load a syntax group. Includes both CLI plugin and CLIgen spec syntax files.
 * @param[in]     h       Clicon handle
 */
int
cli_syntax_load(clicon_handle h)
{
    int                retval = -1;
    char              *plugin_dir = NULL;
    char              *clispec_dir = NULL;
    char              *clispec_file = NULL;
    int                ndp;
    int                i;
    struct dirent     *dp = NULL;
    cli_syntax_t      *stx;
    cli_syntaxmode_t  *m;
    cligen_susp_cb_t  *fns = NULL;
    cligen_interrupt_cb_t *fni = NULL;
    clixon_plugin     *cp;

    /* Syntax already loaded.  XXX should we re-load?? */
    if ((stx = cli_syntax(h)) != NULL)
	return 0;

    /* Format plugin directory path */
    plugin_dir = clicon_cli_dir(h);
    clispec_dir = clicon_clispec_dir(h);
    clispec_file = clicon_option_str(h, "CLICON_CLISPEC_FILE");

    /* Allocate plugin group object */
    if ((stx = malloc(sizeof(*stx))) == NULL) {
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(stx, 0, sizeof(*stx));	/* Zero out all */

    cli_syntax_set(h, stx);

    /* Load cli plugins */
    if (plugin_dir &&
	clixon_plugins_load(h, CLIXON_PLUGIN_INIT, plugin_dir, NULL)< 0)
	goto done;
    if (clispec_file){
	if (cli_load_syntax(h, clispec_file, NULL) < 0)
	    goto done;
    }
    if (clispec_dir){
	/* load syntaxfiles */
	if ((ndp = clicon_file_dirent(clispec_dir, &dp, "(.cli)$", S_IFREG)) < 0)
	    goto done;
	/* Load the rest */
	for (i = 0; i < ndp; i++) {
	    clicon_debug(1, "DEBUG: Loading syntax '%.*s'", 
			 (int)strlen(dp[i].d_name)-4, dp[i].d_name);
	    if (cli_load_syntax(h, dp[i].d_name, clispec_dir) < 0)
		goto done;
	}
    }
    /* Did we successfully load any syntax modes? */
    if (stx->stx_nmodes <= 0) {
	retval = 0;
	goto done;
    }	
    /* Parse syntax tree for all modes */
    m = stx->stx_modes;
    do {
	if (gen_parse_tree(h, m) != 0)
	    goto done;
	m = NEXTQ(cli_syntaxmode_t *, m);
    } while (m && m != stx->stx_modes);

    /* Set susp and interrupt callbacks into  CLIgen */
    cp = NULL;
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if (fns==NULL && (fns = cp->cp_api.ca_suspend) != NULL)
	    if (cli_susp_hook(h, fns) < 0)
		goto done;
	if (fni==NULL && (fni = cp->cp_api.ca_interrupt) != NULL)
	    if (cli_susp_hook(h, fns) < 0)
		goto done;
    }

    /* All good. We can now proudly return a new group */
    retval = 0;

done:
    if (retval != 0) {
	clixon_plugin_exit(h);
	cli_syntax_unload(h);
	cli_syntax_set(h, NULL);
    }
    if (dp)
	free(dp);
    return retval;
}

/*! Remove syntax modes and remove syntax
 * @param[in]     h       Clicon handle
 */
int
cli_plugin_finish(clicon_handle h)
{
    /* Remove all CLI plugins */
    clixon_plugin_exit(h);
    /* Remove all cligen syntax modes */
    cli_syntax_unload(h);
    cli_syntax_set(h, NULL);
    return 0;
}

/*! Help function to print a meaningful error string. 
 * Sometimes the libraries specify an error string, if so print that.
 * Otherwise just print 'command error'.
 * @param[in]  f   File handler to write error to.
 */
int 
cli_handler_err(FILE *f)
{
    if (clicon_errno){
	fprintf(f,  "%s: %s", clicon_strerror(clicon_errno), clicon_err_reason);
	if (clicon_suberrno)
	    fprintf(f, ": %s", strerror(clicon_suberrno));
	fprintf(f,  "\n");
    }
    else
	fprintf(f, "CLI command error\n");
    return 0;
}


/*! Evaluate a matched command
 * @param[in]     h       Clicon handle
 * @param[in]     cmd	  The command string
 * @retval   int If there is a callback, the return value of the callback is returned,
 * @retval   0   otherwise
 */
int
clicon_eval(clicon_handle h,
	    char         *cmd,
	    cg_obj       *match_obj,
	    cvec         *cvv)
{
    int retval = 0;

    if (!cligen_exiting(cli_cligen(h))) {	
	clicon_err_reset();
	if ((retval = cligen_eval(cli_cligen(h), match_obj, cvv)) < 0) {
#if 0 /* This is removed since we get two error messages on failure.
	 But maybe only sometime?
	 Both a real log when clicon_err is called, and the  here again.
	 (Before clicon_err was silent)  */
	    cli_handler_err(stdout); 
#endif
	}
    }
    return retval;
}

/*! Given a command string, parse and if match single command, eval it.
 * Parse and evaluate the string according to
 * the syntax parse tree of the syntax mode specified by *mode.
 * If there is no match in the tree for the command, the parse hook 
 * will be called to see if another mode should be evaluated. If a
 * match is found in another mode, the mode variable is updated to point at 
 * the new mode string.
 *
 * @param[in]     h         Clicon handle
 * @param[in]     cmd	    Command string
 * @param[in,out] modenamep Pointer to the mode string pointer
 * @param[out]    evalres   Evaluation result if retval=1
 *                       -2      On eof (shouldnt happen)
 *                       -1	  On parse error
 *                      >=0       Number of matches
 * @retval -2              Eof               CG_EOF
 * @retval -1              Error             CG_ERROR
 * @retval  0              No match          CG_NOMATCH
 * @retval  1              Exactly one match CG_MATCH
 * @retval  2+             Multiple matches
 */
int
clicon_parse(clicon_handle h, 
	     char         *cmd, 
	     char        **modenamep, 
	     int          *evalres)
{
    int        retval = -1;
    char       *modename;
    char       *modename0;
    int        r;
    cli_syntax_t *stx = NULL;
    cli_syntaxmode_t *smode;
    parse_tree *pt;     /* Orig */
    cg_obj     *match_obj;
    cvec       *cvv = NULL;
    FILE       *f;
    
    if (clicon_get_logflags()&CLICON_LOG_STDOUT)
	f = stdout;
    else
	f = stderr;
    stx = cli_syntax(h);
    if ((modename = *modenamep) == NULL) {
	smode = stx->stx_active_mode;
	modename = smode->csm_name;
    }
    else {
	if ((smode = syntax_mode_find(stx, modename, 0)) == NULL) {
	    fprintf(f, "Can't find syntax mode '%s'\n", modename);
	    goto done;
	}
    }
    if (smode){
	modename0 = NULL;
	if ((pt = cligen_tree_active_get(cli_cligen(h))) != NULL)
	    modename0 = pt->pt_name;
	if (cligen_tree_active_set(cli_cligen(h), modename) < 0){
	    fprintf(stderr, "No such parse-tree registered: %s\n", modename);
	    goto done;
	}
	if ((pt = cligen_tree_active_get(cli_cligen(h))) == NULL){
	    fprintf(stderr, "No such parse-tree registered: %s\n", modename);
	    goto done;
	}
	if ((cvv = cvec_new(0)) == NULL){
	    clicon_err(OE_UNIX, errno, "cvec_new");
	    goto done;;
	}
	retval = cliread_parse(cli_cligen(h), cmd, pt, &match_obj, cvv);
	if (retval != CG_MATCH)
	    pt_expand_cleanup_1(pt); /* XXX change to pt_expand_treeref_cleanup */
	if (modename0){
	    cligen_tree_active_set(cli_cligen(h), modename0);
	    modename0 = NULL;
	}
	switch (retval) {
	case CG_EOF: /* eof */
	case CG_ERROR:
	    fprintf(f, "CLI parse error: %s\n", cmd);
	    break;
	case CG_NOMATCH: /* no match */
	    /*	    clicon_err(OE_CFG, 0, "CLI syntax error: \"%s\": %s", 
		    cmd, cli_nomatch(h));*/
	    fprintf(f, "CLI syntax error: \"%s\": %s\n", cmd, cli_nomatch(h));
	    break;
	case CG_MATCH:
	    if (strcmp(modename, *modenamep)){	/* Command in different mode */
		*modenamep = modename;
		cli_set_syntax_mode(h, modename);
	    }
	    if ((r = clicon_eval(h, cmd, match_obj, cvv)) < 0)
		cli_handler_err(stdout);
	    pt_expand_cleanup_1(pt); /* XXX change to pt_expand_treeref_cleanup */
	    if (evalres)
		*evalres = r;
	    break;
	default:
	    fprintf(f, "CLI syntax error: \"%s\" is ambiguous\n", cmd);
	    break;
	} /* switch retval */
    }
done:
    if (cvv)
	cvec_free(cvv);
    return retval;
}

/*! Read command from CLIgen's cliread() using current syntax mode.
 * @param[in] h       Clicon handle
 * @retval    string  char* buffer containing CLIgen command
 * @retval    NULL    Fatal error
 */
char *
clicon_cliread(clicon_handle h)
{
    char             *ret;
    char             *pfmt = NULL;
    cli_syntaxmode_t *mode;
    cli_syntax_t     *stx;
    cli_prompthook_t *fn;
    clixon_plugin     *cp;
    
    stx = cli_syntax(h);
    mode = stx->stx_active_mode;
    /* Get prompt from plugin callback? */
    cp = NULL;
    while ((cp = clixon_plugin_each(h, cp)) != NULL) {
	if ((fn = cp->cp_api.ca_prompt) == NULL)
	    continue;
	pfmt = fn(h, mode->csm_name);
	break;
    }
    if (clicon_quiet_mode(h))
	cli_prompt_set(h, "");
    else
	cli_prompt_set(h, cli_prompt(pfmt ? pfmt : mode->csm_prompt));
    cligen_tree_active_set(cli_cligen(h), mode->csm_name);
    ret = cliread(cli_cligen(h));
    if (pfmt)
	free(pfmt);
    return ret;
}

/*
 *
 * CLI PLUGIN INTERFACE, PUBLIC SECTION
 *
 */


/*! Set syntax mode mode for existing current plugin group.
 * @param[in]     h       Clicon handle
 */
int
cli_set_syntax_mode(clicon_handle h,
		    const char   *name)
{
    cli_syntaxmode_t *mode;
    
    if ((mode = syntax_mode_find(cli_syntax(h), name, 1)) == NULL)
	return 0;
    
    cli_syntax(h)->stx_active_mode = mode;
    return 1;
}

/*! Get syntax mode name
 * @param[in]     h       Clicon handle
 */
char *
cli_syntax_mode(clicon_handle h)
{
    cli_syntaxmode_t *csm;

    if ((csm = cli_syntax(h)->stx_active_mode) == NULL)
	return NULL;
    return csm->csm_name;
}

/*! Callback from cli_set_prompt(). Set prompt format for syntax mode
 * @param[in]  h       Clicon handle
 * @param[in]  name    Name of syntax mode 
 * @param[in]  prompt  Prompt format
 */
int
cli_set_prompt(clicon_handle h,
	       const char   *name,
	       const char   *prompt)
{
    cli_syntaxmode_t *m;

    if ((m = syntax_mode_find(cli_syntax(h), name, 1)) == NULL)
	return -1;
    
    strncpy(m->csm_prompt, prompt, sizeof(m->csm_prompt)-1);
    return 0;
}

/*! Format prompt 
 * @param[out]    prompt  Prompt string to be written
 * @param[in]     plen    Length of prompt string
 * @param[in]     fmt     Stdarg fmt string
 */
static int
prompt_fmt(char  *prompt,
	   size_t plen,
	   char  *fmt, ...)
{
  va_list ap;
  char   *s = fmt;
  char    hname[1024];
  char    tty[32];
  char   *tmp;
  int     ret = -1;
  cbuf   *cb = NULL;

  if ((cb = cbuf_new()) == NULL){
      clicon_err(OE_XML, errno, "cbuf_new");
      goto done;
  }
  
  /* Start with empty string */
  while(*s) {
      if (*s == '%' && *++s) {
	  switch(*s) {
	  case 'H': /* Hostname */
	      if (gethostname(hname, sizeof(hname)) != 0)
		  strncpy(hname, "unknown", sizeof(hname)-1);
	      cprintf(cb, "%s", hname);
	      break;
	  case 'U': /* Username */
	      tmp = getenv("USER");
	      cprintf(cb, "%s", tmp?tmp:"nobody");
	      break;
	  case 'T': /* TTY */
	      if(ttyname_r(fileno(stdin), tty, sizeof(tty)-1) < 0)
		  strcpy(tty, "notty");
	      cprintf(cb, "%s", tty);
	      break;
	  default:
	      cprintf(cb, "%%");
	      cprintf(cb, "%c", *s);
	  }
      }
      else if (*s == '\\' && *++s) {
	  switch(*s) {
	  case 'n':
	      cprintf(cb, "\n");
              break;
	  default:
	      cprintf(cb, "\\");
	      cprintf(cb, "%c", *s);
	  }
      }
      else 
	  cprintf(cb, "%c", *s);
      s++;
  }
done:
  if (cb)
      fmt = cbuf_get(cb);
  va_start(ap, fmt);
  ret = vsnprintf(prompt, plen, fmt, ap);
  va_end(ap);
  if (cb)
      cbuf_free(cb);
  return ret;
}

/*! Return a formatted prompt string
 * @param[in]     fmt      Format string
 */
char *
cli_prompt(char *fmt)
{
    static char prompt[CLI_PROMPT_LEN];

    if (prompt_fmt(prompt, sizeof(prompt), fmt) < 0)
	return CLI_DEFAULT_PROMPT;
    
    return prompt;
}

