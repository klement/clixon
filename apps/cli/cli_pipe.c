/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2023 Olof Hagsand 

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
 * @note Paths to bins, such as GREP_BIN, are detected in configure.ac
 * @note These functions are normally run in a forked sub-process as spawned in cligen_eval()
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#include <unistd.h>
#include <dirent.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pwd.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_cli_api.h"

/* Grep pipe output function
 *
 * @param[in]  h      Clixon handle
 * @param[in]  cmd    Command to exec
 * @param[in]  option Option to command (or NULL)
 * @param[in]  value  Command argument value (or NULL)
 * @code
 *   grep <arg:rest>, grep_fn("-e", "arg");
 * @endcode
 */
int
pipe_arg_fn(clicon_handle h,
            char         *cmd,
            char         *option,
            char         *value)
{
    int          retval = -1;
    struct stat  fstat;
    char       **argv = NULL;
    int          i;
    
    if (cmd == NULL || strlen(cmd) == 0){
        clicon_err(OE_PLUGIN, EINVAL, "cmd '%s' NULL or empty", cmd);
        goto done;
    }
    if (stat(cmd, &fstat) < 0) {
        clicon_err(OE_UNIX, errno, "stat(%s)", cmd);
        goto done;
    }
    if (!S_ISREG(fstat.st_mode)){
        clicon_err(OE_UNIX, errno, "%s is not a regular file", cmd);
        goto done;
    }
    if ((argv = calloc(4, sizeof(char *))) == NULL){
        clicon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    i = 0;
    argv[i++] = cmd;
    argv[i++] = option;
    argv[i++] = value;
    argv[i++] = NULL;
    retval = execv(cmd, argv);
 done:
    if (argv)
        free(argv);
    return retval;
}

/* Grep pipe output function
 *
 * @param[in]  h     Clicon handle
 * @param[in]  cvv   Vector of cli string and instantiated variables 
 * @param[in]  argv  String vector of options. Format: <option> <value>
 */
int
pipe_grep_fn(clicon_handle h,
             cvec         *cvv,
             cvec         *argv)
{
    int     retval = -1;
    char   *value = NULL;
    cg_var *cv;
    char   *str;
    char   *option = NULL;
    char   *argname = NULL;

    if (cvec_len(argv) != 2){
        clicon_err(OE_PLUGIN, EINVAL, "Received %d arguments. Expected: <option> <argname>", cvec_len(argv));
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) != NULL &&
        (str = cv_string_get(cv)) != NULL &&
        strlen(str))
        option = str;
    if ((cv = cvec_i(argv, 1)) != NULL &&
        (str = cv_string_get(cv)) != NULL &&
        strlen(str))
        argname = str;
    if (argname && strlen(argname)){
        if ((cv = cvec_find_var(cvv, argname)) != NULL &&
            (str = cv_string_get(cv)) != NULL &&
            strlen(str))
            value = str;
    }
    retval = pipe_arg_fn(h, GREP_BIN, option, value);
 done:
    return retval;
}

/*! wc pipe output function
 *
 * @param[in]  h     Clicon handle
 * @param[in]  cvv   Vector of cli string and instantiated variables 
 * @param[in]  argv  String vector of options. Format: <option> <value>
 */
int
pipe_wc_fn(clicon_handle h,
             cvec         *cvv,
             cvec         *argv)
{
    int     retval = -1;
    cg_var *cv;
    char   *str;
    char   *option = NULL;
    
    if (cvec_len(argv) != 1){
        clicon_err(OE_PLUGIN, EINVAL, "Received %d arguments. Expected: <option>", cvec_len(argv));
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) != NULL &&
        (str = cv_string_get(cv)) != NULL &&
        strlen(str))
        option = str;
    retval = pipe_arg_fn(h, WC_BIN, option, NULL);
 done:
    return retval;
}

/*! wc pipe output function
 *
 * @param[in]  h     Clicon handle
 * @param[in]  cvv   Vector of cli string and instantiated variables 
 * @param[in]  argv  String vector of options. Format: <option> <value>
 */
int
pipe_tail_fn(clicon_handle h,
           cvec         *cvv,
           cvec         *argv)
{
    return pipe_arg_fn(h, TAIL_BIN, "-5", NULL);
}

/*! Output pipe translate from xml to other format: json,text,
 *
 * @param[in]  h     Clicon handle
 * @param[in]  cvv   Vector of cli string and instantiated variables 
 * @param[in]  argv  String vector of show options, format:
 *   <format>        "text"|"xml"|"json"|"cli"|"netconf" (see format_enum), default: xml
 *   <pretty>        true|false: pretty-print or not
 *   <prepend>       CLI prefix: prepend before cli syntax output
 * @see cli_show_auto_devs
 */
int
pipe_showas_fn(clicon_handle h,
             cvec         *cvv,
             cvec         *argv)
{
    int              retval = -1;
    cxobj           *xt = NULL;
    int              argc = 0;
    enum format_enum format = FORMAT_XML;
    int              ybind = 1;
    yang_stmt       *yspec;
    int              pretty = 1;
    char            *prepend = NULL;

    if (cvec_len(argv) < 1 || cvec_len(argv) > 3){
        clicon_err(OE_PLUGIN, EINVAL, "Received %d arguments. Expected:: <format> [<pretty> [<prepend>]]", cvec_len(argv));
        goto done;
    }
    if (cvec_len(argv) > argc){
        fprintf(stderr, "%s formatstr:%s\n", __FUNCTION__, cv_string_get(cvec_i(argv, argc)));
        if (cli_show_option_format(argv, argc++, &format) < 0)
            goto done;
    }
    if (cvec_len(argv) > argc){
        if (cli_show_option_bool(argv, argc++, &pretty) < 0)
            goto done;
    }
    if (cvec_len(argv) > argc){
        prepend = cv_string_get(cvec_i(argv, argc++));
    }
    if (ybind){
        yspec = clicon_dbspec_yang(h);
        if (clixon_xml_parse_file(stdin, YB_MODULE, yspec, &xt, NULL) < 0)
            goto done;
    }
    else if (clixon_xml_parse_file(stdin, YB_NONE, NULL, &xt, NULL) < 0)
        goto done;
    fprintf(stderr, "%s format:%d\n", __FUNCTION__, format);
    switch (format){
    case FORMAT_XML:
        if (clixon_xml2file(stdout, xt, 0, pretty, NULL, cligen_output, 1, 0) < 0)
            goto done;
        break;
    case FORMAT_JSON:
        if (clixon_json2file(stdout, xt, pretty, cligen_output, 1, 0) < 0)
            goto done;
        break;
    case FORMAT_TEXT:
        if (clixon_txt2file(stdout, xt, 0, cligen_output, 1, 1) < 0)
            goto done;
        break;
    case FORMAT_CLI:
        if (clixon_cli2file(h, stdout, xt, prepend, cligen_output, 1) < 0) /* cli syntax */
            goto done;
        break;
    default:
        break;
    }
    retval = 0;
 done:
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Test cli callback calls cligen_output with output lines as given by function arguments
 *
 * Only for test or debugging to generate output to eg cligen_output scrolling
 * Example:
 * a, printlines_fn("line1 abc", "line2 def");
 */
int
output_fn(cligen_handle handle,
          cvec         *cvv,
          cvec         *argv)
{
    cg_var *cv;
    
    cv = NULL;
    while ((cv = cvec_each(argv, cv)) != NULL){
        cligen_output(stdout, "%s\n", cv_string_get(cv));
    }
    return 0;
}
