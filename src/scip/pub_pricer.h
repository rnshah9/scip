/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2003 Tobias Achterberg                              */
/*                                                                           */
/*                  2002-2003 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the SCIP Academic Licence.        */
/*                                                                           */
/*  You should have received a copy of the SCIP Academic License             */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma ident "@(#) $Id: pub_pricer.h,v 1.1 2003/12/01 14:41:30 bzfpfend Exp $"

/**@file   pub_pricer.h
 * @brief  public methods for variable pricers
 * @author Tobias Achterberg
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __PUB_PRICER_H__
#define __PUB_PRICER_H__


#include "def.h"
#include "type_pricer.h"



/** gets user data of variable pricer */
extern
PRICERDATA* SCIPpricerGetData(
   PRICER*          pricer              /**< variable pricer */
   );

/** sets user data of variable pricer; user has to free old data in advance! */
extern
void SCIPpricerSetData(
   PRICER*          pricer,             /**< variable pricer */
   PRICERDATA*      pricerdata          /**< new variable pricer user data */
   );

/** gets name of variable pricer */
extern
const char* SCIPpricerGetName(
   PRICER*          pricer              /**< variable pricer */
   );

/** gets description of variable pricer */
extern
const char* SCIPpricerGetDesc(
   PRICER*          pricer              /**< variable pricer */
   );

/** gets priority of variable pricer */
extern
int SCIPpricerGetPriority(
   PRICER*          pricer              /**< variable pricer */
   );

/** gets the number of times, the pricer was called and tried to find a variable with negative reduced costs */
extern
int SCIPpricerGetNCalls(
   PRICER*          pricer              /**< variable pricer */
   );

/** gets the number of variables with negative reduced costs found by this pricer */
extern
int SCIPpricerGetNVarsFound(
   PRICER*          pricer              /**< variable pricer */
   );

/** is variable pricer initialized? */
extern
Bool SCIPpricerIsInitialized(
   PRICER*            pricer                /**< variable pricer */
   );

/** gets time in seconds used in this pricer */
extern
Real SCIPpricerGetTime(
   PRICER*            pricer                /**< variable pricer */
   );


#endif
