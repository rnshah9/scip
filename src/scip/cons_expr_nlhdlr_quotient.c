/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2020 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not visit scip.zib.de.         */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   cons_expr_nlhdlr_quotient.c
 * @brief  quotient nonlinear handler
 * @author Benjamin Mueller
 * @author Fabian Wegscheider
 *
 * @todo implement INITSEPA
 * @todo use the convex envelope for x/y described in Tawarmalani and Sahinidis (2002) if y has a finite upper bound
 */

#include <string.h>

#include "scip/cons_expr_nlhdlr_quotient.h"
#include "scip/cons_expr_pow.h"
#include "scip/cons_expr_product.h"
#include "scip/cons_expr_sum.h"
#include "scip/cons_expr_var.h"
#include "scip/cons_expr.h"

/* fundamental nonlinear handler properties */
#define NLHDLR_NAME         "quotient"
#define NLHDLR_DESC         "nonlinear handler for quotient expressions"
#define NLHDLR_PRIORITY     20

/*
 * Data structures
 */

/** nonlinear handler expression data */
struct SCIP_ConsExpr_NlhdlrExprData
{
   SCIP_CONSEXPR_EXPR*   numexpr;            /**< expression of the numerator */
   SCIP_Real             numcoef;            /**< coefficient of the numerator */
   SCIP_Real             numconst;           /**< constant of the numerator */
   SCIP_CONSEXPR_EXPR*   denomexpr;          /**< expression of the denominator */
   SCIP_Real             denomcoef;          /**< coefficient of the denominator */
   SCIP_Real             denomconst;         /**< constant of the denominator */
   SCIP_Real             constant;           /**< constant */
};

/*
 * Local methods
 */

/** helper method to create nonlinear handler expression data */
static
SCIP_RETCODE exprdataCreate(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSEXPR_NLHDLREXPRDATA** nlhdlrexprdata, /**< nonlinear handler expression data */
   SCIP_CONSEXPR_EXPR*   numexpr,             /**< expression of the numerator */
   SCIP_Real             numcoef,            /**< coefficient of the numerator */
   SCIP_Real             numconst,           /**< constant of the numerator */
   SCIP_CONSEXPR_EXPR*   denomexpr,          /**< expression of the denominator */
   SCIP_Real             denomcoef,          /**< coefficient of the denominator */
   SCIP_Real             denomconst,         /**< constant of the denominator */
   SCIP_Real             constant            /**< constant */
   )
{
   assert(nlhdlrexprdata != NULL);
   assert(numexpr != NULL);
   assert(denomexpr != NULL);
   assert(!SCIPisZero(scip, numcoef));
   assert(!SCIPisZero(scip, denomcoef));

   /* allocate memory */
   SCIP_CALL( SCIPallocBlockMemory(scip, nlhdlrexprdata) );

   /* store values */
   (*nlhdlrexprdata)->numexpr = numexpr;
   (*nlhdlrexprdata)->numcoef = numcoef;
   (*nlhdlrexprdata)->numconst = numconst;
   (*nlhdlrexprdata)->denomexpr = denomexpr;
   (*nlhdlrexprdata)->denomcoef = denomcoef;
   (*nlhdlrexprdata)->denomconst = denomconst;
   (*nlhdlrexprdata)->constant = constant;

   /* capture expressions */
   SCIPcaptureConsExprExpr(numexpr);
   SCIPcaptureConsExprExpr(denomexpr);

   return SCIP_OKAY;
}

/** helper method to free nonlinear handler expression data */
static
SCIP_RETCODE exprdataFree(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSEXPR_NLHDLREXPRDATA** nlhdlrexprdata  /**< nonlinear handler expression data */
   )
{
   assert(nlhdlrexprdata != NULL);
   assert(*nlhdlrexprdata != NULL);
   assert((*nlhdlrexprdata)->numexpr != NULL);
   assert((*nlhdlrexprdata)->denomexpr != NULL);

   /* release expressions */
   SCIP_CALL( SCIPreleaseConsExprExpr(scip, &(*nlhdlrexprdata)->denomexpr) );
   SCIP_CALL( SCIPreleaseConsExprExpr(scip, &(*nlhdlrexprdata)->numexpr) );

   /* free expression data of nonlinear handler */
   SCIPfreeBlockMemory(scip, nlhdlrexprdata);

   return SCIP_OKAY;
}

/** helper method to transform an expression g(x) as a * f(x) + b */
static
void transformExpr(
   SCIP_CONSEXPR_EXPR*   expr,               /**< expression */
   SCIP_CONSHDLR*        conshdlr,           /**< constraint handler */
   SCIP_CONSEXPR_EXPR**  target,             /**< pointer to store the expression f(x) */
   SCIP_Real*            coef,               /**< pointer to store the coefficient */
   SCIP_Real*            constant            /**< pointer to store the constant */
   )
{
   assert(expr != NULL);
   assert(conshdlr != NULL);
   assert(target != NULL);
   assert(coef != NULL);
   assert(constant != NULL);

   *target = NULL;
   *coef = 0.0;
   *constant = 0.0;

   /* expression is a sum with one child */
   if( SCIPgetConsExprExprHdlr(expr) == SCIPgetConsExprExprHdlrSum(conshdlr) && SCIPgetConsExprExprNChildren(expr) == 1 )
   {
      *target = SCIPgetConsExprExprChildren(expr)[0];
      *coef = SCIPgetConsExprExprSumCoefs(expr)[0];
      *constant = SCIPgetConsExprExprSumConstant(expr);
   }
   else /* otherwise return 1 * f(x) + 0 */
   {
      *target = expr;
      *coef = 1.0;
      *constant = 0.0;
   }
}

/** helper method to detect an expression of the form (a*x + b) / (c*y + d) + e; due to the expansion of products,
  * there are two types of expressions that can be detected:
  *
  * 1. prod(f(x), pow(g(y),-1))
  * 2. sum(prod(f(x),pow(g(y),-1)), pow(g(y),-1))
  *
  * @todo At the moment quotients like xy / z are not detected, because they are turned into a product expression
  * with three children, i.e., x * y * (1 / z).
  */
static
SCIP_RETCODE detectExpr(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSHDLR*        conshdlr,           /**< constraint handler */
   SCIP_CONSEXPR_EXPR*   expr,               /**< expression */
   SCIP_CONSEXPR_NLHDLREXPRDATA** nlhdlrexprdata, /**< pointer to store nonlinear handler expression data */
   SCIP_Bool*            success             /**< pointer to store whether nonlinear handler should be called for this expression */
   )
{
   SCIP_CONSEXPR_EXPRHDLR* prodhdlr;
   SCIP_CONSEXPR_EXPRHDLR* sumhdlr;
   SCIP_CONSEXPR_EXPRHDLR* powhdlr;
   SCIP_CONSEXPR_EXPR** children;
   SCIP_CONSEXPR_EXPR* denomexpr = NULL;
   SCIP_CONSEXPR_EXPR* numexpr = NULL;
   SCIP_CONSEXPR_EXPR* xexpr = NULL;
   SCIP_CONSEXPR_EXPR* yexpr = NULL;
   SCIP_Real a, b, c, d, e;
   SCIP_Real nomfac = 1.0;
   SCIP_Real numconst = 0.0;

   assert(scip != NULL);
   assert(conshdlr != NULL);
   assert(expr != NULL);

   *success = FALSE;
   a = 0.0;
   b = 0.0;
   c = 0.0;
   d = 0.0;
   e = 0.0;

   /* possible structures only have two children */
   if( SCIPgetConsExprExprNChildren(expr) != 2 )
      return SCIP_OKAY;

   /* collect expression handlers */
   prodhdlr = SCIPgetConsExprExprHdlrProduct(conshdlr);
   sumhdlr = SCIPgetConsExprExprHdlrSum(conshdlr);
   powhdlr = SCIPgetConsExprExprHdlrPower(conshdlr);

   /* expression must be either a product or a sum */
   if( SCIPgetConsExprExprHdlr(expr) != prodhdlr && SCIPgetConsExprExprHdlr(expr) != sumhdlr )
      return SCIP_OKAY;

   children = SCIPgetConsExprExprChildren(expr);
   assert(children != NULL);

   /* case: prod(f(x), pow(g(y),-1)) */
   if( SCIPgetConsExprExprHdlr(expr) == prodhdlr )
   {
      if( SCIPgetConsExprExprHdlr(children[0]) == powhdlr && SCIPgetConsExprExprPowExponent(children[0]) == -1.0 )  /*lint !e777*/
      {
         denomexpr = SCIPgetConsExprExprChildren(children[0])[0];
         numexpr = children[1];
      }
      else if( SCIPgetConsExprExprHdlr(children[1]) == powhdlr && SCIPgetConsExprExprPowExponent(children[1]) == -1.0 )  /*lint !e777*/
      {
         denomexpr = SCIPgetConsExprExprChildren(children[1])[0];
         numexpr = children[0];
      }

      /* remember to scale the numerator by the coefficient stored in the product expression */
      nomfac = SCIPgetConsExprExprProductCoef(expr);
   }
   /* case: sum(prod(f(x),pow(g(y),-1)), pow(g(y),-1)) */
   else
   {
      SCIP_Real* sumcoefs;

      assert(SCIPgetConsExprExprHdlr(expr) == sumhdlr);
      sumcoefs = SCIPgetConsExprExprSumCoefs(expr);

      /* children[0] is 1/g(y) and children[1] is a product of f(x) and 1/g(y) */
      if( SCIPgetConsExprExprHdlr(children[0]) == powhdlr && SCIPgetConsExprExprPowExponent(children[0]) == -1.0
         && SCIPgetConsExprExprHdlr(children[1]) == prodhdlr && SCIPgetConsExprExprNChildren(children[1]) == 2 )  /* lint !e777 */
      {
         SCIP_Real prodcoef = SCIPgetConsExprExprProductCoef(children[1]);

         if( children[0] == SCIPgetConsExprExprChildren(children[1])[0] )
         {
            denomexpr = SCIPgetConsExprExprChildren(children[0])[0];
            numexpr = SCIPgetConsExprExprChildren(children[1])[1];
         }
         else if( children[0] == SCIPgetConsExprExprChildren(children[1])[1] )
         {
            denomexpr = SCIPgetConsExprExprChildren(children[0])[0];
            numexpr = SCIPgetConsExprExprChildren(children[1])[0];
         }

         /* remember scalar and constant for numerator */
         nomfac = sumcoefs[1] * prodcoef;
         numconst = sumcoefs[0];
      }
      /* children[1] is 1/g(y) and children[0] is a product of f(x) and 1/g(y) */
      else if( SCIPgetConsExprExprHdlr(children[1]) == powhdlr && SCIPgetConsExprExprPowExponent(children[1]) == -1.0
         && SCIPgetConsExprExprHdlr(children[0]) == prodhdlr && SCIPgetConsExprExprNChildren(children[0]) == 2 )  /* lint !e777 */
      {
         SCIP_Real prodcoef = SCIPgetConsExprExprProductCoef(children[0]);

         if( children[1] == SCIPgetConsExprExprChildren(children[0])[0] )
         {
            denomexpr = SCIPgetConsExprExprChildren(children[1])[0];
            numexpr = SCIPgetConsExprExprChildren(children[0])[1];
         }
         else if( children[1] == SCIPgetConsExprExprChildren(children[0])[1] )
         {
            denomexpr = SCIPgetConsExprExprChildren(children[1])[0];
            numexpr = SCIPgetConsExprExprChildren(children[0])[0];
         }

         /* remember scalar and constant for numerator */
         nomfac = sumcoefs[0] * prodcoef;
         numconst = sumcoefs[1];
      }

      /* remember the constant of the sum expression */
      e = SCIPgetConsExprExprSumConstant(expr);
   }

   if( denomexpr != NULL && numexpr != NULL )
   {
      /* transform numerator and denominator to detect structures like (a * f(x) + b) / (c * f(x) + d) */
      transformExpr(numexpr, conshdlr, &xexpr, &a, &b);
      transformExpr(denomexpr, conshdlr, &yexpr, &c, &d);

      SCIPdebugMsg(scip, "detected numerator (%g * %p + %g) and denominator (%g * %p + %g)\n", a, (void*)xexpr, b,
         c, (void*)yexpr, d);

      /* detection can only be successful if the expression of the numerator an denominator are the same or when it is
       * possible to create auxiliary variables, i.e., during SOLVING stage
       */
      if( xexpr == yexpr )
      {
         *success = TRUE;
      }
      else if( SCIPgetStage(scip) == SCIP_STAGE_SOLVING )
      {
         xexpr = numexpr;
         a = 1.0;
         b = 0.0;

#ifdef SCIP_DEBUG
         SCIPinfoMessage(scip, NULL, "Expression for numerator: ");
         SCIP_CALL( SCIPprintConsExprExpr(scip, conshdlr, numexpr, NULL) );
#endif

         yexpr = denomexpr;
         c = 1.0;
         d = 0.0;

#ifdef SCIP_DEBUG
         SCIPinfoMessage(scip, NULL, "Expression for denominator: ");
         SCIP_CALL( SCIPprintConsExprExpr(scip, conshdlr, denomexpr, NULL) );
#endif

         *success = TRUE;
      }
   }

   /* create nonlinear handler expression data */
   if( *success )
   {
      assert(xexpr != NULL);
      assert(xexpr != NULL);
      assert(a != 0.0);
      assert(c != 0.0);

      assert(SCIPgetStage(scip) == SCIP_STAGE_SOLVING || xexpr == yexpr);

      /* create auxiliary variables for xexpr and yexpr in SOLVING stage */
      if( SCIPgetStage(scip) == SCIP_STAGE_SOLVING )
      {
         SCIP_CALL( SCIPcreateConsExprExprAuxVar(scip, conshdlr, xexpr, NULL) );
         SCIP_CALL( SCIPcreateConsExprExprAuxVar(scip, conshdlr, yexpr, NULL) );
         SCIPdebugMsg(scip, "added auxiliary variable for %p and %p\n", (void*)xexpr, (void*)yexpr);
      }

      a = nomfac * a;
      b = nomfac * b + numconst;

      SCIPdebug( SCIP_CALL( SCIPprintConsExprExpr(scip, conshdlr, expr, NULL) ); )
      SCIPdebug( SCIPinfoMessage(scip, NULL, "\n") );
      SCIPdebugMsg(scip, "detected quotient expression (%g * %p + %g) / (%g * %p + %g) + %g\n", a, (void*)xexpr,
         b, c, (void*)yexpr, d, e);
      SCIP_CALL( exprdataCreate(scip, nlhdlrexprdata, xexpr, a, b, yexpr, c, d, e) );
   }

   return SCIP_OKAY;
}

/** helper method to compute interval for (a x + b) / (c x + d) + e */
static
SCIP_INTERVAL intEvalQuotient(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_INTERVAL         bnds,               /**< bounds on x */
   SCIP_Real             a,                  /**< coefficient in numerator */
   SCIP_Real             b,                  /**< constant in numerator */
   SCIP_Real             c,                  /**< coefficient in denominator */
   SCIP_Real             d,                  /**< constant in denominator */
   SCIP_Real             e                   /**< constant */
   )
{
   SCIP_INTERVAL result;
   SCIP_INTERVAL denominterval;
   SCIP_INTERVAL numinterval;
   int i;

   assert(scip != NULL);

   /* return empty interval if the domain of x is empty */
   if( SCIPintervalIsEmpty(SCIP_INTERVAL_INFINITY, bnds) )
   {
      SCIPintervalSetEmpty(&result);
      return result;
   }

   /* compute bounds for denominator */
   SCIPintervalMulScalar(SCIP_INTERVAL_INFINITY, &denominterval, bnds, c);
   SCIPintervalAddScalar(SCIP_INTERVAL_INFINITY, &denominterval, denominterval, d);

   /* there is no useful interval if 0 is in the interior of the interval of the denominator */
   if( SCIPintervalGetInf(denominterval) < 0.0 && SCIPintervalGetSup(denominterval) > 0.0 )
   {
      SCIPintervalSetEntire(SCIP_INTERVAL_INFINITY, &result);
      return result;
   }

   /* a d = b c implies that f(x) = b / d + e, i.e., f is constant */
   if( a*d - b*c == 0.0 )
   {
      SCIPintervalSet(&result, b / d + e);
      return result;
   }

   /*
    * evaluate for [x.inf,x.inf] and [x.sup,x.sup] independently
    */
   SCIPintervalSetEmpty(&result);

   for( i = 0; i < 2; ++i )
   {
      SCIP_INTERVAL quotinterval;
      SCIP_Real val = (i == 0) ? bnds.inf : bnds.sup;

      /* set the resulting interval to a / c if the bounds is infinite */
      if( SCIPisInfinity(scip, REALABS(val)) )
      {
         SCIPintervalSet(&quotinterval, a);
         SCIPintervalDivScalar(SCIP_INTERVAL_INFINITY, &quotinterval, quotinterval, c);
      }
      else
      {
         /* a x' + b */
         SCIPintervalSet(&numinterval, val);
         SCIPintervalMulScalar(SCIP_INTERVAL_INFINITY, &numinterval, numinterval, a);
         SCIPintervalAddScalar(SCIP_INTERVAL_INFINITY, &numinterval, numinterval, b);

         /* c x' + d */
         SCIPintervalSet(&denominterval, val);
         SCIPintervalMulScalar(SCIP_INTERVAL_INFINITY, &denominterval, denominterval, c);
         SCIPintervalAddScalar(SCIP_INTERVAL_INFINITY, &denominterval, denominterval, d);

         /* (a x' + b) / (c x' + d) + e */
         SCIPintervalDiv(SCIP_INTERVAL_INFINITY, &quotinterval, numinterval, denominterval);
         SCIPintervalAddScalar(SCIP_INTERVAL_INFINITY, &quotinterval, quotinterval, e);
      }

      /* unify with the resulting interval */
      SCIPintervalUnify(&result, result, quotinterval);
   }

   return result;
}

/** helper method to compute reverse propagation for (a x + b) / (c x + d) + e */
static
SCIP_INTERVAL reversepropQuotient(
   SCIP_INTERVAL         bnds,               /**< bounds on (a x + b) / (c x + d) + e */
   SCIP_Real             a,                  /**< coefficient in numerator */
   SCIP_Real             b,                  /**< constant in numerator */
   SCIP_Real             c,                  /**< coefficient in denominator */
   SCIP_Real             d,                  /**< constant in denominator */
   SCIP_Real             e                   /**< constant */
   )
{
   SCIP_INTERVAL result;
   int i;

   SCIPintervalSetEmpty(&result);

   /* return empty interval if the domain of the expression is empty */
   if( SCIPintervalIsEmpty(SCIP_INTERVAL_INFINITY, bnds) )
      return result;

   /* substract constant from bounds of the expression */
   SCIPintervalSubScalar(SCIP_INTERVAL_INFINITY, &bnds, bnds, e);

   /* if the expression is constant or the limit lies inside the domain, nothing can be propagated */
   if( a*d - b*c == 0.0 || (bnds.inf < a / c && bnds.sup > a / c) )
   {
      SCIPintervalSetEntire(SCIP_INTERVAL_INFINITY, &result);
      return result;
   }

   /* compute bounds for [x.inf,x.inf] and [x.sup,x.sup] independently */
   for( i = 0; i < 2; ++i )
   {
      SCIP_INTERVAL denominator;
      SCIP_INTERVAL numerator;
      SCIP_INTERVAL quotient;
      SCIP_Real val = (i == 0) ? bnds.inf : bnds.sup;

      /* (d * x' - b) */
      SCIPintervalSet(&numerator, d);
      SCIPintervalMulScalar(SCIP_INTERVAL_INFINITY, &numerator, numerator, val);
      SCIPintervalAddScalar(SCIP_INTERVAL_INFINITY, &numerator, numerator, -b);

      /* (a - c * x') */
      SCIPintervalSet(&denominator, -c);
      SCIPintervalMulScalar(SCIP_INTERVAL_INFINITY, &denominator, denominator, val);
      SCIPintervalAddScalar(SCIP_INTERVAL_INFINITY, &denominator, denominator, a);

      /* (d * x' - b) / (a - c * x') */
      SCIPintervalDiv(SCIP_INTERVAL_INFINITY, &quotient, numerator, denominator);

      /* unify with the resulting interval */
      SCIPintervalUnify(&result, result, quotient);
   }

   return result;
}

/** adds data to given rowprep; the generated estimator is always locally valid
 *
 *  @note the constant is moved to the left- or right-hand side
 *  @note other than the name of this function may indicate, it does not create a rowprep
 */
static
SCIP_RETCODE createRowprep(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_ROWPREP *        rowprep,            /**< a rowprep where to store the estimator */
   SCIP_VAR**            vars,               /**< variables */
   SCIP_Real*            coefs,              /**< coefficients */
   SCIP_Real             constant,           /**< constant */
   int                   nlinvars            /**< total number of variables */
   )
{
   assert(scip != NULL);
   assert(rowprep != NULL);
   assert(coefs != NULL);
   assert(vars != NULL);

   /* create rowprep */
   SCIPaddRowprepSide(rowprep, -constant);
   SCIP_CALL( SCIPensureRowprepSize(scip, rowprep, nlinvars + 1) );

   /* add coefficients */
   SCIP_CALL( SCIPaddRowprepTerms(scip, rowprep, nlinvars, vars, coefs) );

   return SCIP_OKAY;
}

/** computes an estimator at a given point for the univariate case (ax + b) / (cx + d) + e
 *
 *  Depending on the reference point, the estimator is a tangent or a secant on the graph.
 *  It depends on whether we are under- or overestimating, whether we are on the left or
 *  on the right side of the singularity at -d/c, and whether it is the monotone increasing
 *  (ad - bc > 0) or decreasing part (ad - bc < 0). Together, there are 8 cases:
 *
 *  mon. incr. + overestimate + left hand side  -->  secant
 *  mon. incr. + overestimate + right hand side -->  tangent
 *  mon. incr. + understimate + left hand side  -->  tangent
 *  mon. incr. + understimate + right hand side -->  secant
 *  mon. decr. + overestimate + left hand side  -->  tangent
 *  mon. decr. + overestimate + right hand side -->  secant
 *  mon. decr. + understimate + left hand side  -->  secant
 *  mon. decr. + understimate + right hand side -->  tangent
 */
static
SCIP_RETCODE estimateUnivariate(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_Real             lbx,                /**< lower bound of x */
   SCIP_Real             ubx,                /**< upper bound of x */
   SCIP_Real             solx,               /**< solution value of x */
   SCIP_Real             a,                  /**< coefficient in numerator */
   SCIP_Real             b,                  /**< constant in numerator */
   SCIP_Real             c,                  /**< coefficient in denominator */
   SCIP_Real             d,                  /**< constant in denominator */
   SCIP_Real             e,                  /**< constant */
   SCIP_Real*            coef,               /**< pointer to store the coefficient */
   SCIP_Real*            constant,           /**< pointer to store the constant */
   SCIP_Bool             overestimate,       /**< whether the expression should be overestimated */
   SCIP_Bool*            success             /**< buffer to store whether separation was successful */
   )
{
   SCIP_Real singularity;
   SCIP_Bool isinleftpart;
   SCIP_Bool monincreasing;

   assert(lbx <= solx && solx <= ubx);
   assert(coef != NULL);
   assert(constant != NULL);
   assert(success != NULL);

   *success = FALSE;
   *coef = 0.0;
   *constant = 0.0;
   singularity = -d / c;

   /* if 0 is in the denom interval, estimation is not possible */
   if( SCIPisLE(scip, lbx, singularity) && SCIPisGE(scip, ubx, singularity) )
      return SCIP_OKAY;

   isinleftpart = (ubx < singularity);
   monincreasing = (a * d - b * c > 0.0);

   /* this encodes the 8 cases explained above */
   if( monincreasing == (overestimate == isinleftpart) )
   {
      SCIP_Real lbeval;
      SCIP_Real ubeval;

      /* if one of the bounds is infinite, secant cannot be computed */
      if( SCIPisInfinity(scip, -lbx) || SCIPisInfinity(scip, ubx) )
         return SCIP_OKAY;

      lbeval = (a * lbx + b) / (c * lbx + d) + e;
      ubeval = (a * ubx + b) / (c * ubx + d) + e;

      /* compute coefficient and constant of linear estimator */
      *coef = (ubeval - lbeval) / (ubx - lbx);
      *constant = ubeval - (*coef) * ubx;
   }
   else
   {
      SCIP_Real soleval;

      soleval = (a * solx + b) / (c * solx + d) + e;

      /* compute coefficient and constant of linear estimator */
      *coef = (a * d - b * c) / SQR(d + c * solx);
      *constant = soleval - (*coef) * solx;
   }

   /* avoid huge values in the cut */
   if( SCIPisHugeValue(scip, REALABS(*coef)) || SCIPisHugeValue(scip, REALABS(*constant)) )
      return SCIP_OKAY;

   *success = TRUE;

   return SCIP_OKAY;
}

/** helper method to compute estimator for the univariate case; the estimator is stored in a given rowprep */
static
SCIP_RETCODE estimateUnivariateQuotient(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_SOL*             sol,                /**< solution point (or NULL for the LP solution) */
   SCIP_VAR*             x,                  /**< argument variable */
   SCIP_Real             a,                  /**< coefficient in numerator */
   SCIP_Real             b,                  /**< constant in numerator */
   SCIP_Real             c,                  /**< coefficient in denominator */
   SCIP_Real             d,                  /**< constant in denominator */
   SCIP_Real             e,                  /**< constant */
   SCIP_Bool             overestimate,       /**< whether the expression should be overestimated */
   SCIP_ROWPREP*         rowprep,            /**< a rowprep where to store the estimator */
   SCIP_Bool*            success             /**< buffer to store whether separation was successful */
   )
{
   SCIP_Real constant;
   SCIP_Real coef;
   SCIP_Real lbx;
   SCIP_Real ubx;
   SCIP_Real solx;

   /* get variable bounds */
   lbx = SCIPvarGetLbLocal(x);
   ubx = SCIPvarGetUbLocal(x);

   /* get and adjust solution value */
   solx = SCIPgetSolVal(scip, sol, x);
   solx = MIN(MAX(solx, lbx), ubx);

   /* compute an estimator */
   SCIP_CALL( estimateUnivariate(scip, lbx, ubx, solx, a, b, c, d, e, &coef, &constant, overestimate, success) );

   /* add estimator to rowprep, if successful */
   if( *success )
   {
      (void) SCIPsnprintf(rowprep->name, SCIP_MAXSTRLEN, "quot_%s_%lld", SCIPvarGetName(x), SCIPgetNLPs(scip));
      SCIP_CALL( createRowprep(scip, rowprep, &x, &coef, constant, 1) );
   }

   return SCIP_OKAY;
}

/** helper method to compute a gradient cut for
 *
 *     h^c(x,y) := 1/y ((x + sqrt(lbx * ubx)) / (sqrt(lbx) + sqrt(ubx)))^2
 *
 *  at a given reference point, see Zamora and Grossmann (1988) for more details
 */
static
void hcGradCut(
   SCIP_Real             lbx,                /**< lower bound of x */
   SCIP_Real             ubx,                /**< upper bound of x */
   SCIP_Real             solx,               /**< solution value of x */
   SCIP_Real             soly,               /**< solution value of y */
   SCIP_Real*            coefx,              /**< pointer to store the coefficient of x */
   SCIP_Real*            coefy,              /**< pointer to store the coefficient of y */
   SCIP_Real*            constant            /**< pointer to store the constant */
   )
{
   SCIP_Real tmp1;
   SCIP_Real tmp2;

   assert(lbx >= 0.0);
   assert(lbx <= ubx);
   assert(soly > 0.0);
   assert(coefx != NULL);
   assert(coefy != NULL);
   assert(constant != NULL);

   tmp1 = SQRT(lbx * ubx) + solx;
   tmp2 = SQR(SQRT(lbx) + SQRT(ubx)) * soly; /*lint !e666*/
   assert(tmp2 > 0.0);

   *coefx = 2.0 * tmp1 / tmp2;
   *coefy = -SQR(tmp1) / (tmp2 * soly);
   *constant = 2.0 * SQRT(lbx * ubx) * tmp1 / tmp2;
}

/** computes an over- or underestimator at a given point for the bivariate case x/y <=/>= z
 *
 *  There are the following cases for y > 0:
 *
 *    1. lbx < 0 < ubx
 *          Rewrite x / y = z as x = y * z and use McCormick to compute a valid inequality of the form
 *          x = y * z <= a * y +  b * z + c. Note that b > 0 because of y > 0. The inequality is then transformed
 *          to x / b - a/b * y - c/b <= z, which results in a valid underestimator for x / y over the set
 *          {(x,y) | lbz <= x / y <= ubz}. Note that overestimating/underestimating the bilinear term with McCormick
 *          results in an underestimator/overestimator for x / y.
 *
 *    2. lbx >= 0 or ubx <= 0
 *       a) overestimation:  use z <= 1/(lby*uby) * min{uby*x - lbx*y + lbx*lby, lby*x - ubx*y + ubx*uby}
 *       b) underestimation: use z >= x/y >= (1/y) * ( (x + sqrt(lbx * ubx)) / (sqrt(lbx) + sqrt(ubx)) )
 *                           and build gradient cut
 *
 *    If y < 0, swap and negate its bounds and compute the respective opposite estimator (and negate it).
 *    If 0 is in the interval of y, nothing is possible.
 */
static
SCIP_RETCODE estimateBivariate(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_Real             lbx,                /**< lower bound of x */
   SCIP_Real             ubx,                /**< upper bound of x */
   SCIP_Real             lby,                /**< lower bound of y */
   SCIP_Real             uby,                /**< upper bound of y */
   SCIP_Real             lbz,                /**< lower bound of z */
   SCIP_Real             ubz,                /**< lower bound of z */
   SCIP_Real             solx,               /**< reference point for x */
   SCIP_Real             soly,               /**< reference point for y */
   SCIP_Real             solz,               /**< reference point for z */
   SCIP_Bool             overestimate,       /**< whether the expression should be overestimated */
   SCIP_Real*            coefx,              /**< pointer to store the x coefficient */
   SCIP_Real*            coefy,              /**< pointer to store the y coefficient */
   SCIP_Real*            constant,           /**< pointer to store the constant */
   SCIP_Bool*            success             /**< buffer to store whether computing the estimator was successful */
   )
{
   SCIP_Bool negatedx = FALSE;
   SCIP_Bool negatedy = FALSE;

   assert(lbx <= solx && solx <= ubx);
   assert(lby <= soly && soly <= uby);
   assert(lbz <= solz && solz <= ubz);
   assert(coefx != NULL);
   assert(coefy != NULL);
   assert(constant != NULL);
   assert(success != NULL);

   *success = TRUE;
   *coefx = 0.0;
   *coefy = 0.0;
   *constant = 0.0;

   /* if 0 is in [lby,uby], then it is not possible to compute an estimator */
   if( SCIPisLE(scip, lby, 0.0) && SCIPisGE(scip, uby, 0.0) )
   {
      *success = FALSE;
      return SCIP_OKAY;
   }

   /* negate bounds of y if it is not positive */
   if( uby < 0.0 )
   {
      SCIP_Real tmp = uby;

      uby = -lby;
      lby = -tmp;
      soly = -soly;
      negatedy = TRUE;
      overestimate = !overestimate;
   }

   /* case 1: 0 is in the interior of [lbx,ubx] */
   if( lbx < 0.0 && 0.0 < ubx )
   {
      SCIP_Real mccoefy = 0.0;
      SCIP_Real mccoefaux = 0.0;
      SCIP_Real mcconst = 0.0;

      /* as explained in the description of this method, overestimating/underestimating the bilinear term results in an
       * underestimator/overestimator for x / y
       */
      SCIPaddBilinMcCormick(scip, 1.0, lbz, ubz, solz, lby, uby, soly, !overestimate, &mccoefaux, &mccoefy, &mcconst,
         success);
      assert(mccoefaux >= 0.0);

      if( !(*success) )
         return SCIP_OKAY;

      /* resulting estimator is x/b - a/b * y - c/b, where a*y +  b*z + c is the estimator for y*z */
      *coefx = 1.0 / mccoefaux;
      *coefy = -mccoefy / mccoefaux;
      *constant = -mcconst / mccoefaux;
   }
   /* case 2: 0 is not in the interior of [lbx,ubx] */
   else
   {
      /* negate bounds of x if it is negative */
      if( ubx < 0.0 )
      {
         SCIP_Real tmp = ubx;

         ubx = -lbx;
         lbx = -tmp;
         solx = -solx;
         negatedx = TRUE;
         overestimate = !overestimate;
      }

      /* case 2a */
      if( overestimate )
      {
         /* check where the minimum is attained */
         if( uby * solx - lbx * soly + lbx * lby <= lby * solx - ubx * soly + ubx * uby )
         {
            *coefx = 1.0 / lby;
            *coefy = -lbx / (lby * uby);
            *constant = lbx / uby;
         }
         else
         {
            *coefx = 1.0 / uby;
            *coefy = -ubx / (lby * uby);
            *constant = ubx / lby;
         }
      }
      /* case 2b */
      else
      {
         /* compute gradient cut for h^c(x,y) at (solx,soly) */
         hcGradCut(lbx, ubx, solx, soly, coefx, coefy, constant);
      }
   }

   /* reverse negations of x and y in the resulting estimator */
   if( negatedx )
      *coefx = -(*coefx);
   if( negatedy )
      *coefy = -(*coefy);

   /* if exactly one variable has been negated, then we have computed an underestimate/overestimate for the negated
    * expression, which results in an overestimate/underestimate for the original expression
    */
   if( negatedx != negatedy )
   {
      *coefx = -(*coefx);
      *coefy = -(*coefy);
      *constant = -(*constant);
   }

   /* avoid huge values in the estimator */
   if( SCIPisHugeValue(scip, REALABS(*coefx)) || SCIPisHugeValue(scip, REALABS(*coefy))
      || SCIPisHugeValue(scip, REALABS(*constant)) )
   {
      *success = FALSE;
      return SCIP_OKAY;
   }

   return SCIP_OKAY;
}

/** method to construct an estimator for a quotient expression of the form (ax + b) / (cy + d) + e; the
 *  resulting estimator is stored in a rowprep; the method first computes an estimator for x' / y' with
 *  x := ax + b and y := cy + d and then transforms this estimator to one for the quotient (ax + b) / (cy + d) + e
 */
static
SCIP_RETCODE estimateBivariateQuotient(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_VAR*             x,                  /**< numerator variable */
   SCIP_VAR*             y,                  /**< denominator variable */
   SCIP_VAR*             auxvar,             /**< auxiliary variable */
   SCIP_SOL*             sol,                /**< solution point (or NULL for the LP solution) */
   SCIP_Real             a,                  /**< coefficient of numerator */
   SCIP_Real             b,                  /**< constant of numerator */
   SCIP_Real             c,                  /**< coefficient of denominator */
   SCIP_Real             d,                  /**< constant of denominator */
   SCIP_Real             e,                  /**< constant term */
   SCIP_Bool             overestimate,       /**< whether the expression should be overestimated */
   SCIP_ROWPREP*         rowprep,            /**< a rowprep where to store the estimator */
   SCIP_Bool*            success             /**< buffer to store whether separation was successful */
   )
{
   SCIP_VAR* vars[2] = {x, y};
   SCIP_Real coefs[2] = {0.0, 0.0};
   SCIP_Real constant = 0.0;
   SCIP_Real solx;
   SCIP_Real soly;
   SCIP_Real solz;
   SCIP_Real lbx;
   SCIP_Real ubx;
   SCIP_Real lby;
   SCIP_Real uby;
   SCIP_Real lbz;
   SCIP_Real ubz;

   assert(x != NULL);
   assert(y != NULL);
   assert(x != y);
   assert(auxvar != NULL);
   assert(rowprep != NULL);
   assert(success != NULL);

   /* get variable bounds */
   lbx = SCIPvarGetLbLocal(x);
   ubx = SCIPvarGetUbLocal(x);
   lby = SCIPvarGetLbLocal(y);
   uby = SCIPvarGetUbLocal(y);
   lbz = SCIPvarGetLbLocal(auxvar);
   ubz = SCIPvarGetUbLocal(auxvar);

   /* get and adjust solution values */
   solx = SCIPgetSolVal(scip, sol, x);
   soly = SCIPgetSolVal(scip, sol, y);
   solz = SCIPgetSolVal(scip, sol, auxvar);
   solx = MIN(MAX(solx, lbx), ubx);
   soly = MIN(MAX(soly, lby), uby);
   solz = MIN(MAX(solz, lbz), ubz);

   /* compute an estimator */
   SCIP_CALL( estimateBivariate(scip,
      MIN(a * lbx, a * ubx) + b, MAX(a * lbx, a * ubx) + b, /* bounds of x' */
      MIN(c * lby, c * uby) + d, MAX(c * lby, c * uby) + d, /* bounds of y' */
      lbz, ubz, a * solx + b, c * soly + d, solz, overestimate, &coefs[0], &coefs[1], &constant, success) );

   /* add estimator to rowprep, if successful */
   if( *success )
   {
      /* transform estimator Ax' + By'+ C = A(ax + b) + B (cy + d) + C = (Aa) x + (Bc) y + (C + Ab + Bd);
       * add the constant e separately
       */
      constant += coefs[0] * b + coefs[1] * d + e;
      coefs[0] *= a;
      coefs[1] *= c;

      /* prepare rowprep */
      (void) SCIPsnprintf(rowprep->name, SCIP_MAXSTRLEN, "quot_%s_%s_%lld", SCIPvarGetName(x), SCIPvarGetName(y),
         SCIPgetNLPs(scip));
      SCIP_CALL( createRowprep(scip, rowprep, vars, coefs, constant, 2) );
   }

   return SCIP_OKAY;
}

/*
 * Callback methods of nonlinear handler
 */

/** nonlinear handler copy callback */
static
SCIP_DECL_CONSEXPR_NLHDLRCOPYHDLR(nlhdlrCopyhdlrQuotient)
{ /*lint --e{715}*/
   assert(targetscip != NULL);
   assert(targetconsexprhdlr != NULL);
   assert(sourcenlhdlr != NULL);
   assert(strcmp(SCIPgetConsExprNlhdlrName(sourcenlhdlr), NLHDLR_NAME) == 0);

   SCIP_CALL( SCIPincludeConsExprNlhdlrQuotient(targetscip, targetconsexprhdlr) );

   return SCIP_OKAY;
}


/** callback to free expression specific data */
static
SCIP_DECL_CONSEXPR_NLHDLRFREEEXPRDATA(nlhdlrFreeExprDataQuotient)
{  /*lint --e{715}*/
   assert(nlhdlrexprdata != NULL);
   assert(*nlhdlrexprdata != NULL);

   /* free expression data of nonlinear handler */
   SCIP_CALL( exprdataFree(scip, nlhdlrexprdata) );

   return SCIP_OKAY;
}


/** callback to detect structure in expression tree */
static
SCIP_DECL_CONSEXPR_NLHDLRDETECT(nlhdlrDetectQuotient)
{ /*lint --e{715}*/
   assert(nlhdlrexprdata != NULL);

   /* call detection routine */
   SCIP_CALL( detectExpr(scip, conshdlr, expr, nlhdlrexprdata, success) );

   return SCIP_OKAY;
}


/** auxiliary evaluation callback of nonlinear handler */
static
SCIP_DECL_CONSEXPR_NLHDLREVALAUX(nlhdlrEvalauxQuotient)
{ /*lint --e{715}*/
   SCIP_VAR* auxvarx;
   SCIP_VAR* auxvary;
   SCIP_Real solvalx;
   SCIP_Real solvaly;
   SCIP_Real nomval;
   SCIP_Real denomval;

   assert(expr != NULL);
   assert(auxvalue != NULL);

   /* get auxiliary variables */
   auxvarx = SCIPgetConsExprExprAuxVar(nlhdlrexprdata->numexpr);
   auxvary = SCIPgetConsExprExprAuxVar(nlhdlrexprdata->denomexpr);
   assert(auxvarx != NULL);
   assert(auxvary != NULL);

   /* get solution values of the auxiliary variables */
   solvalx = SCIPgetSolVal(scip, sol, auxvarx);
   solvaly = SCIPgetSolVal(scip, sol, auxvary);

   /* evaluate expression w.r.t. the values of the auxiliary variables */
   nomval = nlhdlrexprdata->numcoef *  solvalx + nlhdlrexprdata->numconst;
   denomval = nlhdlrexprdata->denomcoef *  solvaly + nlhdlrexprdata->denomconst;

   /* return SCIP_INVALID if the denominator evaluates to zero */
   *auxvalue = (denomval != 0.0) ? nlhdlrexprdata->constant + nomval / denomval : SCIP_INVALID;

   return SCIP_OKAY;
}


/** nonlinear handler under/overestimation callback
 *
 * @todo which of the paramters did I not use, but have to be taken into consideration?
*/
static
SCIP_DECL_CONSEXPR_NLHDLRESTIMATE(nlhdlrEstimateQuotient)
{ /*lint --e{715}*/
   SCIP_VAR* auxvarx;
   SCIP_VAR* auxvary;

   assert(conshdlr != NULL);
   assert(nlhdlr != NULL);
   assert(expr != NULL);
   assert(nlhdlrexprdata != NULL);
   assert(rowprep != NULL);

   *success = FALSE;

   /* get auxiliary variables */
   auxvarx = SCIPgetConsExprExprAuxVar(nlhdlrexprdata->numexpr);
   auxvary = SCIPgetConsExprExprAuxVar(nlhdlrexprdata->denomexpr);

   if( auxvarx == auxvary )
   {
      assert(nlhdlrexprdata->numexpr == nlhdlrexprdata->denomexpr);

      /* univariate case */
      SCIP_CALL( estimateUnivariateQuotient(scip, sol, auxvarx, nlhdlrexprdata->numcoef, nlhdlrexprdata->numconst,
         nlhdlrexprdata->denomcoef, nlhdlrexprdata->denomconst, nlhdlrexprdata->constant, overestimate, rowprep,
         success) );
   }
   else
   {
      assert(nlhdlrexprdata->numexpr != nlhdlrexprdata->denomexpr);

      /* bivariate case */
      SCIP_CALL( estimateBivariateQuotient(scip, auxvarx, auxvary, SCIPgetConsExprExprAuxVar(expr), sol,
         nlhdlrexprdata->numcoef, nlhdlrexprdata->numconst, nlhdlrexprdata->denomcoef, nlhdlrexprdata->denomconst,
         nlhdlrexprdata->constant, overestimate, rowprep, success) );
   }

   return SCIP_OKAY;
}


/** nonlinear handler interval evaluation callback */
static
SCIP_DECL_CONSEXPR_NLHDLRINTEVAL(nlhdlrIntevalQuotient)
{ /*lint --e{715}*/
   SCIP_INTERVAL bnds;

   assert(nlhdlrexprdata != NULL);
   assert(nlhdlrexprdata->numexpr != NULL);
   assert(nlhdlrexprdata->denomexpr != NULL);

   /* it is not possible to compute tighter intervals if both expressions are different */
   if( nlhdlrexprdata->numexpr != nlhdlrexprdata->denomexpr )
      return SCIP_OKAY;

   /* get activity of the numerator (= denominator) expression */
   bnds = SCIPgetConsExprExprActivity(scip, nlhdlrexprdata->numexpr);

   /* call interval evaluation for the univariate quotient expression */
   *interval = intEvalQuotient(scip, bnds, nlhdlrexprdata->numcoef, nlhdlrexprdata->numconst,
      nlhdlrexprdata->denomcoef, nlhdlrexprdata->denomconst, nlhdlrexprdata->constant);

   return SCIP_OKAY;
}


/** nonlinear handler callback for reverse propagation */
static
SCIP_DECL_CONSEXPR_NLHDLRREVERSEPROP(nlhdlrReversepropQuotient)
{ /*lint --e{715}*/
   SCIP_INTERVAL exprbounds;
   SCIP_INTERVAL result;

   assert(nlhdlrexprdata != NULL);
   assert(nlhdlrexprdata->numexpr != NULL);
   assert(nlhdlrexprdata->denomexpr != NULL);

   /* it is not possible to compute tighter intervals if both expressions are different */
   if( nlhdlrexprdata->numexpr != nlhdlrexprdata->denomexpr )
      return SCIP_OKAY;

   /* get activity of the expression and the numerator (= denominator) expression */
   exprbounds = SCIPgetConsExprExprActivity(scip, expr);

   SCIPdebugMsg(scip, "call reverse propagation for expression (%g %p + %g) / (%g %p + %g) + %g bounds [%g,%g]\n",
      nlhdlrexprdata->numcoef, (void*)nlhdlrexprdata->numexpr, nlhdlrexprdata->numconst,
      nlhdlrexprdata->denomcoef, (void*)nlhdlrexprdata->denomexpr, nlhdlrexprdata->denomconst,
      nlhdlrexprdata->constant, exprbounds.inf, exprbounds.sup);

   /* call reverse propagation */
   result = reversepropQuotient(exprbounds, nlhdlrexprdata->numcoef, nlhdlrexprdata->numconst,
      nlhdlrexprdata->denomcoef, nlhdlrexprdata->denomconst, nlhdlrexprdata->constant);

   SCIPdebugMsg(scip, "try to tighten bounds of %p: [%g,%g] -> [%g,%g]\n",
      (void*)nlhdlrexprdata->numexpr, SCIPgetConsExprExprActivity(scip, nlhdlrexprdata->numexpr).inf,
      SCIPgetConsExprExprActivity(scip, nlhdlrexprdata->numexpr).sup, result.inf, result.sup);

   /* tighten bounds of the expression */
   SCIP_CALL( SCIPtightenConsExprExprInterval(scip, conshdlr, nlhdlrexprdata->numexpr, result, force,
      reversepropqueue, infeasible, nreductions) );

   return SCIP_OKAY;
}


/*
 * nonlinear handler specific interface methods
 */

/** includes Quotient nonlinear handler to consexpr */
SCIP_RETCODE SCIPincludeConsExprNlhdlrQuotient(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSHDLR*        consexprhdlr        /**< expression constraint handler */
   )
{
   SCIP_CONSEXPR_NLHDLRDATA* nlhdlrdata;
   SCIP_CONSEXPR_NLHDLR* nlhdlr;

   assert(scip != NULL);
   assert(consexprhdlr != NULL);

   /* create nonlinear handler data */
   nlhdlrdata = NULL;

   SCIP_CALL( SCIPincludeConsExprNlhdlrBasic(scip, consexprhdlr, &nlhdlr, NLHDLR_NAME,
      NLHDLR_DESC, NLHDLR_PRIORITY, nlhdlrDetectQuotient, nlhdlrEvalauxQuotient, nlhdlrdata) );
   assert(nlhdlr != NULL);

   SCIPsetConsExprNlhdlrCopyHdlr(scip, nlhdlr, nlhdlrCopyhdlrQuotient);
   SCIPsetConsExprNlhdlrFreeExprData(scip, nlhdlr, nlhdlrFreeExprDataQuotient);
   SCIPsetConsExprNlhdlrSepa(scip, nlhdlr, NULL, NULL, nlhdlrEstimateQuotient, NULL);
   SCIPsetConsExprNlhdlrProp(scip, nlhdlr, nlhdlrIntevalQuotient, nlhdlrReversepropQuotient);

   return SCIP_OKAY;
}
