/* Copyright (C) 2001-2020 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  1305 Grant Avenue - Suite 200, Novato,
   CA 94945, U.S.A., +1(415)492-9861, for further information.
*/

/* colour operations for the PDF interpreter */

#include "pdf_int.h"
#include "pdf_colour.h"
#include "pdf_pattern.h"
#include "pdf_stack.h"
#include "pdf_array.h"
#include "pdf_misc.h"
#include "gsicc_manage.h"
#include "gsicc_profilecache.h"
#include "gsicc_create.h"
#include "gsptype2.h"

#include "pdf_file.h"
#include "pdf_dict.h"
#include "pdf_loop_detect.h"
#include "pdf_func.h"
#include "pdf_shading.h"
#include "gscsepr.h"
#include "stream.h"
#include "strmio.h"
#include "gscdevn.h"
#include "gxcdevn.h"

/* Forward definitions for a routine we need */
static int pdfi_create_colorspace_by_array(pdf_context *ctx, pdf_array *color_array, int index, pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs, bool inline_image);
static int pdfi_create_colorspace_by_name(pdf_context *ctx, pdf_name *name, pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs, bool inline_image);

/* This is used only from the page level interpreter code, we need to know the number
 * of spot colours in a PDF file, which we have to pass to the device for spot colour
 * rendering. We deal with it here because its colour related.
 * The PDF context has a page-level object which maintains a list of the spot colour
 * names seen so far, so we can ensure we don't end up with duplictaes.
 */
static int pdfi_check_for_spots_by_name(pdf_context *ctx, pdf_name *name,
                                        pdf_dict *parent_dict, pdf_dict *page_dict, pdf_dict *spot_dict)
{
    pdf_obj *ref_space;
    int code;

    if (pdfi_name_is(name, "G")) {
        return 0;
    } else if (pdfi_name_is(name, "RGB")) {
        return 0;
    } else if (pdfi_name_is(name, "CMYK")) {
        return 0;
    } else if (pdfi_name_is(name, "DeviceRGB")) {
        return 0;
    } else if (pdfi_name_is(name, "DeviceGray")) {
        return 0;
    } else if (pdfi_name_is(name, "DeviceCMYK")) {
        return 0;
    } else if (pdfi_name_is(name, "Pattern")) {
        /* TODO: I think this is fine... */
        return 0;
    } else {
        code = pdfi_find_resource(ctx, (unsigned char *)"ColorSpace", name, parent_dict, page_dict, &ref_space);
        if (code < 0)
            return code;

        /* recursion */
        return pdfi_check_ColorSpace_for_spots(ctx, ref_space, parent_dict, page_dict, spot_dict);
    }
    return 0;
}

static int pdfi_check_for_spots_by_array(pdf_context *ctx, pdf_array *color_array,
                                         pdf_dict *parent_dict, pdf_dict *page_dict, pdf_dict *spot_dict)
{
    pdf_name *space = NULL;
    pdf_array *a = NULL;
    int code = 0;

    if (!spot_dict)
        return 0;

    code = pdfi_array_get_type(ctx, color_array, 0, PDF_NAME, (pdf_obj **)&space);
    if (code != 0)
        goto exit;

    code = 0;
    if (pdfi_name_is(space, "G")) {
        goto exit;
    } else if (pdfi_name_is(space, "I") || pdfi_name_is(space, "Indexed")) {
        pdf_obj *base_space;

        code = pdfi_array_get(ctx, color_array, 1, &base_space);
        if (code == 0) {
            code = pdfi_check_ColorSpace_for_spots(ctx, base_space, parent_dict, page_dict, spot_dict);
            (void)pdfi_countdown(base_space);
        }
        goto exit;
    } else if (pdfi_name_is(space, "Pattern")) {
        pdf_obj *base_space = NULL;
        uint64_t size = pdfi_array_size(color_array);

        /* Array of size 1 "[ /Pattern ]" is okay, just do nothing. */
        if (size == 1)
            goto exit;
        /* Array of size > 2 we don't handle (shouldn't happen?) */
        if (size != 2) {
            dbgmprintf1(ctx->memory,
                        "WARNING: checking Pattern for spots, expected array size 2, got %lu\n",
                        size);
            goto exit;
        }
        /* "[/Pattern base_space]" */
        code = pdfi_array_get(ctx, color_array, 1, &base_space);
        if (code == 0) {
            code = pdfi_check_ColorSpace_for_spots(ctx, base_space, parent_dict, page_dict, spot_dict);
            (void)pdfi_countdown(base_space);
        }
        goto exit;
    } else if (pdfi_name_is(space, "Lab")) {
        goto exit;
    } else if (pdfi_name_is(space, "RGB")) {
        goto exit;
    } else if (pdfi_name_is(space, "CMYK")) {
        goto exit;
    } else if (pdfi_name_is(space, "CalRGB")) {
        goto exit;
    } else if (pdfi_name_is(space, "CalGray")) {
        goto exit;
    } else if (pdfi_name_is(space, "ICCBased")) {
        goto exit;
    } else if (pdfi_name_is(space, "DeviceRGB")) {
        goto exit;
    } else if (pdfi_name_is(space, "DeviceGray")) {
        goto exit;
    } else if (pdfi_name_is(space, "DeviceCMYK")) {
        goto exit;
    } else if (pdfi_name_is(space, "DeviceN")) {
        bool known = false;
        pdf_obj *dummy, *name;
        int i;

        pdfi_countdown(space);
        code = pdfi_array_get_type(ctx, color_array, 1, PDF_ARRAY, (pdf_obj **)&space);
        if (code != 0)
            goto exit;

        for (i=0;i < pdfi_array_size((pdf_array *)space); i++) {
            code = pdfi_array_get_type(ctx, (pdf_array *)space, (uint64_t)i, PDF_NAME, &name);
            if (code < 0)
                goto exit;

            if (pdfi_name_is((const pdf_name *)name, "Cyan") || pdfi_name_is((const pdf_name *)name, "Magenta") ||
                pdfi_name_is((const pdf_name *)name, "Yellow") || pdfi_name_is((const pdf_name *)name, "Black") ||
                pdfi_name_is((const pdf_name *)name, "None") || pdfi_name_is((const pdf_name *)name, "All")) {

                pdfi_countdown(name);
                continue;
            }

            code = pdfi_dict_known_by_key(spot_dict, (pdf_name *)name, &known);
            if (code < 0) {
                pdfi_countdown(name);
                goto exit;
            }
            if (known) {
                pdfi_countdown(name);
                continue;
            }

            code = pdfi_alloc_object(ctx, PDF_INT, 0, &dummy);
            if (code < 0)
                goto exit;

            code = pdfi_dict_put_obj(spot_dict, name, dummy);
            pdfi_countdown(name);
            if (code < 0)
                break;
        }
        goto exit;
    } else if (pdfi_name_is(space, "Separation")) {
        bool known = false;
        pdf_obj *dummy;

        pdfi_countdown(space);
        code = pdfi_array_get_type(ctx, color_array, 1, PDF_NAME, (pdf_obj **)&space);
        if (code != 0)
            goto exit;

        if (pdfi_name_is((const pdf_name *)space, "Cyan") || pdfi_name_is((const pdf_name *)space, "Magenta") ||
            pdfi_name_is((const pdf_name *)space, "Yellow") || pdfi_name_is((const pdf_name *)space, "Black") ||
            pdfi_name_is((const pdf_name *)space, "None") || pdfi_name_is((const pdf_name *)space, "All"))
            goto exit;
        code = pdfi_dict_known_by_key(spot_dict, space, &known);
        if (code < 0 || known)
            goto exit;

        code = pdfi_alloc_object(ctx, PDF_INT, 0, &dummy);
        if (code < 0)
            goto exit;

        code = pdfi_dict_put_obj(spot_dict, (pdf_obj *)space, dummy);
        goto exit;
    } else {
        code = pdfi_find_resource(ctx, (unsigned char *)"ColorSpace",
                                  space, parent_dict, page_dict, (pdf_obj **)&a);
        if (code < 0)
            goto exit;

        if (a->type != PDF_ARRAY) {
            code = gs_note_error(gs_error_typecheck);
            goto exit;
        }

        /* recursion */
        code = pdfi_check_for_spots_by_array(ctx, a, parent_dict, page_dict, spot_dict);
    }

 exit:
    if (space)
        pdfi_countdown(space);
    if (a)
        pdfi_countdown(a);
    return code;
}

int pdfi_check_ColorSpace_for_spots(pdf_context *ctx, pdf_obj *space, pdf_dict *parent_dict,
                                    pdf_dict *page_dict, pdf_dict *spot_dict)
{
    int code;

    if (!spot_dict)
        return 0;

    code = pdfi_loop_detector_mark(ctx);
    if (code < 0)
        return code;

    if (space->type == PDF_NAME) {
        code = pdfi_check_for_spots_by_name(ctx, (pdf_name *)space, parent_dict, page_dict, spot_dict);
    } else {
        if (space->type == PDF_ARRAY) {
            code = pdfi_check_for_spots_by_array(ctx, (pdf_array *)space, parent_dict, page_dict, spot_dict);
        } else {
            pdfi_loop_detector_cleartomark(ctx);
            return 0;
        }
    }

    (void)pdfi_loop_detector_cleartomark(ctx);
    return code;
}

/* Rendering intent is a bit of an oddity, but its clearly colour related, so we
 * deal with it here. Cover it first to get it out of the way.
 */
int pdfi_ri(pdf_context *ctx)
{
    pdf_name *n;
    int code;

    if (pdfi_count_stack(ctx) < 1) {
        if(ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }

    if (ctx->stack_top[-1]->type != PDF_NAME) {
        pdfi_pop(ctx, 1);
        if (ctx->pdfstoponerror)
            return_error(gs_error_typecheck);
        return 0;
    }
    n = (pdf_name *)ctx->stack_top[-1];
    code = pdfi_setrenderingintent(ctx, n);
    pdfi_pop(ctx, 1);
    if (code < 0 && ctx->pdfstoponerror)
        return code;
    return 0;
}

/*
 * Pattern lifetime management turns out to be more complex than we would ideally like. Although
 * Patterns are reference counted, and contain a client_data pointer, they don't have a gs_notify
 * setup. This means that there's no simlpe way for us to be informed when a Pattern is released
 * We could patch up the Pattern finalize() method, replacing it with one of our own which calls
 * the original finalize() but that seems like a really nasty hack.
 * For the time being we put code in pdfi_grestore() to check for Pattern colour spaces being
 * restored away, but we also need to check for Pattern spaces being replaced in the current
 * graphics state. We define 'pdfi' variants of several graphics library colour management
 * functions to 'wrap' these with code to check for replacement of Patterns.
 * This comment is duplicated in pdf_pattern.c
 */
int pdfi_gs_setgray(pdf_context *ctx, double d)
{
    /* PDF Reference 1.7 p423, any colour operators in a CharProc, following a d1, should be ignored */
    if (ctx->inside_CharProc && ctx->CharProc_is_d1)
        return 0;

    (void)pdfi_color_cleanup(ctx, 0);
    return gs_setgray(ctx->pgs, d);
}

int pdfi_gs_setrgbcolor(pdf_context *ctx, double r, double g, double b)
{
    /* PDF Reference 1.7 p423, any colour operators in a CharProc, following a d1, should be ignored */
    if (ctx->inside_CharProc && ctx->CharProc_is_d1)
        return 0;

    (void)pdfi_color_cleanup(ctx, 0);
    return gs_setrgbcolor(ctx->pgs, r, g, b);
}

static int pdfi_gs_setcmykcolor(pdf_context *ctx, double c, double m, double y, double k)
{
    /* PDF Reference 1.7 p423, any colour operators in a CharProc, following a d1, should be ignored */
    if (ctx->inside_CharProc && ctx->CharProc_is_d1)
        return 0;

    (void)pdfi_color_cleanup(ctx, 0);
    return gs_setcmykcolor(ctx->pgs, c, m, y, k);
}

int pdfi_gs_setcolorspace(pdf_context *ctx, gs_color_space *pcs)
{
    /* PDF Reference 1.7 p423, any colour operators in a CharProc, following a d1, should be ignored */
    if (ctx->inside_CharProc && ctx->CharProc_is_d1)
        return 0;

    (void)pdfi_color_cleanup(ctx, 0);
    return gs_setcolorspace(ctx->pgs, pcs);
}

/* Start with the simple cases, where we set the colour space and colour in a single operation */
int pdfi_setgraystroke(pdf_context *ctx)
{
    pdf_num *n1;
    int code;
    double d1;

    if (pdfi_count_stack(ctx) < 1) {
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }

    n1 = (pdf_num *)ctx->stack_top[-1];
    if (n1->type == PDF_INT){
        d1 = (double)n1->value.i;
    } else{
        if (n1->type == PDF_REAL) {
            d1 = n1->value.d;
        } else {
            pdfi_pop(ctx, 1);
            if (ctx->pdfstoponerror)
                return_error(gs_error_typecheck);
            else
                return 0;
        }
    }
    gs_swapcolors_quick(ctx->pgs);
    code = pdfi_gs_setgray(ctx, d1);
    gs_swapcolors_quick(ctx->pgs);
    pdfi_pop(ctx, 1);
    if(code < 0 && ctx->pdfstoponerror)
        return code;
    else
        return 0;
}

int pdfi_setgrayfill(pdf_context *ctx)
{
    pdf_num *n1;
    int code;
    double d1;

    if (pdfi_count_stack(ctx) < 1) {
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }

    n1 = (pdf_num *)ctx->stack_top[-1];
    if (n1->type == PDF_INT){
        d1 = (double)n1->value.i;
    } else{
        if (n1->type == PDF_REAL) {
            d1 = n1->value.d;
        } else {
            pdfi_pop(ctx, 1);
            if (ctx->pdfstoponerror)
                return_error(gs_error_typecheck);
            else
                return 0;
        }
    }
    code = pdfi_gs_setgray(ctx, d1);
    pdfi_pop(ctx, 1);
    if(code < 0 && ctx->pdfstoponerror)
        return code;
    else
        return 0;
}

int pdfi_setrgbstroke(pdf_context *ctx)
{
    pdf_num *num;
    double Values[3];
    int i, code;

    if (pdfi_count_stack(ctx) < 3) {
        pdfi_clearstack(ctx);
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }

    for (i=0;i < 3;i++){
        num = (pdf_num *)ctx->stack_top[i - 3];
        if (num->type != PDF_INT) {
            if(num->type != PDF_REAL) {
                pdfi_pop(ctx, 3);
                if (ctx->pdfstoponerror)
                    return_error(gs_error_typecheck);
                else
                    return 0;
            }
            else
                Values[i] = num->value.d;
        } else {
            Values[i] = (double)num->value.i;
        }
    }
    gs_swapcolors_quick(ctx->pgs);
    code = pdfi_gs_setrgbcolor(ctx, Values[0], Values[1], Values[2]);
    gs_swapcolors_quick(ctx->pgs);
    pdfi_pop(ctx, 3);
    if(code < 0 && ctx->pdfstoponerror)
        return code;
    else
        return 0;
}

/* Non-standard operator that is used in some annotation /DA
 * Expects stack to be [r g b]
 */
int pdfi_setrgbfill_array(pdf_context *ctx)
{
    int code;
    pdf_array *array = NULL;

    ctx->pdf_warnings |= W_PDF_NONSTANDARD_OP;
    dmprintf(ctx->memory, "WARNING: Non-standard 'r' operator\n");

    if (pdfi_count_stack(ctx) < 1) {
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }

    array = (pdf_array *)ctx->stack_top[-1];
    if (array->type != PDF_ARRAY) {
        code = gs_note_error(gs_error_typecheck);
        goto exit;
    }

    code = pdfi_setcolor_from_array(ctx, array);
 exit:
    pdfi_pop(ctx, 1);
    if (code != 0 && ctx->pdfstoponerror)
        return code;
    else
        return 0;
}

int pdfi_setrgbfill(pdf_context *ctx)
{
    pdf_num *num;
    double Values[3];
    int i, code;

    if (pdfi_count_stack(ctx) < 3) {
        pdfi_clearstack(ctx);
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }

    for (i=0;i < 3;i++){
        num = (pdf_num *)ctx->stack_top[i - 3];
        if (num->type != PDF_INT) {
            if(num->type != PDF_REAL) {
                pdfi_pop(ctx, 3);
                if (ctx->pdfstoponerror)
                    return_error(gs_error_typecheck);
                else
                    return 0;
            }
            else
                Values[i] = num->value.d;
        } else {
            Values[i] = (double)num->value.i;
        }
    }
    code = pdfi_gs_setrgbcolor(ctx, Values[0], Values[1], Values[2]);
    pdfi_pop(ctx, 3);
    if(code < 0 && ctx->pdfstoponerror)
        return code;
    else
        return 0;
}

int pdfi_setcmykstroke(pdf_context *ctx)
{
    pdf_num *num;
    double Values[4];
    int i, code;

    if (pdfi_count_stack(ctx) < 4) {
        pdfi_clearstack(ctx);
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }

    for (i=0;i < 4;i++){
        num = (pdf_num *)ctx->stack_top[i - 4];
        if (num->type != PDF_INT) {
            if(num->type != PDF_REAL) {
                pdfi_pop(ctx, 4);
                if (ctx->pdfstoponerror)
                    return_error(gs_error_typecheck);
                else
                    return 0;
            }
            else
                Values[i] = num->value.d;
        } else {
            Values[i] = (double)num->value.i;
        }
    }
    gs_swapcolors_quick(ctx->pgs);
    code = pdfi_gs_setcmykcolor(ctx, Values[0], Values[1], Values[2], Values[3]);
    gs_swapcolors_quick(ctx->pgs);
    pdfi_pop(ctx, 4);
    if(code < 0 && ctx->pdfstoponerror)
        return code;
    else
        return 0;
}

int pdfi_setcmykfill(pdf_context *ctx)
{
    pdf_num *num;
    double Values[4];
    int i, code;

    if (pdfi_count_stack(ctx) < 4) {
        pdfi_clearstack(ctx);
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }

    for (i=0;i < 4;i++){
        num = (pdf_num *)ctx->stack_top[i - 4];
        if (num->type != PDF_INT) {
            if(num->type != PDF_REAL) {
                pdfi_pop(ctx, 4);
                if (ctx->pdfstoponerror)
                    return_error(gs_error_typecheck);
                else
                    return 0;
            }
            else
                Values[i] = num->value.d;
        } else {
            Values[i] = (double)num->value.i;
        }
    }
    code = pdfi_gs_setcmykcolor(ctx, Values[0], Values[1], Values[2], Values[3]);
    pdfi_pop(ctx, 4);
    if(code < 0 && ctx->pdfstoponerror)
        return code;
    else
        return 0;
}

/* Do a setcolor using values in an array
 * Will do gray, rgb, cmyk for sizes 1,3,4
 * Anything else is an error
 */
int pdfi_setcolor_from_array(pdf_context *ctx, pdf_array *array)
{
    int code = 0;
    uint64_t size;
    double values[4];

    size = pdfi_array_size(array);
    if (size != 1 && size != 3 && size != 4) {
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    code = pdfi_array_to_num_array(ctx, array, values, 0, size);
    if (code < 0) goto exit;

    switch (size) {
    case 1:
        code = pdfi_gs_setgray(ctx, values[0]);
        break;
    case 3:
        code = pdfi_gs_setrgbcolor(ctx, values[0], values[1], values[2]);
        break;
    case 4:
        code = pdfi_gs_setcmykcolor(ctx, values[0], values[1], values[2], values[3]);
        break;
    default:
        break;
    }

 exit:
    return code;
}

/* Get colors from top of stack into a client color */
static int
pdfi_get_color_from_stack(pdf_context *ctx, gs_client_color *cc, int ncomps)
{
    int i;
    pdf_num *n;

    if (pdfi_count_stack(ctx) < ncomps) {
        pdfi_clearstack(ctx);
        return_error(gs_error_stackunderflow);
    }
    for (i=0;i<ncomps;i++){
        n = (pdf_num *)ctx->stack_top[i - ncomps];
        if (n->type == PDF_INT) {
            cc->paint.values[i] = (float)n->value.i;
        } else {
            if (n->type == PDF_REAL) {
                cc->paint.values[i] = n->value.d;
            } else {
                pdfi_clearstack(ctx);
                return_error(gs_error_typecheck);
            }
        }
    }
    pdfi_pop(ctx, ncomps);
    return 0;
}

/* Now deal with the case where we have to set the colour space separately from the
 * colour values. We'll start with the routines to set the colour, because setting
 * colour components is relatively easy.
 */

/* First up, the SC and sc operators. These set the colour for all spaces *except*
 * ICCBased, Pattern, Separation and DeviceN
 */
int pdfi_setstrokecolor(pdf_context *ctx)
{
    const gs_color_space *  pcs;
    int ncomps, code;
    gs_client_color cc;

    gs_swapcolors_quick(ctx->pgs);
    pcs = gs_currentcolorspace(ctx->pgs);
    ncomps = cs_num_components(pcs);
    code = pdfi_get_color_from_stack(ctx, &cc, ncomps);
    if (code == 0) {
        code = gs_setcolor(ctx->pgs, &cc);
    }
    gs_swapcolors_quick(ctx->pgs);
    if (code < 0 && ctx->pdfstoponerror)
        return code;
    return 0;
}

int pdfi_setfillcolor(pdf_context *ctx)
{
    const gs_color_space *  pcs = gs_currentcolorspace(ctx->pgs);
    int ncomps, code;
    gs_client_color cc;

    ncomps = cs_num_components(pcs);
    code = pdfi_get_color_from_stack(ctx, &cc, ncomps);
    if (code == 0) {
        code = gs_setcolor(ctx->pgs, &cc);
    }
    if (code < 0 && ctx->pdfstoponerror)
        return code;
    return 0;
}

static inline bool
pattern_instance_uses_base_space(const gs_pattern_instance_t * pinst)
{
    return pinst->type->procs.uses_base_space(
                   pinst->type->procs.get_pattern(pinst) );
}

/* Now the SCN and scn operators. These set the colour for special spaces;
 * ICCBased, Pattern, Separation and DeviceN
 */
int
pdfi_setcolorN(pdf_context *ctx, pdf_dict *stream_dict, pdf_dict *page_dict, bool is_fill)
{
    const gs_color_space *pcs;
    gs_color_space *base_space = NULL;
    int ncomps=0, code = 0;
    gs_client_color cc;
    bool is_pattern = false;

    if (!is_fill) {
        gs_swapcolors_quick(ctx->pgs);
    }
    pcs = gs_currentcolorspace(ctx->pgs);

    if (pdfi_count_stack(ctx) < 1) {
        code = gs_note_error(gs_error_stackunderflow);
        goto cleanupExit;
    }

    if (pcs->type == &gs_color_space_type_Pattern)
        is_pattern = true;
    if (is_pattern) {
        if (ctx->stack_top[-1]->type != PDF_NAME) {
            pdfi_clearstack(ctx);
            code = gs_note_error(gs_error_syntaxerror);
            goto cleanupExit;
        }
        base_space = pcs->base_space;
        code = pdfi_pattern_set(ctx, stream_dict, page_dict, (pdf_name *)ctx->stack_top[-1], &cc);
        pdfi_pop(ctx, 1);
        if (code < 0) {
            /* Ignore the pattern if we failed to set it */
            dbgmprintf(ctx->memory, "PATTERN: Error setting pattern\n");
            ctx->pdf_warnings |= W_PDF_BADPATTERN;
            code = 0;
            goto cleanupExit;
        }
        if (base_space && pattern_instance_uses_base_space(cc.pattern))
            ncomps = cs_num_components(base_space);
        else
            ncomps = 0;
    } else {
        ncomps = cs_num_components(pcs);
        cc.pattern = NULL;
    }

    if (ncomps > 0)
        code = pdfi_get_color_from_stack(ctx, &cc, ncomps);
    if (code < 0)
        goto cleanupExit;
    if (is_pattern) {
        if (ctx->pgs->color[0].ccolor->pattern != 0) {
            code = pdfi_pattern_cleanup(ctx, ctx->pgs->color[0].ccolor);
            if (code < 0)
                goto cleanupExit;
        }

        code = gs_setcolor(ctx->pgs, &cc);
        /* cc is a local scope variable, holding a reference to a pattern.
         * We need to count the refrence down before the variable goes out of scope
         * in order to prevent the pattern leaking.
         */
        rc_decrement(cc.pattern, "pdfi_setcolorN");
    } else {
        code = gs_setcolor(ctx->pgs, &cc);
    }

 cleanupExit:
    if (!is_fill)
        gs_swapcolors_quick(ctx->pgs);
    return code;
}

/* And now, the routines to set the colour space on its own. */

/* Starting with the ICCBased colour space */

/* This routine is mostly a copy of seticc() in zicc.c */
static int pdfi_create_icc(pdf_context *ctx, char *Name, stream *s, int ncomps, int *icc_N, float *range_buff, gs_color_space **ppcs)
{
    int                     code, k;
    gs_color_space *        pcs;
    cmm_profile_t           *picc_profile = NULL;
    int                     i, expected = 0;

    static const char *const icc_std_profile_names[] = {
            GSICC_STANDARD_PROFILES
        };
    static const char *const icc_std_profile_keys[] = {
            GSICC_STANDARD_PROFILES_KEYS
        };

    if (ppcs!= NULL)
        *ppcs = NULL;

    code = gs_cspace_build_ICC(&pcs, NULL, gs_gstate_memory(ctx->pgs));
    if (code < 0)
        return code;

    if (Name != NULL){
        /* Compare this to the standard profile names */
        for (k = 0; k < GSICC_NUMBER_STANDARD_PROFILES; k++) {
            if ( strcmp( Name, icc_std_profile_keys[k] ) == 0 ) {
                picc_profile = gsicc_get_profile_handle_file(icc_std_profile_names[k],
                    strlen(icc_std_profile_names[k]), gs_gstate_memory(ctx->pgs));
                break;
            }
        }
    } else {
        picc_profile = gsicc_profile_new(s, gs_gstate_memory(ctx->pgs), NULL, 0);
        if (picc_profile == NULL) {
            rc_decrement(pcs,"pdfi_create_icc");
            return gs_throw(gs_error_VMerror, "pdfi_create_icc Creation of ICC profile failed");
        }
        /* We have to get the profile handle due to the fact that we need to know
           if it has a data space that is CIELAB */
        picc_profile->profile_handle =
            gsicc_get_profile_handle_buffer(picc_profile->buffer,
                                            picc_profile->buffer_size,
                                            gs_gstate_memory(ctx->pgs));
    }

    if (picc_profile == NULL || picc_profile->profile_handle == NULL) {
        /* Free up everything, the profile is not valid. We will end up going
           ahead and using a default based upon the number of components */
        rc_decrement(picc_profile,"pdfi_create_icc");
        rc_decrement(pcs,"pdfi_create_icc");
        return -1;
    }
    code = gsicc_set_gscs_profile(pcs, picc_profile, gs_gstate_memory(ctx->pgs));
    if (code < 0) {
        rc_decrement(picc_profile,"pdfi_create_icc");
        rc_decrement(pcs,"pdfi_create_icc");
        return code;
    }

    picc_profile->data_cs =
        gscms_get_profile_data_space(picc_profile->profile_handle,
            picc_profile->memory);
    switch (picc_profile->data_cs) {
        case gsCIEXYZ:
        case gsCIELAB:
        case gsRGB:
            expected = 3;
            break;
        case gsGRAY:
            expected = 1;
            break;
        case gsCMYK:
            expected = 4;
            break;
        case gsNCHANNEL:
        case gsNAMED:            /* Silence warnings */
        case gsUNDEFINED:        /* Silence warnings */
            break;
    }
    /* Return the number of components the ICC profile has */
    *icc_N = expected;
    if (expected != ncomps)
        ncomps = expected;

#if 0
    if (!expected || ncomps != expected) {
        rc_decrement(picc_profile,"pdfi_create_icc");
        rc_decrement(pcs,"pdfi_create_icc");
        return_error(gs_error_rangecheck);
    }
#endif

    picc_profile->num_comps = ncomps;
    /* Lets go ahead and get the hash code and check if we match one of the default spaces */
    /* Later we may want to delay this, but for now lets go ahead and do it */
    gsicc_init_hash_cs(picc_profile, ctx->pgs);

    /* Set the range according to the data type that is associated with the
       ICC input color type.  Occasionally, we will run into CIELAB to CIELAB
       profiles for spot colors in PDF documents. These spot colors are typically described
       as separation colors with tint transforms that go from a tint value
       to a linear mapping between the CIELAB white point and the CIELAB tint
       color.  This results in a CIELAB value that we need to use to fill.  We
       need to detect this to make sure we do the proper scaling of the data.  For
       CIELAB images in PDF, the source is always normal 8 or 16 bit encoded data
       in the range from 0 to 255 or 0 to 65535.  In that case, there should not
       be any encoding and decoding to CIELAB.  The PDF content will not include
       an ICC profile but will set the color space to \Lab.  In this case, we use
       our seticc_lab operation to install the LAB to LAB profile, but we detect
       that we did that through the use of the is_lab flag in the profile descriptor.
       When then avoid the CIELAB encode and decode */
    if (picc_profile->data_cs == gsCIELAB) {
    /* If the input space to this profile is CIELAB, then we need to adjust the limits */
        /* See ICC spec ICC.1:2004-10 Section 6.3.4.2 and 6.4.  I don't believe we need to
           worry about CIEXYZ profiles or any of the other odds ones.  Need to check that though
           at some point. */
        picc_profile->Range.ranges[0].rmin = 0.0;
        picc_profile->Range.ranges[0].rmax = 100.0;
        picc_profile->Range.ranges[1].rmin = -128.0;
        picc_profile->Range.ranges[1].rmax = 127.0;
        picc_profile->Range.ranges[2].rmin = -128.0;
        picc_profile->Range.ranges[2].rmax = 127.0;
        picc_profile->islab = true;
    } else {
        for (i = 0; i < ncomps; i++) {
            picc_profile->Range.ranges[i].rmin = range_buff[2 * i];
            picc_profile->Range.ranges[i].rmax = range_buff[2 * i + 1];
        }
    }
    /* Now see if we are in an overide situation.  We have to wait until now
       in case this is an LAB profile which we will not overide */
    if (gs_currentoverrideicc(ctx->pgs) && picc_profile->data_cs != gsCIELAB) {
        /* Free up the profile structure */
        switch( picc_profile->data_cs ) {
            case gsRGB:
                pcs->cmm_icc_profile_data = ctx->pgs->icc_manager->default_rgb;
                break;
            case gsGRAY:
                pcs->cmm_icc_profile_data = ctx->pgs->icc_manager->default_gray;
                break;
            case gsCMYK:
                pcs->cmm_icc_profile_data = ctx->pgs->icc_manager->default_cmyk;
                break;
            default:
                break;
        }
        /* Have one increment from the color space.  Having these tied
           together is not really correct.  Need to fix that.  ToDo.  MJV */
        rc_adjust(picc_profile, -2, "pdfi_create_icc");
        rc_increment(pcs->cmm_icc_profile_data);
    }

    if (ppcs!= NULL){
        *ppcs = pcs;
    } else {
        code = pdfi_gs_setcolorspace(ctx, pcs);
        rc_decrement_only_cs(pcs, "pdfi_seticc_cal");
    }

    /* The context has taken a reference to the colorspace. We no longer need
     * ours, so drop it. */
    rc_decrement(picc_profile, "pdfi_create_icc");
    return code;
}

static int pdfi_create_iccprofile(pdf_context *ctx, pdf_dict *ICC_dict, char *cname, int64_t Length, int N, int *icc_N, float *range, gs_color_space **ppcs)
{
    pdf_stream *profile_stream = NULL;
    byte *profile_buffer;
    gs_offset_t savedoffset;
    int code, code1;

    /* Save the current stream position, and move to the start of the profile stream */
    savedoffset = pdfi_tell(ctx->main_stream);
    pdfi_seek(ctx, ctx->main_stream, ICC_dict->stream_offset, SEEK_SET);

    /* The ICC profile reading code (irritatingly) requires a seekable stream, because it
     * rewinds it to the start, then seeks to the end to find the size, then rewinds the
     * stream again.
     * Ideally we would use a ReusableStreamDecode filter here, but that is largely
     * implemented in PostScript (!) so we can't use it. What we can do is create a
     * string sourced stream in memory, which is at least seekable.
     */
    code = pdfi_open_memory_stream_from_filtered_stream(ctx, ICC_dict, Length, &profile_buffer, ctx->main_stream, &profile_stream);
    if (code < 0) {
        pdfi_seek(ctx, ctx->main_stream, savedoffset, SEEK_SET);
        return code;
    }

    /* Now, finally, we can call the code to create and set the profile */
    code = pdfi_create_icc(ctx, cname, profile_stream->s, (int)N, icc_N, range, ppcs);

    code1 = pdfi_close_memory_stream(ctx, profile_buffer, profile_stream);

    if (code == 0)
        code = code1;

    pdfi_seek(ctx, ctx->main_stream, savedoffset, SEEK_SET);

    return code;
}

static int pdfi_create_iccbased(pdf_context *ctx, pdf_array *color_array, int index, pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs, bool inline_image)
{
    pdf_dict *ICC_dict = NULL;
    pdf_array *a;
    int64_t Length, N;
    pdf_obj *Name = NULL;
    char *cname = NULL;
    int code;
    bool known;
    float range[8];
    int icc_N;
    gs_color_space *pcs = NULL;

    code = pdfi_array_get_type(ctx, color_array, index + 1, PDF_DICT, (pdf_obj **)&ICC_dict);
    if (code < 0)
        return code;

    if (!pdfi_dict_is_stream(ctx, ICC_dict)) {
        code = gs_note_error(gs_error_undefined);
        goto done;
    }
    Length = pdfi_dict_stream_length(ctx, ICC_dict);
    code = pdfi_dict_get_int(ctx, ICC_dict, "N", &N);
    if (code < 0)
        goto done;
    code = pdfi_dict_knownget(ctx, ICC_dict, "Name", &Name);
    if (code > 0) {
        if(Name->type == PDF_STRING || Name->type == PDF_NAME) {
            cname = (char *)gs_alloc_bytes(ctx->memory, ((pdf_name *)Name)->length + 1, "pdfi_create_iccbased (profile name)");
            if (cname == NULL) {
                code = gs_note_error(gs_error_VMerror);
                goto done;
            }
            memset(cname, 0x00, ((pdf_name *)Name)->length + 1);
            memcpy(cname, ((pdf_name *)Name)->data, ((pdf_name *)Name)->length);
        }
    }
    if (code < 0)
        goto done;

    code = pdfi_dict_knownget_type(ctx, ICC_dict, "Range", PDF_ARRAY, (pdf_obj **)&a);
    if (code < 0)
        goto done;
    if (code > 0) {
        double dbl;
        int i;

        if (pdfi_array_size(a) >= N * 2) {
            for (i = 0; i < pdfi_array_size(a);i++) {
                code = pdfi_array_get_number(ctx, a, i, &dbl);
                if (code < 0) {
                    known = false;
                    break;
                }
                range[i] = (float)dbl;
            }
        } else {
            known = false;
        }
    } else
        known = false;

    /* We don't just use the final else clause above for setting the defaults
     * because we also want to use these if there's a problem with the
     * supplied data. In this case we also want to overwrite any partial
     * data we might have read
     */
    if (!known) {
        int i;
        for (i = 0;i < N; i++) {
            range[i * 2] = 0;
            range[(i * 2) + 1] = 1;
        }
    }

    code = pdfi_create_iccprofile(ctx, ICC_dict, cname, Length, N, &icc_N, range, &pcs);

    /* This is just plain hackery for the benefit of Bug696690.pdf. The old PostScript PDF interpreter says:
     * %% This section is to deal with the horrible pair of files in Bug #696690 and Bug #696120
     * %% These files have ICCBased spaces where the value of /N and the number of components
     * %% in the profile differ. In addition the profile in Bug #696690 is invalid. In the
     * %% case of Bug #696690 the /N value is correct, and the profile is wrong, in the case
     * %% of Bug #696120 the /N value is incorrect and the profile is correct.
     * %% We 'suspect' that Acrobat uses the fact that Bug #696120 is a pure image to detect
     * %% that the /N is incorrect, we can't be sure whether it uses the profile or just uses
     * %% the /N to decide on a device space.
     * We can't precisely duplicate the PostScript approach, but we now set the actual ICC profile
     * and therefore use the number of components in the profile. However, we pass back the number
     * of components in icc_N. We then check to see if N and icc_N are the same, if they are not we
     * try to set a devcie colour using the profile. If that fails (bad profile) then we enter the fallback
     * just as if we had failed to set the profile.
     */
    if (code >= 0 && N != icc_N) {
        gs_client_color cc;
        int i;

        gs_gsave(ctx->pgs);
        code = gs_setcolorspace(ctx->pgs, pcs);
        if (code == 0) {
            cc.pattern = 0;
            for (i = 0;i < icc_N; i++)
                cc.paint.values[i] = 0;
            code = gs_setcolor(ctx->pgs, &cc);
            if (code == 0)
                code = gx_set_dev_color(ctx->pgs);
        }
        gs_grestore(ctx->pgs);
    }

    if (code < 0) {
        pdf_obj *Alternate = NULL;

        if (pcs != NULL)
            rc_decrement(pcs,"pdfi_create_iccbased");

        /* Failed to set the ICCBased space, attempt to use the Alternate */
        code = pdfi_dict_knownget(ctx, ICC_dict, "Alternate", &Alternate);
        if (code > 0) {
            /* The Alternate should be one of the device spaces, therefore a Name object. If its not, fallback to using /N */
            if (Alternate->type == PDF_NAME)
                code = pdfi_create_colorspace_by_name(ctx, (pdf_name *)Alternate, stream_dict, page_dict, ppcs, inline_image);
                pdfi_countdown(Alternate);
                if (code == 0) {
                    ctx->pdf_warnings |= W_PDF_BADICC_USE_ALT;
                    goto done;
                }
        }
        /* Use the number of components *from the profile* to set a space.... */
        ctx->pdf_warnings |= W_PDF_BADICC_USECOMPS;
        switch(N) {
            case 1:
                pcs = gs_cspace_new_DeviceGray(ctx->memory);
                if (pcs == NULL)
                    code = gs_note_error(gs_error_VMerror);
                break;
            case 3:
                pcs = gs_cspace_new_DeviceRGB(ctx->memory);
                if (pcs == NULL)
                    code = gs_note_error(gs_error_VMerror);
                break;
            case 4:
                pcs = gs_cspace_new_DeviceCMYK(ctx->memory);
                if (pcs == NULL)
                    code = gs_note_error(gs_error_VMerror);
                break;
            default:
                code = gs_note_error(gs_error_undefined);
                break;
        }
    }
    if (ppcs!= NULL){
        /* FIXME
         * I can see no justification for this whatever, but if I don't do this then some
         * files with images in a /Separation colour space come out incorrectly. Even surrounding
         * this with a gsave/grestore pair causes differences.
         */
        code = pdfi_gs_setcolorspace(ctx, pcs);
        *ppcs = pcs;
    } else {
        code = pdfi_gs_setcolorspace(ctx, pcs);
        /* release reference from construction */
        rc_decrement_only_cs(pcs, "setseparationspace");
    }


done:
    if (cname)
        gs_free_object(ctx->memory, cname, "pdfi_create_iccbased (profile name)");
    pdfi_countdown(Name);
    pdfi_countdown(ICC_dict);
    return code;
}

/*
 * This, and pdfi_set_cal() below are copied from the similarly named routines
 * in zicc.c
 */
/* Install a ICC type color space and use the ICC LABLUT profile. */
static int
pdfi_seticc_lab(pdf_context *ctx, float *range_buff, gs_color_space **ppcs)
{
    int                     code;
    gs_color_space *        pcs;
    int                     i;

    /* build the color space object */
    code = gs_cspace_build_ICC(&pcs, NULL, gs_gstate_memory(ctx->pgs));
    if (code < 0)
        return code;

    /* record the current space as the alternative color space */
    /* Get the lab profile.  It may already be set in the icc manager.
       If not then lets populate it.  */
    if (ctx->pgs->icc_manager->lab_profile == NULL ) {
        /* This can't happen as the profile
           should be initialized during the
           setting of the user params */
        return_error(gs_error_unknownerror);
    }
    /* Assign the LAB to LAB profile to this color space */
    code = gsicc_set_gscs_profile(pcs, ctx->pgs->icc_manager->lab_profile, gs_gstate_memory(ctx->pgs));
    if (code < 0)
        return code;

    pcs->cmm_icc_profile_data->Range.ranges[0].rmin = 0.0;
    pcs->cmm_icc_profile_data->Range.ranges[0].rmax = 100.0;
    for (i = 1; i < 3; i++) {
        pcs->cmm_icc_profile_data->Range.ranges[i].rmin =
            range_buff[2 * (i-1)];
        pcs->cmm_icc_profile_data->Range.ranges[i].rmax =
            range_buff[2 * (i-1) + 1];
    }
    if (ppcs!= NULL){
        *ppcs = pcs;
    } else {
        code = pdfi_gs_setcolorspace(ctx, pcs);
        rc_decrement_only_cs(pcs, "pdfi_seticc_lab");
    }

    return code;
}

static int pdfi_create_Lab(pdf_context *ctx, pdf_array *color_array, int index, pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs)
{
    int code = 0, i;
    pdf_dict *Lab_dict = NULL;
    pdf_array *Range = NULL;
    float RangeBuf[4];
    double f;

    code = pdfi_array_get_type(ctx, color_array, index + 1, PDF_DICT, (pdf_obj **)&Lab_dict);
    if (code < 0)
        return code;

    code = pdfi_dict_get_type(ctx, Lab_dict, "Range", PDF_ARRAY, (pdf_obj **)&Range);
    if (code < 0) {
        goto exit;
    }
    if (pdfi_array_size(Range) != 4){
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    for (i=0; i < 4; i++) {
        code = pdfi_array_get_number(ctx, Range, (uint64_t)i, &f);
        if (code < 0)
            goto exit;
        RangeBuf[i] = (float)f;
    }

    code = pdfi_seticc_lab(ctx, RangeBuf, ppcs);

exit:
    pdfi_countdown(Lab_dict);
    pdfi_countdown(Range);
    return code;
}

/* Install an ICC space from the PDF CalRGB or CalGray types */
static int
pdfi_seticc_cal(pdf_context *ctx, float *white, float *black, float *gamma,
           float *matrix, int num_colorants, ulong dictkey, gs_color_space **ppcs)
{
    int                     code = 0;
    gs_color_space *        pcs;
    int                     i;
    cmm_profile_t           *cal_profile;

    /* See if the color space is in the profile cache */
    pcs = gsicc_find_cs(dictkey, ctx->pgs);
    if (pcs == NULL ) {
        /* build the color space object.  Since this is cached
           in the profile cache which is a member variable
           of the graphic state, we will want to use stable
           memory here */
        code = gs_cspace_build_ICC(&pcs, NULL, ctx->memory);
        if (code < 0)
            return code;
        /* There is no alternate for this.  Perhaps we should set DeviceRGB? */
        pcs->base_space = NULL;
        /* Create the ICC profile from the CalRGB or CalGray parameters */
        cal_profile = gsicc_create_from_cal(white, black, gamma, matrix,
                                            ctx->memory, num_colorants);
        if (cal_profile == NULL) {
            rc_decrement(pcs, "seticc_cal");
            return_error(gs_error_VMerror);
        }
        /* Assign the profile to this color space */
        code = gsicc_set_gscs_profile(pcs, cal_profile, ctx->memory);
        /* profile is created with ref count of 1, gsicc_set_gscs_profile()
         * increments the ref count, so we need to decrement it here.
         */
        rc_decrement(cal_profile, "seticc_cal");
        if (code < 0) {
            rc_decrement(pcs, "seticc_cal");
            return code;
        }
        for (i = 0; i < num_colorants; i++) {
            pcs->cmm_icc_profile_data->Range.ranges[i].rmin = 0;
            pcs->cmm_icc_profile_data->Range.ranges[i].rmax = 1;
        }
        /* Add the color space to the profile cache */
        gsicc_add_cs(ctx->pgs, pcs, dictkey);
    } else {
        /* We're passing back a new reference, increment the count */
        rc_adjust_only(pcs, 1, "pdfi_seticc_cal, return cached ICC profile");
    }

    if (ppcs!= NULL){
        *ppcs = pcs;
    } else {
        code = pdfi_gs_setcolorspace(ctx, pcs);
        rc_decrement_only_cs(pcs, "pdfi_seticc_cal");
    }

    return code;
}

static int pdfi_create_CalGray(pdf_context *ctx, pdf_array *color_array, int index, pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs)
{
    int code = 0, i;
    pdf_dict *CalGray_dict = NULL;
    pdf_array *PDFArray = NULL;
    /* The default values here are as per the PDF 1.7 specification, there is
     * no default for the WhitePoint as it is a required entry. The Matrix is
     * not specified for CalGray, but we need it for the general 'pdfi_set_icc'
     * routine, so we use the same default as CalRGB.
     */
    float WhitePoint[3], BlackPoint[3] = {0.0f, 0.0f, 0.0f}, Gamma = 1.0f;
    float Matrix[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    double f;

    code = pdfi_array_get_type(ctx, color_array, index + 1, PDF_DICT, (pdf_obj **)&CalGray_dict);
    if (code < 0)
        return code;

    code = pdfi_dict_get_type(ctx, CalGray_dict, "WhitePoint", PDF_ARRAY, (pdf_obj **)&PDFArray);
    if (code < 0) {
        pdfi_countdown(PDFArray);
        goto exit;
    }
    if (pdfi_array_size(PDFArray) != 3){
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    for (i=0; i < 3; i++) {
        code = pdfi_array_get_number(ctx, PDFArray, (uint64_t)i, &f);
        if (code < 0)
            goto exit;
        WhitePoint[i] = (float)f;
    }
    pdfi_countdown(PDFArray);
    PDFArray = NULL;

    /* Check the WhitePoint values, the PDF 1.7 reference states that
     * Xw ad Zw must be positive and Yw must be 1.0
     */
    if (WhitePoint[0] < 0 || WhitePoint[2] < 0 || WhitePoint[1] != 1.0f) {
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    if (pdfi_dict_knownget_type(ctx, CalGray_dict, "BlackPoint", PDF_ARRAY, (pdf_obj **)&PDFArray)) {
        if (pdfi_array_size(PDFArray) != 3){
            code = gs_note_error(gs_error_rangecheck);
            goto exit;
        }
        for (i=0; i < 3; i++) {
            code = pdfi_array_get_number(ctx, PDFArray, (uint64_t)i, &f);
            if (code < 0)
                goto exit;
            /* The PDF 1.7 reference states that all three components of the BlackPoint
             * (if present) must be positive.
             */
            if (f < 0) {
                code = gs_note_error(gs_error_rangecheck);
                goto exit;
            }
            BlackPoint[i] = (float)f;
        }
        pdfi_countdown(PDFArray);
        PDFArray = NULL;
    }

    if (pdfi_dict_knownget_number(ctx, CalGray_dict, "Gamma", &f))
        Gamma = (float)f;
    /* The PDF 1.7 reference states that Gamma
     * (if present) must be positive.
     */
    if (Gamma < 0) {
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    code = pdfi_seticc_cal(ctx, WhitePoint, BlackPoint, &Gamma, Matrix, 1, color_array->object_num, ppcs);

exit:
    pdfi_countdown(PDFArray);
    pdfi_countdown(CalGray_dict);
    return code;
}

static int pdfi_create_CalRGB(pdf_context *ctx, pdf_array *color_array, int index, pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs)
{
    int code = 0, i;
    pdf_dict *CalRGB_dict = NULL;
    pdf_array *PDFArray = NULL;
    /* The default values here are as per the PDF 1.7 specification, there is
     * no default for the WhitePoint as it is a required entry
     */
    float WhitePoint[3], BlackPoint[3] = {0.0f, 0.0f, 0.0f}, Gamma[3] = {1.0f, 1.0f, 1.0f};
    float Matrix[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    double f;

    code = pdfi_array_get_type(ctx, color_array, index + 1, PDF_DICT, (pdf_obj **)&CalRGB_dict);
    if (code < 0)
        return code;

    code = pdfi_dict_get_type(ctx, CalRGB_dict, "WhitePoint", PDF_ARRAY, (pdf_obj **)&PDFArray);
    if (code < 0) {
        pdfi_countdown(PDFArray);
        goto exit;
    }
    if (pdfi_array_size(PDFArray) != 3){
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    for (i=0; i < 3; i++) {
        code = pdfi_array_get_number(ctx, PDFArray, (uint64_t)i, &f);
        if (code < 0)
            goto exit;
        WhitePoint[i] = (float)f;
    }
    pdfi_countdown(PDFArray);
    PDFArray = NULL;

    /* Check the WhitePoint values, the PDF 1.7 reference states that
     * Xw ad Zw must be positive and Yw must be 1.0
     */
    if (WhitePoint[0] < 0 || WhitePoint[2] < 0 || WhitePoint[1] != 1.0f) {
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    if (pdfi_dict_knownget_type(ctx, CalRGB_dict, "BlackPoint", PDF_ARRAY, (pdf_obj **)&PDFArray)) {
        if (pdfi_array_size(PDFArray) != 3){
            code = gs_note_error(gs_error_rangecheck);
            goto exit;
        }
        for (i=0; i < 3; i++) {
            code = pdfi_array_get_number(ctx, PDFArray, (uint64_t)i, &f);
            if (code < 0)
                goto exit;
            /* The PDF 1.7 reference states that all three components of the BlackPoint
             * (if present) must be positive.
             */
            if (f < 0) {
                code = gs_note_error(gs_error_rangecheck);
                goto exit;
            }
            BlackPoint[i] = (float)f;
        }
        pdfi_countdown(PDFArray);
        PDFArray = NULL;
    }

    if (pdfi_dict_knownget_type(ctx, CalRGB_dict, "Gamma", PDF_ARRAY, (pdf_obj **)&PDFArray)) {
        if (pdfi_array_size(PDFArray) != 3){
            code = gs_note_error(gs_error_rangecheck);
            goto exit;
        }
        for (i=0; i < 3; i++) {
            code = pdfi_array_get_number(ctx, PDFArray, (uint64_t)i, &f);
            if (code < 0)
                goto exit;
            Gamma[i] = (float)f;
        }
        pdfi_countdown(PDFArray);
        PDFArray = NULL;
    }

    if (pdfi_dict_knownget_type(ctx, CalRGB_dict, "Matrix", PDF_ARRAY, (pdf_obj **)&PDFArray)) {
        if (pdfi_array_size(PDFArray) != 9){
            code = gs_note_error(gs_error_rangecheck);
            goto exit;
        }
        for (i=0; i < 9; i++) {
            code = pdfi_array_get_number(ctx, PDFArray, (uint64_t)i, &f);
            if (code < 0)
                goto exit;
            Matrix[i] = (float)f;
        }
        pdfi_countdown(PDFArray);
        PDFArray = NULL;
    }
    code = pdfi_seticc_cal(ctx, WhitePoint, BlackPoint, Gamma, Matrix, 3, color_array->object_num, ppcs);

exit:
    pdfi_countdown(PDFArray);
    pdfi_countdown(CalRGB_dict);
    return code;
}

static int pdfi_create_Separation(pdf_context *ctx, pdf_array *color_array, int index, pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs, bool inline_image)
{
    pdf_obj *o = NULL;
    pdf_name *name = NULL, *NamedAlternate = NULL;
    pdf_array *ArrayAlternate = NULL;
    pdf_dict *transform = NULL;
    int code;
    gs_color_space *pcs = NULL, *pcs_alt = NULL;
    gs_function_t * pfn = NULL;
    separation_type sep_type;

    code = pdfi_array_get_type(ctx, color_array, index + 1, PDF_NAME, (pdf_obj **)&name);
    if (code < 0)
        goto pdfi_separation_error;

    sep_type = SEP_OTHER;
    if (name->length == 4 && memcmp(name->data, "None", 4) == 0)
        sep_type = SEP_NONE;
    if (name->length == 3 && memcmp(name->data, "All", 3) == 0)
        sep_type = SEP_ALL;

    code = pdfi_array_get(ctx, color_array, index + 2, &o);
    if (code < 0)
        goto pdfi_separation_error;

    if (o->type == PDF_NAME) {
        NamedAlternate = (pdf_name *)o;
        code = pdfi_create_colorspace_by_name(ctx, NamedAlternate, stream_dict, page_dict, &pcs_alt, inline_image);
        if (code < 0)
            goto pdfi_separation_error;

    } else {
        if (o->type == PDF_ARRAY) {
            ArrayAlternate = (pdf_array *)o;
            code = pdfi_create_colorspace_by_array(ctx, ArrayAlternate, 0, stream_dict, page_dict, &pcs_alt, inline_image);
            if (code < 0)
                goto pdfi_separation_error;
        }
        else {
            code = gs_error_typecheck;
            goto pdfi_separation_error;
        }
    }

    code = pdfi_array_get_type(ctx, color_array, index + 3, PDF_DICT, (pdf_obj **)&transform);
    if (code < 0)
        goto pdfi_separation_error;

    code = pdfi_build_function(ctx, &pfn, NULL, 1, transform, page_dict);
    if (code < 0)
        goto pdfi_separation_error;

    code = gs_cspace_new_Separation(&pcs, pcs_alt, ctx->memory);
    if (code < 0)
        goto pdfi_separation_error;

    rc_decrement(pcs_alt, "pdfi_create_Separation");
    pcs->params.separation.mem = ctx->memory;
    pcs->params.separation.sep_type = sep_type;
    pcs->params.separation.sep_name = (char *)gs_alloc_bytes(ctx->memory->non_gc_memory, name->length + 1, "pdfi_setseparationspace(ink)");
    memcpy(pcs->params.separation.sep_name, name->data, name->length);
    pcs->params.separation.sep_name[name->length] = 0x00;

    code = gs_cspace_set_sepr_function(pcs, pfn);
    if (code < 0)
        goto pdfi_separation_error;

    if (ppcs!= NULL){
        /* FIXME
         * I can see no justification for this whatever, but if I don't do this then some
         * files with images in a /Separation colour space come out incorrectly. Even surrounding
         * this with a gsave/grestore pair causes differences.
         */
        code = pdfi_gs_setcolorspace(ctx, pcs);
        *ppcs = pcs;
    } else {
        code = pdfi_gs_setcolorspace(ctx, pcs);
        /* release reference from construction */
        rc_decrement_only_cs(pcs, "setseparationspace");
    }

    pdfi_countdown(name);
    pdfi_countdown(NamedAlternate);
    pdfi_countdown(ArrayAlternate);
    pdfi_countdown(transform);
    return_error(0);

pdfi_separation_error:
    pdfi_free_function(ctx, pfn);
    if (pcs_alt != NULL)
        rc_decrement_only_cs(pcs_alt, "setseparationspace");
    if(pcs != NULL)
        rc_decrement_only_cs(pcs, "setseparationspace");
    pdfi_countdown(name);
    pdfi_countdown(NamedAlternate);
    pdfi_countdown(ArrayAlternate);
    pdfi_countdown(transform);
    return code;
}

static int pdfi_create_DeviceN(pdf_context *ctx, pdf_array *color_array, int index, pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs, bool inline_image)
{
    pdf_obj *o = NULL;
    pdf_name *NamedAlternate = NULL;
    pdf_array *ArrayAlternate = NULL, *inks = NULL;
    pdf_dict *transform = NULL;
    pdf_dict *attributes = NULL;
    pdf_dict *Colorants = NULL, *Process = NULL;
    gs_color_space *process_space;
    int code;
    uint64_t ix;
    gs_color_space *pcs = NULL, *pcs_alt = NULL;
    gs_function_t * pfn = NULL;

    /* Deal with alternate space */
    code = pdfi_array_get(ctx, color_array, index + 2, &o);
    if (code < 0)
        goto pdfi_devicen_error;

    if (o->type == PDF_NAME) {
        NamedAlternate = (pdf_name *)o;
        code = pdfi_create_colorspace_by_name(ctx, NamedAlternate, stream_dict, page_dict, &pcs_alt, inline_image);
        if (code < 0)
            goto pdfi_devicen_error;

    } else {
        if (o->type == PDF_ARRAY) {
            ArrayAlternate = (pdf_array *)o;
            code = pdfi_create_colorspace_by_array(ctx, ArrayAlternate, 0, stream_dict, page_dict, &pcs_alt, inline_image);
            if (code < 0)
                goto pdfi_devicen_error;
        }
        else {
            code = gs_error_typecheck;
            goto pdfi_devicen_error;
        }
    }

    /* Now the tint transform */
    code = pdfi_array_get_type(ctx, color_array, index + 3, PDF_DICT, (pdf_obj **)&transform);
    if (code < 0)
        goto pdfi_devicen_error;

    code = pdfi_build_function(ctx, &pfn, NULL, 1, transform, page_dict);
    if (code < 0)
        goto pdfi_devicen_error;

    /* Finally the array of inks */
    code = pdfi_array_get_type(ctx, color_array, index + 1, PDF_ARRAY, (pdf_obj **)&inks);
    if (code < 0)
        goto pdfi_devicen_error;

    /* Sigh, Acrobat allows this, even though its contra the spec. Convert to
     * a /Separation space and go on
     */
    if (pdfi_array_size(inks) == 1) {
        pdf_name *ink_name = NULL;

        code = pdfi_array_get_type(ctx, inks, 0, PDF_NAME, (pdf_obj **)&ink_name);
        if (code < 0)
            goto pdfi_devicen_error;

        if (ink_name->length == 3 && memcmp(ink_name->data, "All", 3) == 0) {
            /* FIXME make a separation space instead (but make sure ink_name still gets freed!) */
            code = gs_note_error(gs_error_undefined);
        }
        pdfi_countdown(ink_name);
        if (code < 0)
            goto pdfi_devicen_error;
    }

    code = gs_cspace_new_DeviceN(&pcs, pdfi_array_size(inks), pcs_alt, ctx->memory);
    if (code < 0)
        return code;

    rc_decrement(pcs_alt, "pdfi_create_DeviceN");
    pcs->params.device_n.mem = ctx->memory;

    for (ix = 0;ix < pdfi_array_size(inks);ix++) {
        pdf_name *ink_name;

        ink_name = NULL;
        code = pdfi_array_get_type(ctx, inks, ix, PDF_NAME, (pdf_obj **)&ink_name);
        if (code < 0)
            goto pdfi_devicen_error;

        pcs->params.device_n.names[ix] = (char *)gs_alloc_bytes(ctx->memory->non_gc_memory, ink_name->length + 1, "pdfi_setdevicenspace(ink)");
        memcpy(pcs->params.device_n.names[ix], ink_name->data, ink_name->length);
        pcs->params.device_n.names[ix][ink_name->length] = 0x00;
        pdfi_countdown(ink_name);
    }

    code = gs_cspace_set_devn_function(pcs, pfn);
    if (code < 0)
        goto pdfi_devicen_error;

    if (pdfi_array_size(color_array) >= index + 5) {
        pdf_obj *ColorSpace = NULL;
        pdf_array *Components = NULL;
        pdf_obj *subtype = NULL;

        code = pdfi_array_get_type(ctx, color_array, index + 4, PDF_DICT, (pdf_obj **)&attributes);
        if (code < 0)
            goto pdfi_devicen_error;

        code = pdfi_dict_knownget(ctx, attributes, "Subtype", (pdf_obj **)&subtype);
        if (code < 0)
            goto pdfi_devicen_error;

        if (code == 0) {
            pcs->params.device_n.subtype = gs_devicen_DeviceN;
        } else {
            if (subtype->type == PDF_NAME || subtype->type == PDF_STRING) {
                if (memcmp(((pdf_name *)subtype)->data, "DeviceN", 7) == 0) {
                    pcs->params.device_n.subtype = gs_devicen_DeviceN;
                } else {
                    if (memcmp(((pdf_name *)subtype)->data, "NChannel", 8) == 0) {
                        pcs->params.device_n.subtype = gs_devicen_NChannel;
                    } else {
                        pdfi_countdown(subtype);
                        goto pdfi_devicen_error;
                    }
                }
                pdfi_countdown(subtype);
            } else {
                pdfi_countdown(subtype);
                goto pdfi_devicen_error;
            }
        }

        code = pdfi_dict_knownget_type(ctx, attributes, "Process", PDF_DICT, (pdf_obj **)&Process);
        if (code < 0)
            goto pdfi_devicen_error;

        if (Process != NULL && pdfi_dict_entries(Process) != 0) {
            int ix = 0;
            pdf_obj *name;

            code = pdfi_dict_get(ctx, Process, "ColorSpace", (pdf_obj **)&ColorSpace);
            if (code < 0)
                goto pdfi_devicen_error;

            code = pdfi_create_colorspace(ctx, ColorSpace, stream_dict, page_dict, &process_space, inline_image);
            pdfi_countdown(ColorSpace);
            if (code < 0)
                goto pdfi_devicen_error;

            pcs->params.device_n.devn_process_space = process_space;

            code = pdfi_dict_get_type(ctx, Process, "Components", PDF_ARRAY, (pdf_obj **)&Components);
            if (code < 0)
                goto pdfi_devicen_error;

            pcs->params.device_n.num_process_names = pdfi_array_size(Components);
            pcs->params.device_n.process_names = (char **)gs_alloc_bytes(pcs->params.device_n.mem->non_gc_memory, pdfi_array_size(Components) * sizeof(char *), "pdfi_devicen(Processnames)");
            if (pcs->params.device_n.process_names == NULL) {
                code = gs_error_VMerror;
                goto pdfi_devicen_error;
            }

            for (ix = 0; ix < pcs->params.device_n.num_process_names; ix++) {
                code = pdfi_array_get(ctx, Components, ix, &name);
                if (code < 0) {
                    pdfi_countdown(Components);
                    goto pdfi_devicen_error;
                }

                if (name->type == PDF_NAME || name->type == PDF_STRING) {
                    pcs->params.device_n.process_names[ix] = (char *)gs_alloc_bytes(pcs->params.device_n.mem->non_gc_memory, ((pdf_name *)name)->length + 1, "pdfi_devicen(Processnames)");
                    if (pcs->params.device_n.process_names[ix] == NULL) {
                        pdfi_countdown(Components);
                        pdfi_countdown(name);
                        code = gs_error_VMerror;
                        goto pdfi_devicen_error;
                    }
                    memcpy(pcs->params.device_n.process_names[ix], ((pdf_name *)name)->data, ((pdf_name *)name)->length);
                    pcs->params.device_n.process_names[ix][((pdf_name *)name)->length] = 0x00;
                    pdfi_countdown(name);
                } else {
                    pdfi_countdown(Components);
                    pdfi_countdown(name);
                    goto pdfi_devicen_error;
                }
            }
            pdfi_countdown(Components);
        }

        code = pdfi_dict_knownget_type(ctx, attributes, "Colorants", PDF_DICT, (pdf_obj **)&Colorants);
        if (code < 0)
            goto pdfi_devicen_error;

        if (Colorants != NULL && pdfi_dict_entries(Colorants) != 0) {
            int ix = 0;
            pdf_obj *Colorant = NULL, *Space = NULL;
            char *colorant_name;
            gs_color_space *colorant_space;

            code = pdfi_dict_first(ctx, Colorants, &Colorant, &Space, &ix);
            if (code < 0)
                goto pdfi_devicen_error;

            do {
                if (Space->type != PDF_STRING && Space->type != PDF_NAME && Space->type != PDF_ARRAY) {
                    code = gs_note_error(gs_error_typecheck);
                    goto pdfi_devicen_error;
                }
                if (Colorant->type != PDF_STRING && Colorant->type != PDF_NAME) {
                    code = gs_note_error(gs_error_typecheck);
                    goto pdfi_devicen_error;
                }

                code = pdfi_create_colorspace(ctx, Space, stream_dict, page_dict, &colorant_space, inline_image);
                if (code < 0) {
                    pdfi_countdown(Space);
                    pdfi_countdown(Colorant);
                    goto pdfi_devicen_error;
                }

                colorant_name = (char *)gs_alloc_bytes(pcs->params.device_n.mem->non_gc_memory, ((pdf_name *)Colorant)->length + 1, "pdfi_devicen(colorant)");
                if (colorant_name == NULL) {
                    rc_decrement_cs(colorant_space, "pdfi_devicen(colorant)");
                    pdfi_countdown(Space);
                    pdfi_countdown(Colorant);
                    code = gs_note_error(gs_error_VMerror);
                    goto pdfi_devicen_error;
                }
                memcpy(colorant_name, ((pdf_name *)Colorant)->data, ((pdf_name *)Colorant)->length);
                colorant_name[((pdf_name *)Colorant)->length] = 0x00;

                code = gs_attach_colorant_to_space(colorant_name, pcs, colorant_space, pcs->params.device_n.mem->non_gc_memory);
                if (code < 0) {
                    gs_free_object(pcs->params.device_n.mem->non_gc_memory, colorant_name, "pdfi_devicen(colorant)");
                    rc_decrement_cs(colorant_space, "pdfi_devicen(colorant)");
                    pdfi_countdown(Space);
                    pdfi_countdown(Colorant);
                    code = gs_note_error(gs_error_VMerror);
                    goto pdfi_devicen_error;
                }

                code = pdfi_dict_next(ctx, Colorants, &Colorant, &Space, &ix);
                if (code == gs_error_undefined)
                    break;

                if (code < 0) {
                    pdfi_countdown(Space);
                    pdfi_countdown(Colorant);
                    goto pdfi_devicen_error;
                }
            }while (1);
        }
    }

    if (ppcs!= NULL){
        *ppcs = pcs;
    } else {
        code = pdfi_gs_setcolorspace(ctx, pcs);
        /* release reference from construction */
        rc_decrement_only_cs(pcs, "setdevicenspace");
    }
    pdfi_countdown(Process);
    pdfi_countdown(Colorants);
    pdfi_countdown(attributes);
    pdfi_countdown(inks);
    pdfi_countdown(NamedAlternate);
    pdfi_countdown(ArrayAlternate);
    pdfi_countdown(transform);
    return_error(0);

pdfi_devicen_error:
    pdfi_free_function(ctx, pfn);
    if (pcs_alt != NULL)
        rc_decrement_only_cs(pcs_alt, "setseparationspace");
    if(pcs != NULL)
        rc_decrement_only_cs(pcs, "setseparationspace");
    pdfi_countdown(Process);
    pdfi_countdown(Colorants);
    pdfi_countdown(attributes);
    pdfi_countdown(inks);
    pdfi_countdown(NamedAlternate);
    pdfi_countdown(ArrayAlternate);
    pdfi_countdown(transform);
    return code;
}

/* Now /Indexed spaces, essentially we just need to set the underlying space(s) and then set
 * /Indexed.
 */
static int
pdfi_create_indexed(pdf_context *ctx, pdf_array *color_array, int index,
                    pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs, bool inline_image)
{
    pdf_obj *space=NULL, *lookup=NULL;
    int code;
    int64_t hival, lookup_length = 0;
    int num_values;
    gs_color_space *pcs, *pcs_base;
    gs_color_space_index base_type;
    byte *Buffer = NULL;

    if (index != 0)
        return_error(gs_error_syntaxerror);

    code = pdfi_array_get_int(ctx, color_array, index + 2, &hival);
    if (code < 0)
        return code;

    if (hival > 255 || hival < 0)
        return_error(gs_error_syntaxerror);

    code = pdfi_array_get(ctx, color_array, index + 1, &space);
    if (code < 0)
        goto exit;

    code = pdfi_create_colorspace(ctx, space, stream_dict, page_dict, &pcs_base, inline_image);
    if (code < 0)
        goto exit;

    (void)pcs_base->type->install_cspace(pcs_base, ctx->pgs);
    if (code < 0)
        goto exit;

    base_type = gs_color_space_get_index(pcs_base);

    code = pdfi_array_get(ctx, color_array, index + 3, &lookup);
    if (code < 0)
        goto exit;

    if (lookup->type == PDF_DICT) {
        code = pdfi_stream_to_buffer(ctx, (pdf_dict *)lookup, &Buffer, &lookup_length);
        if (code < 0)
            goto exit;
    } else if (lookup->type == PDF_STRING) {
        /* This is not legal, but Acrobat seems to accept it */
        pdf_string *lookup_string = (pdf_string *)lookup; /* alias */

        Buffer = gs_alloc_bytes(ctx->memory, lookup_string->length, "pdfi_create_indexed (lookup buffer)");
        if (Buffer == NULL) {
            code = gs_note_error(gs_error_VMerror);
            goto exit;
        }

        memcpy(Buffer, lookup_string->data, lookup_string->length);
        lookup_length = lookup_string->length;
    } else {
        code = gs_note_error(gs_error_typecheck);
        goto exit;
    }

    num_values = (hival+1) * cs_num_components(pcs_base);
    if (num_values > lookup_length) {
        dmprintf2(ctx->memory, "WARNING: pdfi_create_indexed() got %ld values, expected at least %d values\n",
                  lookup_length, num_values);
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    /* If we have a named color profile and the base space is DeviceN or
       Separation use a different set of procedures to ensure the named
       color remapping code is used */
    if (ctx->pgs->icc_manager->device_named != NULL &&
        (base_type == gs_color_space_index_Separation ||
         base_type == gs_color_space_index_DeviceN))
        pcs = gs_cspace_alloc(ctx->memory, &gs_color_space_type_Indexed_Named);
    else
        pcs = gs_cspace_alloc(ctx->memory, &gs_color_space_type_Indexed);

    /* NOTE: we don't need to increment the reference to pcs_base, since it is already 1 */
    pcs->base_space = pcs_base;

    pcs->params.indexed.lookup.table.size = num_values;
    pcs->params.indexed.use_proc = 0;
    pcs->params.indexed.hival = hival;
    pcs->params.indexed.n_comps = cs_num_components(pcs_base);
    pcs->params.indexed.lookup.table.data = Buffer;
    Buffer = NULL;

    if (ppcs != NULL)
        *ppcs = pcs;
    else {
        code = pdfi_gs_setcolorspace(ctx, pcs);
        /* release reference from construction */
        rc_decrement_only_cs(pcs, "setindexedspace");
    }

 exit:
    if (Buffer)
        gs_free_object(ctx->memory, Buffer, "pdfi_create_indexed (decompression buffer)");
    pdfi_countdown(space);
    pdfi_countdown(lookup);
    return code;
}

static int pdfi_create_DeviceGray(pdf_context *ctx, gs_color_space **ppcs)
{
    int code = 0;

    if (ppcs != NULL) {
        *ppcs = gs_cspace_new_DeviceGray(ctx->memory);
        if (*ppcs == NULL)
            code = gs_note_error(gs_error_VMerror);
        else {
            code = ((gs_color_space *)*ppcs)->type->install_cspace(*ppcs, ctx->pgs);
            if (code < 0) {
                rc_decrement_only_cs(*ppcs, "pdfi_create_DeviceGray");
                *ppcs = NULL;
            }
        }
    } else {
        code = pdfi_gs_setgray(ctx, 1);
    }
    return code;
}

static int pdfi_create_DeviceRGB(pdf_context *ctx, gs_color_space **ppcs)
{
    int code = 0;

    if (ppcs != NULL) {
        *ppcs = gs_cspace_new_DeviceRGB(ctx->memory);
        if (*ppcs == NULL)
            code = gs_note_error(gs_error_VMerror);
        else {
            code = ((gs_color_space *)*ppcs)->type->install_cspace(*ppcs, ctx->pgs);
            if (code < 0) {
                rc_decrement_only_cs(*ppcs, "pdfi_create_DeviceRGB");
                *ppcs = NULL;
            }
        }
    } else {
        code = pdfi_gs_setrgbcolor(ctx, 0, 0, 0);
    }
    return code;
}

static int pdfi_create_DeviceCMYK(pdf_context *ctx, gs_color_space **ppcs)
{
    int code = 0;

    if (ppcs != NULL) {
        *ppcs = gs_cspace_new_DeviceCMYK(ctx->memory);
        if (*ppcs == NULL)
            code = gs_note_error(gs_error_VMerror);
        else {
            code = ((gs_color_space *)*ppcs)->type->install_cspace(*ppcs, ctx->pgs);
            if (code < 0) {
                rc_decrement_only_cs(*ppcs, "pdfi_create_DeviceCMYK");
                *ppcs = NULL;
            }
        }
    } else {
        code = pdfi_gs_setcmykcolor(ctx, 0, 0, 0, 1);
    }
    return code;
}

/* These next routines allow us to use recursion to set up colour spaces. We can set
 * colour space starting from a name (which can be a named resource) or an array.
 * If we get a name, and its a named resource we dereference it and go round again.
 * If its an array we select the correct handler (above) for that space. The space
 * handler will call pdfi_create_colorspace() to set the underlying space(s) which
 * may mean calling pdfi_create_colorspace again....
 */
static int
pdfi_create_colorspace_by_array(pdf_context *ctx, pdf_array *color_array, int index,
                                pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs,
                                bool inline_image)
{
    int code;
    pdf_name *space = NULL;
    pdf_array *a = NULL;

    code = pdfi_array_get_type(ctx, color_array, index, PDF_NAME, (pdf_obj **)&space);
    if (code != 0)
        goto exit;

    code = 0;
    if (pdfi_name_is(space, "G") || pdfi_name_is(space, "DeviceGray")) {
        if (pdfi_name_is(space, "G") && !inline_image) {
            ctx->pdf_warnings|= W_PDF_BAD_INLINECOLORSPACE;
            if (ctx->pdfstoponwarning)
                return_error(gs_error_syntaxerror);
        }
        code = pdfi_create_DeviceGray(ctx, ppcs);
    } else if (pdfi_name_is(space, "I") || pdfi_name_is(space, "Indexed")) {
        if (pdfi_name_is(space, "I") && !inline_image) {
            ctx->pdf_warnings|= W_PDF_BAD_INLINECOLORSPACE;
            if (ctx->pdfstoponwarning)
                return_error(gs_error_syntaxerror);
        }
        code = pdfi_create_indexed(ctx, color_array, index, stream_dict, page_dict, ppcs, inline_image);
    } else if (pdfi_name_is(space, "Lab")) {
        code = pdfi_create_Lab(ctx, color_array, index, stream_dict, page_dict, ppcs);
    } else if (pdfi_name_is(space, "RGB") || pdfi_name_is(space, "DeviceRGB")) {
        if (pdfi_name_is(space, "RGB") && !inline_image) {
            ctx->pdf_warnings|= W_PDF_BAD_INLINECOLORSPACE;
            if (ctx->pdfstoponwarning)
                return_error(gs_error_syntaxerror);
        }
        code = pdfi_create_DeviceRGB(ctx, ppcs);
    } else if (pdfi_name_is(space, "CMYK") || pdfi_name_is(space, "DeviceCMYK")) {
        if (pdfi_name_is(space, "CMYK") && !inline_image) {
            ctx->pdf_warnings|= W_PDF_BAD_INLINECOLORSPACE;
            if (ctx->pdfstoponwarning)
                return_error(gs_error_syntaxerror);
        }
        code = pdfi_create_DeviceCMYK(ctx, ppcs);
    } else if (pdfi_name_is(space, "CalRGB")) {
        code = pdfi_create_CalRGB(ctx, color_array, index, stream_dict, page_dict, ppcs);
    } else if (pdfi_name_is(space, "CalGray")) {
        code = pdfi_create_CalGray(ctx, color_array, index, stream_dict, page_dict, ppcs);
    } else if (pdfi_name_is(space, "Pattern")) {
        if (index != 0)
            code = gs_note_error(gs_error_syntaxerror);
        else
            code = pdfi_pattern_create(ctx, color_array, stream_dict, page_dict, ppcs);
    } else if (pdfi_name_is(space, "DeviceN")) {
        code = pdfi_create_DeviceN(ctx, color_array, index, stream_dict, page_dict, ppcs, inline_image);
    } else if (pdfi_name_is(space, "ICCBased")) {
        code = pdfi_create_iccbased(ctx, color_array, index, stream_dict, page_dict, ppcs, inline_image);
    } else if (pdfi_name_is(space, "Separation")) {
        code = pdfi_create_Separation(ctx, color_array, index, stream_dict, page_dict, ppcs, inline_image);
    } else {
        code = pdfi_find_resource(ctx, (unsigned char *)"ColorSpace",
                                  space, stream_dict, page_dict, (pdf_obj **)&a);
        if (code < 0)
            goto exit;

        if (a->type != PDF_ARRAY) {
            code = gs_note_error(gs_error_typecheck);
            goto exit;
        }

        /* recursion */
        code = pdfi_create_colorspace_by_array(ctx, a, 0, stream_dict, page_dict, ppcs, inline_image);
    }

 exit:
    pdfi_countdown(space);
    pdfi_countdown(a);
    return code;
}

static int
pdfi_create_colorspace_by_name(pdf_context *ctx, pdf_name *name,
                               pdf_dict *stream_dict, pdf_dict *page_dict,
                               gs_color_space **ppcs, bool inline_image)
{
    int code = 0;

    if (pdfi_name_is(name, "G") || pdfi_name_is(name, "DeviceGray")) {
        if (pdfi_name_is(name, "G") && !inline_image) {
            ctx->pdf_warnings|= W_PDF_BAD_INLINECOLORSPACE;
            if (ctx->pdfstoponwarning)
                return_error(gs_error_syntaxerror);
        }
        code = pdfi_create_DeviceGray(ctx, ppcs);
    } else if (pdfi_name_is(name, "RGB") || pdfi_name_is(name, "DeviceRGB")) {
        if (pdfi_name_is(name, "RGB") && !inline_image) {
            ctx->pdf_warnings|= W_PDF_BAD_INLINECOLORSPACE;
            if (ctx->pdfstoponwarning)
                return_error(gs_error_syntaxerror);
        }
        code = pdfi_create_DeviceRGB(ctx, ppcs);
    } else if (pdfi_name_is(name, "CMYK") || pdfi_name_is(name, "DeviceCMYK")) {
        if (pdfi_name_is(name, "CMYK") && !inline_image) {
            ctx->pdf_warnings|= W_PDF_BAD_INLINECOLORSPACE;
            if (ctx->pdfstoponwarning)
                return_error(gs_error_syntaxerror);
        }
        code = pdfi_create_DeviceCMYK(ctx, ppcs);
    } else if (pdfi_name_is(name, "Pattern")) {
        code = pdfi_pattern_create(ctx, NULL, stream_dict, page_dict, ppcs);
    } else {
        pdf_obj *ref_space = NULL;
        code = pdfi_find_resource(ctx, (unsigned char *)"ColorSpace", name, stream_dict, page_dict, &ref_space);
        if (code < 0)
            return code;

        /* recursion */
        code = pdfi_create_colorspace(ctx, ref_space, stream_dict, page_dict, ppcs, inline_image);
        pdfi_countdown(ref_space);
        return code;
    }

    /* If we got here, it's a recursion base case, and ppcs should have been set if requested */
    if (ppcs != NULL && *ppcs == NULL)
        code = gs_note_error(gs_error_VMerror);
    return code;
}

/*
 * Gets icc profile data from the provided stream.
 * Position in the stream is NOT preserved.
 * This is raw data, not filtered, so no need to worry about compression.
 * (Used for JPXDecode images)
 */
int
pdfi_create_icc_colorspace_from_stream(pdf_context *ctx, pdf_stream *stream, gs_offset_t offset,
                                       unsigned int length, int comps, int *icc_N, gs_color_space **ppcs)
{
    pdf_stream *profile_stream = NULL;
    byte *profile_buffer;
    int code, code1;
    float range[8] = {0,1,0,1,0,1,0,1};

    /* Move to the start of the profile data */
    pdfi_seek(ctx, stream, offset, SEEK_SET);

    /* The ICC profile reading code (irritatingly) requires a seekable stream, because it
     * rewinds it to the start, then seeks to the end to find the size, then rewinds the
     * stream again.
     * Ideally we would use a ReusableStreamDecode filter here, but that is largely
     * implemented in PostScript (!) so we can't use it. What we can do is create a
     * string sourced stream in memory, which is at least seekable.
     */
    code = pdfi_open_memory_stream_from_stream(ctx, length, &profile_buffer,
                                               stream, &profile_stream);
    if (code < 0) {
        return code;
    }

    /* Now, finally, we can call the code to create and set the profile */
    code = pdfi_create_icc(ctx, NULL, profile_stream->s, comps, icc_N, range, ppcs);

    code1 = pdfi_close_memory_stream(ctx, profile_buffer, profile_stream);

    if (code == 0)
        code = code1;

    return code;
}

/* Cleanup (deallocate) extra things for various types of color spaces
 *   pcs -- colorspace (assumed not to be null)
 *   pcc -- client color (can be null, but won't be in current usage)
 */
static int
pdfi_color_cleanup_inner(pdf_context *ctx, gs_color_space *pcs, gs_client_color *pcc)
{
    int code = 0;
    gs_function_t *pfn;

    /* Handle cleanup of Separation functions if applicable */
    pfn = gs_cspace_get_sepr_function(pcs);
    if (pfn)
        pdfi_free_function(ctx, pfn);

    /* Handle cleanup of DeviceN functions if applicable */
    pfn = gs_cspace_get_devn_function(pcs);
    if (pfn)
        pdfi_free_function(ctx, pfn);

    if (pcc) {
        /* Handle Pattern cleanup if applicable */
        if (pcs->type->index == gs_color_space_index_Pattern)
            code = pdfi_pattern_cleanup(ctx, pcc);
    }
    return code;
}

/* This is called in places where the colorspace might be about to get freed.
 * It gives us a hook to cleanup the data associated with some of the more
 * complicated colorspaces, such as patterns and spaces with functions.
 *
 * It's broken up into extra pdfi_color_cleanup_inner() because I thought I
 * might need to call the actual cleanup in different ways, but it turned out
 * not to be necessary (so far).  This keeps the code a bit more clear anyway.
 */
int pdfi_color_cleanup(pdf_context *ctx, int index)
{
    gs_color_space *pcs;
    gs_client_color *pcc;

    /* Only do the cleanup if it is about to be freed */
    if (ctx->pgs->color[index].color_space->rc.ref_count != 1)
        return 0;

    pcs = ctx->pgs->color[index].color_space;
    pcc = ctx->pgs->color[index].ccolor;
    return pdfi_color_cleanup_inner(ctx, pcs, pcc);
}

int pdfi_create_colorspace(pdf_context *ctx, pdf_obj *space, pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs, bool inline_image)
{
    int code;

    code = pdfi_loop_detector_mark(ctx);
    if (code < 0)
        return code;

    if (space->type == PDF_NAME) {
        code = pdfi_create_colorspace_by_name(ctx, (pdf_name *)space, stream_dict, page_dict, ppcs, inline_image);
    } else {
        if (space->type == PDF_ARRAY) {
            code = pdfi_create_colorspace_by_array(ctx, (pdf_array *)space, 0, stream_dict, page_dict, ppcs, inline_image);
        } else {
            pdfi_loop_detector_cleartomark(ctx);
            return_error(gs_error_typecheck);
        }
    }
    if (ppcs && *ppcs && code >= 0)
        (void)(*ppcs)->type->install_cspace(*ppcs, ctx->pgs);

    (void)pdfi_loop_detector_cleartomark(ctx);
    return code;
}

int pdfi_setcolorspace(pdf_context *ctx, pdf_obj *space, pdf_dict *stream_dict, pdf_dict *page_dict)
{
    return pdfi_create_colorspace(ctx, space, stream_dict, page_dict, NULL, false);
}

/* And finally, the implementation of the actual PDF operators CS and cs */
int pdfi_setstrokecolor_space(pdf_context *ctx, pdf_dict *stream_dict, pdf_dict *page_dict)
{
    int code;

    if (pdfi_count_stack(ctx) < 1) {
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }
    if (ctx->stack_top[-1]->type != PDF_NAME) {
        pdfi_pop(ctx, 1);
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }
    gs_swapcolors_quick(ctx->pgs);
    code = pdfi_setcolorspace(ctx, ctx->stack_top[-1], stream_dict, page_dict);
    gs_swapcolors_quick(ctx->pgs);
    pdfi_pop(ctx, 1);

    if (code < 0 && ctx->pdfstoponerror)
        return code;
    return 0;
}

int pdfi_setfillcolor_space(pdf_context *ctx, pdf_dict *stream_dict, pdf_dict *page_dict)
{
    int code;

    if (pdfi_count_stack(ctx) < 1) {
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }
    if (ctx->stack_top[-1]->type != PDF_NAME) {
        pdfi_pop(ctx, 1);
        if (ctx->pdfstoponerror)
            return_error(gs_error_stackunderflow);
        return 0;
    }
    code = pdfi_setcolorspace(ctx, ctx->stack_top[-1], stream_dict, page_dict);
    pdfi_pop(ctx, 1);

    if (code < 0 && ctx->pdfstoponerror)
        return code;
    return 0;
}