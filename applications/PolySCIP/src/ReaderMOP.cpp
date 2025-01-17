/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*        This file is part of the program PolySCIP                          */
/*                                                                           */
/*    Copyright (C) 2012-2022 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  PolySCIP is distributed under the terms of the ZIB Academic License.     */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with PolySCIP; see the file LICENCE.                               */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**
 * @file ReaderMOP.cpp
 * @brief Class implementing .mop file reader
 * @author Sebastian Schenker
 * @author Timo Strunk
 *
 * Adaption of SCIP MPS reader towards MOP format with multiple objectives.
 * The input file has to follow some simple conventions
 * - It has to contain a problem in
 * <a href="http://en.wikipedia.org/wiki/MPS_%28format%29">MPS</a> format
 * - The file extension must be <code>.mop</code>
 * - Every row marked <code>N</code> is treated as an objective
 */

#include "ReaderMOP.h"

#include <iostream>
#include <assert.h>
#include <string.h>
#include <ctype.h>

#include "objscip/objscip.h"
#include "prob_data_objectives.h"
#include "scip/cons_knapsack.h"
#include "scip/cons_indicator.h"
#include "scip/cons_linear.h"
#include "scip/cons_logicor.h"
#include "scip/cons_setppc.h"
#include "scip/cons_varbound.h"
#include "scip/cons_sos1.h"
#include "scip/cons_sos2.h"
#include "scip/cons_nonlinear.h"
#include "scip/cons_bounddisjunction.h"
#include "scip/pub_misc.h"

#define MPS_MAX_LINELEN  1024 ///< global define
#define MPS_MAX_NAMELEN   256 ///< global define
#define MPS_MAX_VALUELEN   26 ///< global define
#define MPS_MAX_FIELDLEN   20 ///< global define

#define PATCH_CHAR    '_' ///< global define
#define BLANK         ' ' ///< global define

/** enum containing all mps sections */
enum MpsSection
{
   MPS_NAME,
   MPS_OBJSEN,
   MPS_OBJNAME,
   MPS_ROWS,
   MPS_USERCUTS,
   MPS_LAZYCONS,
   MPS_COLUMNS,
   MPS_RHS,
   MPS_RANGES,
   MPS_BOUNDS,
   MPS_SOS,
   MPS_QUADOBJ,
   MPS_QMATRIX,
   MPS_QCMATRIX,
   MPS_INDICATORS,
   MPS_ENDATA
};
typedef enum MpsSection MPSSECTION; ///< typedef

/** mps input structure */
struct MpsInput
{
   MPSSECTION            section;  ///< MpsSection enum
   SCIP_FILE*            fp; ///< SCIP file pointer
   int                   lineno; ///< line number
   SCIP_OBJSENSE         objsense; ///< Objective sense
   SCIP_Bool             haserror; ///< Indicates error
   char                  buf[MPS_MAX_LINELEN]; ///< character
   const char*           f0; ///< @todo
   const char*           f1; ///< @todo
   const char*           f2; ///< @todo
   const char*           f3; ///< @todo
   const char*           f4; ///< @todo
   const char*           f5; ///< @todo
   char                  probname[MPS_MAX_NAMELEN]; ///< problem name
   char                  objname [MPS_MAX_NAMELEN]; ///< objective identifier
   SCIP_Bool             isinteger; ///< Indicates integer
   SCIP_Bool             isnewformat; ///< Indicates new MPS format
};
typedef struct MpsInput MPSINPUT; ///< typedef

/** sparse matrix representation */
struct SparseMatrix
{
   SCIP_Real*            values;             /**< matrix element */
   SCIP_VAR**            columns;            /**< corresponding variables */
   const char**          rows;               /**< corresponding constraint names */ 
   int                   nentries;           /**< number of elements in the arrays */
   int                   sentries;           /**< number of slots in the arrays */
};
typedef struct SparseMatrix SPARSEMATRIX; ///< typedef

/** creates the mps input structure */
static
SCIP_RETCODE mpsinputCreate(
   SCIP*                 scip,               /**< SCIP data structure */
   MPSINPUT**            mpsi,               /**< mps input structure */
   SCIP_FILE*            fp                  /**< file object for the input file */
   )
{
   assert(mpsi != NULL);
   assert(fp != NULL);

   SCIP_CALL( SCIPallocBlockMemory(scip, mpsi) );

   (*mpsi)->section     = MPS_NAME;
   (*mpsi)->fp          = fp;
   (*mpsi)->lineno      = 0;
   (*mpsi)->objsense    = SCIP_OBJSENSE_MINIMIZE;
   (*mpsi)->haserror    = FALSE;
   (*mpsi)->isinteger   = FALSE;
   (*mpsi)->isnewformat = FALSE;
   (*mpsi)->buf     [0] = '\0';
   (*mpsi)->probname[0] = '\0';
   (*mpsi)->objname [0] = '\0';
   (*mpsi)->f0          = NULL;
   (*mpsi)->f1          = NULL;
   (*mpsi)->f2          = NULL;
   (*mpsi)->f3          = NULL;
   (*mpsi)->f4          = NULL;
   (*mpsi)->f5          = NULL;

   return SCIP_OKAY;
}

/** free the mps input structure */
static
void mpsinputFree(
   SCIP*                 scip,               /**< SCIP data structure */
   MPSINPUT**            mpsi                /**< mps input structure */
   )
{
   SCIPfreeBlockMemory(scip, mpsi);
}

/** returns the current section */
static
MPSSECTION mpsinputSection(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->section;
}

/** return the current value of field 0 */
static
const char* mpsinputField0(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->f0;
}

/** return the current value of field 1 */
static
const char* mpsinputField1(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->f1;
}

/** return the current value of field 2 */
static
const char* mpsinputField2(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->f2;
}

/** return the current value of field 3 */
static
const char* mpsinputField3(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->f3;
}

/** return the current value of field 4 */
static
const char* mpsinputField4(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->f4;
}

/** return the current value of field 5 */
static
const char* mpsinputField5(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->f5;
}

/** returns the objective sense */
static
SCIP_OBJSENSE mpsinputObjsense(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->objsense;
}

/** returns if an error was detected */
static
SCIP_Bool mpsinputHasError(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->haserror;
}

/** returns the value of the Bool "is integer" in the mps input */
static
SCIP_Bool mpsinputIsInteger(
   const MPSINPUT*       mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   return mpsi->isinteger;
}

/** set the section in the mps input structure to given section */
static
void mpsinputSetSection(
   MPSINPUT*             mpsi,               /**< mps input structure */
   MPSSECTION            section             /**< section that is set */
   )
{
   assert(mpsi != NULL);

   mpsi->section = section;
}

/** set the problem name in the mps input structure to given problem name */
static
void mpsinputSetProbname(
   MPSINPUT*             mpsi,               /**< mps input structure */
   const char*           probname            /**< name of the problem to set */
   )
{
   assert(mpsi     != NULL);
   assert(probname != NULL);
   assert(strlen(probname) < sizeof(mpsi->probname));

   (void)SCIPmemccpy(mpsi->probname, probname, '\0', MPS_MAX_NAMELEN - 1);
}

/** set the objective name in the mps input structure to given objective name */
static
void mpsinputSetObjname(
   MPSINPUT*             mpsi,               /**< mps input structure */
   const char*           objname             /**< name of the objective function to set */
   )
{
   assert(mpsi != NULL);
   assert(objname != NULL);
   assert(strlen(objname) < sizeof(mpsi->objname));

   (void)SCIPmemccpy(mpsi->objname, objname, '\0', MPS_MAX_NAMELEN - 1);
}

/** set the objective sense in the mps input structure to given objective sense */
static
void mpsinputSetObjsense(
   MPSINPUT*             mpsi,               /**< mps input structure */
   SCIP_OBJSENSE         sense               /**< sense of the objective function */
   )
{
   assert(mpsi != NULL);

   mpsi->objsense = sense;
}

static
void mpsinputSyntaxerror(
   MPSINPUT*             mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   SCIPerrorMessage("Syntax error in line %d\n", mpsi->lineno);
   mpsi->section  = MPS_ENDATA;
   mpsi->haserror = TRUE;
}

/** method post a ignore message  */
static
void mpsinputEntryIgnored(
   SCIP*                 scip,               /**< SCIP data structure */
   MPSINPUT*             mpsi,               /**< mps input structure */
   const char*           what,               /**< what get ignored */
   const char*           what_name,          /**< name of that object */
   const char*           entity,             /**< entity */
   const char*           entity_name,        /**< entity name */
   SCIP_VERBLEVEL        verblevel           /**< SCIP verblevel for this message */
   )
{
   assert(mpsi        != NULL);
   assert(what        != NULL);
   assert(what_name   != NULL);
   assert(entity      != NULL);
   assert(entity_name != NULL);

   SCIPverbMessage(scip, verblevel, NULL,
      "Warning line %d: %s \"%s\" for %s \"%s\" ignored\n", mpsi->lineno, what, what_name, entity, entity_name);
}

/** fill the line from \p pos up to column 80 with blanks. */
static
void clearFrom(
   char*                 buf,                /**< buffer to clear */
   unsigned int          pos                 /**< position to start the clearing process */
   )
{
   unsigned int i;

   for(i = pos; i < 80; i++)
      buf[i] = BLANK;
   buf[80] = '\0';
}

/** change all blanks inside a field to #PATCH_CHAR. */
static
void patchField(
   char*                 buf,                /**< buffer to patch */
   int                   beg,                /**< position to begin */
   int                   end                 /**< position to end */
   )
{
   int i;

   while( (beg <= end) && (buf[end] == BLANK) )
      end--;

   while( (beg <= end) && (buf[beg] == BLANK) )
      beg++;

   for( i = beg; i <= end; i++ )
      if( buf[i] == BLANK )
         buf[i] = PATCH_CHAR;
}

/** read a mps format data line and parse the fields. */
static
SCIP_Bool mpsinputReadLine(
   MPSINPUT*             mpsi                /**< mps input structure */
   )
{
   unsigned int len;
   unsigned int i;
   int space;
   char* s;
   SCIP_Bool is_marker;
   SCIP_Bool is_empty;
   char* nexttok;

   do
   {
      mpsi->f0 = mpsi->f1 = mpsi->f2 = mpsi->f3 = mpsi->f4 = mpsi->f5 = 0;
      is_marker = FALSE;

      /* Read until we have not a comment line. */
      do
      {
         mpsi->buf[MPS_MAX_LINELEN-1] = '\0';
         if( NULL == SCIPfgets(mpsi->buf, sizeof(mpsi->buf), mpsi->fp) )
            return FALSE;
         mpsi->lineno++;
      }
      while( *mpsi->buf == '*' );

      /* Normalize line */
      len = strlen(mpsi->buf);

      for( i = 0; i < len; i++ )
         if( (mpsi->buf[i] == '\t') || (mpsi->buf[i] == '\n') || (mpsi->buf[i] == '\r') )
            mpsi->buf[i] = BLANK;

      if( len < 80 )
         clearFrom(mpsi->buf, len);

      SCIPdebugMessage("line %d: <%s>\n", mpsi->lineno, mpsi->buf);

      assert(strlen(mpsi->buf) >= 80);

      /* Look for new section */
      if( *mpsi->buf != BLANK )
      {
         mpsi->f0 = SCIPstrtok(&mpsi->buf[0], " ", &nexttok);

         assert(mpsi->f0 != 0);

         mpsi->f1 = SCIPstrtok(NULL, " ", &nexttok);

         return TRUE;
      }

      /* If we decide to use the new format we never revert this decision */
      if( !mpsi->isnewformat )
      {
         /* Test for fixed format comments */
         if( (mpsi->buf[14] == '$') && (mpsi->buf[13] == ' ') )
            clearFrom(mpsi->buf, 14);
         else if( (mpsi->buf[39] == '$') && (mpsi->buf[38] == ' ') )
            clearFrom(mpsi->buf, 39);

         /* Test for fixed format */
         space = mpsi->buf[12] | mpsi->buf[13]
            | mpsi->buf[22] | mpsi->buf[23]
            | mpsi->buf[36] | mpsi->buf[37] | mpsi->buf[38]
            | mpsi->buf[47] | mpsi->buf[48]
            | mpsi->buf[61] | mpsi->buf[62] | mpsi->buf[63];

         if( space == BLANK )
         {
            /* Now we have space at the right positions.
             * But are there also the non space where they
             * should be ?
             */
            SCIP_Bool number;
            
            number = isdigit((unsigned char)mpsi->buf[24]) || isdigit((unsigned char)mpsi->buf[25])
               || isdigit((unsigned char)mpsi->buf[26]) || isdigit((unsigned char)mpsi->buf[27])
               || isdigit((unsigned char)mpsi->buf[28]) || isdigit((unsigned char)mpsi->buf[29])
               || isdigit((unsigned char)mpsi->buf[30]) || isdigit((unsigned char)mpsi->buf[31])
               || isdigit((unsigned char)mpsi->buf[32]) || isdigit((unsigned char)mpsi->buf[33])
               || isdigit((unsigned char)mpsi->buf[34]) || isdigit((unsigned char)mpsi->buf[35]);

            /* len < 14 is handle ROW lines with embedded spaces
             * in the names correctly
             */
            if( number || len < 14 )
            {
               /* We assume fixed format, so we patch possible embedded spaces. */
               patchField(mpsi->buf,  4, 12);
               patchField(mpsi->buf, 14, 22);
               patchField(mpsi->buf, 39, 47);
            }
            else
            {
               if( mpsi->section == MPS_COLUMNS || mpsi->section == MPS_RHS
                  || mpsi->section == MPS_RANGES  || mpsi->section == MPS_BOUNDS )
                  mpsi->isnewformat = TRUE;
            }
         }
         else
         {
            mpsi->isnewformat = TRUE;
         }
      }
      s = &mpsi->buf[1];

      /* At this point it is not clear if we have a indicator field.
       * If there is none (e.g. empty) f1 will be the first name field.
       * If there is one, f2 will be the first name field.
       *
       * Initially comment marks '$' are only allowed in the beginning
       * of the 2nd and 3rd name field. We test all fields but the first.
       * This makes no difference, since if the $ is at the start of a value
       * field, the line will be erroneous anyway.
       */
      do
      {
         if( NULL == (mpsi->f1 = SCIPstrtok(s, " ", &nexttok)) )
            break;

         if( (NULL == (mpsi->f2 = SCIPstrtok(NULL, " ", &nexttok))) || (*mpsi->f2 == '$') )
         {
            mpsi->f2 = 0;
            break;
         }
         if( !strcmp(mpsi->f2, "'MARKER'") )
            is_marker = TRUE;

         if( (NULL == (mpsi->f3 = SCIPstrtok(NULL, " ", &nexttok))) || (*mpsi->f3 == '$') )
         {
            mpsi->f3 = 0;
            break;
         }
         if( is_marker )
         {
            if( !strcmp(mpsi->f3, "'INTORG'") )
               mpsi->isinteger = TRUE;
            else if( !strcmp(mpsi->f3, "'INTEND'") )
               mpsi->isinteger = FALSE;
            else
               break; /* unknown marker */
         }
         if( !strcmp(mpsi->f3, "'MARKER'") )
            is_marker = TRUE;

         if( (NULL == (mpsi->f4 = SCIPstrtok(NULL, " ", &nexttok))) || (*mpsi->f4 == '$') )
         {
            mpsi->f4 = 0;
            break;
         }
         if( is_marker )
         {
            if( !strcmp(mpsi->f4, "'INTORG'") )
               mpsi->isinteger = TRUE;
            else if( !strcmp(mpsi->f4, "'INTEND'") )
               mpsi->isinteger = FALSE;
            else
               break; /* unknown marker */
         }
         if( (NULL == (mpsi->f5 = SCIPstrtok(NULL, " ", &nexttok))) || (*mpsi->f5 == '$') )
            mpsi->f5 = 0;
      }
      while( FALSE );

      /* check for empty lines */
      is_empty = (mpsi->f0 == NULL && mpsi->f1 == NULL);
   }
   while( is_marker || is_empty );

   return TRUE;
}

/** Insert \p name as field 1 or 2 and shift all other fields up. */
static
void mpsinputInsertName(
   MPSINPUT*             mpsi,               /**< mps input structure */
   const char*           name,               /**< name to insert */
   SCIP_Bool             second              /**< insert as second field? */
   )
{
   assert(mpsi != NULL);
   assert(name != NULL);

   mpsi->f5 = mpsi->f4;
   mpsi->f4 = mpsi->f3;
   mpsi->f3 = mpsi->f2;

   if( second )
      mpsi->f2 = name;
   else
   {
      mpsi->f2 = mpsi->f1;
      mpsi->f1 = name;
   }
}

/** Process NAME section. */
static
SCIP_RETCODE readName(
   SCIP*                 scip,               /**< SCIP data structure */
   MPSINPUT*             mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   SCIPdebugMessage("read problem name\n");

   /* This has to be the Line with the NAME section. */
   if( !mpsinputReadLine(mpsi) || mpsinputField0(mpsi) == NULL || strcmp(mpsinputField0(mpsi), "NAME") )
   {
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }

   /* Sometimes the name is omitted. */
   mpsinputSetProbname(mpsi, (mpsinputField1(mpsi) == 0) ? "_MOP_" : mpsinputField1(mpsi));

   /* This hat to be a new section */
   if( !mpsinputReadLine(mpsi) || (mpsinputField0(mpsi) == NULL) )
   {
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }

   if( !strncmp(mpsinputField0(mpsi), "ROWS", 4) )
      mpsinputSetSection(mpsi, MPS_ROWS);
   else if( !strncmp(mpsinputField0(mpsi), "USERCUTS", 8) )
      mpsinputSetSection(mpsi, MPS_USERCUTS);
   else if( !strncmp(mpsinputField0(mpsi), "LAZYCONS", 8) )
      mpsinputSetSection(mpsi, MPS_LAZYCONS);
   else if( !strncmp(mpsinputField0(mpsi), "OBJSEN", 6) )
      mpsinputSetSection(mpsi, MPS_OBJSEN);
   else if( !strncmp(mpsinputField0(mpsi), "OBJNAME", 7) )
      mpsinputSetSection(mpsi, MPS_OBJNAME);
   else
   {
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }

   return SCIP_OKAY;
}

/** Process OBJSEN section. This Section is a CPLEX extension. */
static
SCIP_RETCODE readObjsen(
   SCIP*                 scip,               /**< SCIP data structure */
   MPSINPUT*             mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   SCIPdebugMessage("read objective sense\n");

   /* This has to be the Line with MIN or MAX. */
   if( !mpsinputReadLine(mpsi) || (mpsinputField1(mpsi) == NULL) )
   {
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }

   if( !strncmp(mpsinputField1(mpsi), "MIN", 3) )
      mpsinputSetObjsense(mpsi, SCIP_OBJSENSE_MINIMIZE);
   else if( !strncmp(mpsinputField1(mpsi), "MAX", 3) )
      mpsinputSetObjsense(mpsi, SCIP_OBJSENSE_MAXIMIZE);
   else
   {
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }

   /* Look for ROWS, USERCUTS, LAZYCONS, or OBJNAME Section */
   if( !mpsinputReadLine(mpsi) || mpsinputField0(mpsi) == NULL )
   {
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }

   if( !strcmp(mpsinputField0(mpsi), "ROWS") )
      mpsinputSetSection(mpsi, MPS_ROWS);
   else if( !strcmp(mpsinputField0(mpsi), "USERCUTS") )
      mpsinputSetSection(mpsi, MPS_USERCUTS);
   else if( !strcmp(mpsinputField0(mpsi), "LAZYCONS") )
      mpsinputSetSection(mpsi, MPS_LAZYCONS);
   else if( !strcmp(mpsinputField0(mpsi), "OBJNAME") )
      mpsinputSetSection(mpsi, MPS_OBJNAME);
   else
   {
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }

   return SCIP_OKAY;
}

/** Process OBJNAME section. This Section is a CPLEX extension. */
static
SCIP_RETCODE readObjname(
   SCIP*                 scip,               /**< SCIP data structure */
   MPSINPUT*             mpsi                /**< mps input structure */
   )
{
   assert(mpsi != NULL);

   SCIPdebugMessage("read objective name\n");
   
   /* This has to be the Line with the name. */
   if( !mpsinputReadLine(mpsi) || mpsinputField1(mpsi) == NULL )
   {
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }

   mpsinputSetObjname(mpsi, mpsinputField1(mpsi));

   /* Look for ROWS, USERCUTS, or LAZYCONS Section */
   if( !mpsinputReadLine(mpsi) || mpsinputField0(mpsi) == NULL )
   {
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }
   if( !strcmp(mpsinputField0(mpsi), "ROWS") )
      mpsinputSetSection(mpsi, MPS_ROWS);
   else if( !strcmp(mpsinputField0(mpsi), "USERCUTS") )
      mpsinputSetSection(mpsi, MPS_USERCUTS);
   else if( !strcmp(mpsinputField0(mpsi), "LAZYCONS") )
      mpsinputSetSection(mpsi, MPS_LAZYCONS);
   else
      mpsinputSyntaxerror(mpsi);

   return SCIP_OKAY;
}

/** Process RHS section. */
static
SCIP_RETCODE readRhs(
   MPSINPUT*             mpsi,               /**< mps input structure */
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   char        rhsname[MPS_MAX_NAMELEN] = { '\0' };
   SCIP_CONS*  cons;
   SCIP_Real   lhs;
   SCIP_Real   rhs;
   SCIP_Real   val;

   SCIPdebugMessage("read right hand sides\n");

   while( mpsinputReadLine(mpsi) )
   {
      if( mpsinputField0(mpsi) != NULL )
      {
         if( !strcmp(mpsinputField0(mpsi), "RANGES") )
            mpsinputSetSection(mpsi, MPS_RANGES);
         else if( !strcmp(mpsinputField0(mpsi), "BOUNDS") )
            mpsinputSetSection(mpsi, MPS_BOUNDS);
         else if( !strcmp(mpsinputField0(mpsi), "SOS") )
            mpsinputSetSection(mpsi, MPS_SOS);
         else if( !strcmp(mpsinputField0(mpsi), "QMATRIX") )
            mpsinputSetSection(mpsi, MPS_QMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "QUADOBJ") )
            mpsinputSetSection(mpsi, MPS_QUADOBJ);
         else if( !strcmp(mpsinputField0(mpsi), "QCMATRIX") )
            mpsinputSetSection(mpsi, MPS_QCMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "INDICATORS") )
            mpsinputSetSection(mpsi, MPS_INDICATORS);
         else if( !strcmp(mpsinputField0(mpsi), "ENDATA") )
            mpsinputSetSection(mpsi, MPS_ENDATA);
         else
            break;
         return SCIP_OKAY;
      }
      if( (mpsinputField2(mpsi) != NULL && mpsinputField3(mpsi) == NULL)
         || (mpsinputField4(mpsi) != NULL && mpsinputField5(mpsi) == NULL) )
         mpsinputInsertName(mpsi, "_RHS_", FALSE);

      if( mpsinputField1(mpsi) == NULL || mpsinputField2(mpsi) == NULL || mpsinputField3(mpsi) == NULL )
         break;

      if( *rhsname == '\0' )
	 (void)SCIPmemccpy(rhsname, mpsinputField1(mpsi), '\0', MPS_MAX_NAMELEN - 1);

      if( !strcmp(rhsname, mpsinputField1(mpsi)) )
      {
         cons = SCIPfindCons(scip, mpsinputField2(mpsi));
         if( cons == NULL )
            mpsinputEntryIgnored(scip, mpsi, "RHS", mpsinputField1(mpsi), "row", mpsinputField2(mpsi), SCIP_VERBLEVEL_NORMAL);
         else
         {
            val = atof(mpsinputField3(mpsi));

            /* find out the row sense */
            lhs = SCIPgetLhsLinear(scip, cons);
            rhs = SCIPgetRhsLinear(scip, cons);
            if( SCIPisInfinity(scip, -lhs) )
            {
               /* lhs = -infinity -> lower or equal */
               assert(SCIPisZero(scip, rhs));
               SCIP_CALL( SCIPchgRhsLinear(scip, cons, val) );
            }
            else if( SCIPisInfinity(scip, rhs) )
            {
               /* rhs = +infinity -> greater or equal */
               assert(SCIPisZero(scip, lhs));
               SCIP_CALL( SCIPchgLhsLinear(scip, cons, val) );
            }
            else
            {
               /* lhs > -infinity, rhs < infinity -> equality */
               assert(SCIPisZero(scip, lhs));
               assert(SCIPisZero(scip, rhs));
               SCIP_CALL( SCIPchgLhsLinear(scip, cons, val) );
               SCIP_CALL( SCIPchgRhsLinear(scip, cons, val) );
            }
            SCIPdebugMessage("RHS <%s> lhs: %g  rhs: %g  val: <%22.12g>\n", mpsinputField2(mpsi), lhs, rhs, val);
         }
         if( mpsinputField5(mpsi) != NULL )
         {
            cons = SCIPfindCons(scip, mpsinputField4(mpsi));
            if( cons == NULL )
               mpsinputEntryIgnored(scip, mpsi, "RHS", mpsinputField1(mpsi), "row", mpsinputField4(mpsi), SCIP_VERBLEVEL_NORMAL);
            else
            {
               val = atof(mpsinputField5(mpsi));

               /* find out the row sense */
               lhs = SCIPgetLhsLinear(scip, cons);
               rhs = SCIPgetRhsLinear(scip, cons);
               if( SCIPisInfinity(scip, -lhs) )
               {
                  /* lhs = -infinity -> lower or equal */
                  assert(SCIPisZero(scip, rhs));
                  SCIP_CALL( SCIPchgRhsLinear(scip, cons, val) );
               }
               else if( SCIPisInfinity(scip, rhs) )
               {
                  /* rhs = +infinity -> greater or equal */
                  assert(SCIPisZero(scip, lhs));
                  SCIP_CALL( SCIPchgLhsLinear(scip, cons, val) );
               }
               else
               {
                  /* lhs > -infinity, rhs < infinity -> equality */
                  assert(SCIPisZero(scip, lhs));
                  assert(SCIPisZero(scip, rhs));
                  SCIP_CALL( SCIPchgLhsLinear(scip, cons, val) );
                  SCIP_CALL( SCIPchgRhsLinear(scip, cons, val) );
               }
               SCIPdebugMessage("RHS <%s> lhs: %g  rhs: %g  val: <%22.12g>\n", mpsinputField4(mpsi), lhs, rhs, val);
            }
         }
      }
   }
   mpsinputSyntaxerror(mpsi);

   return SCIP_OKAY;
}

/** Process RANGES section */
static
SCIP_RETCODE readRanges(
   MPSINPUT*             mpsi,               /**< mps input structure */
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   char        rngname[MPS_MAX_NAMELEN] = { '\0' };
   SCIP_CONS*  cons;
   SCIP_Real   lhs;
   SCIP_Real   rhs;
   SCIP_Real   val;

   SCIPdebugMessage("read ranges\n");

   while( mpsinputReadLine(mpsi) )
   {
      if( mpsinputField0(mpsi) != NULL )
      {
         if( !strcmp(mpsinputField0(mpsi), "BOUNDS") )
            mpsinputSetSection(mpsi, MPS_BOUNDS);
         else if( !strcmp(mpsinputField0(mpsi), "SOS") )
            mpsinputSetSection(mpsi, MPS_SOS);
         else if( !strcmp(mpsinputField0(mpsi), "QMATRIX") )
            mpsinputSetSection(mpsi, MPS_QMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "QUADOBJ") )
            mpsinputSetSection(mpsi, MPS_QUADOBJ);
         else if( !strcmp(mpsinputField0(mpsi), "QCMATRIX") )
            mpsinputSetSection(mpsi, MPS_QCMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "INDICATORS") )
            mpsinputSetSection(mpsi, MPS_INDICATORS);
         else if( !strcmp(mpsinputField0(mpsi), "ENDATA") )
            mpsinputSetSection(mpsi, MPS_ENDATA);
         else
            break;
         return SCIP_OKAY;
      }
      if( (mpsinputField2(mpsi) != NULL && mpsinputField3(mpsi) == NULL)
         || (mpsinputField4(mpsi) != NULL && mpsinputField5(mpsi) == NULL) )
         mpsinputInsertName(mpsi, "_RNG_", FALSE);

      if( mpsinputField1(mpsi) == NULL || mpsinputField2(mpsi) == NULL || mpsinputField3(mpsi) == NULL )
         break;

      if( *rngname == '\0' )
	 (void)SCIPmemccpy(rngname, mpsinputField1(mpsi), '\0', MPS_MAX_NAMELEN - 1);

      /* The rules are:
       * Row Sign   LHS             RHS
       * ----------------------------------------
       *  G   +/-   rhs             rhs + |range|
       *  L   +/-   rhs - |range|   rhs
       *  E   +     rhs             rhs + range
       *  E   -     rhs + range     rhs
       * ----------------------------------------
       */
      if( !strcmp(rngname, mpsinputField1(mpsi)) )
      {
         cons = SCIPfindCons(scip, mpsinputField2(mpsi));
         if( cons == NULL )
            mpsinputEntryIgnored(scip, mpsi, "Range", mpsinputField1(mpsi), "row", mpsinputField2(mpsi), SCIP_VERBLEVEL_NORMAL);
         else
         {
            val = atof(mpsinputField3(mpsi));

            /* find out the row sense */
            lhs = SCIPgetLhsLinear(scip, cons);
            rhs = SCIPgetRhsLinear(scip, cons);
            if( SCIPisInfinity(scip, -lhs) )
            {
               /* lhs = -infinity -> lower or equal */
               SCIP_CALL( SCIPchgLhsLinear(scip, cons, rhs - REALABS(val)) );
            }
            else if( SCIPisInfinity(scip, rhs) )
            {
               /* rhs = +infinity -> greater or equal */
               SCIP_CALL( SCIPchgRhsLinear(scip, cons, lhs + REALABS(val)) );
            }
            else
            {
               /* lhs > -infinity, rhs < infinity -> equality */
               assert(SCIPisEQ(scip, lhs, rhs));
               if( val >= 0.0 )
               {
                  SCIP_CALL( SCIPchgRhsLinear(scip, cons, rhs + val) );
               }
               else
               {
                  SCIP_CALL( SCIPchgLhsLinear(scip, cons, lhs + val) );
               }
            }
         }
         if( mpsinputField5(mpsi) != NULL )
         {
            cons = SCIPfindCons(scip, mpsinputField4(mpsi));
            if( cons == NULL )
               mpsinputEntryIgnored(scip, mpsi, "Range", mpsinputField1(mpsi), "row", mpsinputField4(mpsi), SCIP_VERBLEVEL_NORMAL);
            else
            {
               val = atof(mpsinputField5(mpsi));

               /* find out the row sense */
               lhs = SCIPgetLhsLinear(scip, cons);
               rhs = SCIPgetRhsLinear(scip, cons);
               if( SCIPisInfinity(scip, -lhs) )
               {
                  /* lhs = -infinity -> lower or equal */
                  SCIP_CALL( SCIPchgLhsLinear(scip, cons, rhs - REALABS(val)) );
               }
               else if( SCIPisInfinity(scip, rhs) )
               {
                  /* rhs = +infinity -> greater or equal */
                  SCIP_CALL( SCIPchgRhsLinear(scip, cons, lhs + REALABS(val)) );
               }
               else
               {
                  /* lhs > -infinity, rhs < infinity -> equality */
                  assert(SCIPisEQ(scip, lhs, rhs));
                  if( val >= 0.0 )
                  {
                     SCIP_CALL( SCIPchgRhsLinear(scip, cons, rhs + val) );
                  }
                  else
                  {
                     SCIP_CALL( SCIPchgLhsLinear(scip, cons, lhs + val) );
                  }
               }
            }
         }
      }
   }
   mpsinputSyntaxerror(mpsi);

   return SCIP_OKAY;
}

/** Process BOUNDS section. */
static
SCIP_RETCODE readBounds(
   MPSINPUT*             mpsi,               /**< mps input structure */
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   char        bndname[MPS_MAX_NAMELEN] = { '\0' };
   SCIP_VAR*   var;
   SCIP_Real   val;
   SCIP_Bool   shifted;

   SCIP_VAR** semicont;
   int nsemicont;
   int semicontsize;
   SCIP_Bool dynamiccols;
   SCIP_Bool dynamicconss;

   semicont = NULL;
   nsemicont = 0;
   semicontsize = 0;  

   SCIP_CALL( SCIPgetBoolParam(scip, "reading/dynamiccols", &dynamiccols) );
   SCIP_CALL( SCIPgetBoolParam(scip, "reading/dynamicconss", &dynamicconss) );

   SCIPdebugMessage("read bounds\n");

   while( mpsinputReadLine(mpsi) )
   {
      if( mpsinputField0(mpsi) != 0 )
      {
         if( !strcmp(mpsinputField0(mpsi), "SOS") )
            mpsinputSetSection(mpsi, MPS_SOS);
         else if( !strcmp(mpsinputField0(mpsi), "QMATRIX") )
            mpsinputSetSection(mpsi, MPS_QMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "QUADOBJ") )
            mpsinputSetSection(mpsi, MPS_QUADOBJ);
         else if( !strcmp(mpsinputField0(mpsi), "QCMATRIX") )
            mpsinputSetSection(mpsi, MPS_QCMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "INDICATORS") )
            mpsinputSetSection(mpsi, MPS_INDICATORS);
         else if( !strcmp(mpsinputField0(mpsi), "ENDATA") )
            mpsinputSetSection(mpsi, MPS_ENDATA);
         else
            break;
         goto READBOUNDS_FINISH;
      }

      shifted = FALSE;

      /* Is the value field used ? */
      if( !strcmp(mpsinputField1(mpsi), "LO")  /* lower bound given in field 4 */
         || !strcmp(mpsinputField1(mpsi), "UP")  /* upper bound given in field 4 */
         || !strcmp(mpsinputField1(mpsi), "FX")  /* fixed value given in field 4 */
         || !strcmp(mpsinputField1(mpsi), "LI")  /* CPLEX extension: lower bound of integer variable given in field 4 */
         || !strcmp(mpsinputField1(mpsi), "UI")  /* CPLEX extension: upper bound of integer variable given in field 4 */
         || !strcmp(mpsinputField1(mpsi), "SC") )/* CPLEX extension: semi continuous variable, upper bound given in field 4 */
      {
         if( mpsinputField3(mpsi) != NULL && mpsinputField4(mpsi) == NULL )
         {
            mpsinputInsertName(mpsi, "_BND_", TRUE);
            shifted = TRUE;
         }
      }
      else if( !strcmp(mpsinputField1(mpsi), "FR") /* free variable */
         || !strcmp(mpsinputField1(mpsi), "MI")    /* lower bound is minus infinity */
         || !strcmp(mpsinputField1(mpsi), "PL")    /* upper bound is plus infinity */
         || !strcmp(mpsinputField1(mpsi), "BV") )  /* CPLEX extension: binary variable */
      {
         if( mpsinputField2(mpsi) != NULL && mpsinputField3(mpsi) == NULL )
         {
            mpsinputInsertName(mpsi, "_BND_", TRUE);
            shifted = TRUE;
         }
      }
      else
      {
         mpsinputSyntaxerror(mpsi);
         return SCIP_OKAY;
      }

      if( mpsinputField1(mpsi) == NULL || mpsinputField2(mpsi) == NULL || mpsinputField3(mpsi) == NULL )
         break;

      if( *bndname == '\0' )
	 (void)SCIPmemccpy(bndname, mpsinputField2(mpsi), '\0', MPS_MAX_NAMELEN - 1);

      /* Only read the first Bound in section */
      if( !strcmp(bndname, mpsinputField2(mpsi)) )
      {
         SCIP_Bool infeasible;

         var = SCIPfindVar(scip, mpsinputField3(mpsi));
         /* if variable did not appear in columns section before, then it may still come in later sections (QCMATRIX, QMATRIX, SOS, ...)
          * thus add it as continuous variables, which has default bounds 0.0 <= x, and default cost 0.0 */
         if( var == NULL )
         {
            SCIP_VAR* varcpy;

            SCIP_CALL( SCIPcreateVar(scip, &var, mpsinputField3(mpsi), 0.0, SCIPinfinity(scip), 0.0, 
                  SCIP_VARTYPE_CONTINUOUS, !dynamiccols, dynamiccols, NULL, NULL, NULL, NULL, NULL) );

            SCIP_CALL( SCIPaddVar(scip, var) );
            varcpy = var;
            SCIP_CALL( SCIPreleaseVar(scip, &varcpy) );
            /* mpsinputEntryIgnored(scip, mpsi, "column", mpsinputField3(mpsi), "bound", bndname, SCIP_VERBLEVEL_NORMAL); */
         }
         assert(var != NULL);

         if( mpsinputField4(mpsi) == NULL )
            val = 0.0;
         else
            val = atof(mpsinputField4(mpsi));

         /* if a bound of a binary variable is given, the variable is converted into an integer variable
          * with default bounds 0 <= x <= infinity
          */
         if( SCIPvarGetType(var) == SCIP_VARTYPE_BINARY )
         {
            if( (mpsinputField1(mpsi)[1] == 'I') /* CPLEX extension (Integer Bound) */
               || (!(mpsinputField1(mpsi)[0] == 'L' && SCIPisFeasEQ(scip, val, 0.0))
                  && !(mpsinputField1(mpsi)[0] == 'U' && SCIPisFeasEQ(scip, val, 1.0))) )
            {
               assert(SCIPisFeasEQ(scip, SCIPvarGetLbGlobal(var), 0.0));
               assert(SCIPisFeasEQ(scip, SCIPvarGetUbGlobal(var), 1.0));
               SCIP_CALL( SCIPchgVarType(scip, var, SCIP_VARTYPE_INTEGER, &infeasible) );
               /* don't assert feasibility here because the presolver will and should detect a infeasibility */
               SCIP_CALL( SCIPchgVarUb(scip, var, SCIPinfinity(scip)) );
            }
         }

         switch( mpsinputField1(mpsi)[0] )
         {
         case 'L':
            if( mpsinputField1(mpsi)[1] == 'I' ) /* CPLEX extension (Integer Bound) */
            {
               SCIP_CALL( SCIPchgVarType(scip, var, SCIP_VARTYPE_INTEGER, &infeasible) );
               /* don't assert feasibility here because the presolver will and should detect a infeasibility */
            }
            SCIP_CALL( SCIPchgVarLb(scip, var, val) );
            break;
         case 'U':
            if( mpsinputField1(mpsi)[1] == 'I' ) /* CPLEX extension (Integer Bound) */
            {
               SCIP_CALL( SCIPchgVarType(scip, var, SCIP_VARTYPE_INTEGER, &infeasible) );
               /* don't assert feasibility here because the presolver will and should detect a infeasibility */
            }
            SCIP_CALL( SCIPchgVarUb(scip, var, val) );
            break;
         case 'S':
            assert(mpsinputField1(mpsi)[1] == 'C'); /* CPLEX extension (Semi-Continuous) */
            /* remember that variable is semi-continuous */
            if( semicontsize <= nsemicont )
            {
               semicontsize = SCIPcalcMemGrowSize(scip, nsemicont+1);
               if( semicont == NULL )
               {
                  SCIP_CALL( SCIPallocBufferArray(scip, &semicont, semicontsize) );
               }
               else
               {
                  SCIP_CALL( SCIPreallocBufferArray(scip, &semicont, semicontsize) );
               }
            }
            assert(semicont != NULL);
            semicont[nsemicont] = var;
            ++nsemicont;

            SCIP_CALL( SCIPchgVarUb(scip, var, val) );
            break;
         case 'F':
            if( mpsinputField1(mpsi)[1] == 'X' )
            {
               SCIP_CALL( SCIPchgVarLb(scip, var, val) );
               SCIP_CALL( SCIPchgVarUb(scip, var, val) );
            }
            else
            {
               SCIP_CALL( SCIPchgVarLb(scip, var, -SCIPinfinity(scip)) );
               SCIP_CALL( SCIPchgVarUb(scip, var, +SCIPinfinity(scip)) );
            }
            break;
         case 'M':
            SCIP_CALL( SCIPchgVarLb(scip, var, -SCIPinfinity(scip)) );
            break;
         case 'P':
            SCIP_CALL( SCIPchgVarUb(scip, var, +SCIPinfinity(scip)) );
            break;
         case 'B' : /* CPLEX extension (Binary) */
            SCIP_CALL( SCIPchgVarLb(scip, var, 0.0) );
            SCIP_CALL( SCIPchgVarUb(scip, var, 1.0) );
            SCIP_CALL( SCIPchgVarType(scip, var, SCIP_VARTYPE_BINARY, &infeasible) );
            /* don't assert feasibility here because the presolver will and should detect a infeasibility */
            break;
         default:
            mpsinputSyntaxerror(mpsi);
            return SCIP_OKAY;
         }
      }
      else
      {
         /* check for syntax error */
         assert(*bndname != '\0');
         if( strcmp(bndname, mpsinputField3(mpsi)) == 0 && shifted )
         {
            mpsinputSyntaxerror(mpsi);
            return SCIP_OKAY;
         }

         mpsinputEntryIgnored(scip, mpsi, "bound", mpsinputField2(mpsi), "variable", mpsinputField3(mpsi), SCIP_VERBLEVEL_NORMAL);
      }
   }
   mpsinputSyntaxerror(mpsi);


 READBOUNDS_FINISH:
   if( nsemicont > 0 )
   {
      int i;
      SCIP_Real oldlb;
      char name[SCIP_MAXSTRLEN];
      SCIP_CONS* cons;

      SCIP_VAR* vars[2];
      SCIP_BOUNDTYPE boundtypes[2];
      SCIP_Real bounds[2];

      assert(semicont != NULL);

      /* add bound disjunction constraints for semi-continuous variables */
      for( i = 0; i < nsemicont; ++i )
      {
         var = semicont[i];

         oldlb = SCIPvarGetLbGlobal(var);
         /* if no bound was specified (which we assume if we see lower bound 0.0),
          * then the default lower bound for a semi-continuous variable is 1.0 */
         if( oldlb == 0.0 )
            oldlb = 1.0;

         /* change the lower bound to 0.0 */
         SCIP_CALL( SCIPchgVarLb(scip, var, 0.0) );

         /* add a bound disjunction constraint to say var <= 0.0 or var >= oldlb */
         (void) SCIPsnprintf(name, SCIP_MAXSTRLEN, "semicont_%s", SCIPvarGetName(var));

         vars[0] = var;
         vars[1] = var;
         boundtypes[0] = SCIP_BOUNDTYPE_UPPER;
         boundtypes[1] = SCIP_BOUNDTYPE_LOWER;
         bounds[0] = 0.0;
         bounds[1] = oldlb;

         SCIP_CALL( SCIPcreateConsBounddisjunction(scip, &cons, name, 2, vars, boundtypes, bounds,
               !dynamiccols, TRUE, TRUE, TRUE, TRUE, FALSE, FALSE, dynamicconss, dynamiccols, FALSE) );
         SCIP_CALL( SCIPaddCons(scip, cons) );

         SCIPdebugMessage("add bound disjunction constraint for semi-continuity of <%s>:\n\t", SCIPvarGetName(var));
         SCIPdebugPrintCons(scip, cons, NULL);

         SCIP_CALL( SCIPreleaseCons(scip, &cons) );
      }
   }

   SCIPfreeBufferArrayNull(scip, &semicont);

   return SCIP_OKAY;
}


/** Process SOS section.
 *
 *  We read the SOS section, which is a nonstandard section introduced by CPLEX.
 *
 *  @note Currently we do not support the standard way of specifying SOS constraints via markers.
 */
static
SCIP_RETCODE readSOS(
   MPSINPUT*             mpsi,               /**< mps input structure */
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_Bool initial;
   SCIP_Bool separate;
   SCIP_Bool enforce;
   SCIP_Bool check;
   SCIP_Bool propagate;
   SCIP_Bool local;
   SCIP_Bool modifiable;
   SCIP_Bool dynamic;
   SCIP_Bool removable;
   char name[MPS_MAX_NAMELEN] = { '\0' };
   SCIP_CONS* cons = NULL;
   int consType = -1;
   int cnt = 0;

   SCIPdebugMessage("read SOS constraints\n");
   
   /* standard settings for SOS constraints: */
   initial = TRUE;
   separate = FALSE;
   enforce = TRUE;
   check = TRUE;
   propagate = TRUE;
   local = FALSE;
   modifiable = FALSE;
   dynamic = FALSE;
   removable = FALSE;

   /* loop through section */
   while( mpsinputReadLine(mpsi) )
   {
      int type = -1;

      /* check if next section is found */
      if( mpsinputField0(mpsi) != NULL )
      {
         if( !strcmp(mpsinputField0(mpsi), "ENDATA") )
            mpsinputSetSection(mpsi, MPS_ENDATA);
         else if( !strcmp(mpsinputField0(mpsi), "QMATRIX") )
            mpsinputSetSection(mpsi, MPS_QMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "QUADOBJ") )
            mpsinputSetSection(mpsi, MPS_QUADOBJ);
         else if( !strcmp(mpsinputField0(mpsi), "QCMATRIX") )
            mpsinputSetSection(mpsi, MPS_QCMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "INDICATORS") )
            mpsinputSetSection(mpsi, MPS_INDICATORS);
         break;
      }
      if( mpsinputField1(mpsi) == NULL )
      {
         SCIPerrorMessage("empty data in a non-comment line.\n");
         mpsinputSyntaxerror(mpsi);
         return SCIP_OKAY;
      }

      /* check for new SOS set */
      if( strcmp(mpsinputField1(mpsi), "S1") == 0 )
         type = 1;
      if( strcmp(mpsinputField1(mpsi), "S2") == 0 )
         type = 2;

      /* add last constraint and create a new one */
      if( type > 0 )
      {
         assert( type == 1 || type == 2 );
         if( cons != NULL )
         {
            /* add last constraint */
            SCIP_CALL( SCIPaddCons(scip, cons) );
            SCIPdebugMessage("(line %d) added constraint <%s>: ", mpsi->lineno, SCIPconsGetName(cons));
            SCIPdebugPrintCons(scip, cons, NULL);
            SCIP_CALL( SCIPreleaseCons(scip, &cons) );
         }

         /* check name */
         if( mpsinputField2(mpsi) != NULL )
	    (void)SCIPmemccpy(name, mpsinputField2(mpsi), '\0', MPS_MAX_NAMELEN - 1);
         else
         {
            /* create new name */
            (void) SCIPsnprintf(name, MPS_MAX_NAMELEN, "SOS%d", ++cnt);
         }

         /* create new SOS constraint */
         if( type == 1 )
         {
            /* we do not know the name of the constraint */
            SCIP_CALL( SCIPcreateConsSOS1(scip, &cons, name, 0, NULL, NULL, initial, separate, enforce, check, propagate,
                  local, modifiable, dynamic, removable) );
         }
         else
         {
            assert( type == 2 );
            SCIP_CALL( SCIPcreateConsSOS2(scip, &cons, name, 0, NULL, NULL, initial, separate, enforce, check, propagate,
                  local, modifiable, dynamic, removable) );
         }
         consType = type;
         SCIPdebugMessage("created constraint <%s> of type %d.\n", name, type);
         /* note: we ignore the priorities! */
      }
      else
      {
         /* otherwise we are in the section given variables */
         SCIP_VAR* var;
         SCIP_Real weight;
         char* endptr;

         if( consType != 1 && consType != 2 )
         {
            SCIPerrorMessage("missing SOS type specification.\n");
            mpsinputSyntaxerror(mpsi);
            return SCIP_OKAY;
         }

         /* get variable */
         var = SCIPfindVar(scip, mpsinputField1(mpsi));
         if( var == NULL )
         {
            /* ignore unknown variables - we would not know the type anyway */
            mpsinputEntryIgnored(scip, mpsi, "column", mpsinputField1(mpsi), "SOS", name, SCIP_VERBLEVEL_NORMAL);
         }
         else
         {
            /* get weight */
            weight = strtod(mpsinputField2(mpsi), &endptr);
            if( endptr == mpsinputField2(mpsi) || *endptr != '\0' )
            {
               SCIPerrorMessage("weight for variable <%s> not specified.\n", mpsinputField1(mpsi));
               mpsinputSyntaxerror(mpsi);
               return SCIP_OKAY;
            }

            /* add variable and weight */
            assert( consType == 1 || consType == 2 );
            switch( consType )
            {
            case 1: 
               SCIP_CALL( SCIPaddVarSOS1(scip, cons, var, weight) );
               break;
            case 2: 
               SCIP_CALL( SCIPaddVarSOS2(scip, cons, var, weight) );
               break;
            default: 
               SCIPerrorMessage("unknown SOS type: <%d>\n", type); /* should not happen */
               SCIPABORT();
            }
            SCIPdebugMessage("added variable <%s> with weight %g.\n", SCIPvarGetName(var), weight);
         }
         /* check other fields */
         if( (mpsinputField3(mpsi) != NULL && *mpsinputField3(mpsi) != '\0' ) ||
            (mpsinputField4(mpsi) != NULL && *mpsinputField4(mpsi) != '\0' ) ||
            (mpsinputField5(mpsi) != NULL && *mpsinputField5(mpsi) != '\0' ) )
         {
            SCIPwarningMessage(scip, "ignoring data in fields 3-5 <%s> <%s> <%s>.\n",
               mpsinputField3(mpsi), mpsinputField4(mpsi), mpsinputField5(mpsi));
         }
      }
   }

   if( cons != NULL )
   {
      /* add last constraint */
      SCIP_CALL( SCIPaddCons(scip, cons) );
      SCIPdebugMessage("(line %d) added constraint <%s>: ", mpsi->lineno, SCIPconsGetName(cons));
      SCIPdebugPrintCons(scip, cons, NULL);
      SCIP_CALL( SCIPreleaseCons(scip, &cons) );
   }

   return SCIP_OKAY;
}


/** Process QMATRIX or QUADOBJ section.
 *
 *  - We read the QMATRIX or QUADOBJ section, which is a nonstandard section introduced by CPLEX.
 *  - We create a quadratic constraint for this matrix and add a variable to the objective to
 *    represent the value of the QMATRIX.
 *  - For a QMATRIX, we expect that both lower and upper diagonal elements are given and every
 *    coefficient has to be divided by 2.0.
 *  - For a QUADOBJ, we expect that only the upper diagonal elements are given and thus only
 *    coefficients on the diagonal have to be divided by 2.0.
 */
static
SCIP_RETCODE readQMatrix(
   MPSINPUT*             mpsi,               /**< mps input structure */
   SCIP_Bool             isQuadObj,          /**< whether we actually read a QUADOBJ section */
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_VAR** quadvars1;
   SCIP_VAR** quadvars2;
   SCIP_Real* quadcoefs;
   int cnt  = 0; /* number of qmatrix elements processed so far */
   int size;     /* size of quad* arrays */

   SCIPdebugMessage("read %s objective\n", isQuadObj ? "QUADOBJ" : "QMATRIX");

   size = 1;
   SCIP_CALL( SCIPallocBufferArray(scip, &quadvars1, size) );
   SCIP_CALL( SCIPallocBufferArray(scip, &quadvars2, size) );
   SCIP_CALL( SCIPallocBufferArray(scip, &quadcoefs, size) );

   /* loop through section */
   while( mpsinputReadLine(mpsi) )
   {
      /* otherwise we are in the section given variables */
      SCIP_VAR* var1;
      SCIP_VAR* var2;
      SCIP_Real coef;

      /* check if next section is found */
      if( mpsinputField0(mpsi) != NULL )
      {
         if( !strcmp(mpsinputField0(mpsi), "QCMATRIX") )
            mpsinputSetSection(mpsi, MPS_QCMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "INDICATORS") )
            mpsinputSetSection(mpsi, MPS_INDICATORS);
         else if( !strcmp(mpsinputField0(mpsi), "ENDATA") )
            mpsinputSetSection(mpsi, MPS_ENDATA);
         break;
      }
      if( mpsinputField1(mpsi) == NULL && mpsinputField2(mpsi) == NULL )
      {
         SCIPerrorMessage("empty data in a non-comment line.\n");
         mpsinputSyntaxerror(mpsi);
         SCIPfreeBufferArray(scip, &quadvars1);
         SCIPfreeBufferArray(scip, &quadvars2);
         SCIPfreeBufferArray(scip, &quadcoefs);
         return SCIP_OKAY;
      }

      /* get first variable */
      var1 = SCIPfindVar(scip, mpsinputField1(mpsi));
      if( var1 == NULL )
      {
         /* ignore unknown variables - we would not know the type anyway */
         mpsinputEntryIgnored(scip, mpsi, "column", mpsinputField1(mpsi), "QMatrix", "QMATRIX", SCIP_VERBLEVEL_NORMAL);
      }
      else
      {
         /* get second variable */
         var2 = SCIPfindVar(scip, mpsinputField2(mpsi));
         if( var2 == NULL )
         {
            /* ignore unknown variables - we would not know the type anyway */
            mpsinputEntryIgnored(scip, mpsi, "column", mpsinputField2(mpsi), "QMatrix", "QMATRIX", SCIP_VERBLEVEL_NORMAL);
         }
         else
         {
            char* endptr;
            /* get coefficient */
            coef = strtod(mpsinputField3(mpsi), &endptr);
            if( endptr == mpsinputField3(mpsi) || *endptr != '\0' )
            {
               SCIPerrorMessage("coefficient of term <%s>*<%s> not specified.\n", mpsinputField1(mpsi), mpsinputField2(mpsi));
               mpsinputSyntaxerror(mpsi);
               SCIPfreeBufferArray(scip, &quadvars1);
               SCIPfreeBufferArray(scip, &quadvars2);
               SCIPfreeBufferArray(scip, &quadcoefs);
               return SCIP_OKAY;
            }

            /* store variables and coefficient */
            if( cnt >= size )
            {
               int newsize = SCIPcalcMemGrowSize(scip, size+1);
               assert(newsize > size);
               SCIP_CALL( SCIPreallocBufferArray(scip, &quadvars1, newsize) );
               SCIP_CALL( SCIPreallocBufferArray(scip, &quadvars2, newsize) );
               SCIP_CALL( SCIPreallocBufferArray(scip, &quadcoefs, newsize) );
               size = newsize;
            }
            assert(cnt < size);
            quadvars1[cnt] = var1;
            quadvars2[cnt] = var2;
            quadcoefs[cnt] = coef;
            /* diagonal elements have to be divided by 2.0
             * in a QMATRIX section also off-diagonal have to be divided by 2.0, since both lower and upper diagonal elements are given
             */
            if( var1 == var2 || !isQuadObj )
               quadcoefs[cnt] /= 2.0;
            ++cnt;

            SCIPdebugMessage("stored term %g*<%s>*<%s>.\n", coef, SCIPvarGetName(var1), SCIPvarGetName(var2));

            /* check other fields */
            if( (mpsinputField4(mpsi) != NULL && *mpsinputField4(mpsi) != '\0' ) ||
               (mpsinputField5(mpsi) != NULL && *mpsinputField5(mpsi) != '\0' ) )
            {
               SCIPwarningMessage(scip, "ignoring data in fields 4 and 5 <%s> <%s>.\n", mpsinputField4(mpsi), mpsinputField5(mpsi));
            }
         }
      }
   }

   /* add constraint */
   if( cnt )
   {
      SCIP_Bool  initial, separate, enforce, check, propagate;
      SCIP_Bool  local, modifiable, dynamic, removable;
      SCIP_CONS* cons = NULL;
      SCIP_VAR*  qmatrixvar = NULL;
      SCIP_Real  lhs, rhs;
      SCIP_Real  minusone = -1.0;

      /* standard settings for quadratic constraints: */
      initial    = TRUE;
      separate   = TRUE;
      enforce    = TRUE;
      check      = TRUE;
      propagate  = TRUE;
      local      = FALSE;
      modifiable = FALSE;
      dynamic    = FALSE;
      removable  = FALSE;

      SCIP_CALL( SCIPcreateVar(scip, &qmatrixvar, "qmatrixvar", -SCIPinfinity(scip), SCIPinfinity(scip), 1.0,
            SCIP_VARTYPE_CONTINUOUS, initial, removable, NULL, NULL, NULL, NULL, NULL) );
      SCIP_CALL( SCIPaddVar(scip, qmatrixvar) );

      if( mpsinputObjsense(mpsi) == SCIP_OBJSENSE_MINIMIZE )
      {
         lhs = -SCIPinfinity(scip);
         rhs = 0.0;
      }
      else
      {
         lhs = 0.0;
         rhs = SCIPinfinity(scip);
      }

      SCIP_CALL( SCIPcreateConsQuadraticNonlinear(scip, &cons, "qmatrix", 1, &qmatrixvar, &minusone, cnt, quadvars1, quadvars2, quadcoefs, lhs, rhs,
            initial, separate, enforce, check, propagate, local, modifiable, dynamic, removable) );

      SCIP_CALL( SCIPaddCons(scip, cons) );
      SCIPdebugMessage("(line %d) added constraint <%s>: ", mpsi->lineno, SCIPconsGetName(cons));
      SCIPdebugPrintCons(scip, cons, NULL);

      SCIP_CALL( SCIPreleaseCons(scip, &cons) );
      SCIP_CALL( SCIPreleaseVar(scip, &qmatrixvar) );
   }
   else
   {
      SCIPwarningMessage(scip, "%s section has no entries.\n", isQuadObj ? "QUADOBJ" : "QMATRIX");
   }

   SCIPfreeBufferArray(scip, &quadvars1);
   SCIPfreeBufferArray(scip, &quadvars2);
   SCIPfreeBufferArray(scip, &quadcoefs);

   return SCIP_OKAY;
}


/** Process QCMATRIX section.
 *
 *  We read the QCMATRIX section, which is a nonstandard section introduced by CPLEX.
 *
 *  We replace the corresponding linear constraint by a quadratic constraint which contains the
 *  original linear constraint plus the quadratic part specified in the QCMATRIX.
 */
static
SCIP_RETCODE readQCMatrix(
   MPSINPUT*             mpsi,               /**< mps input structure */
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_CONS* lincons; /* the linear constraint that was added for the corresponding row */
   SCIP_VAR** quadvars1;
   SCIP_VAR** quadvars2;
   SCIP_Real* quadcoefs;
   int cnt  = 0; /* number of qcmatrix elements processed so far */
   int size;     /* size of quad* arrays */

   if( mpsinputField1(mpsi) == NULL )
   {
      SCIPerrorMessage("no row name in QCMATRIX line.\n");
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }
   
   SCIPdebugMessage("read QCMATRIX section for row <%s>\n", mpsinputField1(mpsi));
   
   lincons = SCIPfindCons(scip, mpsinputField1(mpsi));
   if( lincons == NULL )
   {
      SCIPerrorMessage("no row under name <%s> processed so far.\n", mpsinputField1(mpsi));
      mpsinputSyntaxerror(mpsi);
      return SCIP_OKAY;
   }

   size = 1;
   SCIP_CALL( SCIPallocBufferArray(scip, &quadvars1, size) );
   SCIP_CALL( SCIPallocBufferArray(scip, &quadvars2, size) );
   SCIP_CALL( SCIPallocBufferArray(scip, &quadcoefs, size) );

   /* loop through section */
   while( mpsinputReadLine(mpsi) )
   {
      /* otherwise we are in the section given variables */
      SCIP_VAR* var1;
      SCIP_VAR* var2;
      SCIP_Real coef;
      
      /* check if next section is found */
      if( mpsinputField0(mpsi) != NULL )
      {
         if( !strcmp(mpsinputField0(mpsi), "QMATRIX") )
            mpsinputSetSection(mpsi, MPS_QMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "QUADOBJ") )
            mpsinputSetSection(mpsi, MPS_QUADOBJ);
         else if( !strcmp(mpsinputField0(mpsi), "QCMATRIX") )
            mpsinputSetSection(mpsi, MPS_QCMATRIX);
         else if( !strcmp(mpsinputField0(mpsi), "INDICATORS") )
            mpsinputSetSection(mpsi, MPS_INDICATORS);
         else if( !strcmp(mpsinputField0(mpsi), "ENDATA") )
            mpsinputSetSection(mpsi, MPS_ENDATA);
         break;
      }
      if( mpsinputField1(mpsi) == NULL && mpsinputField2(mpsi) == NULL )
      {
         SCIPerrorMessage("empty data in a non-comment line.\n");
         mpsinputSyntaxerror(mpsi);
         SCIPfreeBufferArray(scip, &quadvars1);
         SCIPfreeBufferArray(scip, &quadvars2);
         SCIPfreeBufferArray(scip, &quadcoefs);
         return SCIP_OKAY;
      }

      /* get first variable */
      var1 = SCIPfindVar(scip, mpsinputField1(mpsi));
      if( var1 == NULL )
      {
         /* ignore unknown variables - we would not know the type anyway */
         mpsinputEntryIgnored(scip, mpsi, "column", mpsinputField1(mpsi), "QCMatrix", SCIPconsGetName(lincons), SCIP_VERBLEVEL_NORMAL);
      }
      else
      {
         /* get second variable */
         var2 = SCIPfindVar(scip, mpsinputField2(mpsi));
         if( var2 == NULL )
         {
            /* ignore unknown variables - we would not know the type anyway */
            mpsinputEntryIgnored(scip, mpsi, "column", mpsinputField2(mpsi), "QCMatrix", SCIPconsGetName(lincons), SCIP_VERBLEVEL_NORMAL);
         }
         else
         {
            char* endptr;
            /* get coefficient */
            coef = strtod(mpsinputField3(mpsi), &endptr);
            if( endptr == mpsinputField3(mpsi) || *endptr != '\0' )
            {
               SCIPerrorMessage("coefficient of term <%s>*<%s> not specified.\n", mpsinputField1(mpsi), mpsinputField2(mpsi));
               mpsinputSyntaxerror(mpsi);
               SCIPfreeBufferArray(scip, &quadvars1);
               SCIPfreeBufferArray(scip, &quadvars2);
               SCIPfreeBufferArray(scip, &quadcoefs);
               return SCIP_OKAY;
            }

            /* store variables and coefficient */
            if( cnt >= size )
            {
               int newsize = SCIPcalcMemGrowSize(scip, size+1);
               assert(newsize > size);
               SCIP_CALL( SCIPreallocBufferArray(scip, &quadvars1, newsize) );
               SCIP_CALL( SCIPreallocBufferArray(scip, &quadvars2, newsize) );
               SCIP_CALL( SCIPreallocBufferArray(scip, &quadcoefs, newsize) );
               size = newsize;
            }
            assert(cnt < size);
            quadvars1[cnt] = var1;
            quadvars2[cnt] = var2;
            quadcoefs[cnt] = coef;
            ++cnt;

            SCIPdebugMessage("stored term %g*<%s>*<%s>.\n", coef, SCIPvarGetName(var1), SCIPvarGetName(var2));

            /* check other fields */
            if( (mpsinputField4(mpsi) != NULL && *mpsinputField4(mpsi) != '\0' ) ||
               (mpsinputField5(mpsi) != NULL && *mpsinputField5(mpsi) != '\0' ) )
            {
               SCIPwarningMessage(scip, "ignoring data in fields 4 and 5 <%s> <%s>.\n", mpsinputField4(mpsi), mpsinputField5(mpsi));
            }
         }
      }
   }

   /* replace linear constraint by quadratic constraint */
   if( cnt )
   {
      SCIP_CONS* cons = NULL;

      SCIP_CALL( SCIPcreateConsQuadraticNonlinear(scip, &cons, SCIPconsGetName(lincons),
            SCIPgetNVarsLinear(scip, lincons), SCIPgetVarsLinear(scip, lincons), SCIPgetValsLinear(scip, lincons),
            cnt, quadvars1, quadvars2, quadcoefs, SCIPgetLhsLinear(scip, lincons), SCIPgetRhsLinear(scip, lincons),
            SCIPconsIsInitial(lincons), SCIPconsIsSeparated(lincons), SCIPconsIsEnforced(lincons), SCIPconsIsChecked(lincons),
            SCIPconsIsPropagated(lincons), SCIPconsIsLocal(lincons), SCIPconsIsModifiable(lincons), SCIPconsIsDynamic(lincons),
            SCIPconsIsRemovable(lincons)) );

      SCIP_CALL( SCIPaddCons(scip, cons) );
      SCIPdebugMessage("(line %d) added constraint <%s>: ", mpsi->lineno, SCIPconsGetName(cons));
      SCIPdebugPrintCons(scip, cons, NULL);

      SCIP_CALL( SCIPreleaseCons(scip, &cons) );

      SCIP_CALL( SCIPdelCons(scip, lincons) );
   }
   else
   {
      SCIPwarningMessage(scip, "QCMATRIX section has no entries.\n");
   }

   SCIPfreeBufferArray(scip, &quadvars1);
   SCIPfreeBufferArray(scip, &quadvars2);
   SCIPfreeBufferArray(scip, &quadcoefs);

   return SCIP_OKAY;
}


/** Process INDICATORS section.
 *
 *  We read the INDICATORS section, which is a nonstandard section introduced by CPLEX.
 *
 *  The section has to come after the QMATRIX* sections.
 */
static
SCIP_RETCODE readIndicators(
   MPSINPUT*             mpsi,               /**< mps input structure */
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_Bool initial;
   SCIP_Bool separate;
   SCIP_Bool enforce;
   SCIP_Bool check;
   SCIP_Bool propagate;
   SCIP_Bool local;
   SCIP_Bool dynamic;
   SCIP_Bool removable;
   SCIP_Bool stickingatnode;
   char name[MPS_MAX_NAMELEN] = { '\0' };

   SCIPdebugMessage("read INDICATORS constraints\n");
   
   /* standard settings for indicator constraints: */
   initial = TRUE;
   separate = TRUE;
   enforce = TRUE;
   check = TRUE;
   propagate = TRUE;
   local = FALSE;
   dynamic = FALSE;
   removable = FALSE;
   stickingatnode = FALSE;

   /* loop through section */
   while( mpsinputReadLine(mpsi) )
   {
      SCIP_CONSHDLR* conshdlr;
      SCIP_VARTYPE slackvartype;
      SCIP_CONS* cons;
      SCIP_CONS* lincons;
      SCIP_VAR* binvar;
      SCIP_VAR* slackvar;
      SCIP_Real lhs;
      SCIP_Real rhs;
      SCIP_Real sign;
      SCIP_VAR** linvars;
      SCIP_Real* linvals;
      int nlinvars;
      int i;

      /* check if next section is found */
      if( mpsinputField0(mpsi) != NULL )
      {
         if( !strcmp(mpsinputField0(mpsi), "ENDATA") )
            mpsinputSetSection(mpsi, MPS_ENDATA);
         break;
      }
      if( mpsinputField1(mpsi) == NULL || mpsinputField2(mpsi) == NULL )
      {
         SCIPerrorMessage("empty data in a non-comment line.\n");
         mpsinputSyntaxerror(mpsi);
         return SCIP_OKAY;
      }

      /* check for new indicator constraint */
      if( strcmp(mpsinputField1(mpsi), "IF") != 0 )
      {
         SCIPerrorMessage("Indicator constraints need to be introduced by 'IF' in column 1.\n");
         mpsinputSyntaxerror(mpsi);
         return SCIP_OKAY;
      }

      /* get linear constraint (row) */
      lincons = SCIPfindCons(scip, mpsinputField2(mpsi));
      if( lincons == NULL )
      {
         SCIPerrorMessage("row <%s> does not exist.\n", mpsinputField2(mpsi));
         mpsinputSyntaxerror(mpsi);
         return SCIP_OKAY;
      }

      /* check whether constraint is really linear */
      conshdlr = SCIPconsGetHdlr(lincons);
      if( strcmp(SCIPconshdlrGetName(conshdlr), "linear") != 0 )
      {
         SCIPerrorMessage("constraint <%s> is not linear.\n", mpsinputField2(mpsi));
         mpsinputSyntaxerror(mpsi);
         return SCIP_OKAY;
      }

      /* get binary variable */
      binvar = SCIPfindVar(scip, mpsinputField3(mpsi));
      if( binvar == NULL )
      {
         SCIPerrorMessage("binary variable <%s> does not exist.\n", mpsinputField3(mpsi));
         mpsinputSyntaxerror(mpsi);
         return SCIP_OKAY;
      }

      /* check type */
      if( SCIPvarGetType(binvar) != SCIP_VARTYPE_BINARY )
      {
         SCIPerrorMessage("variable <%s> is not binary.\n", mpsinputField3(mpsi));
         mpsinputSyntaxerror(mpsi);
         return SCIP_OKAY;
      }

      /* check whether we need the negated variable */
      if( mpsinputField4(mpsi) != NULL )
      {
         if( *mpsinputField4(mpsi) == '0' )
         {
            SCIP_VAR* var;
            SCIP_CALL( SCIPgetNegatedVar(scip, binvar, &var) );
            binvar = var;
            assert( binvar != NULL );
         }
         else
         {
            if( *mpsinputField4(mpsi) != '1' )
            {
               SCIPerrorMessage("binary variable <%s> can only take values 0/1 (%s).\n", mpsinputField3(mpsi), mpsinputField4(mpsi));
               mpsinputSyntaxerror(mpsi);
               return SCIP_OKAY;
            }
         }
      }

      /* check lhs/rhs */
      lhs = SCIPgetLhsLinear(scip, lincons);
      rhs = SCIPgetLhsLinear(scip, lincons);
      nlinvars = SCIPgetNVarsLinear(scip, lincons);
      linvars = SCIPgetVarsLinear(scip, lincons);
      linvals = SCIPgetValsLinear(scip, lincons);

      sign = -1.0;
      if( !SCIPisInfinity(scip, -lhs) )
      {
         if( SCIPisInfinity(scip, rhs) )
            sign = 1.0;
         else
         {
            if( !SCIPisEQ(scip, lhs, rhs) )
            {
               SCIPerrorMessage("ranged row <%s> is not allowed in indicator constraints.\n", mpsinputField2(mpsi));
               mpsinputSyntaxerror(mpsi);
               return SCIP_OKAY;
            }
            else
            {
               /* create second indicator constraint */
               SCIP_VAR** vars;
               SCIP_Real* vals;

               SCIP_CALL( SCIPallocBufferArray(scip, &vars, nlinvars+1) );
               SCIP_CALL( SCIPallocBufferArray(scip, &vals, nlinvars+1) );
               for( i = 0; i < nlinvars; ++i )
               {
                  vars[i] = linvars[i];
                  vals[i] = -linvals[i];
               }

               /* create new name */
               (void) SCIPsnprintf(name, MPS_MAX_NAMELEN, "indlhs_%s", SCIPconsGetName(lincons));

               /* create indicator constraint */
               SCIP_CALL( SCIPcreateConsIndicator(scip, &cons, name, binvar, nlinvars+1, vars, vals, -lhs,
                     initial, separate, enforce, check, propagate, local, dynamic, removable, stickingatnode) );
               SCIP_CALL( SCIPaddCons(scip, cons) );
               SCIPdebugMessage("created indicator constraint <%s>\n", mpsinputField2(mpsi));
               SCIPdebugPrintCons(scip, cons, NULL);
               SCIP_CALL( SCIPreleaseCons(scip, &cons) );

               SCIPfreeBufferArray(scip, &vals);
               SCIPfreeBufferArray(scip, &vars);
            }
         }
      }

      /* check if slack variable can be made implicitly integer */
      slackvartype = SCIP_VARTYPE_IMPLINT;
      for( i = 0; i < nlinvars; ++i )
      {
         if( !SCIPvarIsIntegral(linvars[i]) || ! SCIPisIntegral(scip, linvals[i]) )
         {
            slackvartype = SCIP_VARTYPE_CONTINUOUS;
            break;
         }
      }

      /* create slack variable */
      (void) SCIPsnprintf(name, MPS_MAX_NAMELEN, "indslack_%s", SCIPconsGetName(lincons));
      SCIP_CALL( SCIPcreateVar(scip, &slackvar, name, 0.0, SCIPinfinity(scip), 0.0, slackvartype, TRUE, FALSE,
            NULL, NULL, NULL, NULL, NULL) );

      /* add slack variable */      
      SCIP_CALL( SCIPaddVar(scip, slackvar) );
      SCIP_CALL( SCIPaddCoefLinear(scip, lincons, slackvar, sign) );

      /* create new name */
      (void) SCIPsnprintf(name, MPS_MAX_NAMELEN, "indlhs_%s", SCIPconsGetName(lincons));

      /* create indicator constraint */
      SCIP_CALL( SCIPcreateConsIndicatorLinCons(scip, &cons, name, binvar, lincons, slackvar,
            initial, separate, enforce, check, propagate, local, dynamic, removable, stickingatnode) );

      SCIP_CALL( SCIPaddCons(scip, cons) );
      SCIPdebugMessage("created indicator constraint <%s>", mpsinputField2(mpsi));
      SCIPdebugPrintCons(scip, cons, NULL);
      SCIP_CALL( SCIPreleaseCons(scip, &cons) );
   }

   return SCIP_OKAY;
}

/*
 * Local methods
 */

/** Process ROWS, USERCUTS, or LAZYCONS section. */
static
SCIP_RETCODE readRowsMop(
   MPSINPUT*             mpsi,               /**< mps input structure */
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   ProbDataObjectives* probdata;
   SCIP_Bool dynamicrows;
   SCIP_Bool dynamicconss;

   SCIPdebugMessage("read rows\n");

   SCIP_CALL( SCIPgetBoolParam(scip, "reading/dynamicconss", &dynamicconss) );
   SCIP_CALL( SCIPgetBoolParam(scip, "reading/dynamicrows", &dynamicrows) );

   while( mpsinputReadLine(mpsi) )
   {
      if( mpsinputField0(mpsi) != NULL )
      {
         if( !strcmp(mpsinputField0(mpsi), "ROWS") )
            mpsinputSetSection(mpsi, MPS_ROWS);
         else if( !strcmp(mpsinputField0(mpsi), "USERCUTS") )
            mpsinputSetSection(mpsi, MPS_USERCUTS);
         else if( !strcmp(mpsinputField0(mpsi), "LAZYCONS") )
            mpsinputSetSection(mpsi, MPS_LAZYCONS);
         else if( !strcmp(mpsinputField0(mpsi), "COLUMNS") )
            mpsinputSetSection(mpsi, MPS_COLUMNS);
         else
            mpsinputSyntaxerror(mpsi);

         return SCIP_OKAY;
      }

      if( *mpsinputField1(mpsi) == 'N' )
      {
         probdata = dynamic_cast<ProbDataObjectives *>(SCIPgetObjProbData(scip));
         probdata->addObjName(mpsinputField2(mpsi));
      }
      else
      {
         SCIP_CONS* cons;
         SCIP_Bool initial;
         SCIP_Bool separate;
         SCIP_Bool enforce;
         SCIP_Bool check;
         SCIP_Bool propagate;
         SCIP_Bool local;
         SCIP_Bool modifiable;
         SCIP_Bool dynamic;
         SCIP_Bool removable;

         cons = SCIPfindCons(scip, mpsinputField2(mpsi));
         if( cons != NULL )
            break;

         initial = !dynamicrows && (mpsinputSection(mpsi) == MPS_ROWS);
         separate = TRUE;
         enforce = (mpsinputSection(mpsi) != MPS_USERCUTS);
         check = (mpsinputSection(mpsi) != MPS_USERCUTS);
         propagate = TRUE;
         local = FALSE;
         modifiable = FALSE;
         dynamic = dynamicconss;
         removable = dynamicrows || (mpsinputSection(mpsi) == MPS_USERCUTS);

         switch(*mpsinputField1(mpsi))
         {
         case 'G' :
            SCIP_CALL( SCIPcreateConsLinear(scip, &cons, mpsinputField2(mpsi), 0, NULL, NULL, 0.0, SCIPinfinity(scip),
                  initial, separate, enforce, check, propagate, local, modifiable, dynamic, removable, FALSE) );
            break;
         case 'E' :
            SCIP_CALL( SCIPcreateConsLinear(scip, &cons, mpsinputField2(mpsi), 0, NULL, NULL, 0.0, 0.0,
                  initial, separate, enforce, check, propagate, local, modifiable, dynamic, removable, FALSE) );
            break;
         case 'L' :
            SCIP_CALL( SCIPcreateConsLinear(scip, &cons, mpsinputField2(mpsi), 0, NULL, NULL, -SCIPinfinity(scip), 0.0,
                  initial, separate, enforce, check, propagate, local, modifiable, dynamic, removable, FALSE) );
            break;
         default :
            mpsinputSyntaxerror(mpsi);
            return SCIP_OKAY;
         }
         SCIP_CALL( SCIPaddCons(scip, cons) );
         SCIP_CALL( SCIPreleaseCons(scip, &cons) );
      }
   }
   mpsinputSyntaxerror(mpsi);

   return SCIP_OKAY;
}

/** Process COLUMNS section. */
static SCIP_RETCODE readColsMop(MPSINPUT* mpsi, /**< mps input structure */
SCIP* scip /**< SCIP data structure */
)
{
   ProbDataObjectives* probdata;
   char colname[MPS_MAX_NAMELEN] = { '\0' };
   SCIP_CONS* cons;
   SCIP_VAR* var;
   SCIP_Real val;
   SCIP_Bool dynamiccols;

   probdata = dynamic_cast<ProbDataObjectives *>(SCIPgetObjProbData(scip));

   SCIPdebugMessage("read columns\n");

   SCIP_CALL( SCIPgetBoolParam(scip, "reading/dynamiccols", &dynamiccols));
   var = NULL;
   while( mpsinputReadLine(mpsi) )
   {
      if( mpsinputField0(mpsi) != 0 )
      {
         if( strcmp(mpsinputField0(mpsi), "RHS") )
            break;

         /* add the last variable to the problem */
         if( var != NULL )
         {
            SCIP_CALL( SCIPaddVar(scip, var));
            SCIP_CALL( SCIPreleaseVar(scip, &var));
         }
         assert(var == NULL);

         mpsinputSetSection(mpsi, MPS_RHS);
         return SCIP_OKAY;
      }
      if( mpsinputField1(mpsi) == NULL || mpsinputField2(mpsi) == NULL || mpsinputField3(mpsi) == NULL )
         break;

      /* new column? */
      if( strcmp(colname, mpsinputField1(mpsi)) )
      {
         /* add the last variable to the problem */
         if( var != NULL )
         {
            SCIP_CALL( SCIPaddVar(scip, var));
            SCIP_CALL( SCIPreleaseVar(scip, &var));
         }
         assert(var == NULL);

         (void)SCIPmemccpy(colname, mpsinputField1(mpsi), '\0', MPS_MAX_NAMELEN - 1);
         if( mpsinputIsInteger(mpsi) )
         {
            /* for integer variables, default bounds are 0 <= x < 1(not +infinity, like it is for continuous variables), and default cost is 0 */
            SCIP_CALL(
               SCIPcreateVar(scip, &var, colname, 0.0, 1.0, 0.0, SCIP_VARTYPE_BINARY, !dynamiccols, dynamiccols, NULL, NULL, NULL, NULL, NULL));
         }
         else
         {
            /* for continuous variables, default bounds are 0 <= x, and default cost is 0 */
            SCIP_CALL(
               SCIPcreateVar(scip, &var, colname, 0.0, SCIPinfinity(scip), 0.0, SCIP_VARTYPE_CONTINUOUS, !dynamiccols, dynamiccols, NULL, NULL, NULL, NULL, NULL));
         }
      }
      assert(var != NULL);

      val = atof(mpsinputField3(mpsi));

      cons = SCIPfindCons(scip, mpsinputField2(mpsi));
      if( cons == NULL ) 
      {
         /* row is objective */
         probdata->addObjCoeff(var, mpsinputField2(mpsi), val);
         //std::cout << "obj : " << mpsinputField2(mpsi) << " val : " << val << "\n";

      }
      else if( !SCIPisZero(scip, val) )
      {
         SCIP_CALL( SCIPaddCoefLinear(scip, cons, var, val));
      }

      if( mpsinputField5(mpsi) != NULL )
      {
         assert(mpsinputField4(mpsi) != NULL);

         val = atof(mpsinputField5(mpsi));

         cons = SCIPfindCons(scip, mpsinputField4(mpsi));
         if( cons == NULL )
         {
            /* row is objective */
            probdata->addObjCoeff(var, mpsinputField4(mpsi), val);
            //std::cout << "obj : " << mpsinputField4(mpsi) << " val : " << val << "\n";
         }
         else if( !SCIPisZero(scip, val) )
         {
            SCIP_CALL( SCIPaddCoefLinear(scip, cons, var, val));
         }

      }
   }
   mpsinputSyntaxerror(mpsi);

   return SCIP_OKAY;
}

static
SCIP_RETCODE readMOP(
   SCIP*                 scip,               /**< SCIP data structure */
   const char*           filename            /**< name of the input file */
   )
{
   SCIP_FILE *fp;

   MPSINPUT* mpsi;
   SCIP_Bool error;

   fp = SCIPfopen(filename, "r");

   if( fp == NULL )
   {
      SCIPerrorMessage("cannot open file <%s> for reading\n", filename);
      SCIPprintSysError(filename);
      return SCIP_NOFILE;
   }
   
   SCIP_CALL( mpsinputCreate(scip, &mpsi, fp) );

   SCIP_CALL( readName(scip, mpsi) );

   SCIP_CALL( SCIPcreateObjProb(scip, mpsi->probname, new ProbDataObjectives(), TRUE) ); 

   if( mpsinputSection(mpsi) == MPS_OBJSEN )
   {
      SCIP_CALL( readObjsen(scip, mpsi) );
   }
   if( mpsinputSection(mpsi) == MPS_OBJNAME )
   {
     SCIP_CALL( readObjname(scip, mpsi) );
   }
   while( mpsinputSection(mpsi) == MPS_ROWS
      || mpsinputSection(mpsi) == MPS_USERCUTS
      || mpsinputSection(mpsi) == MPS_LAZYCONS )
   {
      SCIP_CALL( readRowsMop(mpsi, scip) );
   }
   if( mpsinputSection(mpsi) == MPS_COLUMNS )
   {
     SCIP_CALL( readColsMop(mpsi, scip) );
   }
   if( mpsinputSection(mpsi) == MPS_RHS )
   {
     SCIP_CALL( readRhs(mpsi, scip) );
   }
   if( mpsinputSection(mpsi) == MPS_RANGES )
   {
      SCIP_CALL( readRanges(mpsi, scip) );
   }
   if( mpsinputSection(mpsi) == MPS_BOUNDS )
   {
      SCIP_CALL( readBounds(mpsi, scip) );
   }
   if( mpsinputSection(mpsi) == MPS_SOS )
   {
      SCIP_CALL( readSOS(mpsi, scip) );
   }
   while( mpsinputSection(mpsi) == MPS_QCMATRIX )
   {
      SCIP_CALL( readQCMatrix(mpsi, scip) );
   }
   if( mpsinputSection(mpsi) == MPS_QMATRIX )
   {
      SCIP_CALL( readQMatrix(mpsi, FALSE, scip) );
   }
   if( mpsinputSection(mpsi) == MPS_QUADOBJ )
   {
      SCIP_CALL( readQMatrix(mpsi, TRUE, scip) );
   }
   while( mpsinputSection(mpsi) == MPS_QCMATRIX )
   {
      SCIP_CALL( readQCMatrix(mpsi, scip) );
   }
   if( mpsinputSection(mpsi) == MPS_INDICATORS )
   {
      SCIP_CALL( readIndicators(mpsi, scip) );
   }
   if( mpsinputSection(mpsi) != MPS_ENDATA )
      mpsinputSyntaxerror(mpsi);

   SCIPfclose(fp);

   error = mpsinputHasError(mpsi);

   if( !error )
   {
     SCIP_CALL( SCIPsetObjsense(scip, mpsinputObjsense(mpsi)) );
   }
   mpsinputFree(scip, &mpsi);

   if( error )
      return SCIP_READERROR;
   else
      return SCIP_OKAY;
}


/** destructor of file reader to free user data (called when SCIP is exiting) */
SCIP_DECL_READERFREE(ReaderMOP::scip_free) {
        return SCIP_OKAY;
}

/** problem reading method of reader */
SCIP_DECL_READERREAD(ReaderMOP::scip_read) {
  assert(reader != NULL);
  assert(scip != NULL);
  assert(result != NULL);
  assert(filename != NULL);

  SCIP_RETCODE retcode;
  retcode = readMOP(scip, filename);

  if( retcode == SCIP_NOFILE || retcode == SCIP_READERROR )
    return retcode;

  SCIP_CALL( retcode );

  *result = SCIP_SUCCESS;

  return SCIP_OKAY;
}



