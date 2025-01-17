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

/**@file   xternal_scheduler.c
 * @brief  main document page
 * @author Stefan Heinz
 * @author Jens Schulz
 */

/*--+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

/**@page SCHEDULER_MAIN Scheduler
 * @author Stefan Heinz
 * @author Jens Schulz
 *
 * This example contains several readers and @subpage LISTHEUR "one primal heuristic" for scheduling
 * problems. Via this example three different type of scheduling problem can be parsed and solved with \SCIP. These are:
 *
 *  - resource-constrained project scheduling problems (RCPSP) (see reader_sm.h)
 *  - resource-constrained project scheduling problem with minimal and maximal time lags (RCPSP/max) (see reader_sch.h)
 *  - pack instances (see reader_rcp.h)
 *
 * Installation
 * ------------
 *
 * See the @ref INSTALL_APPLICATIONS_EXAMPLES "Install file"
 */
