/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
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

  * Parse a SINGLE yang file - no dependencies - utility function only useful
  * for basic syntactic checks.
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <string.h>
#include <arpa/inet.h>
#include <regex.h>
#include <dirent.h>
#include <syslog.h>
#include <signal.h>
#include <sys/stat.h>
#include <libgen.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"

/*
*/
static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options] # input yang spec on stdin\n"
            "where options are\n"
            "\t-h \t\tHelp\n"
            "\t-D <level> \tDebug\n"
            "\t-l <s|e|o> \tLog on (s)yslog, std(e)rr, std(o)ut (stderr is default)\n",
            argv0);
    exit(0);
}

int
main(int argc, char **argv)
{
    yang_stmt *yspec = NULL;
    int        c;
    int        logdst = CLICON_LOG_STDERR;
    int        dbg = 0;
    
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, "hD:l:")) != -1)
        switch (c) {
        case 'h':
            usage(argv[0]);
            break;
        case 'D':
            if (sscanf(optarg, "%d", &dbg) != 1)
                usage(argv[0]);
            break;
        case 'l': /* Log destination: s|e|o|f */
            if ((logdst = clicon_log_opt(optarg[0])) < 0)
                usage(argv[0]);
            break;
        default:
            usage(argv[0]);
            break;
        }
    clicon_log_init("clixon_util_yang", dbg?LOG_DEBUG:LOG_INFO, logdst);
    clixon_debug_init(dbg, NULL);
    if ((yspec = yspec_new()) == NULL)
        goto done;
    if (yang_parse_file(stdin, "yang test", yspec) == NULL){
        fprintf(stderr, "yang parse error %s\n", clicon_err_reason);
        return -1;
    }
    yang_print(stdout, yspec);
 done:
    if (yspec)
        ys_free(yspec);
     return 0;
}

