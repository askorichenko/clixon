/*
 *
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren

  This file is part of CLICON.

  CLICON is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  CLICON is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with CLICON; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>.

 * XML database
 * TODO: xmldb_del: or dbxml_put_xkey delete
 * TODO: xmldb_get: xpath: only load partial tree
 */
#ifdef HAVE_CONFIG_H
#include "clicon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <fnmatch.h>
#include <stdint.h>
#include <assert.h>
#include <syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clicon_err.h"
#include "clicon_log.h"
#include "clicon_queue.h"
#include "clicon_string.h"
#include "clicon_chunk.h"
#include "clicon_hash.h"
#include "clicon_handle.h"
#include "clicon_qdb.h"
#include "clicon_yang.h"
#include "clicon_handle.h"
#include "clicon_xml.h"
#include "clicon_xsl.h"
#include "clicon_xml_parse.h"
#include "clicon_xml_db.h"

/*
 * An xml database consists of key-value pairs for xml-trees.
 * Each node in an xml-tree has a key and an optional value.
 * The key (xmlkey) is constructed from the xml node name concatenated
 * with its ancestors and any eventual list keys.
 * A xmlkeyfmt is a help-structure used when accessing the XML database.
 * It consists of an xmlkey but with the key fields replaced with wild-chars(%s)
 * Example: /aaa/bbb/%s/%s/ccc
 * Such an xmlkeyfmt can be obtained from a yang-statement by following
 * its ancestors to the root module. If one of the ancestors is a list, 
 * a wildchar (%s) is inserted for each key.
 * These xmlkeyfmt keys are saved and used in cli callbacks such as when
 * modifying syntax (eg cli_merge/cli_delete) or when completing for sub-symbols
 * In this case, the variables are set and the wildcards can be instantiated.
 * An xml tree can then be formed that can be used to the xmldb_get() or 
 * xmldb_put() functions.
 * The relations between the functions and formats are as follows:
 *
 * +-----------------+                   +-----------------+
 * | yang-stmt       |    yang2xmlkeyfmt |   xmlkeyfmt     |
 * | list aa,leaf k  | ----------------->|     /aa/%s      |
 * +-----------------+                   +-----------------+
 *                                               |
 *                                               | xmlkeyfmt2key
 *                                               | k=17
 *                                               v
 * +-------------------+                +-----------------+
 * | xml-tree/cxobj    |   xmlkey2xml   |  xmlkey         |
 * | <aa><k>17</k></aa>| <------------- |   /aa/17        |
 * +-------------------+                +-----------------+
 *
 * ALternative for xmlkeyfmt would be xpath: eg 
 * instead of    /interfaces/interface/%s/ipv4/address/ip/%s
 * you can have: /interfaces/interface[name=%s]/ipv4/address/[ip=%s]
 */
/*! Recursive help function */
static int
yang2xmlkeyfmt_1(yang_stmt *ys, cbuf *cb)
{
    yang_node *yn;
    yang_stmt *ykey;
    int        i;
    cvec      *cvk = NULL; /* vector of index keys */
    int        retval = -1;

    yn = ys->ys_parent;
    if (yn != NULL && 
	yn->yn_keyword != Y_MODULE && 
	yn->yn_keyword != Y_SUBMODULE){
	if (yang2xmlkeyfmt_1((yang_stmt *)yn, cb) < 0)
	    goto done;
    }
    if (ys->ys_keyword != Y_CHOICE && ys->ys_keyword != Y_CASE)
	cprintf(cb, "/%s", ys->ys_argument);
    switch (ys->ys_keyword){
    case Y_LIST:
	if ((ykey = yang_find((yang_node*)ys, Y_KEY, NULL)) == NULL){
	    clicon_err(OE_XML, errno, "%s: List statement \"%s\" has no key", 
		       __FUNCTION__, ys->ys_argument);
	    goto done;
	}
	/* The value is a list of keys: <key>[ <key>]*  */
	if ((cvk = yang_arg2cvec(ykey, " ")) == NULL)
	    goto done;
	/* Iterate over individual keys  */
	for (i=0; i<cvec_len(cvk); i++)
	    cprintf(cb, "/%%s");
	break;
    case Y_LEAF_LIST:
	cprintf(cb, "/%%s");
	break;
    default:
	break;
    } /* switch */
    retval = 0;
 done:
    return retval;
}

/*! Construct an xml key format from yang statement using wildcards for keys
 * Recursively contruct it to the top.
 * Example: 
 *   yang:  container a -> list b -> key c -> leaf d
 *   xpath: /a/b/%s/d
 * @param[in]  ys     Yang statement
 * @param[out] xpath  String, needs to be freed after use
 */ 
int
yang2xmlkeyfmt(yang_stmt *ys, char **xkfmt)
{
    int   retval = -1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) == NULL)
	goto done;
    if (yang2xmlkeyfmt_1(ys, cb) < 0)
	goto done;
    if ((*xkfmt = strdup(cbuf_get(cb))) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Transform an xml key format and a vector of values to an XML key
 * Example: 
 *   xmlkeyfmt:  /aaa/%s
 *   cvv:        key=17
 *   xmlkey:     /aaa/17
 * @param[in]  xkfmt  XML key format, eg /aaa/%s
 * @param[in]  cvv    cligen variable vector, one for every wildchar in xkfmt
 * @param[out] xk     XML key, eg /aaa/17. Free after use
 * @note first and last elements of cvv are not used,..
 */ 
int
xmlkeyfmt2key(char  *xkfmt, 
	      cvec  *cvv, 
	      char **xk)
{
    int   retval = -1;
    char  c;
    int   esc=0;
    cbuf *cb = NULL;
    int   i;
    int   j;
    char *str;

    /* Sanity check */
    j = 0; /* Count % */
    for (i=0; i<strlen(xkfmt); i++)
	if (xkfmt[i] == '%')
	    j++;
    if (j+2 < cvec_len(cvv)) {
	clicon_log(LOG_WARNING, "%s xmlkey format string mismatch(j=%d, cvec_len=%d): %s", 
		   xkfmt, 
		   j,
		   cvec_len(cvv), 
		   cv_string_get(cvec_i(cvv, 0)));
	//	goto done;
    }
    if ((cb = cbuf_new()) == NULL)
	goto done;
    j = 1; /* j==0 is cli string */
    for (i=0; i<strlen(xkfmt); i++){
	c = xkfmt[i];
	if (esc){
	    esc = 0;
	    if (c!='s')
		continue;
	    if ((str = cv2str_dup(cvec_i(cvv, j++))) == NULL){
		clicon_err(OE_UNIX, errno, "strdup");
		goto done;
	    }
	    cprintf(cb, "%s", str);
	    free(str);
	}
	else
	    if (c == '%')
		esc++;
	    else
		cprintf(cb, "%c", c);
    }
    if ((*xk = strdup(cbuf_get(cb))) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

int
xmlkeyfmt2key2(char  *xkfmt, 
	      cvec  *cvv, 
	      char **xk)
{
    int   retval = -1;
    char  c;
    int   esc=0;
    cbuf *cb = NULL;
    int   i;
    int   j;
    char *str;

    /* Sanity check */
#if 1
    j = 0; /* Count % */
    for (i=0; i<strlen(xkfmt); i++)
	if (xkfmt[i] == '%')
	    j++;
    if (j < cvec_len(cvv)-1) {
	clicon_log(LOG_WARNING, "%s xmlkey format string mismatch(j=%d, cvec_len=%d): %s", 
		   xkfmt, 
		   j,
		   cvec_len(cvv), 
		   cv_string_get(cvec_i(cvv, 0)));
	//	goto done;
    }
#endif
    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "^");
    j = 1; /* j==0 is cli string */
    for (i=0; i<strlen(xkfmt); i++){
	c = xkfmt[i];
	if (esc){
	    esc = 0;
	    if (c!='s')
		continue;
	    if (j == cvec_len(cvv)){
		if ((str = strdup(".*")) == NULL){
		    clicon_err(OE_UNIX, errno, "strdup");
		    goto done;
		}
	    }
	    else
		if ((str = cv2str_dup(cvec_i(cvv, j++))) == NULL){
		    clicon_err(OE_UNIX, errno, "cv2str_dup");
		    goto done;
		}
	    cprintf(cb, "%s", str);
	    free(str);
	}
	else
	    if (c == '%')
		esc++;
	    else
		cprintf(cb, "%c", c);
    }
    cprintf(cb, "$");
    if ((*xk = strdup(cbuf_get(cb))) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}



/*! Help function to append key values from an xml list to a cbuf 
 * Example, a yang node x with keys a and b results in "x/a/b"
 */
static int
append_listkeys(cbuf      *ckey, 
		cxobj     *xt, 
		yang_stmt *ys)
{
    int        retval = -1;
    yang_stmt *ykey;
    cxobj     *xkey;
    cg_var    *cvi;
    cvec      *cvk = NULL; /* vector of index keys */
    char      *keyname;

    if ((ykey = yang_find((yang_node*)ys, Y_KEY, NULL)) == NULL){
	clicon_err(OE_XML, errno, "%s: List statement \"%s\" has no key", 
		   __FUNCTION__, ys->ys_argument);
	goto done;
    }
    /* The value is a list of keys: <key>[ <key>]*  */
    if ((cvk = yang_arg2cvec(ykey, " ")) == NULL)
	goto done;
    cvi = NULL;
    /* Iterate over individual keys  */
    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
	keyname = cv_string_get(cvi);
	if ((xkey = xml_find(xt, keyname)) == NULL){
	    clicon_err(OE_XML, errno, "XML list node \"%s\" does not have key \"%s\" child",
		       xml_name(xt), keyname);
	    goto done;
	}
	cprintf(ckey, "/%s", xml_body(xkey));
    }
    retval = 0;
 done:
    if (cvk)
	cvec_free(cvk);
    return retval;
}

/*! Help function to create xml key values 
 * @param[in]  x
 * @param[in]  y
 * @param[in]  keyname  yang key name
 */
static int
create_keyvalues(cxobj     *x, 
		 yang_stmt *y, 
		 yang_stmt *ykey, 
		 char      *name, 
		 char      *arg, 
		 char      *keyname,
		 cxobj    **xcp)
{
    int        retval = -1;
    cbuf      *cpath = NULL;
    cxobj     *xb;
    cxobj     *xc = NULL;

    if ((cpath = cbuf_new()) == NULL)
	goto done;
    cprintf(cpath, "%s[%s=%s]", name, keyname, arg);
    /* Check if key node exists */
    if ((xc = xpath_first(x, cbuf_get(cpath)))==NULL){
	if ((xc = xml_new_spec(name, x, y)) == NULL)
	    goto done;
	if ((xb = xml_new_spec(keyname, xc, ykey)) == NULL)
	    goto done;
	if ((xb = xml_new("body", xb)) == NULL)
	    goto done;
	xml_type_set(xb, CX_BODY);
	xml_value_set(xb, arg);
    }
    retval = 0;
 done:
    *xcp = xc;
    if (cpath)
	cbuf_free(cpath);
    return retval;
}


/*! Prune everything that has not been marked
 * @param[in]   xt      XML tree with some node marked
 * @param[out]  upmark  Set if a child (recursively) has marked set.
 * The function removes all branches that does not contain a marked child
 * XXX: maybe key leafs should not be purged if list is not purged?
 * XXX: consider move to clicon_xml
 */
static int
xml_tree_prune_unmarked(cxobj *xt, 
			int   *upmark)
{
    int    retval = -1;
    int    submark;
    int    mark;
    cxobj *x;
    cxobj *xprev;

    mark = 0;
    x = NULL;
    xprev = x = NULL;
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL) {
	if (xml_flag(x, XML_FLAG_MARK)){
	    mark++;
	    xprev = x;
	    continue; /* mark and stop here */
	}
	if (xml_tree_prune_unmarked(x, &submark) < 0)
	    goto done;
	if (submark)
	    mark++;
	else{ /* Safe with xml_child_each if last */
	    if (xml_prune(xt, x, 1) < 0)
		goto done;
	    x = xprev;
	}
	xprev = x;
    }
    retval = 0;
 done:
    if (upmark)
	*upmark = mark;
    return retval;
}

/*!
 * @param[in]   xkey xmlkey
 * @param[out]  xt   XML tree as result
 */
static int
get(char      *dbname,
    yang_spec *ys,
    char      *xk,
    char      *val,
    cxobj     *xt)
{
    int         retval = -1;
    char      **vec;
    int         nvec;
    int         i;
    char       *name;
    yang_stmt  *y;
    cxobj      *x;
    cxobj      *xc;
    cxobj      *xb;
    yang_stmt *ykey;
    cg_var    *cvi;
    cvec      *cvk = NULL; /* vector of index keys */
    char      *keyname;
    char      *arg;
    
    x = xt;
    if (xk == NULL || *xk!='/'){
	clicon_err(OE_DB, 0, "Invalid key: %s", xk);
	goto done;
    }
    if ((vec = clicon_strsplit(xk, "/", &nvec, __FUNCTION__)) == NULL)
	goto done;
    /* Element 0 is NULL '/', 
       Element 1 is top symbol and needs to find subs in all modules:
       spec->module->syntaxnode
     */
    if (nvec < 2){
	clicon_err(OE_XML, 0, "Malformed key: %s", xk);
	goto done;
    }
    name = vec[1];
    if ((y = yang_find_topnode(ys, name)) == NULL){
	clicon_err(OE_UNIX, errno, "No yang node found: %s", name);
	goto done;
    }
    if ((xc = xml_find(x, name))==NULL)
	if ((xc = xml_new_spec(name, x, y)) == NULL)
	    goto done;
    x = xc;
    i = 2;
    while (i<nvec){
	name = vec[i];
	if ((y = yang_find_syntax((yang_node*)y, name)) == NULL){
	    clicon_err(OE_UNIX, errno, "No yang node found: %s", name);
	    goto done;
	}
	switch (y->ys_keyword){
	case Y_LEAF_LIST:
	    /* 
	     * If xml element is a leaf-list, then the next element is expected to
	     * be a value
	     */
	    i++;
	    if (i>=nvec){
		clicon_err(OE_XML, errno, "Leaf-list %s without argument", name);
		goto done;
	    }
	    arg = vec[i];
	    if ((xc = xml_find(x, name))==NULL ||
		(xb = xml_find(xc, arg))==NULL){
		if ((xc = xml_new_spec(name, x, y)) == NULL)
		    goto done;
		/* Assume body is created at end of function */
	    }
	    break;
	case Y_LIST:
	    /* 
	     * If xml element is a list, then the next element(s) is expected to be
	     * a key value. Check if this key value is already in the xml tree,
	     * otherwise create it.
	     */
	    if ((ykey = yang_find((yang_node*)y, Y_KEY, NULL)) == NULL){
		clicon_err(OE_XML, errno, "%s: List statement \"%s\" has no key", 
			   __FUNCTION__, y->ys_argument);
		goto done;
	    }
	    /* The value is a list of keys: <key>[ <key>]*  */
	    if ((cvk = yang_arg2cvec(ykey, " ")) == NULL)
		goto done;
	    cvi = NULL;
	    /* Iterate over individual yang keys  */
	    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		keyname = cv_string_get(cvi);
		i++;
		if (i>=nvec){
		    clicon_err(OE_XML, errno, "List %s without argument", name);
		    goto done;
		}
		arg = vec[i];
		if (create_keyvalues(x, 
				     y, 
				     ykey,
				     name, 
				     arg, 
				     keyname, 
				     &xc) < 0)
		    goto done;
		x = xc;
	    } /* while */
	    break;
	default:
	    if ((xc = xml_find(x, name))==NULL)
		if ((xc = xml_new_spec(name, x, y)) == NULL)
		    goto done;
	    break;
	} /* switch */
	x = xc;
	i++;
    }
    if (val && xml_body(x)==NULL){
	if ((x = xml_new("body", x)) == NULL)
	    goto done;
	xml_type_set(x, CX_BODY);
	xml_value_set(x, val);
    }
    if(debug){
	fprintf(stderr, "%s %s\n", __FUNCTION__, xk);
	clicon_xml2file(stderr, xt, 0, 1);
    }
    retval = 0;
 done:
    unchunk_group(__FUNCTION__);  
    return retval;
}

/*! Sanitize an xml tree: xml node has matching yang_stmt pointer 
 */
static int
xml_sanity(cxobj *x, 
	   void  *arg)
{
    int        retval = -1;
    yang_stmt *ys;

    ys = (yang_stmt*)xml_spec(x);
    if (ys==NULL){
	clicon_err(OE_XML, 0, "No spec for xml node %s", xml_name(x));
	return -1;
    }
    if (strcmp(xml_name(x), ys->ys_argument)){
	clicon_err(OE_XML, 0, "xml node name '%s' does not match yang spec arg '%s'", 
		   xml_name(x), ys->ys_argument);
	return -1;
    }
    retval = 0;
    // done:
    return retval;
}

/*! Add default values (if not set)
 */
static int
xml_default(cxobj *x, 
	   void  *arg)
{
    int        retval = -1;
    yang_stmt *ys;
    yang_stmt *y;
    int        i;
    cxobj     *xc;
    cxobj     *xb;
    char      *str;

    ys = (yang_stmt*)xml_spec(x);
    /* Check leaf defaults */
    if (ys->ys_keyword == Y_CONTAINER || ys->ys_keyword == Y_LIST){
	for (i=0; i<ys->ys_len; i++){
	    y = ys->ys_stmt[i];
	    if (y->ys_keyword != Y_LEAF)
		continue;
	    assert(y->ys_cv);
	    if (!cv_flag(y->ys_cv, V_UNSET)){  /* Default value exists */
		if (!xml_find(x, y->ys_argument)){
		    if ((xc = xml_new_spec(y->ys_argument, x, y)) == NULL)
			goto done;
		    if ((xb = xml_new("body", xc)) == NULL)
			goto done;
		    xml_type_set(xb, CX_BODY);
		    if ((str = cv2str_dup(y->ys_cv)) == NULL){
			clicon_err(OE_UNIX, errno, "cv2str_dup");
			goto done;
		    }
		    if (xml_value_set(xb, str) < 0)
			goto done;
		    free(str);
		}
	    }
	}
    }
    retval = 0;
 done:
    return retval;
}


/*! Get content of database using xpath. return a single tree
 * The function returns a minimal tree that includes all sub-trees that match
 * xpath.
 * @param[in]  dbname Name of database to search in (filename including dir path
 * @param[in]  xpath  String with XPATH syntax (or NULL for all)
 * @param[in]  yspec  Yang specification
 * @param[out] xtop   XML tree. Freed by xml_free()
 * @retval     0      OK
 * @retval     -1     Error
 * @code
 *   cxobj     *xt;
 *   yang_spec *yspec = clicon_dbspec_yang(h);
 *   if (xmldb_get(dbname, "/interfaces/interface[name="eth*"]", yspec, &xt) < 0)
 *     err;
 *   xml_free(xt);
 * @endcode
 */
int
xmldb_get(char      *dbname, 
	  char      *xpath,
	  yang_spec *yspec,
	  cxobj    **xtop)
{
    int             retval = -1;
    int             i;
    int             npairs;
    struct db_pair *pairs;
    cxobj          *xt = NULL;
    cxobj         **xvec=NULL;
    size_t          len;

    /* Read in complete database (this can be optimized) */
    if ((npairs = db_regexp(dbname, "", __FUNCTION__, &pairs, 0)) < 0)
	goto done;
    if ((xt = xml_new("clicon", NULL)) == NULL)
	goto done;
    if (debug) /* debug */
	for (i = 0; i < npairs; i++) 
	    fprintf(stderr, "%s %s\n", pairs[i].dp_key, pairs[i].dp_val?pairs[i].dp_val:"");

    for (i = 0; i < npairs; i++) {
	if (get(dbname, 
		yspec, 
		pairs[i].dp_key, /* xml key */
		pairs[i].dp_val, /* may be NULL */
		xt) < 0)
	    goto done;
    }
    /*
     * 1. Read whole tree, call xpath, then retrace 'back'
     * 2. only read necessary parts,...?
     */
    if (xpath){
	if ((xvec = xpath_vec(xt, xpath, &len)) != NULL){
	    /* Prune everything except nodes in xvec and how to get there
	     * Alt: Create a new subtree from xt with that property,...(no)
	     */
	    for (i=0; i<len; i++)
		xml_flag_set(xvec[i], XML_FLAG_MARK);
	}
	if (xml_tree_prune_unmarked(xt, NULL) < 0)
	    goto done;
    }
    *xtop = xt;
    if (xml_apply(xt, CX_ELMNT, xml_default, NULL) < 0)
	goto done;
    if (1)
	if (xml_apply(xt, CX_ELMNT, xml_sanity, NULL) < 0)
	    goto done;
    if (0)
	clicon_xml2file(stdout, xt, 0, 1);
    retval = 0;
 done:
    if (retval < 0 && xt){
	xml_free(xt);
	*xtop = NULL;
    }
    if (xvec)
	free(xvec);
    unchunk_group(__FUNCTION__);
    return retval;
}

/*! Get content of database using xpath. return a set of matching sub-trees
 * The function returns a minimal tree that includes all sub-trees that match
 * xpath.
 * @param[in]  dbname Name of database to search in (filename including dir path
 * @param[in]  xpath  String with XPATH syntax (or NULL for all)
 * @param[in]  yspec  Yang specification
 * @param[out] xtop   Single XML tree which xvec points to. Free with xml_free()
 * @param[out] xvec   Vector of xml trees. Free after use
 * @param[out] xlen   Length of vector.
 * @retval     0      OK
 * @retval     -1     Error
 * @code
 *   cxobj   *xt;
 *   cxobj  **xvec;
 *   size_t   xlen;
 *   yang_spec *yspec = clicon_dbspec_yang(h);
 *   if (xmldb_get_vec(dbname, "/interfaces/interface[name="eth*"]", yspec, 
 *                     &xt, &xvec, &xlen) < 0)
 *      err;
 *   for (i=0; i<xlen; i++){
 *      xn = xv[i];
 *      ...
 *   }
 *   xml_free(xt);
 *   free(xvec);
 * @endcode
 * @see xpath_vec
 * @see xmldb_get
 */
int
xmldb_get_vec(char      *dbname, 
	      char      *xpath,
	      yang_spec *yspec,
	      cxobj    **xtop,
	      cxobj   ***xvec,
	      size_t    *xlen)
{
    int             retval = -1;
    int             i;
    int             npairs;
    struct db_pair *pairs;
    cxobj          *xt = NULL;

    /* Read in complete database (this can be optimized) */
    if ((npairs = db_regexp(dbname, "", __FUNCTION__, &pairs, 0)) < 0)
	goto done;
    if ((xt = xml_new("clicon", NULL)) == NULL)
	goto done;
    if (debug) /* debug */
	for (i = 0; i < npairs; i++) 
	    fprintf(stderr, "%s %s\n", pairs[i].dp_key, pairs[i].dp_val?pairs[i].dp_val:"");

    for (i = 0; i < npairs; i++) {
	if (get(dbname, 
		yspec, 
		pairs[i].dp_key, /* xml key */
		pairs[i].dp_val, /* may be NULL */
		xt) < 0)
	    goto done;
    }
    if ((*xvec = xpath_vec(xt, xpath, xlen)) == NULL)
	goto done;
    if (xml_apply(xt, CX_ELMNT, xml_default, NULL) < 0)
	goto done;
    if (1)
	if (xml_apply(xt, CX_ELMNT, xml_sanity, NULL) < 0)
	    goto done;
    if (0)
	clicon_xml2file(stdout, xt, 0, 1);
    retval = 0;
    *xtop = xt;
 done:
    unchunk_group(__FUNCTION__);
    return retval;
}

/*! Get value of the "operation" attribute and change op if given
 * @param[in]   xn  XML node
 * @param[out]  op  "operation" attribute may change operation
 */
static int
get_operation(cxobj               *xn, 
	      enum operation_type *op)
{
    char *opstr;

    if ((opstr = xml_find_value(xn, "operation")) != NULL){
	if (strcmp("merge", opstr) == 0)
	    *op = OP_MERGE;
	else
	if (strcmp("replace", opstr) == 0)
	    *op = OP_REPLACE;
	else
	if (strcmp("create", opstr) == 0)
	    *op = OP_CREATE;
	else
	if (strcmp("delete", opstr) == 0)
	    *op = OP_DELETE;
	else
	if (strcmp("remove", opstr) == 0)
	    *op = OP_REMOVE;
	else{
	    clicon_err(OE_XML, 0, "Bad-attribute operation: %s", opstr);
	    return -1;
	}
    }
    return 0;
}

/*! Add data to database internal recursive function
 * @param[in]  dbname Name of database to search in (filename incl dir path)
 * @param[in]  xt     xml-node.
 * @param[in]  ys     Yang statement corresponding to xml-node
 * @param[in]  op     OP_MERGE, OP_REPLACE, OP_REMOVE, etc 
 * @param[in]  xkey0  aggregated xmlkey
 * @retval     0      OK
 * @retval     -1     Error
 * @note XXX op only supports merge
 */
static int
put(char               *dbname, 
    cxobj              *xt,
    yang_stmt          *ys, 
    enum operation_type op, 
    const char         *xk0)
{
    int        retval = -1;
    cxobj     *x = NULL;
    char      *xk;
    cbuf      *cbxk = NULL;
    char      *body;
    yang_stmt *y;
    int        exists;

    if (get_operation(xt, &op) < 0)
	goto done;
    body = xml_body(xt);
    if ((cbxk = cbuf_new()) == NULL)
	goto done;
    cprintf(cbxk, "%s/%s", xk0, xml_name(xt));
    switch (ys->ys_keyword){
    case Y_LIST: /* Note: can have many keys */
	if (append_listkeys(cbxk, xt, ys) < 0)
	    goto done;
	break;
    case Y_LEAF_LIST:
	cprintf(cbxk, "/%s", body);
	break;
    default:
	break;
    }
    xk = cbuf_get(cbxk);
    //    fprintf(stderr, "%s %s\n", key, body?body:"");
    /* Write to database, key and a vector of variables */
    switch (op){
    case OP_CREATE:
	if ((exists = db_exists(dbname, xk)) < 0)
	    goto done;
	if (exists == 1){
	    clicon_err(OE_DB, 0, "OP_CREATE: %s already exists in database", xk);
	    goto done;
	}
    case OP_MERGE:
    case OP_REPLACE:
	if (db_set(dbname, xk, body?body:NULL, body?strlen(body)+1:0) < 0)
	    goto done;
	break;
    case OP_DELETE:
	if ((exists = db_exists(dbname, xk)) < 0)
	    goto done;
	if (exists == 0){
	    clicon_err(OE_DB, 0, "OP_DELETE: %s does not exists in database", xk);
	    goto done;
	}
    case OP_REMOVE:
	if (db_del(dbname, xk) < 0)
	    goto done;
	break;
    case OP_NONE:
	break;
    }
    /* For every node, create a key with values */
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL){
	if ((y = yang_find_syntax((yang_node*)ys, xml_name(x))) == NULL){
	    clicon_err(OE_UNIX, errno, "No yang node found: %s", xml_name(x));
	    goto done;
	}
	if (put(dbname, x, y, op, xk) < 0)
	    goto done;
    }
    retval = 0;
 done:
    if (cbxk)
	cbuf_free(cbxk);
    return retval;
}

/*! Modify database provided an xml tree and an operation
 * @param[in]  dbname Name of database to search in (filename including dir path)
 * @param[in]  xt     xml-tree. Top-level symbol is dummy
 * @param[in]  yspec  Yang specification
 * @param[in]  op     OP_MERGE: just add it. 
                      OP_REPLACE: first delete whole database
                      OP_NONE: operation attribute in xml determines operation
 * @retval     0      OK
 * @retval     -1     Error
 * The xml may contain the "operation" attribute which defines the operation.
 * @code
 *   cxobj     *xt;
 *   yang_spec *yspec = clicon_dbspec_yang(h);
 *   if (xmldb_put(dbname, xt, yspec, OP_MERGE) < 0)
 *     err;
 * @endcode
 */
int
xmldb_put(char               *dbname, 
	  cxobj              *xt,
	  yang_spec          *yspec, 
	  enum operation_type op) 
{
    int        retval = -1;
    cxobj     *x = NULL;
    yang_stmt *ys;

    if (op == OP_REPLACE){
	unlink(dbname);
	if (db_init(dbname) < 0) 
	    goto done;
    }
    while ((x = xml_child_each(xt, x, CX_ELMNT)) != NULL){
	if ((ys = yang_find_topnode(yspec, xml_name(x))) == NULL){
	    clicon_err(OE_UNIX, errno, "No yang node found: %s", xml_name(x));
	    goto done;
	}
    	if (put(dbname, /* database name */
		x,      /* xml root node */
		ys,     /* yang statement of xml node */
		op,     /* operation, eg merge/delete */
		""      /* aggregate xml key */
		) < 0)
    	    goto done;
    }
    retval = 0;
 done:
    return retval;
}


/*! Modify database provided an XML database key and an operation
 * @param[in]  dbname Name of database to search in (filename including dir path)
 * @param[in]  xk     XML Key, eg /aa/bb/17/name
 * @param[in]  val    Key value, eg "17"
 * @param[in]  yspec  Yang specification
 * @param[in]  op     OP_MERGE, OP_REPLACE, OP_REMOVE, etc
 * @retval     0      OK
 * @retval     -1     Error
 * @code
 *   yang_spec *yspec = clicon_dbspec_yang(h);
 *   if (xmldb_put_xkey(dbname, "/aa/bb/17/name", "17", yspec, OP_MERGE) < 0)
 *     err;
 * @endcode
 */
int
xmldb_put_xkey(char               *dbname, 
	       char               *xk,
	       char               *val,
	       yang_spec          *yspec, 
	       enum operation_type op) 
{
    int        retval = -1;
    cxobj     *x = NULL;
    yang_stmt *y = NULL;
    yang_stmt *ykey;
    char     **vec;
    int        nvec;
    int        i;
    char      *name;
    cg_var    *cvi;
    cvec      *cvk = NULL; /* vector of index keys */
    char      *val2;
    cbuf      *ckey=NULL; /* partial keys */
    cbuf      *csubkey=NULL; /* partial keys */
    cbuf      *crx=NULL; /* partial keys */
    char      *keyname;
    int        exists;
    int        npairs;
    struct db_pair *pairs;

    if (xk == NULL || *xk!='/'){
	clicon_err(OE_DB, 0, "Invalid key: %s", xk);
	goto done;
    }
    if ((ckey = cbuf_new()) == NULL)
	goto done;
    if ((csubkey = cbuf_new()) == NULL)
	goto done;
    if ((vec = clicon_strsplit(xk, "/", &nvec, __FUNCTION__)) == NULL)
	goto done;
    if (nvec < 2){
	clicon_err(OE_XML, 0, "Malformed key: %s", xk);
	goto done;
    }
    i = 1;
    while (i<nvec){
	name = vec[i];
	if (i==1){
	    if ((y = yang_find_topnode(yspec, name)) == NULL){
		clicon_err(OE_UNIX, errno, "No yang node found: %s", xml_name(x));
		goto done;
	    }
	}
	else
	    if ((y = yang_find_syntax((yang_node*)y, name)) == NULL){
		clicon_err(OE_UNIX, errno, "No yang node found: %s", name);
		goto done;
	    }
	if ((op==OP_DELETE || op == OP_REMOVE) &&
	    y->ys_keyword == Y_LEAF && 
	    y->ys_parent->yn_keyword == Y_LIST &&
	    yang_key_match(y->ys_parent, y->ys_argument))
	    /* Special rule if key, dont write last key-name, rm whole*/;
	else
	    cprintf(ckey, "/%s", name);
	i++;
	switch (y->ys_keyword){
	case Y_LEAF_LIST:
	    val2 = vec[i];
	    if (i>=nvec){
		clicon_err(OE_XML, errno, "Leaf-list %s without argument", name);
		goto done;
	    }
	    i++;
	    cprintf(ckey, "/%s", val2);
	    break;
	case Y_LIST:
	    if ((ykey = yang_find((yang_node*)y, Y_KEY, NULL)) == NULL){
		clicon_err(OE_XML, errno, "%s: List statement \"%s\" has no key", 
			   __FUNCTION__, y->ys_argument);
		goto done;
	    }
	    /* The value is a list of keys: <key>[ <key>]*  */
	    if ((cvk = yang_arg2cvec(ykey, " ")) == NULL)
		goto done;
	    cvi = NULL;
	    /* Iterate over individual yang keys  */
	    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		keyname = cv_string_get(cvi);
		val2 = vec[i++];
		if (i>=nvec){
		    clicon_err(OE_XML, errno, "List %s without argument", name);
		    goto done;
		}
		cprintf(ckey, "/%s", val2);
		cbuf_reset(csubkey);
		cprintf(csubkey, "%s/%s", cbuf_get(ckey), keyname);
		if (op == OP_MERGE || op == OP_REPLACE || op == OP_CREATE)
		    if (db_set(dbname, cbuf_get(csubkey), val2, strlen(val2)+1) < 0)
			goto done;
		break;
	    }
	default:
	    if (op == OP_MERGE || op == OP_REPLACE || op == OP_CREATE)
		if (db_set(dbname, cbuf_get(ckey), NULL, 0) < 0)
		    goto done;
	    break;
	}
    }
    xk = cbuf_get(ckey);
    /* final key */
    switch (op){
    case OP_CREATE:
	if ((exists = db_exists(dbname, xk)) < 0)
	    goto done;
	if (exists == 1){
	    clicon_err(OE_DB, 0, "OP_CREATE: %s already exists in database", xk);
	    goto done;
	}
    case OP_MERGE:
    case OP_REPLACE:
	if (y->ys_keyword == Y_LEAF || y->ys_keyword == Y_LEAF_LIST){
	    if (db_set(dbname, xk, val, strlen(val)+1) < 0)
		goto done;
	}
	else
	    if (db_set(dbname, xk, NULL, 0) < 0)
		goto done;
	break;
    case OP_DELETE:
	if ((exists = db_exists(dbname, xk)) < 0)
	    goto done;
	if (exists == 0){
	    clicon_err(OE_DB, 0, "OP_DELETE: %s does not exists in database", xk);
	    goto done;
	}
    case OP_REMOVE:
	/* Read in complete database (this can be optimized) */
	if ((crx = cbuf_new()) == NULL)
	    goto done;
	cprintf(crx, "^%s.*$", xk);
	if ((npairs = db_regexp(dbname, cbuf_get(crx), __FUNCTION__, &pairs, 0)) < 0)
	    goto done;
	for (i = 0; i < npairs; i++) {
	    if (db_del(dbname, pairs[i].dp_key) < 0)
		goto done;
	}

	break;
    default:
	break;
    }
    retval = 0;
 done:
    if (ckey)
	cbuf_free(ckey);
    if (csubkey)
	cbuf_free(csubkey);
    if (crx)
	cbuf_free(crx);
    unchunk_group(__FUNCTION__);  
    return retval;
}

#if 1 /* Test program */
/*
 * Turn this on to get an xpath test program 
 * Usage: clicon_xpath [<xpath>] 
 * read xml from input
 * Example compile:
 gcc -g -o xmldb -I. -I../clicon ./clicon_xmldb.c -lclicon -lcligen
*/

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:\n%s\tget <db> <yangdir> <yangmod> [<xpath>]\t\txml on stdin\n", argv0);
    fprintf(stderr, "\tput <db> <yangdir> <yangmod> set|merge|delete\txml to stdout\n");
    exit(0);
}

int
main(int argc, char **argv)
{
    cxobj      *xt;
    cxobj      *xn;
    char       *xpath;
    enum operation_type      op;
    char       *cmd;
    char       *db;
    char       *yangdir;
    char       *yangmod;
    yang_spec  *yspec = NULL;
    clicon_handle h;

    if ((h = clicon_handle_init()) == NULL)
	goto done;
    clicon_log_init("xmldb", LOG_DEBUG, CLICON_LOG_STDERR);
    if (argc < 4){
	usage(argv[0]);
	goto done;
    }
    cmd = argv[1];
    db = argv[2];
    yangdir = argv[3];
    yangmod = argv[4];
    db_init(db);
    if ((yspec = yspec_new()) == NULL)
	goto done;
    if (yang_parse(h, yangdir, yangmod, NULL, yspec) < 0)
	goto done;
    if (strcmp(cmd, "get")==0){
	if (argc < 5)
	    usage(argv[0]);
	xpath = argc>5?argv[5]:NULL;
	if (xmldb_get(db, xpath, yspec, &xt) < 0)
	    goto done;
	clicon_xml2file(stdout, xt, 0, 1);	
    }
    else
    if (strcmp(cmd, "put")==0){
	if (argc != 6)
	    usage(argv[0]);
	if (clicon_xml_parse_file(0, &xt, "</clicon>") < 0)
	    goto done;
	xn = xml_child_i(xt, 0);
	xml_prune(xt, xn, 0); /* kludge to remove top-level tag (eg top/clicon) */
	xml_parent_set(xn, NULL);
	xml_free(xt);
	if (strcmp(argv[5], "set") == 0)
	    op = OP_REPLACE;
	else 	
	    if (strcmp(argv[4], "merge") == 0)
	    op = OP_MERGE;
	else 	if (strcmp(argv[5], "delete") == 0)
	    op = OP_REMOVE;
	else
	    usage(argv[0]);
	if (xmldb_put(db, xn, yspec, op) < 0)
	    goto done;
    }
    else
	usage(argv[0]);
    printf("\n");
 done:
    return 0;
}

#endif /* Test program */
