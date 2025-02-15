/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 * Restconf method implementation  
 */

#ifndef _RESTCONF_METHODS_H_
#define _RESTCONF_METHODS_H_

/*
 * Prototypes
 */
int api_data_options(clicon_handle h, void *req);
int api_data_write(clicon_handle h, void *req, char *api_path0,
                   int pi,
                   cvec *qvec, char *data,
                   int pretty, restconf_media media_in, restconf_media media_out,
                   int plain_patch, ietf_ds_t ds);

int api_data_put(clicon_handle h, void *req, char *api_path,
                 int pi,
                 cvec *qvec, char *data,
                 int pretty, restconf_media media_out, ietf_ds_t ds);

int api_data_patch(clicon_handle h, void *req, char *api_path,
                   int pi,
                   cvec *qvec, char *data, int pretty,
                   restconf_media media_out, ietf_ds_t ds);

int api_data_delete(clicon_handle h, void *req, char *api_path, int pi,
                    int pretty, restconf_media media_out, ietf_ds_t ds);

#endif /* _RESTCONF_METHODS_H_ */
