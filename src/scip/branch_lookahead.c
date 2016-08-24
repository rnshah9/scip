/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2016 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#define SCIP_DEBUG
/**@file   branch_lookahead.c
 * @brief  lookahead branching rule
 * @author Christoph Schubert
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include "scip/branch_lookahead.h"

#include <assert.h>
#include <string.h>

#include "def.h"
#include "pub_branch.h"
#include "pub_message.h"
#include "pub_var.h"
#include "scip.h"
#include "type_branch.h"
#include "type_lp.h"
#include "type_result.h"
#include "type_retcode.h"
#include "type_scip.h"
#include "type_tree.h"
#include "type_var.h"

#define BRANCHRULE_NAME            "lookahead"
#define BRANCHRULE_DESC            "fullstrong branching with depth of 2" /* TODO CS: expand description */
#define BRANCHRULE_PRIORITY        536870911
#define BRANCHRULE_MAXDEPTH        -1
#define BRANCHRULE_MAXBOUNDDIST    1.0

/*
 * Data structures
 */

/* TODO: fill in the necessary branching rule data */

/** branching rule data */
struct SCIP_BranchruleData
{
   SCIP_Bool somerandomfield;
};

typedef struct
{
   SCIP_Real             highestweight;
   SCIP_Real             sumofweights;
   int                   numberofweights;
} WeightData;

typedef struct
{
   int                   varindex;
   int                   ncutoffs;
   WeightData            upperbounddata;
   WeightData            lowerbounddata;
} ScoreData;

typedef struct
{
   SCIP_Real             objval;
   SCIP_Bool             cutoff;
   SCIP_Bool             lperror;
   SCIP_Bool             nobranch;
} BranchingResultData;

/*
 * Local methods
 */
static
void initWeightData(
   WeightData*           weightdata
)
{
   weightdata->highestweight = 0;
   weightdata->numberofweights = 0;
   weightdata->sumofweights = 0;
}

static
void initScoreData(
   ScoreData*            scoredata,
   int                   currentbranchvar
)
{
   scoredata->ncutoffs = 0;
   scoredata->varindex = currentbranchvar;
   initWeightData(&scoredata->lowerbounddata);
   initWeightData(&scoredata->upperbounddata);
}

static
void initBranchingResultData(
   SCIP*                 scip,               /**< SCIP data structure */
   BranchingResultData*  resultdata
)
{
   resultdata->objval = SCIPinfinity(scip);
   resultdata->cutoff = TRUE;
   resultdata->lperror = FALSE;
   resultdata->nobranch = FALSE;
}

/**
 * Executes the branching on the current probing node by adding a probing node with a new upper bound.
 */
static
SCIP_RETCODE executeBranchingOnUpperBound(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_VAR*             branchvar,          /**< variable to branch on */
   SCIP_Real             branchvarsolval,    /**< current (fractional) solution value of the variable */
   BranchingResultData*  resultdata
   )
{
   SCIP_Real oldupperbound;
   SCIP_Real oldlowerbound;
   SCIP_Real newupperbound;
   SCIP_LPSOLSTAT solstat;

   assert(scip != NULL);
   assert(branchvar != NULL);
   assert(!SCIPisFeasIntegral(scip, branchvarsolval));
   assert(resultdata != NULL);

   newupperbound = SCIPfeasFloor(scip, branchvarsolval);
   oldupperbound = SCIPvarGetUbLocal(branchvar);
   oldlowerbound = SCIPvarGetLbLocal(branchvar);

   SCIPdebugMessage("New upper bound: <%g>, old lower bound: <%g>, old upper bound: <%g>\n", newupperbound, oldlowerbound,
      oldupperbound);

   if( SCIPisFeasLT(scip, newupperbound, oldlowerbound) )
   {
      resultdata->lperror = TRUE;
   }
   else
   {
      SCIP_CALL( SCIPnewProbingNode(scip) );
      if( SCIPisEQ(scip, oldupperbound, oldlowerbound) )
      {
         /* TODO: do smth with this info. */
         resultdata->nobranch = TRUE;
      }
      else if( SCIPisFeasLT(scip, newupperbound, oldupperbound) )
      {
         /* if the new upper bound is lesser than the old upper bound and also
          * greater than (or equal to) the old lower bound we set the new upper bound.
          * oldLowerBound <= newUpperBound < oldUpperBound */
         SCIP_CALL( SCIPchgVarUbProbing(scip, branchvar, newupperbound) );
      }

      SCIP_CALL( SCIPsolveProbingLP(scip, -1, &resultdata->lperror, &resultdata->cutoff) );
      solstat = SCIPgetLPSolstat(scip);

      resultdata->lperror = resultdata->lperror || (solstat == SCIP_LPSOLSTAT_NOTSOLVED && resultdata->cutoff == FALSE) ||
            (solstat == SCIP_LPSOLSTAT_ITERLIMIT) || (solstat == SCIP_LPSOLSTAT_TIMELIMIT);
      assert(solstat != SCIP_LPSOLSTAT_UNBOUNDEDRAY);

      if( !resultdata->lperror )
      {
         resultdata->objval = SCIPgetLPObjval(scip);
         resultdata->cutoff = resultdata->cutoff || SCIPisGE(scip, resultdata->objval, SCIPgetCutoffbound(scip));
         assert(((solstat != SCIP_LPSOLSTAT_INFEASIBLE) && (solstat != SCIP_LPSOLSTAT_OBJLIMIT)) || resultdata->cutoff);
      }
   }

   return SCIP_OKAY;
}

/**
 * Executes the branching on the current probing node by adding a probing node with a new lower bound.
 */
static
SCIP_RETCODE executeBranchingOnLowerBound(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_VAR*             branchvar,          /**< variable to branch on */
   SCIP_Real             branchvarsolval,    /**< current (fractional) solution value of the variable */
   BranchingResultData*  resultdata
   )
{
   SCIP_Real oldlowerbound;
   SCIP_Real oldupperbound;
   SCIP_Real newlowerbound;
   SCIP_LPSOLSTAT solstat;

   assert(scip != NULL );
   assert(branchvar != NULL );
   assert(!SCIPisFeasIntegral(scip, branchvarsolval));
   assert(resultdata != NULL );

   newlowerbound = SCIPfeasCeil(scip, branchvarsolval);
   oldlowerbound = SCIPvarGetLbLocal(branchvar);
   oldupperbound = SCIPvarGetUbLocal(branchvar);

   SCIPdebugMessage("New lower bound: <%g>, old lower bound: <%g>, old upper bound: <%g>\n", newlowerbound, oldlowerbound,
      oldupperbound);

   if( SCIPisFeasGT(scip, newlowerbound, oldupperbound) )
   {
      resultdata->cutoff = TRUE;
      resultdata->lperror = TRUE;
   }
   else
   {
      SCIP_CALL( SCIPnewProbingNode(scip) );
      if( SCIPisEQ(scip, oldupperbound, oldlowerbound) )
      {
         /* TODO: do smth with this info. */
         resultdata->nobranch = TRUE;
      }
      else if( SCIPisFeasGT(scip, newlowerbound, oldlowerbound) )
      {
         /* if the new lower bound is greater than the old lower bound and also
          * lesser than (or equal to) the old upper bound we set the new lower bound.
          * oldLowerBound < newLowerBound <= oldUpperBound */
         SCIP_CALL( SCIPchgVarLbProbing(scip, branchvar, newlowerbound) );
      }

      SCIP_CALL( SCIPsolveProbingLP(scip, -1, &resultdata->lperror, &resultdata->cutoff) );
      solstat = SCIPgetLPSolstat(scip);

      resultdata->lperror = resultdata->lperror || (solstat == SCIP_LPSOLSTAT_NOTSOLVED && resultdata->cutoff == FALSE) ||
            (solstat == SCIP_LPSOLSTAT_ITERLIMIT) || (solstat == SCIP_LPSOLSTAT_TIMELIMIT);
      assert(solstat != SCIP_LPSOLSTAT_UNBOUNDEDRAY);

      if( !resultdata->lperror )
      {
         resultdata->objval = SCIPgetLPObjval(scip);
         resultdata->cutoff = resultdata->cutoff || SCIPisGE(scip, resultdata->objval, SCIPgetCutoffbound(scip));
         assert(((solstat != SCIP_LPSOLSTAT_INFEASIBLE) && (solstat != SCIP_LPSOLSTAT_OBJLIMIT)) || resultdata->cutoff);
      }
   }

   return SCIP_OKAY;
}

/**
 * Returns TRUE, if a bound of the given type was not yet set.
 * Returns FALSE, otherwise.
 */
static
SCIP_Bool addBound(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_VAR*             branchvar,          /**< The variable for which a bound should be added. */
   SCIP_Real             newbound,           /**< The value of the new bound. */
   SCIP_Bool             keepminbound,       /**< In case there is already a bound for the variable, this Indicates whether
                                              *   the min or the max value of the new and the old bound should be kept. */
   BOUNDSTATUS           boundtype,          /**< The type of the new bound. Must be BOUNDSTATUS_UPPERBOUND or
                                              *   BOUNDSTATUS_LOWERBOUND. */
   SCIP_Real*            oldbound,           /**< Pointer to the old bound. Depending on the oldboundstatus this may contain
                                              *   no meaningful data. */
   BOUNDSTATUS*          oldboundstatus      /**< Pointer to the old boundstatus. */
   )
{
   SCIP_Bool newboundadded = FALSE;

   assert(scip != NULL);
   assert(branchvar != NULL);
   assert(boundtype == BOUNDSTATUS_LOWERBOUND || boundtype == BOUNDSTATUS_UPPERBOUND);
   assert(oldbound != NULL);
   assert(oldboundstatus != NULL);

   if( *oldboundstatus == boundtype || *oldboundstatus == BOUNDSTATUS_BOTH )
   {
      /* we already have a valid bound with a fitting type set, so we can take min/max of this and the "newbound. */

      SCIPdebugMessage("Updating an existent new bound. var <%s> type <%d> oldbound <%g> newbound <%g>.\n",
         SCIPvarGetName(branchvar), boundtype, *oldbound, newbound);
      if (keepminbound)
      {
         *oldbound = MIN(newbound, *oldbound);
      }
      else
      {
         *oldbound = MAX(newbound, *oldbound);
      }
   }
   else
   {
      /* We either have no new bounds or only a bound with the other type for our var, so we can set the new bound directly. */
      SCIPdebugMessage("Adding new bound. var <%s> type <%d> newbound <%g>.\n", SCIPvarGetName(branchvar), boundtype,
         newbound);
      *oldbound = newbound;

      if( *oldboundstatus == BOUNDSTATUS_NONE )
      {
         *oldboundstatus = boundtype;
         newboundadded = TRUE;
      }
      else
      {
         *oldboundstatus = BOUNDSTATUS_BOTH;
      }
   }
   return newboundadded;
}

static
void addValidUpperBound(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_VAR*             branchvar,
   SCIP_Real             newupperbound,
   ValidBoundData*       validbounds
)
{
   int varindex;
   SCIP_Real* oldupperbound;
   BOUNDSTATUS* oldboundstatus;
   SCIP_Bool newboundadded;

   assert(scip != NULL);
   assert(branchvar != NULL);
   assert(validbounds != NULL);

   varindex = SCIPvarGetProbindex( branchvar );
   oldupperbound = &validbounds->upperbounds[varindex];
   oldboundstatus = &validbounds->boundstatus[varindex];

   newboundadded = addBound(scip, branchvar, newupperbound, TRUE, BOUNDSTATUS_UPPERBOUND, oldupperbound, oldboundstatus);

   if( newboundadded )
   {
      int nboundedvars = validbounds->nboundedvars;
      validbounds->boundedvars[nboundedvars] = varindex;
      validbounds->nboundedvars = nboundedvars + 1;
   }
}

static
void addValidLowerBound(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_VAR*             branchvar,
   SCIP_Real             newlowerbound,
   ValidBoundData*       validbounds
)
{
   int varindex;
   SCIP_Real* oldlowerbound;
   BOUNDSTATUS* oldboundstatus;
   SCIP_Bool newboundadded;

   assert(scip != NULL);
   assert(branchvar != NULL);
   assert(validbounds != NULL);

   varindex = SCIPvarGetProbindex( branchvar );
   oldlowerbound = &validbounds->lowerbounds[varindex];
   oldboundstatus = &validbounds->boundstatus[varindex];

   newboundadded = addBound(scip, branchvar, newlowerbound, FALSE, BOUNDSTATUS_LOWERBOUND, oldlowerbound, oldboundstatus);

   if( newboundadded )
   {
      int nboundedvars = validbounds->nboundedvars;
      validbounds->boundedvars[nboundedvars] = varindex;
      validbounds->nboundedvars = nboundedvars + 1;
   }
}

static
void addSupposedUpperBound(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_VAR*             branchvar,          /**/
   SCIP_Real             newupperbound,      /**/
   SupposedBoundData*    supposedbounds      /**/
)
{
   int varindex;
   SCIP_Real* oldupperbound;
   int prevnupdated;
   BOUNDSTATUS oldboundstatus;
   SCIP_Bool newboundadded;

   assert(scip != NULL);
   assert(branchvar != NULL);
   assert(supposedbounds != NULL);

   varindex = SCIPvarGetProbindex( branchvar );
   oldupperbound = &supposedbounds->upperbounds[varindex];
   prevnupdated = supposedbounds->nupperboundupdates[varindex];
   oldboundstatus = prevnupdated > 0 ? BOUNDSTATUS_UPPERBOUND : BOUNDSTATUS_NONE;

   newboundadded = addBound(scip, branchvar, newupperbound, FALSE, BOUNDSTATUS_UPPERBOUND, oldupperbound, &oldboundstatus);

   supposedbounds->nupperboundupdates[varindex] = prevnupdated + 1;

   if( newboundadded )
   {
      int nboundedvars = supposedbounds->nboundedvars;
      supposedbounds->boundedvars[nboundedvars] = varindex;
      supposedbounds->nboundedvars = nboundedvars + 1;
   }
}

static
void addSupposedLowerBound(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_VAR*             branchvar,          /**/
   SCIP_Real             newlowerbound,      /**/
   SupposedBoundData*    supposedbounds      /**/
)
{
   int varindex;
   SCIP_Real* oldlowerbound;
   int prevnupdated;
   BOUNDSTATUS oldboundstatus;
   SCIP_Bool newboundadded;

   assert(scip != NULL);
   assert(branchvar != NULL);
   assert(supposedbounds != NULL);

   varindex = SCIPvarGetProbindex( branchvar );
   oldlowerbound = &supposedbounds->lowerbounds[varindex];
   prevnupdated = supposedbounds->nlowerboundupdates[varindex];
   oldboundstatus = prevnupdated > 0 ? BOUNDSTATUS_LOWERBOUND : BOUNDSTATUS_NONE;

   newboundadded = addBound(scip, branchvar, newlowerbound, TRUE, BOUNDSTATUS_LOWERBOUND, oldlowerbound, &oldboundstatus);

   supposedbounds->nlowerboundupdates[varindex] = prevnupdated + 1;

   if( newboundadded )
   {
      int nboundedvars = supposedbounds->nboundedvars;
      supposedbounds->boundedvars[nboundedvars] = varindex;
      supposedbounds->nboundedvars = nboundedvars + 1;
   }
}

static
SCIP_RETCODE calculateWeight(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_Real             upgain,
   SCIP_Real             downgain,
   SCIP_Real*            result
)
{
   SCIP_Real min;
   SCIP_Real max;
   SCIP_Real minweight = 4;
   SCIP_Real maxweight = 1;

   assert(scip != NULL);
   assert(result != NULL);

   min = MIN(downgain, upgain);
   max = MAX(upgain, downgain);

   *result = minweight * min + maxweight * max;

   SCIPdebugMessage("The calculated weight of <%g> and <%g> is <%g>.\n", upgain, downgain, *result);

   return SCIP_OKAY;
}

static
SCIP_RETCODE executeDeepBranchingOnVar(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_Real             lpobjval,           /**< objective value of the base lp */
   SCIP_VAR*             deepbranchvar,      /**< variable to branch up and down on */
   SCIP_Real             deepbranchvarsolval,/**< (fractional) solution value of the branching variable */
   SCIP_Bool*            fullcutoff,         /**< resulting decision whether this branch is cutoff */
   SCIP_Bool*            lperror,
   WeightData*           weightdata,         /**< container to be filled with the weight relevant data */
   int*                  ncutoffs,           /**< current (input) and resulting (output) number of cutoffs */
   SupposedBoundData*    supposedbounds
)
{
   BranchingResultData* downresultdata;
   BranchingResultData* upresultdata;
   SCIP_Real downgain;
   SCIP_Real upgain;
   SCIP_Real currentweight;

   assert(scip != NULL);
   assert(deepbranchvar != NULL);
   assert(ncutoffs != NULL);

   SCIP_CALL( SCIPallocBuffer(scip, &downresultdata) );
   SCIP_CALL( SCIPallocBuffer(scip, &upresultdata) );
   initBranchingResultData(scip, downresultdata);
   initBranchingResultData(scip, upresultdata);

   SCIPdebugMessage("Second level down branching on variable <%s>\n", SCIPvarGetName(deepbranchvar));
   SCIP_CALL( executeBranchingOnUpperBound(scip, deepbranchvar, deepbranchvarsolval, downresultdata) );

   if( downresultdata->lperror )
   {
      /* Something went wrong while solving the lp. Maybe exceeded the time-/iterlimit or we tried to add an upper
       * bound, which is lower than the current lower bound (may be the case if the lower bound was raised due to
       * propagation from other branches.) */
      *lperror = TRUE;
   }
   else
   {
      SCIPdebugMessage("Going back to layer 1.\n");
      /* go back one layer (we are currently in depth 2) */
      SCIP_CALL( SCIPbacktrackProbing(scip, 1) );

      SCIPdebugMessage("Second level up branching on variable <%s>\n", SCIPvarGetName(deepbranchvar));
      SCIP_CALL( executeBranchingOnLowerBound(scip, deepbranchvar, deepbranchvarsolval, upresultdata) );

      if( upresultdata->lperror )
      {
         /* Something went wrong while solving the lp. Maybe exceeded the time-/iterlimit or we tried to add an upper
          * bound, which is lower than the current lower bound (may be the case if the lower bound was raised due to
          * propagation from other branches.) */
         *lperror = TRUE;
      }
      else
      {
         SCIPdebugMessage("Going back to layer 1.\n");
         /* go back one layer (we are currently in depth 2) */
         SCIP_CALL( SCIPbacktrackProbing(scip, 1) );

         if( !downresultdata->cutoff && !upresultdata->cutoff )
         {
            downgain = downresultdata->objval - lpobjval;
            upgain = upresultdata->objval - lpobjval;

            SCIPdebugMessage("The difference between the objective values of the base lp and the upper bounded lp is <%g>\n",
               downgain);
            SCIPdebugMessage("The difference between the objective values of the base lp and the lower bounded lp is <%g>\n",
               upgain);

            assert(!SCIPisFeasNegative(scip, downgain));
            assert(!SCIPisFeasNegative(scip, upgain));

            SCIP_CALL( calculateWeight(scip, upgain, downgain, &currentweight) );

            weightdata->highestweight = MAX(weightdata->highestweight, currentweight);
            weightdata->sumofweights = weightdata->sumofweights + currentweight;
            weightdata->numberofweights++;

            SCIPdebugMessage("The sum of weights is <%g>.\n", weightdata->sumofweights);
            SCIPdebugMessage("The number of weights is <%i>.\n", weightdata->numberofweights);
            *fullcutoff = FALSE;
         }
         else if( downresultdata->cutoff && upresultdata->cutoff )
         {
            *fullcutoff = TRUE;
            *ncutoffs = *ncutoffs + 2;
         }
         else
         {
            *fullcutoff = FALSE;
            *ncutoffs = *ncutoffs + 1;

            if( upresultdata->cutoff )
            {
               /* TODO CS: add the binary grand child implications here */
               addSupposedUpperBound(scip, deepbranchvar, deepbranchvarsolval, supposedbounds);
            }
            if( downresultdata->cutoff )
            {
               /* TODO CS: add the binary grand child implications here */
               addSupposedLowerBound(scip, deepbranchvar, deepbranchvarsolval, supposedbounds);
            }
         }

      }
   }

   SCIPfreeBuffer(scip, &upresultdata);
   SCIPfreeBuffer(scip, &downresultdata);

   return SCIP_OKAY;
}

static
SCIP_RETCODE executeDeepBranching(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_Real             lpobjval,           /**< objective value of the base lp */
   SCIP_Bool*            fullcutoff,         /**< resulting decision whether this branch is cutoff */
   SCIP_Bool*            lperror,
   WeightData*           weightdata,
   int*                  ncutoffs,
   SupposedBoundData*    supposedbounds
)
{
   SCIP_VAR**  lpcands;
   SCIP_Real*  lpcandssol;
   int         nlpcands;
   int         j;

   assert(scip != NULL);
   assert(ncutoffs != NULL);

   SCIP_CALL( SCIPgetLPBranchCands(scip, &lpcands, &lpcandssol, NULL, &nlpcands, NULL, NULL) );

   SCIPdebugMessage("The deeper lp has <%i> variables with fractional value.\n", nlpcands);

   for( j = 0; j < nlpcands; j++ )
   {
      SCIP_VAR* deepbranchvar = lpcands[j];
      SCIP_Real deepbranchvarsolval = lpcandssol[j];

      SCIPdebugMessage("Start deeper branching on variable <%s> with solution value <%g>.\n",
         SCIPvarGetName(deepbranchvar), deepbranchvarsolval);

      SCIP_CALL( executeDeepBranchingOnVar(scip, lpobjval, deepbranchvar, deepbranchvarsolval,
         fullcutoff, lperror, weightdata, ncutoffs, supposedbounds) );

      if( *fullcutoff )
      {
         SCIPdebugMessage("The deeper lp on variable <%s> is cutoff, as both lps are cutoff.\n",
            SCIPvarGetName(deepbranchvar));
         break;
      }
   }

   return SCIP_OKAY;
}

static
SCIP_RETCODE calculateAverageWeight(
   SCIP*                 scip,               /**< SCIP data structure */
   WeightData            weightdata,         /**< calculation data for the average weight */
   SCIP_Real*            averageweight       /**< resulting average weight */
)
{
   assert(scip != NULL);
   assert(!SCIPisFeasNegative(scip, weightdata.sumofweights));
   assert(weightdata.numberofweights >= 0);
   assert(averageweight != NULL);

   if( weightdata.numberofweights > 0 )
   {
      *averageweight = (1 / weightdata.numberofweights) * weightdata.sumofweights;
   }
   else
   {
      *averageweight = 0;
   }
   return SCIP_OKAY;
}

static
SCIP_RETCODE calculateCurrentWeight(
   SCIP*                 scip,               /**< SCIP data structure */
   ScoreData             scoredata,
   SCIP_Real*            highestweight,
   int*                  highestweightindex
)
{
   SCIP_Real averageweightupperbound = 0;
   SCIP_Real averageweightlowerbound = 0;
   SCIP_Real lambda;
   SCIP_Real totalweight;

   assert(scip != NULL);
   assert(!SCIPisFeasNegative(scip, scoredata.upperbounddata.highestweight));
   assert(!SCIPisFeasNegative(scip, scoredata.lowerbounddata.highestweight));
   assert(!SCIPisFeasNegative(scip, scoredata.ncutoffs));
   assert(highestweight != NULL);
   assert(highestweightindex != NULL);

   SCIP_CALL( calculateAverageWeight(scip, scoredata.upperbounddata, &averageweightupperbound) );
   SCIP_CALL( calculateAverageWeight(scip, scoredata.lowerbounddata, &averageweightlowerbound) );
   lambda = averageweightupperbound + averageweightlowerbound;

   assert(!SCIPisFeasNegative(scip, lambda));

   SCIPdebugMessage("The lambda value is <%g>.\n", lambda);

   totalweight = scoredata.lowerbounddata.highestweight + scoredata.upperbounddata.highestweight + scoredata.ncutoffs;
   if( SCIPisFeasGT(scip, totalweight, *highestweight) )
   {
      *highestweight = totalweight;
      *highestweightindex = scoredata.varindex;
   }
   return SCIP_OKAY;
}

static
SCIP_RETCODE handleNewBounds(
   SCIP*                 scip,               /**< SCIP data structure */
   ValidBoundData*       validbounds,        /**< The struct containing all bounds that should be added. */
   SCIP_RESULT*          result              /**< Pointer to the result flag. Used to set the correct flags. */
)
{
   int i;
   int nboundedvars;
   SCIP_VAR** probvars;
   int nboundsadded = 0;

   assert(scip != NULL);
   assert(validbounds != NULL);
   assert(result != NULL);

   nboundedvars = validbounds->nboundedvars;
   probvars = SCIPgetVars(scip);

   /* Instead of iterating over all problem vars, we iterate only over those, that have a new bound set. */
   SCIPdebugMessage("Trying to add <%d> variable bounds.\n", nboundedvars);
   for( i = 0; i < nboundedvars && *result != SCIP_CUTOFF; i++ )
   {
      int probvarindex;
      BOUNDSTATUS status;
      SCIP_VAR* branchvar;

      /* The iteration index is only valid as an index for the boundedvars array. Every other array in validbounds is
       * indexed by the probindex. */
      probvarindex = validbounds->boundedvars[i];
      status = validbounds->boundstatus[probvarindex];
      branchvar = probvars[probvarindex];

      /* Handle the new lower bound (if present) */
      if( status == BOUNDSTATUS_LOWERBOUND || status == BOUNDSTATUS_BOTH )
      {
         SCIP_Bool infeasible;
         SCIP_Bool tightened;
         SCIP_Real oldlowerbound;
         SCIP_Real proposedlowerbound;
         SCIP_Real newlowerbound;

         oldlowerbound = SCIPvarGetLbLocal(branchvar);
         proposedlowerbound = validbounds->lowerbounds[probvarindex];

         SCIP_CALL( SCIPtightenVarLb(scip, branchvar, proposedlowerbound, FALSE, &infeasible, &tightened) );

         newlowerbound = SCIPvarGetLbLocal(branchvar);
         SCIPdebugMessage("Variable <%s>, old lower bound <%g>, proposed lower bound <%g>, new lower bound <%g>\n",
            SCIPvarGetName(branchvar), oldlowerbound, proposedlowerbound, newlowerbound);

         if( infeasible )
         {
            *result = SCIP_CUTOFF;
            SCIPdebugMessage("The lower bound of variable <%s> could not be tightened.\n", SCIPvarGetName(branchvar));
         }
         else if( tightened )
         {
            *result = SCIP_REDUCEDDOM;
            SCIPdebugMessage("The lower bound of variable <%s> was successfully tightened.\n", SCIPvarGetName(branchvar));
            nboundsadded++;
         }
      }

      /* Handle the new upper bound (if present) */
      if( *result != SCIP_CUTOFF && (status == BOUNDSTATUS_UPPERBOUND || status == BOUNDSTATUS_BOTH) )
      {
         SCIP_Bool infeasible;
         SCIP_Bool tightened;
         SCIP_Real oldupperbound;
         SCIP_Real proposedupperbound;
         SCIP_Real newupperbound;

         oldupperbound = SCIPvarGetUbLocal(branchvar);
         proposedupperbound = validbounds->upperbounds[probvarindex];

         SCIP_CALL( SCIPtightenVarUb(scip, branchvar, proposedupperbound, FALSE, &infeasible, &tightened) );

         newupperbound = SCIPvarGetUbLocal(branchvar);
         SCIPdebugMessage("Variable <%s>, old upper bound <%g>, proposed upper bound <%g>, new upper bound <%g>\n",
            SCIPvarGetName(branchvar), oldupperbound, proposedupperbound, newupperbound);

         if( infeasible )
         {
            *result = SCIP_CUTOFF;
            SCIPdebugMessage("The upper bound of variable <%s> could not be tightened.\n", SCIPvarGetName(branchvar));
         }
         else if( tightened )
         {
            *result = SCIP_REDUCEDDOM;
            SCIPdebugMessage("The upper bound of variable <%s> was successfully tightened.\n", SCIPvarGetName(branchvar));
            nboundsadded++;
         }
      }

      if( status != BOUNDSTATUS_NONE )
      {
         /* Reset the array s.t. it only contains zero values. Necessary for the CleanBufferArray usage. */
         validbounds->boundstatus[probvarindex] = BOUNDSTATUS_NONE;
      }
   }

   SCIPdebugMessage("Added <%d> bounds to the problem.\n", nboundsadded);

   return SCIP_OKAY;
}

static
void transferBoundData(
   SCIP*                 scip,               /**< SCIP data structure */
   SupposedBoundData*    supposedbounds,     /**< Bound data from the second level branches. Source for the transfer. */
   ValidBoundData*       validbounds         /**< Bound data from the first level branches. Target for the transfer. */
)
{
   int i;
   int nofsecondlevelbounds = 0;
   SCIP_VAR** problemvars;

   problemvars = SCIPgetVars(scip);

   SCIPdebugMessage("Transferring implicit bound data to the valid bound data.\n");
   SCIPdebugMessage("Number of entries <%d>\n", supposedbounds->nboundedvars);
   for(i = 0; i < supposedbounds->nboundedvars; i++ )
   {
      int boundedvarindex = supposedbounds->boundedvars[i];
      SCIP_VAR* boundedvar = problemvars[boundedvarindex];
      int nupperboundupdates = supposedbounds->nupperboundupdates[boundedvarindex];
      int nlowerboundupdates = supposedbounds->nlowerboundupdates[boundedvarindex];

      SCIPdebugMessage("Var: <%s>, nupperboundupdates: <%d>, nlowerboundupdates: <%d>\n", SCIPvarGetName(boundedvar),
         nupperboundupdates, nlowerboundupdates);

      /* add the supposed lower bounds only, if they were updated 2 times (once for each first level branch side) */
      if( nlowerboundupdates == 2 )
      {
         SCIP_Real lowerbound = supposedbounds->lowerbounds[boundedvarindex];

         SCIPdebugMessage("Adding second level lower bound for variable <%s>. Lower bound: <%g>\n",
            SCIPvarGetName(boundedvar), lowerbound);
         addValidLowerBound(scip, boundedvar, lowerbound, validbounds);
         nofsecondlevelbounds++;
      }

      /* add the supposed upper bounds only, if they were updated 2 times (once for each first level branch side) */
      if( nupperboundupdates == 2 )
      {
         SCIP_Real upperbound = supposedbounds->upperbounds[boundedvarindex];

         SCIPdebugMessage("Adding second level upper bound for variable <%s>. Upper bound: <%g>\n",
            SCIPvarGetName(boundedvar), upperbound);
         addValidUpperBound(scip, boundedvar, upperbound, validbounds);
         nofsecondlevelbounds++;
      }

      /* clean up afterwards, to reuse the same data structure for the next first level branching. */
      supposedbounds->nupperboundupdates[boundedvarindex] = 0;
      supposedbounds->nlowerboundupdates[boundedvarindex] = 0;
   }

   SCIPdebugMessage("Added <%d> bounds from the second level.\n", nofsecondlevelbounds);

}

static
SCIP_RETCODE selectVarLookaheadBranching(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_VAR**            lpcands,            /**< array of fractional variables */
   SCIP_Real*            lpcandssol,         /**< array of fractional solution values */
   int                   nlpcands,           /**< number of fractional variables/solution values */
   int*                  bestcand,           /**< calculated index of the branching variable */
   SCIP_RESULT*          result              /**< pointer to store results of branching */){

   assert(scip != NULL);
   assert(lpcands != NULL);
   assert(lpcandssol != NULL);
   assert(bestcand != NULL);
   assert(result != NULL);

   if( nlpcands == 1)
   {
      /* if there is only one branching variable we can directly branch there */
      *bestcand = 0;
      return SCIP_OKAY;
   }

   /* we need to branch at least 2 steps deep */
   if( SCIPgetDepthLimit(scip) <= (SCIPgetDepth(scip) + 2) )
   {
      SCIPdebugMessage("Cannot perform probing in selectVarLookaheadBranching, depth limit reached.\n");
      *result = SCIP_DIDNOTRUN;
      return SCIP_OKAY;
   }

   if( nlpcands > 1 )
   {
      /* declare all variables */
      BranchingResultData* downbranchingresult;
      BranchingResultData* upbranchingresult;
      ScoreData* scoredata;

      SCIP_Real lpobjval;
      SCIP_Real highestscore = 0;
      int highestscoreindex = -1;
      int i;

      ValidBoundData* validbounds;
      SupposedBoundData* supposedbounds;

      /* allocate all structs */
      SCIP_CALL( SCIPallocBuffer(scip, &downbranchingresult) );
      SCIP_CALL( SCIPallocBuffer(scip, &upbranchingresult) );
      SCIP_CALL( SCIPallocBuffer(scip, &scoredata) );

      SCIP_CALL( allocValidBoundData(scip, &validbounds) );
      SCIP_CALL( allocSupposedBoundData(scip, &supposedbounds) );

      /* init all structs */
      initBranchingResultData(scip, downbranchingresult);
      initBranchingResultData(scip, upbranchingresult);
      initValidBoundData(validbounds);

      lpobjval = SCIPgetLPObjval(scip);

      SCIPdebugMessage("The objective value of the base lp is <%g>.\n", lpobjval);

      SCIP_CALL( SCIPstartProbing(scip) );
      SCIPdebugMessage("Start Probing Mode\n");

      for( i = 0; i < nlpcands && !downbranchingresult->lperror && !upbranchingresult->lperror && !SCIPisStopped(scip); i++ )
      {
         SCIP_VAR* branchvar;
         SCIP_Real branchval;

         assert(lpcands[i] != NULL);

         initSupposedBoundData(supposedbounds);
         initScoreData(scoredata, i);

         branchvar = lpcands[i];
         branchval = lpcandssol[i];

         SCIPdebugMessage("Start branching on variable <%s>\n", SCIPvarGetName(branchvar));
         SCIPdebugMessage("First level down branching on variable <%s>\n", SCIPvarGetName(branchvar));
         SCIP_CALL( executeBranchingOnUpperBound(scip, branchvar, branchval, downbranchingresult) );

         if( !downbranchingresult->lperror && !downbranchingresult->cutoff )
         {
            SCIP_CALL( executeDeepBranching(scip, lpobjval,
               &downbranchingresult->cutoff, &downbranchingresult->lperror, &scoredata->upperbounddata,
               &scoredata->ncutoffs, supposedbounds) );
         }
         if( downbranchingresult->lperror )
         {
            SCIPdebugMessage("There occurred an error while solving an lp of the upper bounded branch.\n");
            break;
         }

         SCIPdebugMessage("Going back to layer 0.\n");
         SCIP_CALL( SCIPbacktrackProbing(scip, 0) );

         SCIPdebugMessage("First Level up branching on variable <%s>\n", SCIPvarGetName(branchvar));
         SCIP_CALL( executeBranchingOnLowerBound(scip, branchvar, branchval, upbranchingresult) );

         if( ! upbranchingresult->lperror && !upbranchingresult->cutoff )
         {
            SCIP_CALL( executeDeepBranching(scip, lpobjval,
               &upbranchingresult->cutoff, &upbranchingresult->lperror, &scoredata->lowerbounddata, &scoredata->ncutoffs,
               supposedbounds) );
         }
         if( upbranchingresult->lperror )
         {
            SCIPdebugMessage("There occurred an error while solving an lp of the lower bounded branch.\n");
            break;
         }

         SCIPdebugMessage("Going back to layer 0.\n");
         SCIP_CALL( SCIPbacktrackProbing(scip, 0) );

         transferBoundData(scip, supposedbounds, validbounds);

         if( upbranchingresult->cutoff && downbranchingresult->cutoff )
         {
            *result = SCIP_CUTOFF;
            SCIPdebugMessage(" -> variable <%s> is infeasible in both directions\n", SCIPvarGetName(branchvar));
            break;
         }
         else if( upbranchingresult->cutoff )
         {
            addValidUpperBound(scip, branchvar, branchval, validbounds);
         }
         else if( downbranchingresult->cutoff )
         {
            addValidLowerBound(scip, branchvar, branchval, validbounds);
         }
         else
         {
            SCIP_CALL( calculateCurrentWeight(scip, *scoredata, &highestscore, &highestscoreindex) );
         }
      }

      SCIPdebugMessage("End Probing Mode\n");
      SCIP_CALL( SCIPendProbing(scip) );

      if( downbranchingresult->lperror || upbranchingresult->lperror )
      {
         *result = SCIP_DIDNOTFIND;
      }
      else if( *result != SCIP_CUTOFF )
      {
         SCIP_CALL( handleNewBounds(scip, validbounds, result) );
      }

      freeSupposedBoundData(scip, &supposedbounds);
      freeValidBoundData(scip, &validbounds);

      SCIPfreeBuffer(scip, &scoredata);
      SCIPfreeBuffer(scip, &upbranchingresult);
      SCIPfreeBuffer(scip, &downbranchingresult);

      if( highestscoreindex != -1 )
      {
         *bestcand = highestscoreindex;
      }

   }

   return SCIP_OKAY;
}

/*
 * Callback methods of branching rule
 */


/** copy method for branchrule plugins (called when SCIP copies plugins) */
static
SCIP_DECL_BRANCHCOPY(branchCopyLookahead)
{  /*lint --e{715}*/
   assert(scip != NULL);
   assert(branchrule != NULL);
   assert(strcmp(SCIPbranchruleGetName(branchrule), BRANCHRULE_NAME) == 0);

   SCIP_CALL( SCIPincludeBranchruleLookahead(scip) );

   return SCIP_OKAY;
}

/** destructor of branching rule to free user data (called when SCIP is exiting) */
static
SCIP_DECL_BRANCHFREE(branchFreeLookahead)
{  /*lint --e{715}*/
   SCIP_BRANCHRULEDATA* branchruledata;

   /* free branching rule data */
   branchruledata = SCIPbranchruleGetData(branchrule);
   assert(branchruledata != NULL);

   SCIPfreeMemory(scip, &branchruledata);
   SCIPbranchruleSetData(branchrule, NULL);

   return SCIP_OKAY;
}


/** initialization method of branching rule (called after problem was transformed) */
static
SCIP_DECL_BRANCHINIT(branchInitLookahead)
{  /*lint --e{715}*/
   return SCIP_OKAY;
}


/** deinitialization method of branching rule (called before transformed problem is freed) */
static
SCIP_DECL_BRANCHEXIT(branchExitLookahead)
{  /*lint --e{715}*/
   return SCIP_OKAY;
}


/** branching execution method for fractional LP solutions */
static
SCIP_DECL_BRANCHEXECLP(branchExeclpLookahead)
{  /*lint --e{715}*/
   SCIP_VAR** tmplpcands;
   SCIP_VAR** lpcands;
   SCIP_Real* tmplpcandssol;
   SCIP_Real* lpcandssol;
   SCIP_Real* tmplpcandsfrac;
   SCIP_Real* lpcandsfrac;
   int nlpcands;
   int npriolpcands;
   int bestcand = -1;

   SCIPdebugMessage("Entering branchExeclpLookahead.\n");

   assert(branchrule != NULL);
   assert(strcmp(SCIPbranchruleGetName(branchrule), BRANCHRULE_NAME) == 0);
   assert(scip != NULL);
   assert(result != NULL);

   *result = SCIP_DIDNOTRUN;

   /* get branching candidates */
   SCIP_CALL( SCIPgetLPBranchCands(scip, &tmplpcands, &tmplpcandssol, &tmplpcandsfrac, &nlpcands, &npriolpcands, NULL) );
   assert(nlpcands > 0);
   assert(npriolpcands > 0);

   /* copy LP banching candidates and solution values, because they will be updated w.r.t. the strong branching LP
    * solution during the second level branchings */
   SCIP_CALL( SCIPduplicateBufferArray(scip, &lpcands, tmplpcands, nlpcands) );
   SCIP_CALL( SCIPduplicateBufferArray(scip, &lpcandssol, tmplpcandssol, nlpcands) );
   SCIP_CALL( SCIPduplicateBufferArray(scip, &lpcandsfrac, tmplpcandsfrac, nlpcands) );

   SCIPdebugMessage("The base lp has <%i> variables with fractional value.\n", nlpcands);

   SCIP_CALL( selectVarLookaheadBranching(scip, lpcands, lpcandssol, nlpcands, &bestcand, result) );

   if( *result != SCIP_CUTOFF && *result != SCIP_REDUCEDDOM && (0 <= bestcand && bestcand < nlpcands) )
   {
      SCIP_NODE* downchild = NULL;
      SCIP_NODE* upchild = NULL;
      SCIP_VAR* var;
      SCIP_Real val;

      assert(*result == SCIP_DIDNOTRUN);

      var = lpcands[bestcand];
      val = lpcandssol[bestcand];

      SCIPdebugMessage(" -> %d candidates, selected candidate %d: variable <%s> (solval=%g)\n",
         nlpcands, bestcand, SCIPvarGetName(var), val);
      SCIP_CALL( SCIPbranchVarVal(scip, var, val, &downchild, NULL, &upchild) );

      assert(downchild != NULL);
      assert(upchild != NULL);

      SCIPdebugMessage("Branched on variable <%s>\n", SCIPvarGetName(var));
      *result = SCIP_BRANCHED;
   }
   else if( *result == SCIP_REDUCEDDOM)
   {
      SCIPdebugMessage("Finished LookaheadBranching by reducing domains.\n");
   }
   else if( *result == SCIP_CUTOFF )
   {
      SCIPdebugMessage("Finished LookaheadBranching by cutting of, as the current problem is infeasible.\n");
   }
   else
   {
      SCIPdebugMessage("Could not find any variable to branch on.\n");
   }

   SCIPfreeBufferArray(scip, &lpcandsfrac);
   SCIPfreeBufferArray(scip, &lpcandssol);
   SCIPfreeBufferArray(scip, &lpcands);

   SCIPdebugMessage("Exiting branchExeclpLookahead.\n");

   return SCIP_OKAY;
}

/*
 * branching rule specific interface methods
 */

/** creates the lookahead branching rule and includes it in SCIP */
SCIP_RETCODE SCIPincludeBranchruleLookahead(
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_BRANCHRULEDATA* branchruledata;
   SCIP_BRANCHRULE* branchrule;

   /* create lookahead branching rule data */
   SCIP_CALL( SCIPallocMemory(scip, &branchruledata) );
   /* TODO: (optional) create branching rule specific data here */

   /* include branching rule */
   SCIP_CALL( SCIPincludeBranchruleBasic(scip, &branchrule, BRANCHRULE_NAME, BRANCHRULE_DESC, BRANCHRULE_PRIORITY,
         BRANCHRULE_MAXDEPTH, BRANCHRULE_MAXBOUNDDIST, branchruledata) );

   assert(branchrule != NULL);

   /* set non fundamental callbacks via setter functions */
   SCIP_CALL( SCIPsetBranchruleCopy(scip, branchrule, branchCopyLookahead) );
   SCIP_CALL( SCIPsetBranchruleFree(scip, branchrule, branchFreeLookahead) );
   SCIP_CALL( SCIPsetBranchruleInit(scip, branchrule, branchInitLookahead) );
   SCIP_CALL( SCIPsetBranchruleExit(scip, branchrule, branchExitLookahead) );
   SCIP_CALL( SCIPsetBranchruleExecLp(scip, branchrule, branchExeclpLookahead) );

   /* add lookahead branching rule parameters */

   return SCIP_OKAY;
}
