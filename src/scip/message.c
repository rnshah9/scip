/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2002 Tobias Achterberg                              */
/*                            Thorsten Koch                                  */
/*                            Alexander Martin                               */
/*                  2002-2002 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the SCIP Academic Licence.        */
/*                                                                           */
/*  You should have received a copy of the SCIP Academic License             */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   message.c
 * @brief  message output methods
 * @author Tobias Achterberg
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include <stdio.h>
#include <assert.h>

#include "message.h"


void errorMessage_call(                 /**< prints an error message */
   const char*      msg,                /**< message to print */
   const char*      filename,           /**< name of the file, where the error occured */
   int              line                /**< line of the file, where the error occured */
   )
{
   fprintf(stderr, "[%s:%d] ERROR: %s\n", filename, line, msg);
}

void todoMessage_call(                  /**< prints a todo message */
   const char*      msg,                /**< message to print */
   const char*      filename,           /**< name of the file, where the error occured */
   int              line                /**< line of the file, where the error occured */
   )
{
   fprintf(stderr, "[%s:%d] TODO: %s\n", filename, line, msg);
}

void infoMessage(                       /**< prints a message depending on the verbosity level */
   VERBLEVEL        verblevel,          /**< actual verbosity level */
   VERBLEVEL        msgverblevel,       /**< verbosity level of this message */
   const char*      msg                 /**< message to print */
   )
{
   assert(verblevel > SCIP_VERBLEVEL_NONE);
   assert(verblevel <= SCIP_VERBLEVEL_FULL);

   if( msgverblevel <= verblevel )
   {
      printf(msg);
      printf("\n");
   }
}

