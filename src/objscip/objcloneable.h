/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2010 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License.             */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma ident "@(#) $Id: objcloneable.h,v 1.4 2010/09/16 17:09:41 bzfheinz Exp $"

/**@file   objcloneable.h
 * @author Michael Winkler
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __SCIP_OBJCLONEABLE_H__
#define __SCIP_OBJCLONEABLE_H__

#include "scip/def.h"
#include "scip/scip.h"

/** all C++ wrapper object plugins should extend this class */
namespace scip
{
   struct ObjCloneable 
   {
      virtual ~ObjCloneable() {}

      /** clone method which will be used to copy a objective plugin */
      virtual ObjCloneable* clone(
         SCIP*                 scip,               /**< SCIP data structure */
         SCIP_Bool*            valid               /**< pointer to store whether to copy is vaild w.r.t. copying dual
                                                    *   reductions */
         ) const 
      { 
         return 0;
      }
      
      /** returns if the objective plugin is copyable */
      virtual SCIP_Bool iscloneable(
         void
         ) const 
      { 
         return false;
      }
   };
}

#endif
