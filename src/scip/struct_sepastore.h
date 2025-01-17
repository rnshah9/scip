/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2022 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not visit scipopt.org.         */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   struct_sepastore.h
 * @ingroup INTERNALAPI
 * @brief  datastructures for storing separated cuts
 * @author Tobias Achterberg
 * @author Leona Gottwald
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __SCIP_STRUCT_SEPASTORE_H__
#define __SCIP_STRUCT_SEPASTORE_H__


#include "scip/def.h"
#include "scip/type_lp.h"
#include "scip/type_var.h"
#include "scip/type_sepastore.h"

#ifdef __cplusplus
extern "C" {
#endif

/** storage for separated cuts */
struct SCIP_SepaStore
{
   SCIP_ROW**            cuts;               /**< array with separated cuts sorted by score */
   SCIP_RANDNUMGEN*      randnumgen;         /**< random number generator used for tie breaking */
   int                   cutssize;           /**< size of cuts and score arrays */
   int                   ncuts;              /**< number of separated cuts (max. is set->sepa_maxcuts) */
   int                   nforcedcuts;        /**< number of forced separated cuts (first positions in cuts array) */
   int                   ncutsadded;         /**< total number of cuts added so far */
   int                   ncutsaddedviapool;  /**< total number of cuts added from cutpool */
   int                   ncutsaddeddirect;   /**< total number of cuts added directly */
   int                   ncutsfoundround;    /**< number of cuts found so far in this separation round */
   int                   ncutsapplied;       /**< total number of cuts applied to the LP */
   SCIP_Bool             initiallp;          /**< is the separation storage currently being filled with the initial LP rows? */
   SCIP_Bool             forcecuts;          /**< should the cuts be used despite the number of cuts parameter limit? */
};

#ifdef __cplusplus
}
#endif

#endif
