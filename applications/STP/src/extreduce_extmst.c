/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2019 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not visit scip.zib.de.         */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   extreduce_extmst.c
 * @brief  extended-reduction specific MST algorithms for Steiner tree problems
 * @author Daniel Rehfeldt
 *
 * This file implements MST algorithms for extended reduction techniques for Steiner problems.
 * Allows to efficiently compute and store special distance (SD) MSTs between the leaves of extension tree.
 * Furthermore, one can check for tree bottlenecks.
 *
 * A 'level' of the extension tree consists of all possible extension edges from the leaf used for extension.
 * For each level there are a number of 'components': all the subsets that were not already ruled-out.
 * Once a level is initiated, all SDs to the other leaves of the tree are computed ('vertical'),
 * as well as the SDs among the level ('horizontal').
 * These SDs are kept until the level has been removed again.
 * Furthermore, for each level of the tree we store two SD MSTs, namely:
 *   1. the MST corresponding to the extension tree without the level and without the tree node at which the level
 *      is rooted: 'msts_levelbase'
 *   2. the MST corresponding to the component of this level in the current tree: "msts_comp'
 *
 * A list of all interface methods can be found in extreduce.h.
 *
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/
// #define SCIP_DEBUG
//#define STP_DEBUG_EXT

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "graph.h"
#include "portab.h"
#include "extreduce.h"

#define EXT_PC_SDMAXVISITS 20  /**< maximum visits for PC specific SD computation */
#define EXT_DOUBLESD_ALWAYS

typedef struct mst_extension_tree_component
{
   const CSR*            mst_parent;         /**< parent MST (for which to use vertical SDs) */
   CSR*                  mst_new;            /**< new MST (out) */
   const int*            comp_nodes;         /**< nodes of component */
   int                   comp_vert;          /**< current vertex */
   int                   comp_extnode;       /**< component node from which we extended; or -1 */
   int                   comp_level;         /**< level of component */
   int                   comp_size;          /**< size of component */
   SCIP_Bool             isExtended;         /**< mst_new already extended? */
} MSTXCOMP;


/** returns special distance computed only for PC and for current leaf */
static inline
void extGetSdPcUpdate(
   const GRAPH*          g,                  /**< graph data structure */
   const PCDATA*         pcdata,             /**< PC data */
   int                   vertex1,            /**< second vertex */
   int                   vertex2,            /**< second vertex */
   SCIP_Real*            sd                  /**< special distance */
)
{
   const SCIP_Real sdpc = pcdata->pcSdToNode[vertex2];

   assert(graph_pc_isPcMw(g));
   assert(pcdata->pcSdStart == vertex1);
   assert(EQ(sdpc, -1.0) || GE(sdpc, 0.0));

   if( sdpc > -0.5 && (sdpc < *sd || *sd < -0.5) )
   {
      SCIPdebugMessage("special distance update for pc: %f to %f \n", *sd, sdpc);
      *sd = sdpc;
   }
}


/** Returns special distance.
 *  Checks normal distance from vertex2 to vertex1 if no opposite distance is known. */
static inline
SCIP_Real extGetSdDouble(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          g,                  /**< graph data structure */
   int                   vertex1,            /**< first vertex */
   int                   vertex2,            /**< second vertex */
   EXTDATA*              extdata             /**< extension data */
)
{
   SCIP_Real sd = extreduce_distDataGetSdDouble(scip, g, vertex1, vertex2, extdata->distdata);
   const PCDATA* const pcdata = extdata->pcdata;

   assert((pcdata->pcSdToNode != NULL) == graph_pc_isPcMw(g));

   if( pcdata->pcSdToNode )
   {
      extGetSdPcUpdate(g, pcdata, vertex1, vertex2, &sd);
   }

   assert(SCIPisEQ(scip, sd, -1.0) || SCIPisGE(scip, sd, 0.0));

   return sd;
}


/** Returns special distance.
 *  Only checks normal distance from vertex1 to vertex2. */
static inline
SCIP_Real extGetSd(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          g,                  /**< graph data structure */
   int                   vertex1,            /**< first vertex */
   int                   vertex2,            /**< second vertex */
   EXTDATA*              extdata             /**< extension data */
)
{
#ifdef EXT_DOUBLESD_ALWAYS
   return extGetSdDouble(scip, g, vertex1, vertex2, extdata);
#else
   SCIP_Real sd = extreduce_distDataGetSd(scip, g, vertex1, vertex2, extdata->distdata);
   const PCDATA* const pcdata = extdata->pcdata;

   assert((pcdata->pcSdToNode != NULL) == graph_pc_isPcMw(g));

   if( pcdata->pcSdToNode )
   {
      extGetSdPcUpdate(g, pcdata, vertex1, vertex2, &sd);
   }

   assert(SCIPisEQ(scip, sd, -1.0) || SCIPisGE(scip, sd, 0.0));

   return sd;
#endif
}


/** returns position of last marked component */
static inline
int extStackGetLastMarked(
   const EXTDATA*        extdata             /**< extension data */
)
{
   const int* const extstack_state = extdata->extstack_state;
   int stackpos = extStackGetPosition(extdata);

   while( extstack_state[stackpos] != EXT_STATE_MARKED )
   {
      stackpos--;
      assert(stackpos >= 0);
   }

   return stackpos;
}


/** returns size of top component on the stack */
static inline
int extStackGetTopSize(
   const EXTDATA*        extdata             /**< extension data */
)
{
   const int stackpos = extStackGetPosition(extdata);
   const int* const stack_start = extdata->extstack_start;
   const int size = stack_start[stackpos + 1] - stack_start[stackpos];

   assert(extdata->extstack_state[stackpos] != EXT_STATE_NONE);
   assert(size > 0 && size < STP_EXT_MAXGRAD);

   return size;
}


/** returns number of ancestor leaves (i.e. number of leaves below current level) */
static inline
int extGetNancestorLeaves(
   const EXTDATA*        extdata             /**< extension data */
)
{
   int nleaves_ancestors;

   if( extIsAtInitialComp(extdata) )
   {
      nleaves_ancestors = 1;
   }
   else
   {
      const int compsize = extStackGetTopSize(extdata);
      const int nleaves = extdata->tree_nleaves;
      nleaves_ancestors = nleaves - compsize;
   }

   assert(nleaves_ancestors > 0 && nleaves_ancestors < extdata->tree_nleaves);

   return nleaves_ancestors;
}


/** Repeatedly extends MST 'new', starting from MST 'parent' (in 'mstextcomp').
 *  In first call 'new' is extended from 'parent', afterwards from itself */
static inline
void mstExtend(
   SCIP*                 scip,               /**< SCIP */
   const SCIP_Real       adjcosts[],         /**< adjacency costs */
   DCMST*                dcmst,              /**< DCMST */
   MSTXCOMP*             mstextcomp          /**< extension component (in/out) */
)
{
   const CSR* mst_parent = mstextcomp->mst_parent;
   CSR* mst_new = mstextcomp->mst_new;

   /* first time we want to extend the MST? */
   if( !mstextcomp->isExtended )
   {
      mst_new->nnodes = mst_parent->nnodes + 1;
      mst_new->nedges_max = mst_parent->nedges_max + 2;
      reduce_dcmstAddNode(scip, mst_parent, adjcosts, dcmst, mst_new);

      mstextcomp->isExtended = TRUE;
   }
   else
   {
      reduce_dcmstAddNodeInplace(scip, adjcosts, dcmst, mst_new);
   }

   assert(mst_new->nnodes >= mst_parent->nnodes + 1);
}


/** fills MST adjacency costs for new vertex in */
static inline
void baseMstGetAdjcosts(
   const REDDATA*        reddata,            /**< reduction data */
   MSTXCOMP*             mstextcomp,         /**< extension component (in/out) */
   SCIP_Real             adjcosts[]          /**< adjacency costs (out) */
)
{
   const MLDISTS* const sds_vertical = reddata->sds_vertical;
   const MLDISTS* const sds_horizontal = reddata->sds_horizontal;
   const CSR* const mst_parent = mstextcomp->mst_parent;
   const int comp_vert = mstextcomp->comp_vert;
   const int comp_extnode = mstextcomp->comp_extnode;
   const int comp_level = mstextcomp->comp_level;
   const int comp_size = mstextcomp->comp_size;
   const int* const comp_nodes = mstextcomp->comp_nodes;
   const int nnodes_parent = mst_parent->nnodes;
   int adjpos = nnodes_parent;

   const SCIP_Real* const adjcosts_ancestors = extreduce_mldistsTargetDists(sds_vertical, comp_level, comp_vert);

   memcpy(adjcosts, adjcosts_ancestors, nnodes_parent * sizeof(adjcosts[0]));

   /* compute adjacent costs to left siblings of 'compvert' */
   for( int j = 0; j < comp_size; j++ )
   {
      const int sibling = comp_nodes[j];

      if( sibling == comp_vert )
      {
         adjcosts[adjpos] = FARAWAY;
         break;
      }

      if( sibling == comp_extnode )
      {
         continue;
      }

      adjcosts[adjpos++] = extreduce_mldistsTargetDist(sds_horizontal, comp_level, comp_vert, sibling);
   }
}


/** Gets nodes of parent component ordered according to their position in the
 *  tree leaves array. */
static inline
void baseMstGetOrderedParentNodes(
   const GRAPH*          graph,              /**< graph */
   const EXTDATA*        extdata,            /**< extension data */
   int*                  parentcomp_size,    /**< size (number of nodes) of parent component */
   int                   parentcomp_nodes[]  /**< ordered nodes of parent component */
)
{
   int leavespos[STP_EXT_MAXGRAD];
   const int* const extstack_data = extdata->extstack_data;
   const int stackpos_parent = extStackGetLastMarked(extdata);
   const int parentedges_start = extStackGetOutEdgesStart(extdata, stackpos_parent);
   const int parentedges_end = extStackGetOutEdgesEnd(extdata, stackpos_parent);
   int compsize = 0;

   assert(parentedges_start < parentedges_end);

   for( int i = parentedges_start; i != parentedges_end; i++ )
   {
      const int edge = extstack_data[i];
      const int compvert = graph->head[edge];

      assert(compsize < STP_EXT_MAXGRAD);
      assert(edge >= 0 && edge < graph->edges);

      parentcomp_nodes[compsize] = compvert;
      leavespos[compsize] = extLeafFindPos(extdata, compvert);

      assert(leavespos[compsize] > 0);

      compsize++;
   }

   assert(compsize > 0);
   assert(compsize == extreduce_extStackCompNOutedges(extdata, stackpos_parent));

   *parentcomp_size = compsize;

   SCIPsortIntInt(leavespos, parentcomp_nodes, compsize);

}


/** initializes base MST data for old and new MSTs */
static inline
void baseMstInitMsts(
   const EXTDATA*        extdata,            /**< extension data */
   REDDATA*              reddata,            /**< reduction data (in/out) */
   CSR*                  mst_parent,         /**< parent MST (out) */
   CSR*                  mst_new             /**< new MST (out) */
)
{
   CSRDEPO* const msts_levelbase = reddata->msts_levelbase;
   const int nleaves = extdata->tree_nleaves;
   const int nnodes_new = nleaves - 1;

#ifndef NDEBUG
   const MLDISTS* const sds_vertical = reddata->sds_vertical;
   const int level_parent = extreduce_mldistsTopLevel(sds_vertical) - 1;
   const int stackpos_parent = extStackGetLastMarked(extdata);
#endif

   /* get the previous levelbase MST */
   graph_csrdepo_getTopCSR(msts_levelbase, mst_parent);

   assert(nnodes_new >= 1 && stackpos_parent >= 0);
   assert(mst_parent->nnodes == extreduce_mldistsLevelNTargets(sds_vertical, level_parent));
   assert(mst_parent->nnodes == nleaves - extreduce_extStackCompNOutedges(extdata, stackpos_parent));

   SCIPdebugMessage("got MST level parent with n=%d, m=%d \n", mst_parent->nnodes, mst_parent->nedges_max);

   /* get space for the new MST */
   graph_csrdepo_addEmptyTopTree(msts_levelbase, nnodes_new);
   graph_csrdepo_getEmptyTop(msts_levelbase, mst_new);
}


/** (partially) initializes base MST data */
static inline
void baseMstInitExtComp(
   const REDDATA*        reddata,            /**< reduction data */
   int                   extnode,            /**< node from which we extended */
   const CSR*            mst_parent,         /**< parent MST */
   CSR*                  mst_new,            /**< new MST (in) */
   MSTXCOMP*             mstextcomp          /**< extension component (out) */
)
{
   const MLDISTS* const sds_vertical = reddata->sds_vertical;
   const int parentcomp_level = extreduce_mldistsTopLevel(sds_vertical) - 1;

   mstextcomp->mst_parent = mst_parent;
   mstextcomp->mst_new = mst_new;
   mstextcomp->comp_nodes = NULL;
   mstextcomp->comp_vert = -1;
   mstextcomp->comp_extnode = extnode;
   mstextcomp->comp_level = parentcomp_level;
   mstextcomp->comp_size = -1;
   mstextcomp->isExtended = FALSE;
}


/** extends parent base MST to obtain current one */
static inline
void baseMstBuildNew(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph */
   REDDATA*              reddata,            /**< reduction data */
   EXTDATA*              extdata,            /**< extension data */
   MSTXCOMP*             mstextcomp          /**< extension component (in/out) */
)
{
   int parentcomp_nodes[STP_EXT_MAXGRAD];
   DCMST* const dcmst = reddata->dcmst;
   SCIP_Real* const adjcosts = reduce_dcmstGetAdjcostBuffer(dcmst);
   const int extnode = mstextcomp->comp_extnode;
   int parentcomp_size = -1;

#ifndef NDEBUG
   const int nnodes_new = extdata->tree_nleaves - 1;
   int nextnode_hits = 0;
#endif

   assert(!mstextcomp->isExtended);
   assert(mstextcomp->comp_vert == -1);
   assert(mstextcomp->comp_size == -1);
   assert(mstextcomp->comp_nodes == NULL);

   /* It is necessary to have the parent nodes ordered, because the internal leaves ordering might
    * have changed since the creation of the parent component. The internal order will not change
    * anymore, though, for the extension trees build from here */
   baseMstGetOrderedParentNodes(graph, extdata, &parentcomp_size, parentcomp_nodes);

   assert(parentcomp_size >= 0 && parentcomp_size < STP_EXT_MAXGRAD);

   mstextcomp->comp_size = parentcomp_size;
   mstextcomp->comp_nodes = parentcomp_nodes;

   /* build 'mst_new' from 'mst_parent' by adding all siblings of 'extnode' */
   for( int i = 0; i < parentcomp_size; i++ )
   {
      const int parentcomp_vert = parentcomp_nodes[i];

      if( parentcomp_vert != extnode )
      {
         mstextcomp->comp_vert = parentcomp_vert;

         baseMstGetAdjcosts(reddata, mstextcomp, adjcosts);

         mstExtend(scip, adjcosts, dcmst, mstextcomp);
      }
#ifndef NDEBUG
      else
         nextnode_hits++;
#endif
   }

   if( !mstextcomp->isExtended )
   {
      CSR* mst_new = mstextcomp->mst_new;
      const CSR* mst_parent = mstextcomp->mst_parent;

      assert(nnodes_new == mstextcomp->mst_parent->nnodes);
      graph_csr_copy(mst_parent, mst_new);
   }

   assert(nnodes_new == mstextcomp->mst_new->nnodes);
   assert(nextnode_hits == 1);
}


/** finalizes base MST built */
static inline
void baseMstFinalizeNew(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph */
   const MSTXCOMP*       mstextcomp,         /**< extension component */
   REDDATA*              reddata,            /**< reduction data */
   EXTDATA*              extdata             /**< extension data */
)
{
   CSRDEPO* const msts_levelbase = reddata->msts_levelbase;

   graph_csrdepo_emptyTopSetMarked(msts_levelbase);


#if defined(STP_DEBUG_EXT) && defined(SCIP_DEBUG)
   graph_csrdepo_print(msts_levelbase);

   printf("---parent: \n");
   graph_csr_print(mstextcomp->mst_parent);
   printf("---new: \n");
   graph_csr_print(mstextcomp->mst_new);
#endif

   assert(extreduce_mstTopLevelBaseObjValid(scip, graph, mstextcomp->comp_extnode, extdata));

#ifdef SCIP_DEBUG
   SCIPdebugMessage("add MST level with n=%d, m=%d \n", mstextcomp->mst_new->nnodes, mstextcomp->mst_new->nedges_max);
   SCIPdebugMessage("weight of levelbase new MST: %f \n", reduce_dcmstGetWeight(scip, mstextcomp->mst_new));
#endif
}


/** (partially) initializes component extension MST data */
static inline
void compMstInitExtComp(
   const GRAPH*          graph,              /**< the graph */
   const EXTDATA*        extdata,            /**< extension data */
   const CSR*            mst_base,           /**< levelbase MST */
   CSR*                  mst_new,            /**< new MST (in) */
   MSTXCOMP*             mstextcomp          /**< extension component (out) */
)
{
   const int topcompsize = extStackGetTopSize(extdata);

   assert(extreduce_mldistsTopLevel(extdata->reddata->sds_vertical) == extdata->tree_depth);

   mstextcomp->mst_parent = mst_base;
   mstextcomp->mst_new = mst_new;
   mstextcomp->comp_nodes = NULL;
   mstextcomp->comp_vert = -1;
   mstextcomp->comp_extnode = -1;
   mstextcomp->comp_level = extdata->tree_depth;
   mstextcomp->comp_size = topcompsize;
   mstextcomp->isExtended = FALSE;
}


/** initializes component MSTs (current component MST and previous levelbase) */
static inline
void compMstInitMsts(
   EXTDATA*              extdata,            /**< extension data */
   CSR*                  mst_base,           /**< the base (out) */
   CSR*                  mst_new             /**< new MST (out) */
)
{
   REDDATA* const reddata = extdata->reddata;
   CSRDEPO* const msts_comp = reddata->msts_comp;
   const CSRDEPO* const msts_levelbase = reddata->msts_levelbase;
   const int nleaves = extdata->tree_nleaves;
   const int nnodes_new = nleaves;

   assert(graph_csrdepo_getNcsrs(msts_comp) == (graph_csrdepo_getNcsrs(msts_levelbase) - 1));

   graph_csrdepo_getTopCSR(msts_levelbase, mst_base);

#ifndef NDEBUG
   if( !extIsAtInitialComp(extdata) || extInitialCompIsEdge(extdata) )
   {
      const MLDISTS* const sds_vertical = reddata->sds_vertical;
      const int nnodes_base = mst_base->nnodes;

      assert(nnodes_base == nleaves - extStackGetTopSize(extdata));
      assert(nnodes_base == extreduce_mldistsLevelNTopTargets(sds_vertical));
   }
#endif

   /* get space for the new MST */
   graph_csrdepo_addEmptyTopTree(msts_comp, nnodes_new);
   graph_csrdepo_getEmptyTop(msts_comp, mst_new);
}


/** finalizes component MST */
static inline
void compMstFinalizeNew(
   const MSTXCOMP*       mstextcomp,         /**< extension component */
   SCIP_Bool             deletemst,          /**< delete the MST? */
   EXTDATA*              extdata             /**< extension data */
)
{
   REDDATA* const reddata = extdata->reddata;
   CSRDEPO* const msts_comp = reddata->msts_comp;

#ifdef SCIP_DEBUG
   CSR* mst_new = mstextcomp->mst_new;
#endif

   if( deletemst )
   {
      graph_csrdepo_removeTop(msts_comp);

      return;
   }

   graph_csrdepo_emptyTopSetMarked(msts_comp);

#if defined(STP_DEBUG_EXT) && defined(SCIP_DEBUG)
   graph_csrdepo_print(reddata->msts_comp);

   printf("---parent: \n");
   graph_csr_print(mstextcomp->mst_parent);
   printf("---new: \n");
   graph_csr_print(mstextcomp->mst_new);
#endif

#ifdef SCIP_DEBUG
   SCIPdebugMessage("added MST component with n=%d, m=%d \n", mst_new->nnodes, mst_new->nedges_max);
#endif
}


/** is given SD non-trivial? */
static inline
SCIP_Real sdIsNonTrivial(
   SCIP_Real             specialDist         /**< SD */
  )
{
   assert(specialDist >= 0 || EQ(specialDist, -1.0));
   assert(LT(specialDist, FARAWAY));

   return (specialDist >= -0.5);
}


/** marks single PcSd array entry */
static inline
void pcSdMarkSingle(
   const GRAPH*          graph,              /**< graph data structure */
   int                   entry,              /**< entry to mark */
   SCIP_Real             value,              /**< value to mark with */
   SCIP_Real*            pcSdToNode,         /**< node mark array */
   int*                  pcSdCands,          /**< marked candidates list */
   int*                  nPcSdCands          /**< pointer to store number of candidates */
)
{
   /* entry not marked yet? */
   if( pcSdToNode[entry] < -0.5 )
   {
      assert(EQ(pcSdToNode[entry], -1.0));
      assert(*nPcSdCands < graph->knots);

      pcSdCands[(*nPcSdCands)++] = entry;
      pcSdToNode[entry] = value;
   }
   else if( value < pcSdToNode[entry] )
   {
      pcSdToNode[entry] = value;
   }

   assert(GE(pcSdToNode[entry], 0.0));
}


/** marks PcSd array */
static
void pcSdToNodeMark(
   const GRAPH*          graph,              /**< graph data structure */
   int                   startvertex,        /**< vertex to start from */
   EXTDATA*              extdata             /**< extension data */
   )
{
   PCDATA* const pcdata = extdata->pcdata;
   SCIP_Real* const pcSdToNode = pcdata->pcSdToNode;
   int* const pcSdCands = pcdata->pcSdCands;
   const DCSR* const dcsr = graph->dcsr_storage;
   const RANGE* const range_csr = dcsr->range;
   const int* const head_csr = dcsr->head;
   const SCIP_Real* const cost_csr = dcsr->cost;
   const SCIP_Real* const prize = graph->prize;
   const int* const tree_deg = extdata->tree_deg;
   const int start = range_csr[startvertex].start;
   const int end = range_csr[startvertex].end;
   int count1 = 0;
   int count2 = 0;

   assert(graph_pc_isPcMw(graph));
   assert(pcSdCands && pcSdToNode && prize);
   assert(startvertex >= 0 && startvertex < graph->knots);
   assert(pcdata->nPcSdCands == -1);
   assert(pcdata->pcSdStart == -1);

#ifndef NDEBUG
   pcdata->pcSdStart = startvertex;
#endif

   pcdata->nPcSdCands = 0;

   for( int i = start; i != end; i++ )
   {
      const SCIP_Real edgecost = cost_csr[i];
      const int head = head_csr[i];
      assert(tree_deg[head] >= 0);

      if( tree_deg[head] == 0 )
      {
         const int start2 = range_csr[head].start;
         const int end2 = range_csr[head].end;

         for( int i2 = start2; i2 != end2; i2++ )
         {
            const int head2 = head_csr[i2];
            assert(tree_deg[head2] >= 0);

            /* tree reached? */
            if( tree_deg[head2] > 0 && head2 != startvertex )
            {
               const SCIP_Real edgecost2 = cost_csr[i2];
               const SCIP_Real maxedgecost = MAX(edgecost, edgecost2);
               SCIP_Real dist2 = MAX(maxedgecost, edgecost + edgecost2 - prize[head]);

               assert(0.0 == prize[head] || Is_term(graph->term[head]));

               pcSdMarkSingle(graph, head2, dist2, pcSdToNode, pcSdCands, &(pcdata->nPcSdCands));
            }

            if( count2++ > EXT_PC_SDMAXVISITS )
               break;
         }
      }
      else
      {
         assert(head != startvertex);
         pcSdMarkSingle(graph, head, edgecost, pcSdToNode, pcSdCands, &(pcdata->nPcSdCands));
      }

      if( count1++ > EXT_PC_SDMAXVISITS )
         break;
   }
}


/** unmarks PcSd array */
static inline
void pcSdToNodeUnmark(
   const GRAPH*          graph,              /**< graph data structure */
   int                   startvertex,        /**< vertex to start from */
   EXTDATA*              extdata             /**< extension data */
   )
{
   PCDATA* const pcdata = extdata->pcdata;
   SCIP_Real* const pcSdToNode = pcdata->pcSdToNode;
   const int* const pcSdCands = pcdata->pcSdCands;
   const int nPcSdCands = pcdata->nPcSdCands;

   assert(graph_pc_isPcMw(graph));
   assert(pcSdCands && pcSdToNode);
   assert(nPcSdCands >= 0);
   assert(pcdata->pcSdStart >= 0 && pcdata->pcSdStart < graph->knots);
   assert(startvertex == pcdata->pcSdStart);

   for( int i = 0; i < nPcSdCands; i++ )
   {
      const int cand = pcSdCands[i];

      assert(pcSdToNode[cand] >= 0.0);

      pcSdToNode[cand] = -1.0;
   }

#ifndef NDEBUG
   pcdata->pcSdStart = -1;
   pcdata->nPcSdCands = -1;
#endif
}


/** marks bottleneck array on path to tree root */
static
void bottleneckMarkRootPath(
   const GRAPH*          graph,              /**< graph data structure */
   int                   vertex,             /**< vertex to start from */
   EXTDATA*              extdata             /**< extension data */
   )
{
   SCIP_Real* const bottleneckDist_node = extdata->tree_bottleneckDistNode;
   const SCIP_Real* const parentEdgeCost = extdata->tree_parentEdgeCost;
   const int* const parentNode = extdata->tree_parentNode;
   const int* const tree_deg = extdata->tree_deg;
   const int tree_root = extdata->tree_root;

   assert(bottleneckDist_node && parentEdgeCost && parentNode && tree_deg);
   assert(vertex >= 0 && vertex < graph->knots);
   assert(bottleneckDist_node[vertex] == -1.0);
   assert(bottleneckDist_node[tree_root] == -1.0);

   if( vertex == tree_root )
   {
      bottleneckDist_node[vertex] = 0.0;
   }
   else
   {
      /* go down from vertex */

      SCIP_Real bottleneck = 0.0;
      SCIP_Real bottleneck_local = 0.0;
      int childNode = vertex;
      int currentNode = parentNode[vertex];
      const SCIP_Bool isPc = graph_pc_isPc(graph);

      assert(currentNode != -1);
      assert(!extInitialCompIsEdge(extdata) || tree_deg[childNode] == 1);
      assert(childNode == extdata->tree_starcenter || extInitialCompIsGenStar(extdata) || tree_deg[childNode] == 1);

      while( currentNode != -1 )
      {
         assert(currentNode >= 0 && tree_deg[currentNode] >= 0);
         assert(parentEdgeCost[childNode] >= 0.0 && bottleneckDist_node[currentNode] == -1.0);
         assert(currentNode != vertex);
         assert(!isPc || !graph_pc_knotIsDummyTerm(graph, currentNode));

         if( tree_deg[childNode] == 2 )
         {
            bottleneck_local += parentEdgeCost[childNode];
            if( isPc && Is_term(graph->term[childNode]) )
            {
               assert(graph_pc_termIsNonLeafTerm(graph, childNode) && graph->prize[childNode] > 0.0);
               bottleneck_local -= graph->prize[childNode];
            }
         }
         else
            bottleneck_local = parentEdgeCost[childNode];

         if( bottleneck < bottleneck_local )
            bottleneck = bottleneck_local;

         bottleneckDist_node[currentNode] = bottleneck;
         childNode = currentNode;
         currentNode = parentNode[currentNode];
      }

      assert(childNode == tree_root);
   }
}

/** unmarks bottleneck array on path to tree root */
static
void bottleneckUnmarkRootPath(
   const GRAPH*          graph,              /**< graph data structure */
   int                   vertex,             /**< vertex to start from */
   EXTDATA*              extdata             /**< extension data */
   )
{
   SCIP_Real* const bottleneckDist_node = extdata->tree_bottleneckDistNode;
   const int* const parentNode = extdata->tree_parentNode;
   const int tree_root = extdata->tree_root;

   assert(extdata && bottleneckDist_node && parentNode);
   assert(bottleneckDist_node[vertex] == -1.0 || vertex == tree_root);
   assert(bottleneckDist_node[tree_root] >= 0.0);

   if( vertex == tree_root )
   {
      bottleneckDist_node[vertex] = -1.0;
      assert(parentNode[vertex] == -1);
   }
   else
   {
      assert(parentNode[vertex] >= 0);
   }

   /* go down from vertex and reset bottleneckDist_node */
   for( int currentNode = parentNode[vertex]; currentNode != -1; currentNode = parentNode[currentNode]  )
   {
      assert(currentNode >= 0);
      assert(extdata->tree_deg[currentNode] >= 0);
      assert(bottleneckDist_node[currentNode] >= 0.0);

      bottleneckDist_node[currentNode] = -1.0;
   }

   assert(bottleneckDist_node[tree_root] == -1.0);
}


/** computes the tree bottleneck between vertices in the current tree,
 * for which vertex_pathmarked root path has been marked already */
static
SCIP_Real bottleneckGetDist(
   const GRAPH*          graph,              /**< graph data structure */
   const EXTDATA*        extdata,            /**< extension data */
#ifndef NDEBUG
   int                   vertex_pathmarked,  /**< vertex with marked rootpath */
#endif
   int                   vertex_unmarked     /**< second vertex */
   )
{
   const SCIP_Real* const bottleneckDist_node = extdata->tree_bottleneckDistNode;
   const SCIP_Real* const parentEdgeCost = extdata->tree_parentEdgeCost;
   const int* const parentNode = extdata->tree_parentNode;
   int* const tree_deg = extdata->tree_deg;
   SCIP_Real bottleneck;
   const int tree_root = extdata->tree_root;
   int currentNode;

   assert(bottleneckDist_node && parentEdgeCost && parentNode);
   assert(bottleneckDist_node[vertex_pathmarked] == -1.0 || vertex_pathmarked == tree_root);
   assert(bottleneckDist_node[vertex_unmarked] == -1.0 || vertex_unmarked == tree_root || tree_deg[vertex_unmarked] > 1);
   assert(bottleneckDist_node[tree_root] >= 0.0);
   assert(vertex_pathmarked != vertex_unmarked);

   /* go down from vertex_unmarked up to lowest common ancestor with vertex_pathmarked  */
   bottleneck = 0.0;

   if( vertex_unmarked == tree_root )
   {
      currentNode = vertex_unmarked;
   }
   else
   {
      SCIP_Real bottleneck_local = 0.0;
      const SCIP_Bool isPc = graph_pc_isPc(graph);

      assert(parentNode[vertex_unmarked] >= 0);

      for( currentNode = vertex_unmarked; bottleneckDist_node[currentNode] < -0.5; currentNode = parentNode[currentNode] )
      {
         assert(tree_deg[currentNode] >= 0 && parentEdgeCost[currentNode] >= 0.0);
         assert(bottleneckDist_node[currentNode] == -1.0);
         assert(currentNode != vertex_pathmarked);

         if( tree_deg[currentNode] == 2 )
         {
            bottleneck_local += parentEdgeCost[currentNode];
            if( isPc && Is_term(graph->term[currentNode]) )
            {
               assert(graph_pc_termIsNonLeafTerm(graph, currentNode) && graph->prize[currentNode] > 0.0);
               bottleneck_local -= graph->prize[currentNode];
            }
         }
         else
            bottleneck_local = parentEdgeCost[currentNode];

         if( bottleneck < bottleneck_local )
            bottleneck = bottleneck_local;

         assert(parentNode[currentNode] >= 0 && parentNode[currentNode] != vertex_unmarked);
      }
   }

   bottleneck = MAX(bottleneck, bottleneckDist_node[currentNode]);

   return bottleneck;
}


/** helper */
static inline
void bottleneckMarkEqualityPath(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   path_start,         /**< vertex to start from */
   int                   path_end,           /**< vertex to end at */
   EXTDATA*              extdata             /**< extension data */
)
{
   SCIP_Bool* const edges_isEqForbidden = extdata->sdeq_edgesIsForbidden;
   const int* const parentNode = extdata->tree_parentNode;

   assert(edges_isEqForbidden);
   assert(path_start != path_end);
   assert(graph_knot_isInRange(graph, path_start));
   assert(graph_knot_isInRange(graph, path_end));

   for( int currentNode = path_start; currentNode != path_end; currentNode = parentNode[currentNode] )
   {
      int e;
      const int parent = parentNode[currentNode];

      assert(graph_knot_isInRange(graph, parent));

      for( e = graph->outbeg[parent]; e != EAT_LAST; e = graph->oeat[e] )
      {
         if( graph->head[e] == currentNode )
         {
            assert(EQ(graph->cost[e], extdata->tree_parentEdgeCost[currentNode]));
            if( !edges_isEqForbidden[e / 2] )
            {
#ifdef SCIP_DEBUG
               SCIPdebugMessage("forbid equality edge: ");
               graph_edge_printInfo(graph, e);
#endif
               edges_isEqForbidden[e / 2] = TRUE;
               extdata->sdeq_hasForbiddenEdges = TRUE;
               StpVecPushBack(scip, extdata->sdeq_resetStack, e / 2);
            }
            break;
         }
      }

      assert(e != EAT_LAST);
   }
}


/** markes bottleneck edges used for equality rule-out */
static inline
void bottleneckMarkEqualityEdges(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   SCIP_Real             dist_eq,            /**< distance that was used for equality rule-out */
   int                   vertex_pathmarked,  /**< vertex with marked rootpath */
   int                   vertex_unmarked,    /**< second vertex */
   EXTDATA*              extdata             /**< extension data */
)
{
   const SCIP_Real* const bottleneckDist_node = extdata->tree_bottleneckDistNode;
   const SCIP_Real* const parentEdgeCost = extdata->tree_parentEdgeCost;
   const int* const parentNode = extdata->tree_parentNode;
   int* const tree_deg = extdata->tree_deg;
   SCIP_Real bottleneck_local;
   const int tree_root = extdata->tree_root;
   int ancestor = UNKNOWN;
   int bottleneck_start;
   const SCIP_Bool isPc = graph_pc_isPc(graph);

   assert(bottleneckDist_node && parentEdgeCost && parentNode);
   assert(bottleneckDist_node[vertex_pathmarked] == -1.0 || vertex_pathmarked == tree_root);
   assert(bottleneckDist_node[vertex_unmarked] == -1.0 || vertex_unmarked == tree_root || tree_deg[vertex_unmarked] > 1);
   assert(bottleneckDist_node[tree_root] >= 0.0);
   assert(vertex_pathmarked != vertex_unmarked);

   /* 1. go down from vertex_unmarked to lowest common ancestor with vertex_pathmarked */

   if( vertex_unmarked == tree_root )
   {
      ancestor = vertex_unmarked;
   }
   else
   {
      int currentNode;
      assert(parentNode[vertex_unmarked] >= 0);
      bottleneck_start = UNKNOWN;
      bottleneck_local = 0.0;

      for( currentNode = vertex_unmarked; bottleneckDist_node[currentNode] < -0.5; currentNode = parentNode[currentNode] )
      {
         assert(tree_deg[currentNode] >= 0 && parentEdgeCost[currentNode] >= 0.0);
         assert(EQ(bottleneckDist_node[currentNode], -1.0));
         assert(currentNode != vertex_pathmarked);

         if( tree_deg[currentNode] == 2 )
         {
            bottleneck_local += parentEdgeCost[currentNode];
            if( isPc && Is_term(graph->term[currentNode]) )
            {
               assert(graph_pc_termIsNonLeafTerm(graph, currentNode) && graph->prize[currentNode] > 0.0);
               bottleneck_local -= graph->prize[currentNode];
            }
         }
         else
         {
            bottleneck_start = currentNode;
            bottleneck_local = parentEdgeCost[currentNode];
         }

         if( EQ(bottleneck_local, dist_eq) )
         {
            assert(parentNode[currentNode] >= 0);
            bottleneckMarkEqualityPath(scip, graph, bottleneck_start, parentNode[currentNode], extdata);

            return;
         }

         assert(parentNode[currentNode] >= 0 && parentNode[currentNode] != vertex_unmarked);
      }

      ancestor = currentNode;
      assert(GE(bottleneckDist_node[ancestor], 0.0));
   }


   /* 2. go down from vertex_marked to ancestor */

   assert(parentNode[vertex_pathmarked] >= 0);
   assert(ancestor != UNKNOWN);
   bottleneck_start = UNKNOWN;
   bottleneck_local = 0.0;

   for( int currentNode = vertex_pathmarked; currentNode != ancestor; currentNode = parentNode[currentNode] )
   {
      assert(tree_deg[currentNode] >= 0 && parentEdgeCost[currentNode] >= 0.0);
      assert(currentNode != vertex_unmarked);

      if( tree_deg[currentNode] == 2 )
      {
         bottleneck_local += parentEdgeCost[currentNode];
         if( isPc && Is_term(graph->term[currentNode]) )
         {
            assert(graph_pc_termIsNonLeafTerm(graph, currentNode) && graph->prize[currentNode] > 0.0);
            bottleneck_local -= graph->prize[currentNode];
         }
      }
      else
      {
         bottleneck_start = currentNode;
         bottleneck_local = parentEdgeCost[currentNode];
      }

      if( EQ(bottleneck_local, dist_eq) )
      {
         // todo not quite sure whether this is correct
         if( bottleneck_start == UNKNOWN  )
         {
            assert(extIsAtInitialGenStar(extdata));
            bottleneck_start = vertex_pathmarked;
         }

         assert(parentNode[currentNode] >= 0);
         bottleneckMarkEqualityPath(scip, graph, bottleneck_start, parentNode[currentNode], extdata);

         return;
      }

      assert(parentNode[currentNode] >= 0 && parentNode[currentNode] != vertex_unmarked);
   }

   assert(0 && "should never arrive here!");
}


/** markes single bottleneck edge used for equality rule-out */
static inline
void bottleneckMarkEqualityEdge(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          g,                  /**< graph data structure */
   int                   edge,               /**< the edge to mark */
   EXTDATA*              extdata             /**< extension data */
)
{
   SCIP_Bool* const edges_isEqForbidden = extdata->sdeq_edgesIsForbidden;

   assert(edges_isEqForbidden);
   assert(graph_edge_isInRange(g, edge));

   if( !edges_isEqForbidden[edge / 2] )
   {
      edges_isEqForbidden[edge / 2] = TRUE;
      extdata->sdeq_hasForbiddenEdges = TRUE;
      StpVecPushBack(scip, extdata->sdeq_resetStack, edge / 2);
   }
}


/** helper to check the case of quality */
static inline
SCIP_Bool bottleneckIsEqualityDominated(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          g,                  /**< graph data structure */
   SCIP_Real             dist_eq,            /**< critical distance */
   int                   edge_forbidden,     /**< forbidden edge */
   int                   vertex1,            /**< first vertex */
   int                   vertex2,            /**< second vertex */
   EXTDATA*              extdata             /**< extension data */
)
{
   const SCIP_Real sd_eq = extreduce_distDataGetSdDoubleForbiddenEq(scip, g, dist_eq,
         edge_forbidden, vertex1, vertex2, extdata);

   if( sd_eq < -0.5 )
      return FALSE;

   assert(GE(sd_eq, dist_eq));

   if( LE(sd_eq, dist_eq) )
   {
      assert(EQ(sd_eq, dist_eq));
      return TRUE;
   }

   return FALSE;
}


/** Does a special distance approximation dominate the tree bottleneck distance between
 *  vertex_pathmarked and vertex_unmarked in the current tree.
 *  NOTE: makes additional checks in case of equality */
static inline
SCIP_Bool bottleneckIsDominated(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   vertex_pathmarked,  /**< vertex for which bottleneck path to root has been marked */
   int                   vertex_unmarked,    /**< second vertex */
   SCIP_Real             specialDist,        /**< best computed special distance approximation (-1.0 if unknown) */
   int                   edge_forbidden,     /**< forbidden edge */
   EXTDATA*              extdata             /**< extension data */
   )
{
   SCIP_Real bottleneckDist;
   const SCIP_Bool hasSpecialDist = sdIsNonTrivial(specialDist);

   assert(graph_knot_isInRange(graph, vertex_pathmarked));
   assert(graph_knot_isInRange(graph, vertex_unmarked));

   if( !hasSpecialDist || vertex_pathmarked == vertex_unmarked )
   {
      return FALSE;
   }

#ifndef NDEBUG
   bottleneckDist = bottleneckGetDist(graph, extdata, vertex_pathmarked, vertex_unmarked);
#else
   bottleneckDist = bottleneckGetDist(graph, extdata, vertex_unmarked);
#endif

   SCIPdebugMessage("domination test %d->%d: sd=%f bottleneck=%f \n", vertex_pathmarked, vertex_unmarked, specialDist, bottleneckDist);

   if( LT(specialDist, bottleneckDist) )
   {
      return TRUE;
   }
   else if( LE(specialDist, bottleneckDist) )
   {
#ifdef EXT_DOUBLESD_ALWAYS
      assert(EQ(extGetSdDouble(scip, graph, vertex_pathmarked, vertex_unmarked, extdata), specialDist));
#else
      assert(LE(extGetSdDouble(scip, graph, vertex_pathmarked, vertex_unmarked, extdata), specialDist));
#endif

      if( bottleneckIsEqualityDominated(scip, graph, specialDist, edge_forbidden,
         vertex_pathmarked, vertex_unmarked, extdata) )
      {
         SCIPdebugMessage("...ruled out with equality! \n");
         bottleneckMarkEqualityEdges(scip, graph, specialDist, vertex_pathmarked, vertex_unmarked, extdata);

         return TRUE;
      }
   }

   return FALSE;
}


/** Does a special distance approximation dominate the tree bottleneck distance of
 *  extension edge (i.e. its edge cost) or bottleneck distance between vertex_pathmarked
 *  and vertex_unmarked in the current tree.
 *  NOTE: makes additional checks in case of equality */
static inline
SCIP_Bool bottleneckWithExtedgeIsDominated(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   extedge,            /**< edge along which we want to extend the tree, or -1 */
   int                   vertex_pathmarked,  /**< vertex for which bottleneck path to root has been marked */
   int                   vertex_unmarked,    /**< second vertex */
   SCIP_Real             specialDist,        /**< best computed special distance approximation (-1.0 if unknown) */
   EXTDATA*              extdata             /**< extension data */
   )
{
   SCIP_Real bottleneckDist;
   const SCIP_Bool hasSpecialDist = sdIsNonTrivial(specialDist);

   assert(vertex_pathmarked == graph->tail[extedge]);

   if( !hasSpecialDist )
      return FALSE;

   if( LT(specialDist, graph->cost[extedge]) )
   {
      return TRUE;
   }
   else if( LE(specialDist, graph->cost[extedge]) )
   {
      const int vertex1 = graph->head[extedge];

      if( bottleneckIsEqualityDominated(scip, graph, specialDist, extedge,
         vertex1, vertex_unmarked, extdata) )
      {
         bottleneckMarkEqualityEdge(scip, graph, extedge, extdata);
         SCIPdebugMessage("...ruled out with equality by single edge ! \n");

         return TRUE;
      }
   }

   if( vertex_pathmarked == vertex_unmarked )
      return FALSE;

#ifndef NDEBUG
   bottleneckDist = bottleneckGetDist(graph, extdata, vertex_pathmarked, vertex_unmarked);
#else
   bottleneckDist = bottleneckGetDist(graph, extdata, vertex_unmarked);
#endif

   SCIPdebugMessage("extedge domination test %d->%d: sd=%f bottleneck=%f \n", vertex_pathmarked, vertex_unmarked, specialDist, bottleneckDist);

   if( LT(specialDist, bottleneckDist) )
   {
      return TRUE;
   }
   else if( LE(specialDist, bottleneckDist) )
   {
      const int vertex1 = graph->head[extedge];

      assert(vertex1 != vertex_unmarked);
      assert(vertex1 != vertex_pathmarked);
#ifdef EXT_DOUBLESD_ALWAYS
      assert(EQ(extGetSdDouble(scip, graph, vertex1, vertex_unmarked, extdata), specialDist));
#else
      assert(LE(extGetSdDouble(scip, graph, vertex1, vertex_unmarked, extdata), specialDist));
#endif

      if( bottleneckIsEqualityDominated(scip, graph, specialDist, extedge,
         vertex1, vertex_unmarked, extdata) )
      {
         bottleneckMarkEqualityEdges(scip, graph, specialDist, vertex_pathmarked, vertex_unmarked, extdata);
         SCIPdebugMessage("...ruled out with equality! \n");

         return TRUE;
      }
   }

   return FALSE;
}


/** Does a special distance approximation dominate the tree bottleneck distance between
 *  vertex_pathmarked and vertex_unmarked in the current tree?
 *  NOTE: makes additional checks in case of equality */
static inline
SCIP_Bool bottleneckToSiblingIsDominated(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   extedge,            /**< edge for extension */
   int                   edge2sibling,       /**< edge to sibling of extedge head */
   SCIP_Real             specialDist,        /**< best computed special distance approximation (FARAWAY if unknown) */
   EXTDATA*              extdata             /**< extension data */
)
{
   const SCIP_Bool hasSpecialDist = LT(specialDist, FARAWAY);

   assert(specialDist >= 0.0);
   assert(extedge >= 0 && edge2sibling >= 0);
   assert(extedge != edge2sibling);
   assert(graph->tail[extedge] == graph->tail[edge2sibling]);

   if( !hasSpecialDist )
   {
      return FALSE;
   }
   else
   {
      const SCIP_Real* const edgecost = graph->cost;

      assert(GE(specialDist, 0.0));

      if( LT(specialDist, edgecost[edge2sibling]) )
         return TRUE;

      if( LT(specialDist, edgecost[extedge]) )
         return TRUE;

      if( LE(specialDist, edgecost[edge2sibling]) )
      {
         const int vertex1 = graph->head[edge2sibling];
         const int vertex2 = graph->head[extedge];

         if( bottleneckIsEqualityDominated(scip, graph, specialDist, edge2sibling,
            vertex1, vertex2, extdata) )
         {
            bottleneckMarkEqualityEdge(scip, graph, edge2sibling, extdata);
            SCIPdebugMessage("...ruled out edge1 with equality! \n");

            return TRUE;
         }
      }

      if( LE(specialDist, edgecost[extedge]) )
      {
         const int vertex1 = graph->head[edge2sibling];
         const int vertex2 = graph->head[extedge];

         if( bottleneckIsEqualityDominated(scip, graph, specialDist, extedge,
            vertex1, vertex2, extdata) )
         {
            bottleneckMarkEqualityEdge(scip, graph, extedge, extdata);
            SCIPdebugMessage("...ruled out edge2 with equality! \n");

            return TRUE;
         }
      }
   }

   return FALSE;
}


/** checks tree bottleneck distances to non-leaves of the tree that were marked before */
static inline
void bottleneckCheckNonLeaves_pc(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge2neighbor,      /**< the edge from the tree to the neighbor */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            ruledOut            /**< could the extension be ruled out */
)
{
   const PCDATA* const pcdata = extdata->pcdata;
   const int* const pcSdCands = pcdata->pcSdCands;
   const int* const tree_deg = extdata->tree_deg;
   const int nPcSdCands = pcdata->nPcSdCands;
   const int neighbor = graph->head[edge2neighbor];
   const int neighbor_base = graph->tail[edge2neighbor];

   assert(pcSdCands);
   assert(ruledOut);
   assert(!(*ruledOut));
   assert(nPcSdCands >= 0);

   /* also check non-leaves */
   for( int c = 0; c < nPcSdCands; c++ )
   {
      SCIP_Real specialDist;
      const int cand = pcSdCands[c];

      assert(cand >= 0 && cand < graph->knots);

      /* leaf, or not contained? */
      if( tree_deg[cand] <= 1 )
         continue;

      specialDist = extGetSd(scip, graph, neighbor, cand, extdata);

      if( bottleneckWithExtedgeIsDominated(scip, graph, edge2neighbor, neighbor_base, cand, specialDist, extdata) )
      {
         SCIPdebugMessage("---non-leaf bottleneck rule-out---\n");
         *ruledOut = TRUE;

         return;
      }
   }
}


/** checks tree bottleneck distances to non-leaves of the tree */
static inline
void bottleneckCheckNonLeaves(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge2neighbor,      /**< the edge from the tree to the neighbor */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            ruledOut            /**< could the extension be ruled out */
)
{
   const int* const innerNodes = extdata->tree_innerNodes;
   const int nInnerNodes = extdata->tree_ninnerNodes;
   const int neighbor = graph->head[edge2neighbor];
   const int neighbor_base = graph->tail[edge2neighbor];

   assert(ruledOut);
   assert(!(*ruledOut));

   /* also check non-leaves */
   for( int i = 0; i < nInnerNodes; i++ )
   {
      SCIP_Real specialDist;
      const int node = innerNodes[i];
      assert(graph_knot_isInRange(graph, node));
      assert(extdata->tree_deg[node] > 1);
      assert(node != neighbor_base);

      specialDist = extGetSd(scip, graph, neighbor, node, extdata);

      if( bottleneckWithExtedgeIsDominated(scip, graph, edge2neighbor, neighbor_base, node, specialDist, extdata) )
      {
         SCIPdebugMessage("---non-leaf bottleneck rule-out---\n");
         *ruledOut = TRUE;
         return;
      }
   }
}


#ifndef NDEBUG
/** has the leaf a dominated bottleneck with other leaves? */
static
SCIP_Bool dbgBottleneckFromLeafIsDominated(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   topleaf,            /**< component leaf to check for */
   SCIP_Bool             with_sd_double,     /**< use SD double method? */
   int                   edge_forbidden,     /**< forbidden edge */
   EXTDATA*              extdata             /**< extension data */
   )
{
   const int* const leaves = extdata->tree_leaves;
   const int nleaves = extdata->tree_nleaves;
   SCIP_Bool ruleOut = FALSE;
   const SCIP_Bool isPc = graph_pc_isPc(graph);

   bottleneckMarkRootPath(graph, topleaf, extdata);

   if( isPc )
      pcSdToNodeMark(graph, topleaf, extdata);

   for( int j = 0; j < nleaves; j++ )
   {
      const int leaf = leaves[j];

      if( leaf != topleaf )
      {
         const SCIP_Real specialDist = with_sd_double ?
               extGetSdDouble(scip, graph, topleaf, leaf, extdata)
             : extGetSd(scip, graph, topleaf, leaf, extdata);

         if( bottleneckIsDominated(scip, graph, topleaf, leaf, specialDist, edge_forbidden, extdata) )
         {
            SCIPdebugMessage("...debug check ruled out! \n");
            ruleOut = TRUE;
            break;
         }
      }
   }

   if( isPc )
      pcSdToNodeUnmark(graph, topleaf, extdata);

   bottleneckUnmarkRootPath(graph, topleaf, extdata);

   return ruleOut;
}

#endif


/** helper; adds single node MST */
static inline
void add1NodeMst(
   SCIP*                 scip,               /**< SCIP */
   CSRDEPO*              msts                /**< MSTs */
)
{
   CSR mst1;

   assert(msts);

   graph_csrdepo_addEmptyTop(msts, 1, 0);
   graph_csrdepo_getEmptyTop(msts, &mst1);

   reduce_dcmstGet1NodeMst(scip, &mst1);

   graph_csrdepo_emptyTopSetMarked(msts);
}


/** helper; adds MSTs */
static
void mstAddRootLevelMsts(
   SCIP*                 scip,               /**< SCIP */
   EXTDATA*              extdata             /**< extension data */
)
{
   REDDATA* const reddata = extdata->reddata;
   CSRDEPO* const msts_comp = reddata->msts_comp;
   CSRDEPO* const msts_levelbase = reddata->msts_levelbase;

   assert(graph_csrdepo_isEmpty(msts_comp));
   assert(graph_csrdepo_isEmpty(msts_levelbase));
   assert(0 == extdata->tree_depth);

   /* initialize 1-node MSTs corresponding to the root of the extension tree */
   add1NodeMst(scip, msts_comp);
   add1NodeMst(scip, msts_levelbase);
}


/** helper; adds SDs */
static
void mstAddRootLevelSDs(
   SCIP*                 scip,               /**< SCIP */
   int                   root,               /**< the root of the extension tree */
   EXTDATA*              extdata             /**< extension data */
)
{
   REDDATA* const reddata = extdata->reddata;
   MLDISTS* const sds_vertical = reddata->sds_vertical;
   MLDISTS* const sds_horizontal = reddata->sds_horizontal;

   extreduce_mldistsLevelAddTop(1, 0, sds_vertical);
   extreduce_mldistsEmptySlotSetBase(root, sds_vertical);
   extreduce_mldistsEmptySlotSetFilled(sds_vertical);
   extreduce_mldistsLevelCloseTop(sds_vertical);

   extreduce_mldistsLevelAddTop(1, 0, sds_horizontal);
   extreduce_mldistsEmptySlotSetBase(root, sds_horizontal);
   extreduce_mldistsEmptySlotSetFilled(sds_horizontal);
   extreduce_mldistsLevelCloseTop(sds_horizontal);

   SCIPdebugMessage("initialized first MST level (%d) \n", extreduce_mldistsTopLevel(sds_vertical));
}


/** can current 3-leaf tree be rule-out in case of equality? */
static inline
SCIP_Bool mstEqComp3RuleOut(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   SCIP_Real             tree_cost,          /**< tree cost */
   EXTDATA*              extdata             /**< extension data */
)
{
   SCIP_Real sds[3];
   const int* leaves = extdata->tree_leaves;

   assert(3 == extdata->tree_nleaves);

   /* NOTE: initial component star should be ok, because in this case we don't
    * use simple paths for equality rule-outs */
   if( extInitialCompIsStar(extdata) || !extdata->sdeq_hasForbiddenEdges )
      return TRUE;

   sds[0] = extreduce_distDataGetSdDoubleForbidden(scip, graph,
         leaves[0], leaves[1], extdata);

   sds[1] = extreduce_distDataGetSdDoubleForbidden(scip, graph,
         leaves[0], leaves[2], extdata);

   if( LE(sds[0] + sds[1], tree_cost) )
   {
      return TRUE;
   }

   sds[2] = extreduce_distDataGetSdDoubleForbidden(scip, graph,
         leaves[1], leaves[2], extdata);

   if( LE(sds[0] + sds[2], tree_cost) || LE(sds[1] + sds[2], tree_cost) )
   {
      return TRUE;
   }

   return FALSE;
}


/** Gets SDs from leaf of top tree component to siblings for MST calculation.
 *  Returns early (with leafRuledOut == TRUE) if extension via 'edge2leaf' can be ruled out already.
 *  NOTE: Only restricted bottleneck tests are performed! */
static inline
void mstCompLeafGetSDsToSiblings(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge2top,           /**< edge to the top component leaf */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Real             sds[],              /**< array to store the SDs */
   SCIP_Bool*            leafRuledOut        /**< could the extension already by ruled out */
   )
{
   const MLDISTS* const sds_horizontal = extdata->reddata->sds_horizontal;
   const int* const extstack_data = extdata->extstack_data;
   const int* const ghead = graph->head;
   const int stackpos = extStackGetPosition(extdata);
   const int topedges_start = extStackGetTopOutEdgesStart(extdata, stackpos);
   const int topedges_end = extStackGetTopOutEdgesEnd(extdata, stackpos);
   const int topleaf = ghead[edge2top];
   SCIP_Bool hitTopLeaf = FALSE;
   const SCIP_Bool isAtInitialGenStar = extIsAtInitialGenStar(extdata);

#ifndef NDEBUG
   {
      const SCIP_Bool atInitialAnyStar = extIsAtInitialStar(extdata) || extIsAtInitialGenStar(extdata);

      assert(leafRuledOut && sds);
      assert((*leafRuledOut) == FALSE);
      assert(atInitialAnyStar || extreduce_mldistsLevelNTopTargets(sds_horizontal) >= extStackGetTopSize(extdata) - 1);
      assert(extreduce_sdshorizontalInSync(scip, graph, topleaf, extdata));
      assert(topedges_start <= topedges_end);
   }
#endif

   if( isAtInitialGenStar )
   {
      bottleneckMarkRootPath(graph, topleaf, extdata);
   }

   for( int i = topedges_start, j = 0; i != topedges_end; i++, j++ )
   {
      const int edge2sibling = extstack_data[i];
      const int sibling = ghead[edge2sibling];

      assert(extreduce_nodeIsInStackTop(graph, extdata, sibling));
      assert(extdata->tree_deg[sibling] == 1);
      assert(EQ(sds[j], -1.0));

      if( sibling == topleaf )
      {
         assert(!hitTopLeaf);

         hitTopLeaf = TRUE;
         sds[j] = FARAWAY;

         continue;
      }

      sds[j] = extreduce_mldistsTopTargetDist(sds_horizontal, topleaf, sibling);

      if( graph->tail[edge2top] != graph->tail[edge2sibling] )
      {
         assert(isAtInitialGenStar);
         if( bottleneckIsDominated(scip, graph, topleaf, sibling, sds[j], edge2sibling, extdata) )
         {
            SCIPdebugMessage("---bottleneck rule-out component (GENSTAR siblings test)---\n");
            *leafRuledOut = TRUE;
            break;
         }

         continue;
      }

      /* only make bottleneck test for 'right' siblings to avoid double checks */
      if( !hitTopLeaf )
      {
         assert(!bottleneckToSiblingIsDominated(scip, graph, edge2top, edge2sibling, sds[j], extdata));
      }
      else if( bottleneckToSiblingIsDominated(scip, graph, edge2top, edge2sibling, sds[j], extdata) )
      {
         SCIPdebugMessage("---bottleneck rule-out component (siblings test)---\n");
         *leafRuledOut = TRUE;
         break;
      }
   }

   if( isAtInitialGenStar )
   {
      bottleneckUnmarkRootPath(graph, topleaf, extdata);
   }

   assert(hitTopLeaf || *leafRuledOut);
}


/** Gets SDs from leaf of top tree component to ancestors for MST calculation.
 *  Returns early (with leafRuledOut == TRUE) if extension via 'edge2leaf' can be ruled out already.
 *  NOTE: Only restricted bottleneck tests are performed, UNLESS the leaf has no siblings! */
static inline
void mstCompLeafGetSDsToAncestors(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge2leaf,          /**< edge to the top component leaf */
   int                   nleaves_ancestors,  /**< number of leaves to ancestors */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Real             sds[],              /**< array to store the SDs */
   SCIP_Bool*            leafRuledOut        /**< could the extension already by ruled out */
   )
{
   const MLDISTS* const sds_vertical = extdata->reddata->sds_vertical;
   const int topleaf = graph->head[edge2leaf];
   const SCIP_Real* const adjedgecosts = extreduce_mldistsTopTargetDists(sds_vertical, topleaf);
   const SCIP_Bool hasSiblings = (extStackGetTopSize(extdata) > 1);

#ifndef NDEBUG
   const SCIP_Bool isPc = graph_pc_isPc(graph);

   assert(adjedgecosts && leafRuledOut && sds);
   assert(!(*leafRuledOut));
   assert(nleaves_ancestors >= 1);
   assert(extreduce_mldistsLevelNTopTargets(sds_vertical) == nleaves_ancestors);
   /* expensive check, maybe only do if STP_DEBUG_EXT is set? */
   assert(extreduce_sdsverticalInSync(scip, graph, extStackGetTopSize(extdata), nleaves_ancestors, topleaf, extdata));

   for( int j = 0; j < nleaves_ancestors; j++ )
      assert(EQ(sds[j], -1.0));
#endif

   memcpy(sds, adjedgecosts, nleaves_ancestors * sizeof(sds[0]));

   /* if there are no siblings, then there is a chance to find a non-trivial bottleneck rule-out
    * ...if the initial component is a general star, rule-out is also possible otherwise */
   if( !hasSiblings || extIsAtInitialGenStar(extdata) )
   {
      const int* const leaves = extdata->tree_leaves;

      bottleneckMarkRootPath(graph, topleaf, extdata);

      /* WARNING: might lead to bugs in OPT, but not in DEBUG mode! */
#ifndef NDEBUG
      if( isPc )
         pcSdToNodeMark(graph, topleaf, extdata);
#endif

      /* get the SDs to the ancestor (lower) leafs and try bottleneck rule out */
      for( int j = 0; j < nleaves_ancestors; j++ )
      {
         const int leaf = leaves[j];
         const SCIP_Real sd = adjedgecosts[j];
         const SCIP_Real specialDist = EQ(sd, FARAWAY) ? -1.0 : sd;

         assert(EQ(specialDist, extGetSd(scip, graph, topleaf, leaf, extdata)));

         if( bottleneckIsDominated(scip, graph, topleaf, leaf, specialDist, edge2leaf, extdata) )
         {
            SCIPdebugMessage("---bottleneck rule-out component (standard test)---\n");
            *leafRuledOut = TRUE;
            break;
         }
      }

      bottleneckUnmarkRootPath(graph, topleaf, extdata);

      /* WARNING: might lead to bugs in OPT, but not in DEBUG mode! */
#ifndef NDEBUG
      if( isPc )
         pcSdToNodeUnmark(graph, topleaf, extdata);
#endif
   }
}


/** Gets SDs from leaf (head of 'edge2leaf') to all other leaves of the tree.
 *  Returns early (with leafRuledOut == TRUE) if extension via 'edge2leaf' can be ruled out already.
 *  NOTE: Only restricted bottleneck tests are performed! */
static inline
void mstCompLeafGetSDs(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge2leaf,          /**< edge to the top component leaf */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Real             sds[],              /**< array to store the SDs */
   SCIP_Bool*            leafRuledOut        /**< could the extension already by ruled out */
   )
{
   const int nleaves_ancestors = extGetNancestorLeaves(extdata);
#ifndef NDEBUG
   const int compleaf = graph->head[edge2leaf];
#endif

   assert(compleaf != extdata->tree_starcenter);
   assert(leafRuledOut && !(*leafRuledOut));

   /* fill in the second part of the sds array */
   mstCompLeafGetSDsToSiblings(scip, graph, edge2leaf, extdata, &(sds[nleaves_ancestors]), leafRuledOut);

   if( *leafRuledOut )
   {
      /* NOTE: does not need to hold in case of equality rule out! */
      assert(!extInitialCompIsStar(extdata) || dbgBottleneckFromLeafIsDominated(scip, graph, compleaf, TRUE, edge2leaf, extdata));
      return;
   }

   /* fill in the first part of the sds array */
   mstCompLeafGetSDsToAncestors(scip, graph, edge2leaf, nleaves_ancestors, extdata, sds, leafRuledOut);

   if( *leafRuledOut )
   {
      /* NOTE: does the following not need to hold in case of equality rule out! */
      assert(!extInitialCompIsStar(extdata) || dbgBottleneckFromLeafIsDominated(scip, graph, compleaf, FALSE, edge2leaf, extdata));
      return;
   }

   assert(!dbgBottleneckFromLeafIsDominated(scip, graph, compleaf, FALSE, edge2leaf, extdata) || graph_pc_isPc(graph));
   assert(extreduce_sdsTopInSync(scip, graph, sds, compleaf, extdata));
}


/** Adds leaf from top component of current tree to MST. I.e., adds SD adjacency costs updates MST.
 * 'edge2leaf' must be in top component of the stack.
 *  Returns early (with leafRuledOut == TRUE) if extension via 'edge2leaf' can be ruled out already.
 *  NOTE: SDs are not computed but taken from storage! */
static inline
void mstCompAddLeaf(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge2leaf,          /**< edge to the top component leaf */
   MSTXCOMP*             mstextcomp,         /**< MST extension component */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            leafRuledOut        /**< could the extension already by ruled out */
)
{
   REDDATA* const reddata = extdata->reddata;
   DCMST* const dcmst = reddata->dcmst;
   SCIP_Real* const adjcosts = reduce_dcmstGetAdjcostBuffer(dcmst);

   assert(leafRuledOut);
   assert(FALSE == *leafRuledOut);
   assert(reduce_dcmstGetMaxnnodes(dcmst) >= extdata->tree_nleaves);

   mstCompLeafGetSDs(scip, graph, edge2leaf, extdata, adjcosts, leafRuledOut);

   if( (*leafRuledOut) )
   {
      return;
   }
   else
   {
      assert(mstextcomp->comp_vert == -1);
      assert(mstextcomp->comp_extnode == -1);

      mstExtend(scip, adjcosts, dcmst, mstextcomp);
   }
}


/** is a rule-out by using the top component possible? */
static inline
SCIP_Bool mstCompRuleOut(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata             /**< extension data */
)
{
   CSR topmst;
   REDDATA* const reddata = extdata->reddata;
   const CSRDEPO* const msts_comp = reddata->msts_comp;
   SCIP_Real mstweight;
   SCIP_Real tree_cost = extdata->tree_cost;
   const SCIP_Bool isPc = (graph->prize != NULL);
   SCIP_Bool ruledOut;

   assert(isPc == graph_pc_isPc(graph));

   if( isPc )
   {
      tree_cost -= extdata->pcdata->tree_innerPrize;
      assert(GE(tree_cost, 0.0));
   }

   graph_csrdepo_getTopCSR(msts_comp, &topmst);

   mstweight = reduce_dcmstGetWeight(scip, &topmst);

   assert(extreduce_mstTopCompObjValid(scip, graph, mstweight, extdata));
   assert(topmst.nedges_max % 2 == 0);

   ruledOut = (topmst.nedges_max > 2)? LE(mstweight, tree_cost) : LT(mstweight, tree_cost);

   if( ruledOut )
   {
      // todo: probably also need to check for > 3! */
      if( extdata->tree_nleaves == 3 && EQ(mstweight, tree_cost) && !mstEqComp3RuleOut(scip, graph, tree_cost, extdata) )
      {
         ruledOut = FALSE;
      }
   }

   if( !ruledOut )
   {
      // todo!

      if( extdata->tree_nleaves == 3 && extInitialCompIsEdge(extdata) && 0 )
      {
         ruledOut = extreduce_spg3LeafTreeRuleOut(scip, graph, tree_cost, extdata);
      }
   }
   else
   {
      SCIPdebugMessage("SD MST alternative found %f < %f \n", mstweight, tree_cost);
   }

   return ruledOut;
}


/** builds (top) component MST */
static inline
void mstCompBuildMst(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            ruledOut            /**< already ruled out? */
)
{
   CSR mst_base;
   CSR mst_new;
   MSTXCOMP mstextcomp;
   const int* const extstack_data = extdata->extstack_data;
   const int stackpos = extStackGetPosition(extdata);
   const int topedges_start = extStackGetTopOutEdgesStart(extdata, stackpos);
   const int topedges_end = extStackGetTopOutEdgesEnd(extdata, stackpos);

   assert(*ruledOut == FALSE);
   assert(EXT_STATE_EXPANDED == extdata->extstack_state[stackpos]);
   assert(0 <= topedges_start && topedges_start < topedges_end);

   compMstInitMsts(extdata, &mst_base, &mst_new);

   compMstInitExtComp(graph, extdata, &mst_base, &mst_new, &mstextcomp);

   /* add nodes (with special distances) to MST,
    * and compare with tree bottleneck distances for early rule-out */
   for( int i = topedges_start; i != topedges_end; i++ )
   {
      const int edge2leaf = extstack_data[i];

      /* add vertex to MST graph and check for bottleneck shortcut */
      mstCompAddLeaf(scip, graph, edge2leaf, &mstextcomp, extdata, ruledOut);

      /* early rule-out? */
      if( *ruledOut )
      {
         break;
      }
   }

   compMstFinalizeNew(&mstextcomp, *ruledOut, extdata);

   assert(*ruledOut || extreduce_mstTopCompInSync(scip, graph, extdata));
}


/** computes SDs from head of extension edge to all leaves of the tree */
static inline
void mstLevelLeafSetVerticalSDs(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge2neighbor,      /**< the edge from the tree to the neighbor */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            ruledOut            /**< early rule out? */
)
{
   MLDISTS* const sds_vertical = extdata->reddata->sds_vertical;
   SCIP_Real* const adjedgecosts = extreduce_mldistsEmptySlotTargetDists(sds_vertical);
   const int* const leaves = extdata->tree_leaves;
   const int nleaves = extdata->tree_nleaves;
   const int neighbor = graph->head[edge2neighbor];
   const int neighbor_base = graph->tail[edge2neighbor];

#ifndef NDEBUG
   int* const adjids = extreduce_mldistsEmptySlotTargetIds(sds_vertical);
#endif

   assert(adjedgecosts && leaves && ruledOut);
   assert(*ruledOut == FALSE);
   assert(extIsAtInitialComp(extdata) || extLeafFindPos(extdata, neighbor_base) > 0);

   for( int j = 0; j < nleaves; j++ )
   {
      SCIP_Real specialDist;
      const int leaf = leaves[j];

      assert(leaf >= 0 && leaf < graph->knots);
      assert(extdata->tree_deg[leaf] == 1 && leaf != neighbor);

      specialDist = extGetSd(scip, graph, neighbor, leaf, extdata);

      adjedgecosts[j] = (specialDist >= -0.5) ? specialDist : FARAWAY;
#ifndef NDEBUG
      adjids[j] = leaf;
#endif

      if( bottleneckWithExtedgeIsDominated(scip, graph, edge2neighbor, neighbor_base, leaf, specialDist, extdata) )
      {
         SCIPdebugMessage("---bottleneck rule-out (%d->%d)---\n", neighbor, leaf);
         assert(*ruledOut == FALSE);

         *ruledOut = TRUE;

         break;
      }
   }
}


/** adjusts vertical SDs by removing the neighbor base entry */
static inline
void mstLevelLeafAdjustVerticalSDs(
   int                   neighbor_base,      /**< the edge from the tree to the neighbor */
   REDDATA*              reddata,            /**< reduction data */
   EXTDATA*              extdata             /**< extension data */
)
{
   const int nleaves = extdata->tree_nleaves;

  /* if the base is the root and the only leaf, we want to keep the SD */
  if( extIsAtInitialComp(extdata) )
  {
     assert(nleaves == 1);
     assert(!extInitialCompIsEdge(extdata) || neighbor_base == extdata->tree_root);
     assert(!extInitialCompIsStar(extdata) || neighbor_base == extdata->tree_starcenter);
  }
  else
  {
     /* shift the adjacent cost to remove the neighbor base */

     MLDISTS* const sds_vertical = reddata->sds_vertical;
     SCIP_Real* const dists = extreduce_mldistsEmptySlotTargetDistsDirty(sds_vertical);
     const int leaves_pos = extLeafFindPos(extdata, neighbor_base);

#ifndef NDEBUG
     int* const ids = extreduce_mldistsEmptySlotTargetIdsDirty(sds_vertical);
#endif

     assert(nleaves >= 2);
     assert(leaves_pos > 0 && leaves_pos < nleaves);
     assert(ids[leaves_pos] == neighbor_base);
     assert(neighbor_base != extdata->tree_root);

     for( int i = leaves_pos + 1; i < nleaves; i++ )
     {
        dists[i - 1] = dists[i];
     }

#ifndef NDEBUG
     for( int i = leaves_pos + 1; i < nleaves; i++ )
     {
        ids[i - 1] = ids[i];
     }

     dists[nleaves - 1] = STP_MLDISTS_DIST_UNSET;
     ids[nleaves - 1] = STP_MLDISTS_ID_UNSET;
#endif
  }
}


/** initialization for adding a leaf to a level */
static inline
void mstLevelLeafInit(
   const GRAPH*          graph,              /**< graph data structure */
   int                   neighbor_base,      /**< neighbor base */
   int                   neighbor,           /**< neighbor */
   EXTDATA*              extdata             /**< extension data */
)
{
   MLDISTS* const sds_vertical = extdata->reddata->sds_vertical;
   const SCIP_Bool isPc = (extdata->pcdata->pcSdToNode != NULL);

   assert(graph_pc_isPc(graph) == isPc);

   extreduce_mldistsEmptySlotSetBase(neighbor, sds_vertical);

   /* Initialization for bottleneck. We start from the base of the neighbor! */
   bottleneckMarkRootPath(graph, neighbor_base, extdata);

   if( isPc )
   {
      pcSdToNodeMark(graph, neighbor, extdata);
   }
}


/** finalization for adding a neighbor leaf to a level */
static inline
void mstLevelLeafExit(
   const GRAPH*          graph,              /**< graph data structure */
   int                   neighbor_base,      /**< neighbor base */
   int                   neighbor,           /**< neighbor */
   SCIP_Bool             ruledOut,           /**< extension along neighbor already ruled out? */
   EXTDATA*              extdata             /**< extension data */
)
{
   REDDATA* const reddata = extdata->reddata;
   MLDISTS* const sds_vertical = reddata->sds_vertical;
   const SCIP_Bool isPc = graph_pc_isPc(graph);

   if( ruledOut )
   {
      extreduce_mldistsEmptySlotReset(sds_vertical);
   }
   else
   {
      /* remove the neighbor base SD entry (which we don't need for further extensions from the neighbor base) */
      mstLevelLeafAdjustVerticalSDs(neighbor_base, reddata, extdata);

      extreduce_mldistsEmptySlotSetFilled(sds_vertical);
   }

   bottleneckUnmarkRootPath(graph, neighbor_base, extdata);

   if( isPc )
      pcSdToNodeUnmark(graph, neighbor, extdata);
}


/** checks whether the MST extended at the given neighbor allows to rule-out any extension along this neighbor */
static inline
void mstLevelLeafTryExtMst(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   extneighbor,        /**< neighbor leaf to extend to */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            leafRuledOut        /**< rule out possible? */
)
{
   CSR topmst;
   SCIP_Real extweight;
   REDDATA* const reddata = extdata->reddata;
   MLDISTS* const sds_vertical = reddata->sds_vertical;
   CSRDEPO* const msts_comp = reddata->msts_comp;
   DCMST* const dcmst = reddata->dcmst;
   const SCIP_Real* const adjcosts = extreduce_mldistsEmptySlotTargetDistsDirty(sds_vertical);
   const SCIP_Bool isPc = (graph->prize != NULL);
   SCIP_Real tree_cost = extdata->tree_cost;

   assert(isPc == graph_pc_isPc(graph));
   assert(FALSE == *leafRuledOut);

   if( isPc )
   {
      tree_cost -= extdata->pcdata->tree_innerPrize;
      assert(GE(tree_cost, 0.0));
   }

   graph_csrdepo_getTopCSR(msts_comp, &topmst);

   assert(topmst.nnodes == extdata->tree_nleaves);

   extweight = reduce_dcmstGetExtWeight(scip, &topmst, adjcosts, dcmst);

/*
   SCIP_Real mstweight = reduce_dcmstGetWeight(scip, &topmst);
   assert(extreduce_mstTopCompObjValid(scip, graph, mstweight, extdata));
*/

   /* make sure that the objective of the MST is ok! */
   assert(extreduce_mstTopCompExtObjValid(scip, graph, extneighbor, extweight, extdata));

   if( LT(extweight, tree_cost) )
   {
      SCIPdebugMessage("extension along vertex %d ruled out by extension MST! (%f < %f) \n",
         extneighbor, extweight, extdata->tree_cost);

      *leafRuledOut = TRUE;
   }
}


/** builds base MST */
static inline
void mstLevelBuildBaseMst(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph */
   int                   extnode,            /**< node from which to extend */
   REDDATA*              reddata,            /**< reduction data */
   EXTDATA*              extdata             /**< extension data */
)
{
   CSR mst_new;
   CSR mst_parent;
   MSTXCOMP mstextcomp;

   assert(extnode >= 0 && extnode < graph->knots);
   assert(extnode != extdata->tree_root);

   baseMstInitMsts(extdata, reddata, &mst_parent, &mst_new);

   /* partially initialize 'mstextcomp' */
   baseMstInitExtComp(reddata, extnode, &mst_parent, &mst_new, &mstextcomp);

   /* now build the new MST */
   baseMstBuildNew(scip, graph, reddata, extdata, &mstextcomp);

   baseMstFinalizeNew(scip, graph, &mstextcomp, reddata, extdata);
}


/** Builds base MST if the previous level is the root.
 *  I.e., just a 1-node MST. */
static inline
void mstLevelBuildBaseMstRoot(
   SCIP*                 scip,               /**< SCIP */
   REDDATA*              reddata             /**< reduction data */
)
{
   CSRDEPO* const msts_levelbase = reddata->msts_levelbase;

   assert(!graph_csrdepo_isEmpty(msts_levelbase));
   assert(graph_csrdepo_getNcsrs(msts_levelbase) == 1);

   add1NodeMst(scip, msts_levelbase);
}


/** Can current tree be peripherally ruled out by using MST based arguments? */
SCIP_Bool extreduce_mstRuleOutPeriph(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata             /**< extension data */
)
{
   SCIP_Bool ruledOut = FALSE;

   /* build the SD MST and check for early rule-out via bottleneck distances */
   mstCompBuildMst(scip, graph, extdata, &ruledOut);

   if( ruledOut)
   {
      SCIPdebugMessage("Rule-out periph (via bottleneck) \n");

      ruledOut = TRUE;
   }
   else if( mstCompRuleOut(scip, graph, extdata) )
   {
      SCIPdebugMessage("Rule-out periph (via MST) \n");

      ruledOut = TRUE;
   }

   assert(extreduce_stackTopIsHashed(graph, extdata));

   return ruledOut;
}


/** adds the initial level corresponding to the root of the extension tree */
void extreduce_mstAddRootLevel(
   SCIP*                 scip,               /**< SCIP */
   int                   root,               /**< the root of the extension tree */
   EXTDATA*              extdata             /**< extension data */
)
{
   assert(scip && extdata);
   assert(root >= 0);

   mstAddRootLevelMsts(scip, extdata);
   mstAddRootLevelSDs(scip, root, extdata);
}


/** Removes current component (subset of the top level) from MST storages */
void extreduce_mstCompRemove(
   const GRAPH*          graph,             /**< graph data structure */
   EXTDATA*              extdata            /**< extension data */
   )
{
   REDDATA* const reddata = extdata->reddata;
   CSRDEPO* const msts_comp = reddata->msts_comp;
   const int msts_comp_level = graph_csrdepo_getNcsrs(msts_comp) - 1;

   if( msts_comp_level > extdata->tree_depth )
   {
      graph_csrdepo_removeTop(msts_comp);
   }

   assert(graph_csrdepo_getNcsrs(msts_comp) - 1 == extdata->tree_depth);
}


/** Adds a full new level at the top.
 *  NOTE: for now only the horizontal distances are initialized */
void extreduce_mstLevelInit(
   REDDATA*              reddata,            /**< reduction data */
   EXTDATA*              extdata             /**< extension data */
)
{
   MLDISTS* const sds_vertical = reddata->sds_vertical;

   /* Reserve space for the SDs from each potential vertex of the new level to all leaves
    * of the tree except for the extending vertex.
    * But for the initial component we need to keep the root! */
   if( extIsAtInitialComp(extdata) )
   {
      assert(extdata->tree_nleaves == 1);
      extreduce_mldistsLevelAddTop(STP_EXT_MAXGRAD, extdata->tree_nleaves, sds_vertical);
   }
   else
   {
      extreduce_mldistsLevelAddTop(STP_EXT_MAXGRAD, extdata->tree_nleaves - 1, sds_vertical);
   }

   SCIPdebugMessage("init MST level %d \n", extreduce_mldistsTopLevel(sds_vertical));

   /* tree has not yet been extended, so sds_vertical is ahead */
   assert(extdata->tree_depth == extreduce_mldistsTopLevel(sds_vertical) - 1);
}


/** Adds neighbor of tree for MST calculation.
 *  Basically, SDs to all leafs are computed and stored in 'reddata->sds_vertical'.
 *  Neighbor is given by head of edge 'edge2neighbor'.
 *  Returns early (with leafRuledOut == TRUE, and without adding the neighbor)
 *  if extension via this edge can be ruled out already by using a bottleneck argument or MST. */
void extreduce_mstLevelVerticalAddLeaf(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge2neighbor,      /**< the edge from the tree to the neighbor */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            leafRuledOut        /**< could the extension already by ruled out */
)
{
   const int neighbor = graph->head[edge2neighbor];
   const int neighbor_base = graph->tail[edge2neighbor];
   const SCIP_Bool isPc = graph_pc_isPc(graph);

   assert(leafRuledOut);
   assert(extdata->tree_deg[neighbor_base] == 1);
   assert(extdata->tree_deg[neighbor] == 0);
   assert(*leafRuledOut == FALSE);
   assert(!extIsAtInitialComp(extdata));

   mstLevelLeafInit(graph, neighbor_base, neighbor, extdata);

   /* compute and store SDs to all leaves;
    * also check for bottleneck rule-out! */
   mstLevelLeafSetVerticalSDs(scip, graph, edge2neighbor, extdata, leafRuledOut);

   /* if not yet ruled out, check whether extending the SD MST helps */
   if( !(*leafRuledOut) )
   {
      mstLevelLeafTryExtMst(scip, graph, neighbor, extdata, leafRuledOut);
   }

   /* if not yet ruled out and in PC mode, try bottleneck distances to tree vertices marked before */
   if( isPc && !(*leafRuledOut) )
   {
      bottleneckCheckNonLeaves_pc(scip, graph, edge2neighbor, extdata, leafRuledOut);
   }

#if 1
   /* if not yet ruled out, try bottleneck distances to non-leaves of the tree */
   if( !(*leafRuledOut) )
   {
      bottleneckCheckNonLeaves(scip, graph, edge2neighbor, extdata, leafRuledOut);
   }
#endif

   mstLevelLeafExit(graph, neighbor_base, neighbor, *leafRuledOut, extdata);
}


/** similar to above, but only for initial component! */
void extreduce_mstLevelVerticalAddLeafInitial(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge2neighbor,      /**< edge to the neighbor */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            leafRuledOut        /**< could the extension already by ruled out? */
)
{
   const int neighbor_base = graph->tail[edge2neighbor];
   const int neighbor = graph->head[edge2neighbor];

   assert(*leafRuledOut == FALSE);
   assert(extIsAtInitialComp(extdata));
   assert(!extInitialCompIsEdge(extdata) || neighbor_base == extdata->tree_root);
   assert(!extInitialCompIsStar(extdata) || neighbor_base == graph->head[extdata->extstack_data[0]]);
   assert(extdata->tree_deg[neighbor_base] >= 1);
   assert(extdata->tree_deg[neighbor] == 0);

   /* NOTE: also initializes bottlenecks from neighbor_base */
   mstLevelLeafInit(graph, neighbor_base, neighbor, extdata);

   /* compute and store SDs to all leaves;
    * also check for bottleneck rule-out! */
   mstLevelLeafSetVerticalSDs(scip, graph, edge2neighbor, extdata, leafRuledOut);

   mstLevelLeafExit(graph, neighbor_base, neighbor, *leafRuledOut, extdata);
}


/** closes vertical part of top MST level for further additions */
void extreduce_mstLevelVerticalClose(
   REDDATA*              reddata             /**< reduction data */
)
{
   MLDISTS* const sds_vertical = reddata->sds_vertical;

   extreduce_mldistsLevelCloseTop(sds_vertical);

#ifdef SCIP_DEBUG
   {
      const int toplevel = extreduce_mldistsTopLevel(sds_vertical);

      SCIPdebugMessage("closing vertical MST level %d, nslots=%d\n",
         toplevel,
         extreduce_mldistsLevelNSlots(sds_vertical, toplevel) );
   }
#endif
}


/** Compute and store horizontal SDs */
// todo we might also check for bottleneck conflicts here!
// and store them in antipairs_start, antipairs_edges
void extreduce_mstLevelHorizontalAdd(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   int                   nextedges,          /**< number of edges for extension */
   const int*            extedges,           /**< array of edges for extension */
   EXTDATA*              extdata             /**< extension data */
)
{
   MLDISTS* const sds_horizontal = extdata->reddata->sds_horizontal;
   const int* const ghead = graph->head;
   const SCIP_Bool isPc = (extdata->pcdata->pcSdToNode != NULL);

   assert(nextedges > 0);
   assert(isPc == graph_pc_isPc(graph));

   extreduce_mldistsLevelAddTop(nextedges, nextedges - 1, sds_horizontal);

   /* tree has not yet been extended, so sds_horizontal is ahead */
   assert(extdata->tree_depth == extreduce_mldistsTopLevel(sds_horizontal) - 1);
   assert(extreduce_mldistsEmptySlotExists(sds_horizontal));

   SCIPdebugMessage("added horizontal level %d \n", extreduce_mldistsTopLevel(sds_horizontal));

   for( int i = 0; i < nextedges; ++i )
   {
      int* const adjids = extreduce_mldistsEmptySlotTargetIds(sds_horizontal);
      SCIP_Real* const adjedgecosts = extreduce_mldistsEmptySlotTargetDists(sds_horizontal);
      const int ext_edge = extedges[i];
      const int ext_head = ghead[ext_edge];

      extreduce_mldistsEmptySlotSetBase(ext_head, sds_horizontal);

      if( isPc )
         pcSdToNodeMark(graph, ext_head, extdata);

      /* for left siblings: use SDs that have already been computed*/
      for( int j = 0; j < i; ++j )
      {
         const int sibling_left = ghead[extedges[j]];
         const SCIP_Real specialDist = extreduce_mldistsTopTargetDist(sds_horizontal, sibling_left, ext_head);

#ifndef NDEBUG
         if( !graph_pc_isPc(graph) )
         {
            const SCIP_Real sd_new = extGetSdDouble(scip, graph, ext_head, sibling_left, extdata);
            assert(EQ(specialDist, sd_new) || (EQ(specialDist, FARAWAY) && EQ(sd_new, -1.0)));
         }
#endif

         adjedgecosts[j] = specialDist;
         adjids[j] = sibling_left;
      }

      /* for right siblings: compute new SDs */
      for( int j = i + 1; j < nextedges; ++j )
      {
         const int sibling_right = ghead[extedges[j]];
         const SCIP_Real specialDist = extGetSdDouble(scip, graph, ext_head, sibling_right, extdata);

         adjedgecosts[j - 1] = (specialDist >= -0.5) ? specialDist : FARAWAY;
         adjids[j - 1] = sibling_right;
      }

      if( isPc )
         pcSdToNodeUnmark(graph, ext_head, extdata);

      extreduce_mldistsEmptySlotSetFilled(sds_horizontal);
   }

   assert(!extreduce_mldistsEmptySlotExists(sds_horizontal));
}


/** Removes top vertical MST level.
 *  NOTE: SDs from level vertices to all leafs will be discarded! */
void extreduce_mstLevelVerticalRemove(
   REDDATA*              reddata             /**< reduction data */
)
{
   MLDISTS* const sds_vertical = reddata->sds_vertical;

   SCIPdebugMessage("remove vertical MST level %d \n", extreduce_mldistsNlevels(sds_vertical));

   extreduce_mldistsLevelRemoveTop(sds_vertical);
}


/** Closes top MST level for further additions.
 *  Will initialize the 'mst_levelbase' MST. */
void extreduce_mstLevelClose(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph */
   int                   extnode,            /**< node from which to extend */
   EXTDATA*              extdata             /**< extension data */
)
{
   REDDATA* const reddata = extdata->reddata;

   assert(extreduce_mstInternalsInSync(extdata));

#ifdef SCIP_DEBUG
   SCIPdebugMessage("close MST level %d, horizontal nslots=%d\n", extreduce_mldistsTopLevel(reddata->sds_horizontal),
         extreduce_mldistsTopLevelNSlots(reddata->sds_horizontal));

   extreduce_printTopLevel(extdata);
#endif

   /* build a new 'mst_levelbase' MST */
   if( extIsAtInitialComp(extdata) )
   {
      assert(extnode == extdata->tree_root);
      mstLevelBuildBaseMstRoot(scip, reddata);
   }
   else
   {
      assert(extnode != extdata->tree_root);
      mstLevelBuildBaseMst(scip, graph, extnode, reddata, extdata);
   }
}


/** Removes top MST level (both vertical and horizontal).
 *  NOTE: SDs from level vertices to all leafs will be discarded! */
void extreduce_mstLevelRemove(
   REDDATA*              reddata             /**< reduction data */
)
{
   CSRDEPO* const msts_levelbase = reddata->msts_levelbase;
   MLDISTS* const sds_vertical = reddata->sds_vertical;
   MLDISTS* const sds_horizontal = reddata->sds_horizontal;
   const int horizontal_nlevels = extreduce_mldistsNlevels(sds_horizontal);
   const int vertical_nlevels = extreduce_mldistsNlevels(sds_vertical);

   assert(horizontal_nlevels == vertical_nlevels || (horizontal_nlevels + 1) == vertical_nlevels);

   SCIPdebugMessage("remove MST level %d \n", vertical_nlevels - 1);

   /* it might happen that the horizontal part has not yet been added */
   if( horizontal_nlevels == vertical_nlevels )
   {
      SCIPdebugMessage("remove horizontal level %d \n", horizontal_nlevels - 1);

      extreduce_mldistsLevelRemoveTop(sds_horizontal);
      graph_csrdepo_removeTop(msts_levelbase);
   }

   assert(graph_csrdepo_getNcsrs(msts_levelbase) == extreduce_mldistsNlevels(sds_horizontal));

   extreduce_mldistsLevelRemoveTop(sds_vertical);
}


/** Returns special distance.
 *  NOTE: Only checks normal distance from vertex1 to vertex2.
 *  I.e., might lead different result if 'vertex1' and 'vertex2' are swapped.
 *  FOR DEBUG CHECKS ONLY! */
SCIP_Real extreduce_extGetSd(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          g,                  /**< graph data structure */
   int                   vertex1,            /**< first vertex */
   int                   vertex2,            /**< second vertex */
   EXTDATA*              extdata             /**< extension data */
)
{
   return extGetSd(scip, g, vertex1, vertex2, extdata);
}

/** Returns special distance.
 *  NOTE: Checks normal distance from vertex2 to vertex1 if no opposite distance is known.
 *  FOR DEBUG CHECKS ONLY! */
SCIP_Real extreduce_extGetSdDouble(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          g,                  /**< graph data structure */
   int                   vertex1,            /**< first vertex */
   int                   vertex2,            /**< second vertex */
   EXTDATA*              extdata             /**< extension data */
)
{
   return extGetSdDouble(scip, g, vertex1, vertex2, extdata);
}


/** Proper SD version of above method. I.e. SD is non-negative, but possibly FARAWAY
 *  FOR DEBUG CHECKS ONLY! */
SCIP_Real extreduce_extGetSdProper(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          g,                  /**< graph data structure */
   int                   vertex1,            /**< first vertex */
   int                   vertex2,            /**< second vertex */
   EXTDATA*              extdata             /**< extension data */
)
{
   return extSdGetProper(extGetSd(scip, g, vertex1, vertex2, extdata));
}


/** Proper SD version of above method. I.e. SD is non-negative, but possibly FARAWAY
 *  FOR DEBUG CHECKS ONLY! */
SCIP_Real extreduce_extGetSdProperDouble(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          g,                  /**< graph data structure */
   int                   vertex1,            /**< first vertex */
   int                   vertex2,            /**< second vertex */
   EXTDATA*              extdata             /**< extension data */
)
{
   return extSdGetProper(extGetSdDouble(scip, g, vertex1, vertex2, extdata));
}
