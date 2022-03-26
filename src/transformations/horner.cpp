/***********************************************************************

    Interfacing to a classic piece of geodetic software

************************************************************************

    gen_pol is a highly efficient, classic implementation of a generic
    2D Horner's Scheme polynomial evaluation routine by Knud Poder and
    Karsten Engsager, originating in the vivid geodetic environment at
    what was then (1960-ish) the Danish Geodetic Institute.

    The original Poder/Engsager gen_pol implementation (where
    the polynomial degree and two sets of polynomial coefficients
    are packed together in one compound array, handled via a plain
    double pointer) is compelling and "true to the code history":

    It has a beautiful classical 1960s ring to it, not unlike the
    original fft implementations, which revolutionized spectral
    analysis in twenty lines of code.

    The Poder coding sound, as classic 1960s as Phil Spector's Wall
    of Sound, is beautiful and inimitable.

	On the other hand: For the uninitiated, the gen_pol code is hard
    to follow, despite being compact.

    Also, since adding metadata and improving maintainability
    of the code are among the implied goals of a current SDFE/DTU Space
	project, the material in this file introduces a version with a
	more modern (or at least 1990s) look, introducing a "double 2D
	polynomial" data type, HORNER.

    Despite introducing a new data type for handling the polynomial
    coefficients, great care has been taken to keep the coefficient
    array organization identical to that of gen_pol.

    Hence, on one hand, the HORNER data type helps improving the
    long term maintainability of the code by making the data
    organization more mentally accessible.

    On the other hand, it allows us to preserve the business end of
    the original gen_pol implementation - although not including the
	famous "Poder dual autocheck" in all its enigmatic elegance.

 **********************************************************************

	The material included here was written by Knud Poder, starting
	around 1960, and Karsten Engsager, starting around 1970. It was
    originally written in Algol 60, later (1980s) reimplemented in C.

    The HORNER data type interface, and the organization as a header
    library was implemented by Thomas Knudsen, starting around 2015.

 ***********************************************************************
 *
 * Copyright (c) 2016, SDFE http://www.sdfe.dk / Thomas Knudsen / Karsten Engsager
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 *****************************************************************************/

#define PJ_LIB__

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <complex>

#include "proj.h"
#include "proj_internal.h"

PROJ_HEAD(horner, "Horner polynomial evaluation");

/* make horner.h interface with proj's memory management */
#define horner_dealloc(x) free(x)
#define horner_calloc(n,x) calloc(n,x)

namespace { // anonymous namespace
struct horner {
    int    uneg;     /* u axis negated? */
    int    vneg;     /* v axis negated? */
    int    order;    /* maximum degree of polynomium */
    int    coefs;    /* number of coefficients for each polynomium  */
    double range;    /* radius of the region of validity */
    int    has_only_fwd; /* inv parameters are not specified, inverse is done by gauss-newton iteration */
    double inverse_tolerance; /* in the units of the destination coords, specifies when to stop iterating if has_only_fwd and direction is reverse */

    double *fwd_u;   /* coefficients for the forward transformations */
    double *fwd_v;   /* i.e. latitude/longitude to northing/easting  */

    double *inv_u;   /* coefficients for the inverse transformations */
    double *inv_v;   /* i.e. northing/easting to latitude/longitude  */

    double *fwd_c;   /* coefficients for the complex forward transformations */
    double *inv_c;   /* coefficients for the complex inverse transformations */

    PJ_UV *fwd_origin;  /* False longitude/latitude */
    PJ_UV *inv_origin;  /* False easting/northing   */
};
} // anonymous namespace

typedef struct horner HORNER;
static PJ_UV   horner_func (PJ* P, const HORNER *transformation, PJ_DIRECTION direction, PJ_UV position);
static HORNER *horner_alloc (size_t order, int complex_polynomia);
static void    horner_free (HORNER *h);

/* e.g. degree = 2: a + bx + cy + dxx + eyy + fxy, i.e. 6 coefficients */
#define horner_number_of_coefficients(order) \
            (((order + 1)*(order + 2)/2))


static void horner_free (HORNER *h) {
    horner_dealloc (h->inv_v);
    horner_dealloc (h->inv_u);
    horner_dealloc (h->fwd_v);
    horner_dealloc (h->fwd_u);
    horner_dealloc (h->fwd_c);
    horner_dealloc (h->inv_c);
    horner_dealloc (h->fwd_origin);
    horner_dealloc (h->inv_origin);
    horner_dealloc (h);
}


static HORNER *horner_alloc (size_t order, int complex_polynomia) {
    /* size_t is unsigned, so we need not check for order > 0 */
    int n = (int)horner_number_of_coefficients(order);
    int polynomia_ok = 0;
    HORNER *h = static_cast<HORNER*>(horner_calloc (1, sizeof (HORNER)));

    if (nullptr==h)
        return nullptr;

    if (complex_polynomia)
        n = 2*(int)order + 2;
    h->order = (int)order;
    h->coefs = n;

    if (complex_polynomia) {
        h->fwd_c = static_cast<double*>(horner_calloc (n, sizeof(double)));
        h->inv_c = static_cast<double*>(horner_calloc (n, sizeof(double)));
        if (h->fwd_c && h->inv_c)
            polynomia_ok = 1;
    }
    else {
        h->fwd_u = static_cast<double*>(horner_calloc (n, sizeof(double)));
        h->fwd_v = static_cast<double*>(horner_calloc (n, sizeof(double)));
        h->inv_u = static_cast<double*>(horner_calloc (n, sizeof(double)));
        h->inv_v = static_cast<double*>(horner_calloc (n, sizeof(double)));
        if (h->fwd_u && h->fwd_v && h->inv_u && h->inv_v)
            polynomia_ok = 1;
    }

    h->fwd_origin = static_cast<PJ_UV*>(horner_calloc (1, sizeof(PJ_UV)));
    h->inv_origin = static_cast<PJ_UV*>(horner_calloc (1, sizeof(PJ_UV)));

    if (polynomia_ok && h->fwd_origin && h->inv_origin)
        return h;

    /* safe, since all pointers are null-initialized (by calloc) */
    horner_free (h);
    return nullptr;
}




/**********************************************************************/
static PJ_UV horner_func (PJ* P, const HORNER *transformation, PJ_DIRECTION direction, PJ_UV position) {
/***********************************************************************

A reimplementation of the classic Engsager/Poder 2D Horner polynomial
evaluation engine "gen_pol".

This version omits the inimitable Poder "dual autocheck"-machinery,
which here is intended to be implemented at a higher level of the
library: We separate the polynomial evaluation from the quality
control (which, given the limited MTBF for "computing machinery",
typical when Knud Poder invented the dual autocheck method,
was not defensible at that time).

Another difference from the original version is that we return the
result on the stack, rather than accepting pointers to result variables
as input. This results in code that is easy to read:

            projected  = horner (s34j,  1, geographic);
            geographic = horner (s34j, -1, projected );

and experiments have shown that on contemporary architectures, the time
taken for returning even comparatively large objects on the stack (and
the UV is not that large - typically only 16 bytes) is negligibly
different from passing two pointers (i.e. typically also 16 bytes) the
other way.

The polynomium has the form:

P = sum (i = [0 : order])
        sum (j = [0 : order - i])
            pow(par_1, i) * pow(par_2, j) * coef(index(order, i, j))

For numerical stability, the summation is carried out backwards,
summing the tiny high order elements first.

***********************************************************************/

    /* These variable names follow the Engsager/Poder  implementation */
    int     sz;              /* Number of coefficients per polynomial */
    double  range; /* Equivalent to the gen_pol's FLOATLIMIT constant */
    double  n, e;
    PJ_UV uv_error;
    uv_error.u = uv_error.v = HUGE_VAL;

    if (nullptr==transformation)
        return uv_error;

    /* Check for valid value of direction (-1, 0, 1) */
    switch (direction) {
        case PJ_IDENT:    /*  no-op  */
            return position;
        case PJ_FWD:   /* forward */
        case PJ_INV:   /* inverse */
            break;
        default:   /* invalid */
            return uv_error;
    }

    /* Prepare for double Horner */
    sz    =  horner_number_of_coefficients(transformation->order);
    range =  transformation->range;

    const bool iterative_inverse = direction == PJ_INV && transformation->has_only_fwd;

    if (direction==PJ_FWD) {                              /* forward */
        e   = position.u - transformation->fwd_origin->u;
        n   = position.v - transformation->fwd_origin->v;
    } else {                                              /* inverse */
        if (!iterative_inverse) {
            e   = position.u - transformation->inv_origin->u;
            n   = position.v - transformation->inv_origin->v;
        } else {
            // in this case fwd_origin needs to be added in the end
            e = position.u;
            n = position.v;
        }
    }

    if ((fabs(n) > range) || (fabs(e) > range)) {
        proj_errno_set(P, PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN);
        return uv_error;
    } else if (iterative_inverse) {
        /*
         * solve iteratively
         *
         * | E |   | u00 |   | u01 + u02*x + ...         ' u10 + u11*x + u20*y + ... | | x |
         * |   | = |     | + |-------------------------- ' --------------------------| |   |
         * | N |   | v00 |   | v10 + v11*y + v20*x + ... ' v01 + v02*y + ...         | | y |
         *
         * | x |   | Ma ' Mb |-1 | E-u00 |
         * |   | = |-------- |   |       |
         * | y |   | Mc ' Md |   | N-v00 |
         */
        const int order = transformation->order;
        const double tol = transformation->inverse_tolerance;
        const double de = e - transformation->fwd_u[0];
        const double dn = n - transformation->fwd_v[0];
        double x0 = 0.0;
        double y0 = 0.0;
        int loops = 32; // usually converges really fast (1-2 loops)
        bool converged = false;
        while (loops-- > 0 && !converged) {
            double Ma = 0.0;
            double Mb = 0.0;
            double Mc = 0.0;
            double Md = 0.0;
            /* Coefficient pointers */
            double *tcx = transformation->fwd_u;
            double *tcy = transformation->fwd_v;
            for (int i = 0; i <= order; ++i) {
                for (int j = 0; j <= (order-i); ++j) {
                    if (i == 0 && j == 0) {
                        // do nothing
                    } else if (i == 0) {
                        Ma += (*tcx) * pow(x0, j-1);
                        Md += (*tcy) * pow(y0, j-1);
                    } else {
                        Mb += (*tcx) * pow(y0, i-1) * pow(x0, j);
                        Mc += (*tcy) * pow(x0, i-1) * pow(y0, j);
                    }
                    tcx++;
                    tcy++;
                }
            }
            double idet = 1.0 / (Ma*Md - Mb*Mc);
            double x = idet * (Md*de - Mb*dn);
            double y = idet * (Ma*dn - Mc*de);
            converged = (fabs(x-x0) < tol) && (fabs(y-y0) < tol);
            x0 = x;
            y0 = y;
        }
        // if loops have been exhausted and we have not converged yet,
        // we are never going to converge
        if (!converged) {
            proj_errno_set(P, PROJ_ERR_COORD_TRANSFM);
            return uv_error;
        } else {
            position.u = x0 + transformation->fwd_origin->u;
            position.v = y0 + transformation->fwd_origin->v;
        }
    }
    /* The melody of this block is straight out of the great Engsager/Poder songbook */
    else {
        int g =  transformation->order;
        int r = g, c;
        double u, v, N, E;
        /* Coefficient pointers */
        double *tcx = direction == PJ_FWD ? (transformation->fwd_u + sz) : (transformation->inv_u + sz);
        double *tcy = direction == PJ_FWD ? (transformation->fwd_v + sz) : (transformation->inv_v + sz);

        /* Double Horner's scheme: N = n*Cy*e -> yout, E = e*Cx*n -> xout */
        N = *--tcy;
        E = *--tcx;
        for (;    r > 0;    r--) {
            u = *--tcy;
            v = *--tcx;
            for (c = g;    c >= r;    c--) {
                u = n*u + *--tcy;
                v = e*v + *--tcx;
            }
            N = e*N + u;
            E = n*E + v;
        }

        position.u = E;
        position.v = N;
    }

    return position;
}







static PJ_COORD horner_forward_4d (PJ_COORD point, PJ *P) {
    point.uv = horner_func (P, (HORNER *) P->opaque, PJ_FWD, point.uv);
    return point;
}

static PJ_COORD horner_reverse_4d (PJ_COORD point, PJ *P) {
    point.uv = horner_func (P, (HORNER *) P->opaque, PJ_INV, point.uv);
    return point;
}




/**********************************************************************/
static PJ_UV complex_horner (PJ *P, const HORNER *transformation, PJ_DIRECTION direction, PJ_UV position) {
/***********************************************************************

A reimplementation of a classic Engsager/Poder Horner complex
polynomial evaluation engine.

***********************************************************************/

    /* These variable names follow the Engsager/Poder  implementation */
    int     sz;                             /* Number of coefficients */
    double  range; /* Equivalent to the gen_pol's FLOATLIMIT constant */
    double  n, e;
    PJ_UV uv_error;
    uv_error.u = uv_error.v = HUGE_VAL;

    if (nullptr==transformation)
        return uv_error;

    /* Check for valid value of direction (-1, 0, 1) */
    switch (direction) {
        case PJ_IDENT:    /*  no-op  */
            return position;
        case PJ_FWD:   /* forward */
        case PJ_INV:   /* inverse */
            break;
        default:   /* invalid */
            return uv_error;
    }

    /* Prepare for double Horner */
    sz    =  2*transformation->order + 2;
    range =  transformation->range;

    const bool iterative_inverse = direction == PJ_INV && transformation->has_only_fwd;

    if (direction==PJ_FWD) {                              /* forward */
        e  =  position.u - transformation->fwd_origin->u;
        n  =  position.v - transformation->fwd_origin->v;
        if (transformation->uneg)
            e  =  -e;
        if (transformation->vneg)
            n  =  -n;
    } else {                                              /* inverse */
        if (!iterative_inverse) {
            e  =  position.u - transformation->inv_origin->u;
            n  =  position.v - transformation->inv_origin->v;
            if (transformation->uneg)
                e  =  -e;
            if (transformation->vneg)
                n  =  -n;
        } else {
            // in this case fwd_origin and any existing flipping needs to be added in the end
            e = position.u;
            n = position.v;
        }
    }

    if ((fabs(n) > range) || (fabs(e) > range)) {
        proj_errno_set(P, PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN);
        return uv_error;
    }

    if (iterative_inverse) {
        const double tol = transformation->inverse_tolerance;
        const std::complex<double> dZ(n-transformation->fwd_c[0], e-transformation->fwd_c[1]);
        std::complex<double> w0(0.0, 0.0);
        int loops = 32; // usually converges really fast (1-2 loops)
        bool converged = false;
        while (loops-- > 0 && !converged) {
            // coefficient pointers from back to front until the first complex pair (fwd_c0+i*fwd_c1)
            double *cb = transformation->fwd_c;
            double *c = cb + sz;
            cb += 2;
            double E = *--c;
            double N = *--c;
            while (c > cb) {
                double w = w0.real()*E + w0.imag()*N + *--c;
                       N = w0.real()*N - w0.imag()*E + *--c;
                       E = w;
            }
            std::complex<double> det(N, E);
            std::complex<double> w1 = dZ / det;
            converged = (fabs(w1.real()-w0.real()) < tol) && (fabs(w1.imag()-w0.imag()) < tol);
            w0 = w1;
        }
        // if loops have been exhausted and we have not converged yet,
        // we are never going to converge
        if (!converged) {
            proj_errno_set(P, PROJ_ERR_COORD_TRANSFM);
            position = uv_error;
        } else {
            double E = w0.imag();
            double N = w0.real();
            if (transformation->uneg)
                E = -E;
            if (transformation->vneg)
                N = -N;
            position.u = E + transformation->fwd_origin->u;
            position.v = N + transformation->fwd_origin->v;
        }
        return position;
    }

    // coefficient pointers
    double *cb = direction == PJ_FWD ? transformation->fwd_c : transformation->inv_c;
    double *c = cb + sz;

    /* Everything's set up properly - now do the actual polynomium evaluation */
    double E = *--c;
    double N = *--c;
    double w;
    while (c > cb) {
        w = n*E + e*N + *--c;
        N = n*N - e*E + *--c;
        E = w;
    }
    position.u = E;
    position.v = N;
    return position;
}



static PJ_COORD complex_horner_forward_4d (PJ_COORD point, PJ *P) {
    point.uv = complex_horner (P, (HORNER *) P->opaque, PJ_FWD, point.uv);
    return point;
}

static PJ_COORD complex_horner_reverse_4d (PJ_COORD point, PJ *P) {
    point.uv = complex_horner (P, (HORNER *) P->opaque, PJ_INV, point.uv);
    return point;
}


static PJ *horner_freeup (PJ *P, int errlev) {                        /* Destructor */
    if (nullptr==P)
        return nullptr;
    if (nullptr==P->opaque)
        return pj_default_destructor (P, errlev);
    horner_free ((HORNER *) P->opaque);
    P->opaque = nullptr;
    return pj_default_destructor (P, errlev);
}


static int parse_coefs (PJ *P, double *coefs, const char *param, int ncoefs) {
    char *buf, *init, *next = nullptr;
    int i;

    buf = static_cast<char*>(calloc (strlen (param) + 2, sizeof(char)));
    if (nullptr==buf) {
        proj_log_error (P, "No memory left");
        return 0;
    }

    sprintf (buf, "t%s", param);
    if (0==pj_param (P->ctx, P->params, buf).i) {
        free (buf);
        return 0;
    }
    sprintf (buf, "s%s", param);
    init = pj_param(P->ctx, P->params, buf).s;
    free (buf);

    for (i = 0; i < ncoefs; i++) {
        if (i > 0) {
            if ( next == nullptr || ','!=*next) {
                proj_log_error (P, "Malformed polynomium set %s. need %d coefs", param, ncoefs);
                return 0;
            }
            init = ++next;
        }
        coefs[i] = pj_strtod (init, &next);
    }
    return 1;
}


/*********************************************************************/
PJ *PROJECTION(horner) {
/*********************************************************************/
    int   degree = 0, n, complex_polynomia = 0;
    int has_only_fwd = 0;
    HORNER *Q;
    P->fwd4d  = horner_forward_4d;
    P->inv4d  = horner_reverse_4d;
    P->fwd3d  =  nullptr;
    P->inv3d  =  nullptr;
    P->fwd    =  nullptr;
    P->inv    =  nullptr;
    P->left   =  P->right  =  PJ_IO_UNITS_PROJECTED;
    P->destructor = horner_freeup;

    /* Polynomial degree specified? */
    if (pj_param (P->ctx, P->params, "tdeg").i) { /* degree specified? */
        degree = pj_param(P->ctx, P->params, "ideg").i;
        if (degree < 0 || degree > 10000) {
            /* What are reasonable minimum and maximums for degree? */
            proj_log_error (P, _("Degree is unreasonable: %d"), degree);
            return horner_freeup (P, PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE);
        }
    } else {
        proj_log_error (P, _("Must specify polynomial degree, (+deg=n)"));
        return horner_freeup (P, PROJ_ERR_INVALID_OP_MISSING_ARG);
    }

    if (pj_param (P->ctx, P->params, "tfwd_c").i || pj_param (P->ctx, P->params, "tinv_c").i) /* complex polynomium? */
		complex_polynomia = 1;

    Q = horner_alloc (degree, complex_polynomia);
    if (Q == nullptr)
        return horner_freeup (P, PROJ_ERR_OTHER /*ENOMEM*/);
    P->opaque = Q;

    if (!complex_polynomia) {
        has_only_fwd =
            !pj_param_exists(P->params, "inv_u") &&
            !pj_param_exists(P->params, "inv_v") &&
            !pj_param_exists(P->params, "inv_origin");
    } else {
        has_only_fwd =
            !pj_param_exists(P->params, "inv_c") &&
            !pj_param_exists(P->params, "inv_origin");
    }
    Q->has_only_fwd = has_only_fwd;

    if (complex_polynomia) {
        /* Westings and/or southings? */
        Q->uneg = pj_param_exists (P->params, "uneg") ? 1 : 0;
        Q->vneg = pj_param_exists (P->params, "vneg") ? 1 : 0;

        n = 2*degree + 2;
        if (0==parse_coefs (P, Q->fwd_c, "fwd_c", n))
        {
            proj_log_error (P, _("missing fwd_c"));
            return horner_freeup (P, PROJ_ERR_INVALID_OP_MISSING_ARG);
        }
        if (!has_only_fwd && 0==parse_coefs (P, Q->inv_c, "inv_c", n))
        {
            proj_log_error (P, _("missing inv_c"));
            return horner_freeup (P, PROJ_ERR_INVALID_OP_MISSING_ARG);
        }
        P->fwd4d = complex_horner_forward_4d;
        P->inv4d = complex_horner_reverse_4d;
    }

    else {
        n = horner_number_of_coefficients (degree);
        if (0==parse_coefs (P, Q->fwd_u, "fwd_u", n))
        {
            proj_log_error (P, _("missing fwd_u"));
            return horner_freeup (P, PROJ_ERR_INVALID_OP_MISSING_ARG);
        }
        if (0==parse_coefs (P, Q->fwd_v, "fwd_v", n))
        {
            proj_log_error (P, _("missing fwd_v"));
            return horner_freeup (P, PROJ_ERR_INVALID_OP_MISSING_ARG);
        }
        if (!has_only_fwd && 0==parse_coefs (P, Q->inv_u, "inv_u", n))
        {
            proj_log_error (P, _("missing inv_u"));
            return horner_freeup (P, PROJ_ERR_INVALID_OP_MISSING_ARG);
        }
        if (!has_only_fwd && 0==parse_coefs (P, Q->inv_v, "inv_v", n))
        {
            proj_log_error (P, _("missing inv_v"));
            return horner_freeup (P, PROJ_ERR_INVALID_OP_MISSING_ARG);
        }
    }

    if (0==parse_coefs (P, (double *)(Q->fwd_origin), "fwd_origin", 2))
    {
        proj_log_error (P, _("missing fwd_origin"));
        return horner_freeup (P, PROJ_ERR_INVALID_OP_MISSING_ARG);
    }
    if (!has_only_fwd && 0==parse_coefs (P, (double *)(Q->inv_origin), "inv_origin", 2))
    {
        proj_log_error (P, _("missing inv_origin"));
        return horner_freeup (P, PROJ_ERR_INVALID_OP_MISSING_ARG);
    }
    if (0==parse_coefs (P, &Q->range, "range", 1))
        Q->range = 500000;
    if (0==parse_coefs (P, &Q->inverse_tolerance, "inv_tolerance", 1))
        Q->inverse_tolerance = 0.001;

    return P;
}
