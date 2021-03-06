/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#ifndef _STRING
#define _STRING

#include "condor_header_features.h"
#include "util_lib_proto.h"

BEGIN_C_DECLS

DLL_IMPORT_MAGIC char* strupr( char *str );
DLL_IMPORT_MAGIC char* strlwr( char *str );

// Get an effective line, after removing line continuation characters and
// trimming leading and trailing whitespace. More than 1 line may be read
// from the file for each line returned here. if you need an accurate
// linecount, use the c++ callable function below.
// This function returns an internal buffer that my be safely edited by the caller
// it will be invalidated on the next call to getline_trim.
char * getline_trim ( FILE *fp );

char * chomp( char *buffer );

END_C_DECLS


#ifdef __cplusplus
/* like strdup() but uses new[] */
char *strnewp( const char * );

// Get an effective line, after removing line continuation characters and
// trimming leading and trailing whitespace. More than 1 line may be read
// from the file for each line returned here. if you need an accurate
// linecount, use the c++ callable function below.
// This function returns an internal buffer that my be safely edited by the caller
// it will be invalidated on the next call to getline_trim.
// if mode==GETLINE_TRIM_SIMPLE_CONTINUATION, then line continuation characters
// are interpreted without regard to comment lines that start with #
#define GETLINE_TRIM_SIMPLE_CONTINUATION 1
extern "C++" char * getline_trim ( FILE *fp, int & lineno, int mode=0 );

#include "condor_arglist.h"
#endif

#include "basename.h"

#endif /* _STRING */
