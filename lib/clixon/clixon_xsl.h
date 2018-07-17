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

 * XML XPATH and XSLT functions.
 */
#ifndef _CLIXON_XSL_H
#define _CLIXON_XSL_H

/*
 * Prototypes
 */
#if defined(__GNUC__) && __GNUC__ >= 3
cxobj *xpath_first_xsl(cxobj *cxtop, char *format, ...) __attribute__ ((format (printf, 2, 3)));
int xpath_vec_xsl(cxobj *cxtop, char *format, cxobj ***vec, size_t  *veclen, ...) __attribute__ ((format (printf, 2, 5)));
int xpath_vec_flag(cxobj *cxtop, char *format, uint16_t flags, 
		   cxobj ***vec, size_t *veclen, ...) __attribute__ ((format (printf, 2, 6)));
#else
cxobj *xpath_first_xsl(cxobj *cxtop, char *format, ...);
int xpath_vec_xsl(cxobj *cxtop, char *format, cxobj ***vec, size_t  *veclen, ...);
int xpath_vec_flag_xsl(cxobj *cxtop, char *xpath, uint16_t flags, 
		   cxobj ***vec, size_t *veclen, ...);
#endif
cxobj *xpath_each(cxobj *xn_top, char *xpath, cxobj *prev);


#endif /* _CLIXON_XSL_H */
