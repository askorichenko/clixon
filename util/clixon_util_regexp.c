/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC (Netgate)

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

  * Utility for compiling regexp and checking validity
  *  gcc -I /usr/include/libxml2 regex.c -o regex -lxml2
  * @see http://www.w3.org/TR/2004/REC-xmlschema-2-20041028
  */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <unistd.h> /* unistd */
#include <string.h>
#include <regex.h> /* posix regex */
#include <syslog.h>
#include <stdlib.h>
#include <limits.h>
#include <signal.h>

#ifdef HAVE_LIBXML2 /* Actually it should check for  a header file */
#include <libxml/xmlregexp.h>
#endif

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"

/*! libxml2 regex implementation
 *
 * @see http://www.w3.org/TR/2004/REC-xmlschema-2-20041028
 * @retval  1   Match
 * @retval  0   Not match
 * @retval -1   Error
 */
static int
regex_libxml2(char *regexp0,
              char *content0,
              int   nr,
              int   debug)
    
{
    int        retval = -1;
#ifdef HAVE_LIBXML2
    xmlChar   *regexp  = (xmlChar*)regexp0;
    xmlChar   *content = (xmlChar*)content0;
    xmlRegexp *xrp = NULL;
    int        ret;
    int        i;
    
    if ((xrp = xmlRegexpCompile(regexp)) == NULL)
        goto done;
    if (nr==0)
        return 1;
    for (i=0; i<nr; i++)
        if ((ret = xmlRegexpExec(xrp, content)) < 0)
            goto done;
    return ret;
 done:
#endif
    return retval;
}

static int
regex_posix(char *regexp,
            char *content,
            int   nr,
            int   debug)
{
    int     retval = -1;
    char   *posix = NULL;
    char    pattern[1024];
    int     status = 0;
    regex_t re;
    char    errbuf[1024];
    int     len0;
    int     i;
    
    if (regexp_xsd2posix(regexp, &posix) < 0)
        goto done;
    clixon_debug(CLIXON_DBG_DEFAULT, "posix: %s", posix);
    len0 = strlen(posix);
    if (len0 > sizeof(pattern)-5){
        fprintf(stderr, "pattern too long\n");
        return -1;
    }
    /* note following two lines trigger [-Wstringop-truncation] warnings, but see no actual error */
    strncpy(pattern, "^(", 3);
    strncpy(pattern+2, posix, sizeof(pattern)-3);
    strncat(pattern, ")$",  sizeof(pattern)-len0-1);
    if (regcomp(&re, pattern, REG_NOSUB|REG_EXTENDED) != 0) 
        return(0);      /* report error */
    if (nr==0)
        return 1;
    for (i=0; i<nr; i++)
        status = regexec(&re, content, (size_t) 0, NULL, 0);
    regfree(&re);
    if (status != 0) {
        regerror(status, &re, errbuf, sizeof(errbuf)); /* XXX error is ignored */
        return(0);      /* report error */
    }
    return(1);
 done:
    if (posix)
        free(posix);
    return retval;
}

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options]\n"
            "where options are\n"
            "\t-h \t\tHelp\n"
            "\t-D <level>\tDebug\n"
            "\t-p          \txsd->posix translation regexp (default)\n"
            "\t-x          \tlibxml2 regexp (alternative to -p)\n"
            "\t-n <nr>     \tIterate content match (default: 1, 0: no match only compile)\n"
            "\t-r <regexp> \tregexp (mandatory)\n"
            "\t-c <string> \tValue content string(mandatory if -n > 0)\n",
            argv0
            );
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int         retval = -1;
    char       *argv0 = argv[0];
    int         c;
    char       *regexp = NULL;
    char       *content = NULL;
    int         ret = 0;
    int         nr = 1;
    int         mode = 0; /* 0 is posix, 1 is libxml */
    int         dbg = 0;

    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, "hD:pxn:r:c:")) != -1)
        switch (c) {
        case 'h':
            usage(argv0);
            break;
        case 'D':
            if (sscanf(optarg, "%d", &dbg) != 1)
                usage(argv0);
            break;
        case 'p': /* xsd->posix */
            mode = 0;
            break;
        case 'n': /* Number of iterations */
            if ((nr = atoi(optarg)) < 0)
                usage(argv0);
            break;
        case 'x': /* libxml2 */
            mode = 1;
            break;
        case 'r': /* regexp */
            regexp = optarg;
            break;
        case 'c': /* value content string */
            content = optarg;
            break;
        default:
            usage(argv[0]);
            break;
        }
    clicon_log_init(__FILE__, dbg?LOG_DEBUG:LOG_INFO, CLICON_LOG_STDERR);
    clixon_debug_init(dbg, NULL);

    if (regexp == NULL){
        fprintf(stderr, "-r mandatory\n");
        usage(argv0);
    }
    if (nr > 0 && content == NULL){
        fprintf(stderr, "-c mandatory (if -n > 0)\n");
        usage(argv0);
    }
    if (mode != 0 && mode != 1){
        fprintf(stderr, "Neither posix or libxml2 set\n");
        usage(argv0);
    }
    clixon_debug(CLIXON_DBG_DEFAULT, "regexp:%s", regexp);
    clixon_debug(CLIXON_DBG_DEFAULT, "content:%s", content);
    if (mode == 0){
        if ((ret = regex_posix(regexp, content, nr, dbg)) < 0)
            goto done;

    }
    else if (mode == 1){
        if ((ret = regex_libxml2(regexp, content, nr, dbg)) < 0)
            goto done;
    }
    else
        usage(argv0);
    fprintf(stdout, "%d\n", ret);
    exit(ret);
    retval = 0;
 done:
    return retval;
}
