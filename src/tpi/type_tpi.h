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

/**@file   type_tpi.h
 * @ingroup TASKINTERFACE
 * @brief  the type definitions for the task processing interface
 * @author Leona Gottwald
 * @author Stephen J. Maher
 * @author Marc Pfetsch
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __TYPE_TPI_H__
#define __TYPE_TPI_H__

#ifdef __cplusplus
extern "C" {
#endif

/** The status after submitting a job */
enum SCIP_Submitstatus
{
   SCIP_SUBMIT_QUEUEFULL   = -3,
   SCIP_SUBMIT_QUEUECLOSED = -2,
   SCIP_SUBMIT_SHUTDOWN    = -1,
   SCIP_SUBMIT_SUCCESS     = 0
};
typedef enum SCIP_Submitstatus SCIP_SUBMITSTATUS;

/** The job status
 *
 *  There is more than one job per job id. So the job status will return either SCIP_JOB_DOESNOTEXIST or the lowest level
 *  of execution. For example, if there is a job running and a job in the queue, then the return will be
 *  SCIP_JOB_INQUEUE.
 */
enum SCIP_Jobstatus
{
   SCIP_JOB_DOESNOTEXIST   = -1,
   SCIP_JOB_INQUEUE        = 0,
   SCIP_JOB_ISRUNNING      = 1,
   SCIP_JOB_ISFINISHED     = 2
};
typedef enum SCIP_Jobstatus SCIP_JOBSTATUS;

typedef struct SCIP_Job SCIP_JOB;            /**< a job to be submitted to a separate thread */


typedef struct SCIP_Lock SCIP_LOCK;          /**< TPI type dependent lock */
typedef struct SCIP_Condition SCIP_CONDITION;/**< TPI type dependent condition */

#ifdef __cplusplus
}
#endif

#endif
