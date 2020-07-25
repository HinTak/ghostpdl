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

/* Image operations for the PDF interpreter */

#include "pdf_int.h"
#include "pdf_stack.h"
#include "pdf_gstate.h"
#include "pdf_page.h"
#include "pdf_image.h"
#include "pdf_file.h"
#include "pdf_dict.h"
#include "pdf_array.h"
#include "pdf_loop_detect.h"
#include "pdf_colour.h"
#include "pdf_trans.h"
#include "pdf_misc.h"
#include "pdf_optcontent.h"
#include "stream.h"     /* for stell() */

#include "gspath2.h"
#include "gsiparm4.h"
#include "gsiparm3.h"
#include "gsipar3x.h"

#include "gstrans.h"

int pdfi_BI(pdf_context *ctx)
{
    if (ctx->TextBlockDepth != 0)
        ctx->pdf_warnings |= W_PDF_OPINVALIDINTEXT;

    return pdfi_mark_stack(ctx, PDF_DICT_MARK);
}

typedef struct {
    int comps;
    int bpc;
    uint32_t cs_enum;
    bool iccbased;
    uint32_t icc_offset;
    uint32_t icc_length;
} pdfi_jpx_info_t;

typedef struct {
    /* Type and SubType were already checked by caller */
    /* OPI, Metadata -- do we care? */
    bool ImageMask;
    bool Interpolate;
    int64_t Length;
    int64_t Height;
    int64_t Width;
    int64_t BPC;
    int64_t StructParent;
    int64_t SMaskInData;
    pdf_obj *Mask;
    pdf_obj *SMask;
    pdf_obj *ColorSpace;
    pdf_name *Intent;
    pdf_obj *Alternates;
    pdf_obj *Name; /* obsolete, do we still support? */
    pdf_obj *Decode;
    pdf_dict *OC;  /* Optional Content */
    /* Filter and DecodeParms handled by pdfi_filter() (can probably remove, but I like the info while debugging) */
    bool is_JPXDecode;
    pdf_obj *Filter;
    pdf_obj *DecodeParms;

    /* Convenience variables (save these here instead of passing around as params) */
    pdf_dict *page_dict;
    pdf_dict *stream_dict;
    bool inline_image;
    pdfi_jpx_info_t jpx_info;
} pdfi_image_info_t;

static void
pdfi_free_image_info_components(pdfi_image_info_t *info)
{
    if (info->Mask)
        pdfi_countdown(info->Mask);
    if (info->SMask)
        pdfi_countdown(info->SMask);
    if (info->ColorSpace)
        pdfi_countdown(info->ColorSpace);
    if (info->Intent)
        pdfi_countdown(info->Intent);
    if (info->Alternates)
        pdfi_countdown(info->Alternates);
    if (info->Name)
        pdfi_countdown(info->Name);
    if (info->Decode)
        pdfi_countdown(info->Decode);
    if (info->OC)
        pdfi_countdown(info->OC);
    if (info->Filter)
        pdfi_countdown(info->Filter);
    if (info->DecodeParms)
        pdfi_countdown(info->DecodeParms);
    memset(info, 0, sizeof(*info));
}


static inline uint64_t
pdfi_get_image_data_size(gs_data_image_t *pim, int comps)
{
    int size;
    int64_t H, W, B;

    H = pim->Height;
    W = pim->Width;
    B = pim->BitsPerComponent;

    size = (((W * comps * B) + 7) / 8) * H;
    return size;
}

static inline uint64_t
pdfi_data_size_from_image_info(pdfi_image_info_t *info, int comps)
{
    int size;
    int64_t H, W, B;

    H = info->Height;
    W = info->Width;
    B = info->BPC;

    size = (((W * comps * B) + 7) / 8) * H;
    return size;
}

static inline uint64_t
pdfi_get_image_line_size(gs_data_image_t *pim, int comps)
{
    int size;
    int64_t W, B;

    W = pim->Width;
    B = pim->BitsPerComponent;

    size = (((W * comps * B) + 7) / 8);
    return size;
}

/* Find first dictionary in array that contains "/DefaultForPrinting true" */
static pdf_dict *
pdfi_find_alternate(pdf_context *ctx, pdf_obj *alt)
{
    pdf_array *array = NULL;
    pdf_obj *item = NULL;
    pdf_dict *alt_dict = NULL;
    int i;
    int code;
    bool flag;

    if (alt->type != PDF_ARRAY)
        return NULL;

    array = (pdf_array *)alt;
    for (i=0; i<pdfi_array_size(array);i++) {
        code = pdfi_array_get_type(ctx, array, (uint64_t)i, PDF_DICT, &item);
        if (code != 0)
            continue;
        code = pdfi_dict_get_bool(ctx, (pdf_dict *)item, "DefaultForPrinting", &flag);
        if (code != 0 || !flag) {
            pdfi_countdown(item);
            item = NULL;
            continue;
        }
        code = pdfi_dict_get_type(ctx, (pdf_dict *)item, "Image", PDF_DICT, (pdf_obj **)&alt_dict);
        pdfi_countdown(item);
        item = NULL;
        if (code != 0)
            continue;
        return alt_dict;
    }
    return NULL;
}

#define READ32BE(i) (((i)[0] << 24) | ((i)[1] << 16) | ((i)[2] << 8) | (i)[3])
#define READ16BE(i) (((i)[0] << 8) | (i)[1])
#define K4(a, b, c, d) ((a << 24) + (b << 16) + (c << 8) + d)
#define LEN_IHDR 14
#define LEN_DATA 2048

/* Returns either < 0, or exactly 8 */
static int
get_box(pdf_context *ctx, pdf_stream *source, int length, uint32_t *box_len, uint32_t *box_val)
{
    int code;
    byte blob[4];

    if (length < 8)
        return_error(gs_error_limitcheck);
    code = pdfi_read_bytes(ctx, blob, 1, 4, source);
    if (code < 0)
        return code;
    *box_len = READ32BE(blob);
    if (*box_len < 8)
        return_error(gs_error_limitcheck);
    code = pdfi_read_bytes(ctx, blob, 1, 4, source);
    if (code < 0)
        return code;
    *box_val = READ32BE(blob);

    if(ctx->pdfdebug)
        dbgmprintf3(ctx->memory, "JPXFilter: BOX: l:%d, v:%x (%4.4s)\n", *box_len, *box_val, blob);
    return 8;
}

/* Scan JPX image for header info */
static int
pdfi_scan_jpxfilter(pdf_context *ctx, pdf_stream *source, int length, pdfi_jpx_info_t *info)
{
    uint32_t box_len = 0;
    uint32_t box_val = 0;
    int code;
    byte ihdr_data[LEN_IHDR];
    byte *data = NULL;
    int data_buf_len = 0;
    int avail = length;
    int bpc = 0;
    int comps = 0;
    int cs_meth = 0;
    uint32_t cs_enum = 0;
    bool got_color = false;

    if (ctx->pdfdebug)
        dbgmprintf1(ctx->memory, "JPXFilter: Image length %d\n", length);

    /* Clear out the info param */
    memset(info, 0, sizeof(pdfi_jpx_info_t));

    /* Allocate a data buffer that hopefully is big enough */
    data_buf_len = LEN_DATA;
    data = gs_alloc_bytes(ctx->memory, data_buf_len, "pdfi_scan_jpxfilter (data)");
    if (!data) {
        code = gs_note_error(gs_error_VMerror);
        goto exit;
    }

    /* Find the 'jp2h' box, skipping over everything else */
    while (avail > 0) {
        code = get_box(ctx, source, avail, &box_len, &box_val);
        if (code < 0)
            goto exit;
        avail -= 8;
        box_len -= 8;
        if (box_len <= 0 || box_len > avail) {
            dmprintf1(ctx->memory, "WARNING: invalid JPX header, box_len=0x%x\n", box_len+8);
            code = gs_note_error(gs_error_syntaxerror);
            goto exit;
        }
        if (box_val == K4('j','p','2','h')) {
            break;
        }
        pdfi_seek(ctx, source, box_len, SEEK_CUR);
        avail -= box_len;
    }
    if (avail <= 0) {
        code = gs_note_error(gs_error_ioerror);
        goto exit;
    }

    /* Now we are only looking inside the jp2h box */
    avail = box_len;

    /* The first thing in the 'jp2h' box is an 'ihdr', get that */
    code = get_box(ctx, source, avail, &box_len, &box_val);
    if (code < 0)
        goto exit;
    avail -= 8;
    box_len -= 8;
    if (box_val != K4('i','h','d','r')) {
        code = gs_note_error(gs_error_syntaxerror);
        goto exit;
    }
    if (box_len != LEN_IHDR) {
        code = gs_note_error(gs_error_syntaxerror);
        goto exit;
    }

    /* Get things we care about from ihdr */
    code = pdfi_read_bytes(ctx, ihdr_data, 1, LEN_IHDR, source);
    if (code < 0)
        goto exit;
    avail -= LEN_IHDR;
    comps = READ16BE(ihdr_data+8);
    if (ctx->pdfdebug)
        dbgmprintf1(ctx->memory, "    COMPS: %d\n", comps);
    bpc = ihdr_data[10];
    if (bpc != 255)
        bpc += 1;
    if (ctx->pdfdebug)
        dbgmprintf1(ctx->memory, "    BPC: %d\n", bpc);

    /* Parse the rest of the things */
    while (avail > 0) {
        code = get_box(ctx, source, avail, &box_len, &box_val);
        if (code < 0)
            goto exit;
        avail -= 8;
        box_len -= 8;
        if (box_len <= 0) {
            code = gs_note_error(gs_error_syntaxerror);
            goto exit;
        }
        /* Re-alloc buffer if it wasn't big enough (unlikely) */
        if (box_len > data_buf_len) {
            if (ctx->pdfdebug)
                dbgmprintf2(ctx->memory, "data buffer (size %d) was too small, reallocing to size %d\n",
                      data_buf_len, box_len);
            gs_free_object(ctx->memory, data, "pdfi_scan_jpxfilter (data)");
            data_buf_len = box_len;
            data = gs_alloc_bytes(ctx->memory, data_buf_len, "pdfi_scan_jpxfilter (data)");
            if (!data) {
                code = gs_note_error(gs_error_VMerror);
                goto exit;
            }
        }
        code = pdfi_read_bytes(ctx, data, 1, box_len, source);
        if (code < 0)
            goto exit;
        avail -= box_len;
        switch(box_val) {
        case K4('b','p','c','c'):
            {
                int i;
                int bpc2;

                bpc2 = data[0];
                for (i=1;i<comps;i++) {
                    if (bpc2 != data[i]) {
                        emprintf(ctx->memory,
                                 "*** Error: JPX image colour channels do not all have the same colour depth\n");
                        emprintf(ctx->memory,
                                 "    Output may be incorrect.\n");
                    }
                }
                bpc = bpc2+1;
                if (ctx->pdfdebug)
                    dbgmprintf1(ctx->memory, "    BPCC: %d\n", bpc);
            }
            break;
        case K4('c','o','l','r'):
            if (got_color) {
                if (ctx->pdfdebug)
                    dbgmprintf(ctx->memory, "JPXFilter: Ignore extra COLR specs\n");
                break;
            }
            cs_meth = data[0];
            if (cs_meth == 1)
                cs_enum = READ32BE(data+3);
            else if (cs_meth == 2 || cs_meth == 3) {
                /* This is an ICCBased color space just sitting there in the buffer.
                 * TODO: I could create the colorspace now while I have the buffer,
                 * but code flow is more consistent if I do it later.  Could change this.
                 *
                 * NOTE: cs_meth == 3 is apparently treated the same as 2.
                 * No idea why... it's really not documented anywhere.
                 */
                info->iccbased = true;
                info->icc_offset = pdfi_tell(source) - (box_len-3);
                info->icc_length = box_len - 3;
                if (ctx->pdfdebug)
                    dbgmprintf5(ctx->memory, "JPXDecode: COLR Meth %d at offset %d(0x%x), length %d(0x%x)\n",
                                cs_meth, info->icc_offset, info->icc_offset,
                                info->icc_length, info->icc_length);
                cs_enum = 0;
            } else {
                if (ctx->pdfdebug)
                    dbgmprintf1(ctx->memory, "JPXDecode: COLR unexpected method %d\n", cs_meth);
                cs_enum = 0;
            }
            if (ctx->pdfdebug)
                dbgmprintf2(ctx->memory, "    COLR: M:%d, ENUM:%d\n", cs_meth, cs_enum);
            got_color = true;
            break;
        case K4('p','c','l','r'):
            /* Apparently we just grab the BPC out of this */
            if (ctx->pdfdebug)
                dbgmprintf7(ctx->memory, "    PCLR Data: %x %x %x %x %x %x %x\n",
                      data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
            bpc = data[3];
            bpc = (bpc & 0x7) + 1;
            if (ctx->pdfdebug)
                dbgmprintf1(ctx->memory, "    PCLR BPC: %d\n", bpc);
            break;
        case K4('c','d','e','f'):
            dbgmprintf(ctx->memory, "JPXDecode: CDEF not supported yet\n");
            break;
        default:
            break;
        }

    }

    info->comps = comps;
    info->bpc = bpc;
    info->cs_enum = cs_enum;

 exit:
    if (data)
        gs_free_object(ctx->memory, data, "pdfi_scan_jpxfilter (data)");
    /* Always return 0 -- there are cases where this no image header at all, and just ignoring
     * the header seems to work.  May need to add an is_valid flag for other weird cases?
     * (need to encounter such a sample first)
     */
    return 0;
}

/* Get image info out of dict into more convenient form, enforcing some requirements from the spec */
static int
pdfi_get_image_info(pdf_context *ctx, pdf_dict *image_dict,
                    pdf_dict *page_dict, pdf_dict *stream_dict, bool inline_image,
                    pdfi_image_info_t *info)
{
    int code;
    double temp_f;

    memset(info, 0, sizeof(*info));
    info->page_dict = page_dict;
    info->stream_dict = stream_dict;
    info->inline_image = inline_image;

    /* Not Handled: "ID", "OPI" */

    /* Length if it's in a stream dict */
    info->Length = pdfi_dict_stream_length(ctx, image_dict);

    /* Required */
    code = pdfi_dict_get_number2(ctx, image_dict, "Height", "H", &temp_f);
    if (code < 0)
        goto errorExit;
    /* This is bonkers, but... Bug695872.pdf has /W and /H which are real numbers */
    info->Height = (int)temp_f;
    if ((int)temp_f != (int)(temp_f+.5)) {
        ctx->pdf_warnings |= W_PDF_BAD_IMAGEDICT;
        if (ctx->pdfstoponwarning) {
            code = gs_note_error(gs_error_rangecheck);
            goto errorExit;
        }
    }

    /* Required */
    code = pdfi_dict_get_number2(ctx, image_dict, "Width", "W", &temp_f);
    if (code < 0)
        goto errorExit;
    info->Width = (int)temp_f;
    if ((int)temp_f != (int)(temp_f+.5)) {
        ctx->pdf_warnings |= W_PDF_BAD_IMAGEDICT;
        if (ctx->pdfstoponwarning) {
            code = gs_note_error(gs_error_rangecheck);
            goto errorExit;
        }
    }

    /* Optional, default false */
    code = pdfi_dict_get_bool2(ctx, image_dict, "ImageMask", "IM", &info->ImageMask);
    if (code != 0) {
        if (code != gs_error_undefined)
            goto errorExit;
        info->ImageMask = false;
    }

    /* Optional, default false */
    code = pdfi_dict_get_bool2(ctx, image_dict, "Interpolate", "I", &info->Interpolate);
    if (code != 0) {
        if (code != gs_error_undefined)
            goto errorExit;
        info->Interpolate = false;
    }

    /* Optional (Required, unless ImageMask is true)
     * But apparently for JPXDecode filter, this can be omitted.
     * Let's try a default of 1 for now...
     */
    code = pdfi_dict_get_int2(ctx, image_dict, "BitsPerComponent", "BPC", &info->BPC);
    if (code < 0) {
        if (code != gs_error_undefined) {
            goto errorExit;
        }
        info->BPC = 1;
    }
    /* TODO: spec says if ImageMask is specified, and BPC is specified, then BPC must be 1
       Should we flag an error if this is violated?
     */

    /* Optional (apparently there is no "M" abbreviation for "Mask"? */
    code = pdfi_dict_get(ctx, image_dict, "Mask", &info->Mask);
    if (code < 0) {
        /* A lack of a Mask is not an error. If there is a genuine error reading
         * the Mask, ignore it unless PDFSTOPONWARNING is set. We can still render
         * the image. Arguably we should not, and Acrobat doesn't, but the current
         * GS implementation does.
         */
        if (code != gs_error_undefined) {
           ctx->pdf_warnings |= W_PDF_BAD_IMAGEDICT;
           if (ctx->pdfstoponwarning)
                goto errorExit;
        }
    }

    /* Optional (apparently there is no abbreviation for "SMask"? */
    code = pdfi_dict_get(ctx, image_dict, "SMask", &info->SMask);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    } else {
        if (info->SMask->type == PDF_NAME) {
            pdf_obj *o = NULL;

            code = pdfi_find_resource(ctx, (unsigned char *)"ExtGState", (pdf_name *)info->SMask, image_dict, page_dict, &o);
            if (code >= 0) {
                pdfi_countdown(info->SMask);
                info->SMask = o;
            }
        }

        if (info->SMask->type != PDF_DICT){
            pdfi_countdown(info->SMask);
            info->SMask = NULL;
        }
    }

    /* Optional, for JPXDecode filter images
     * (If non-zero, then SMask shouldn't be  specified)
     * Default: 0
     */
    code = pdfi_dict_get_int(ctx, image_dict, "SMaskInData", &info->SMaskInData);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
        info->SMaskInData = 0;
    }

    /* Optional (Required except for ImageMask, not allowed for ImageMask)*/
    /* TODO: Should we enforce this required/not allowed thing? */
    code = pdfi_dict_get2(ctx, image_dict, "ColorSpace", "CS", &info->ColorSpace);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    }

    /* Optional (default is to use from graphics state) */
    /* (no abbreviation for inline) */
    code = pdfi_dict_get_type(ctx, image_dict, "Intent", PDF_NAME, (pdf_obj **)&info->Intent);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    }

    /* Optional (array of alternate image dicts, can't be nested) */
    code = pdfi_dict_get(ctx, image_dict, "Alternates", &info->Alternates);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    }

    /* Optional (required in PDF1.0, obsolete, do we support?) */
    code = pdfi_dict_get(ctx, image_dict, "Name", &info->Name);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    }

    /* Required "if image is structural content item" */
    /* TODO: Figure out what to do here */
    code = pdfi_dict_get_int(ctx, image_dict, "StructParent", &info->StructParent);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    }

    /* Optional (default is probably [0,1] per component) */
    code = pdfi_dict_get2(ctx, image_dict, "Decode", "D", &info->Decode);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    }

    /* Optional "Optional Content" */
    code = pdfi_dict_get_type(ctx, image_dict, "OC", PDF_DICT, (pdf_obj **)&info->OC);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    }

    /* Optional */
    code = pdfi_dict_get2(ctx, image_dict, "Filter", "F", &info->Filter);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    }

    /* Check and set JPXDecode flag for later */
    info->is_JPXDecode = false;
    if (info->Filter && info->Filter->type == PDF_NAME) {
        if (pdfi_name_is((pdf_name *)info->Filter, "JPXDecode"))
            info->is_JPXDecode = true;
    }

    /* Optional */
    code = pdfi_dict_get2(ctx, image_dict, "DecodeParms", "DP", &info->DecodeParms);
    if (code < 0) {
        if (code != gs_error_undefined)
            goto errorExit;
    }

    return 0;

 errorExit:
    pdfi_free_image_info_components(info);
    return code;
}

static int pdfi_check_inline_image_keys(pdf_context *ctx, pdf_dict *image_dict)
{
    bool known = false;

    pdfi_dict_known(image_dict, "BPC", &known);
    if (known)
        goto error_inline_check;
    pdfi_dict_known(image_dict, "CS", &known);
    if (known)
        goto error_inline_check;
    pdfi_dict_known(image_dict, "D", &known);
    if (known)
        goto error_inline_check;
    pdfi_dict_known(image_dict, "DP", &known);
    if (known)
        goto error_inline_check;
    pdfi_dict_known(image_dict, "F", &known);
    if (known)
        goto error_inline_check;
    pdfi_dict_known(image_dict, "H", &known);
    if (known)
        goto error_inline_check;
    pdfi_dict_known(image_dict, "IM", &known);
    if (known)
        goto error_inline_check;
    pdfi_dict_known(image_dict, "I", &known);
    if (known)
        goto error_inline_check;
    pdfi_dict_known(image_dict, "W", &known);
    if (known)
        goto error_inline_check;

    return 0;

error_inline_check:
    ctx->pdf_warnings |= W_PDF_BAD_INLINEIMAGEKEY;
    if (ctx->pdfstoponwarning)
        return_error(gs_error_syntaxerror);
    return 0;
}

/* Render a PDF image
 * pim can be type1 (or imagemask), type3, type4
 */
static int
pdfi_render_image(pdf_context *ctx, gs_pixel_image_t *pim, pdf_stream *image_stream,
                  unsigned char *mask_buffer, uint64_t mask_size,
                  int comps, bool ImageMask)
{
    int code;
    gs_image_enum *penum = NULL;
    //    unsigned char Buffer[1024];
    byte *buffer = NULL;
    uint64_t linelen, bytes_left;
    uint64_t bytes_used = 0;
    uint64_t bytes_avail = 0;
    gs_const_string plane_data[GS_IMAGE_MAX_COMPONENTS];
    int main_plane, mask_plane;

#if DEBUG_IMAGES
    dbgmprintf(ctx->memory, "pdfi_render_image BEGIN\n");
#endif
    code = pdfi_trans_set_params(ctx);
    if (code < 0)
        return code;

    code = pdfi_gsave(ctx);
    if (code < 0)
        return code;

    penum = gs_image_enum_alloc(ctx->memory, "pdfi_render_image (gs_image_enum_alloc)");
    if (!penum) {
        code = gs_note_error(gs_error_VMerror);
        goto cleanupExit;
    }

    /* Took this logic from gs_image_init()
     * (the other tests in there have already been handled elsewhere)
     */
    {
        gx_image_enum_common_t *pie;

        if (!ImageMask) {
            /* TODO: Can in_cachedevice ever be set in PDF? */
            if (ctx->pgs->in_cachedevice != CACHE_DEVICE_NONE) {
                code = gs_note_error(gs_error_undefined);
                goto cleanupExit;
            }
        }

        code = gs_image_begin_typed((const gs_image_common_t *)pim, ctx->pgs, ImageMask, false, &pie);
        if (code < 0)
            goto cleanupExit;

        code = gs_image_enum_init(penum, pie, (const gs_data_image_t *)pim, ctx->pgs);
        if (code < 0)
            goto cleanupExit;
    }

    /* NOTE: I used image_file_continue() as my template for this code.
     * But this case is (hopefully) much much simpler.
     * We only handle two situations -- if there is mask_data, then we assume there are two planes.
     * If no mask_data, then there is one plane.
     */
    if (mask_buffer) {
        main_plane = 1;
        mask_plane = 0;
        plane_data[mask_plane].data = mask_buffer;
        plane_data[mask_plane].size = mask_size;
    } else {
        main_plane = 0;
    }

    /* Going to feed the data one line at a time.
     * This isn't required by gs_image_next_planes(), but it might make things simpler.
     */
    linelen = pdfi_get_image_line_size((gs_data_image_t *)pim, comps);
    bytes_left = pdfi_get_image_data_size((gs_data_image_t *)pim, comps);
    buffer = gs_alloc_bytes(ctx->memory, linelen, "pdfi_render_image (buffer)");
    if (!buffer) {
        code = gs_note_error(gs_error_VMerror);
        goto cleanupExit;
    }
    while (bytes_left > 0) {
        uint used[GS_IMAGE_MAX_COMPONENTS];

        if (bytes_avail == 0) {
            code = pdfi_read_bytes(ctx, buffer, 1, linelen, image_stream);
            if (code < 0) {
                dmprintf3(ctx->memory,
                          "WARNING: Image data error (pdfi_read_bytes) bytes_left=%ld, linelen=%ld, code=%d\n",
                          bytes_left, linelen, code);
                goto cleanupExit;
            }
            if (code != linelen) {
                dmprintf3(ctx->memory, "WARNING: Image data mismatch, bytes_left=%ld, linelen=%ld, code=%d\n",
                          bytes_left, linelen, code);
                code = gs_note_error(gs_error_limitcheck);
                goto cleanupExit;
            }
        }

        plane_data[main_plane].data = buffer + bytes_used;
        plane_data[main_plane].size = linelen - bytes_used;

        code = gs_image_next_planes(penum, plane_data, used);
        if (code < 0) {
            goto cleanupExit;
        }
        /* It might not always consume all the data, but so far the only case
         * I have seen with that was one that had mask data.
         * In that case, it used all of plane 0, and none of plane 1 on the first pass.
         * (image_2bpp.pdf)
         *
         * Anyway, this math should handle that case (as well as a case where it consumed only
         * part of the data, if that can actually happen).
         */
        bytes_used = used[main_plane];
        bytes_left -= bytes_used;
        bytes_avail = linelen - bytes_used;
    }

    code = 0;

 cleanupExit:
    if (buffer)
        gs_free_object(ctx->memory, buffer, "pdfi_render_image (buffer)");
    if (penum)
        gs_image_cleanup_and_free_enum(penum, ctx->pgs);
    pdfi_grestore(ctx);
#if DEBUG_IMAGES
    dbgmprintf(ctx->memory, "pdfi_render_image END\n");
#endif
    return code;
}

/* Load up params common to the different image types */
static int
pdfi_data_image_params(pdf_context *ctx, pdfi_image_info_t *info,
                       gs_data_image_t *pim, int comps, gs_color_space *pcs)
{
    int code;

    pim->BitsPerComponent = info->BPC;
    pim->Width = info->Width;
    pim->Height = info->Height;
    pim->ImageMatrix.xx = (float)info->Width;
    pim->ImageMatrix.yy = (float)(info->Height * -1);
    pim->ImageMatrix.ty = (float)info->Height;

    pim->Interpolate = info->Interpolate;

    /* Get the decode array (required for ImageMask, probably for everything) */
    if (info->Decode) {
        pdf_array *decode_array = (pdf_array *)info->Decode;
        int i;
        double num;

        if (pdfi_array_size(decode_array) > GS_IMAGE_MAX_COMPONENTS * 2) {
            code = gs_note_error(gs_error_limitcheck);
            goto cleanupExit;
        }

        for (i=0; i<pdfi_array_size(decode_array); i++) {
            code = pdfi_array_get_number(ctx, decode_array, i, &num);
            if (code < 0)
                goto cleanupExit;
            pim->Decode[i] = (float)num;
        }
    } else {
        /* Provide a default if not specified [0 1 ...] per component */
        int i;
        float minval, maxval;

        /* TODO: Is there a less hacky way to identify Indexed case? */
        if (pcs && pcs->type == &gs_color_space_type_Indexed) {
            /* Default value is [0,N], where N=2^n-1, our hival (which depends on BPC)*/
            minval = 0.0;
            maxval = (float)((1 << info->BPC) - 1);
        } else {
            minval = 0.0;
            maxval = 1.0;
        }
        for (i=0; i<comps*2; i+=2) {
            pim->Decode[i] = minval;
            pim->Decode[i+1] = maxval;
        }
    }
    code = 0;

 cleanupExit:
    return code;
}

/* Returns number of components in Matte array or 0 if not found, <0 if error */
static int
pdfi_image_get_matte(pdf_context *ctx, pdf_dict *smask_dict, float *vals, int size)
{
    int i;
    pdf_array *Matte = NULL;
    int code;
    double f;

    code = pdfi_dict_knownget_type(ctx, smask_dict, "Matte",
                                   PDF_ARRAY, (pdf_obj **)&Matte);
    if (code <= 0)
        goto exit;

    if (pdfi_array_size(Matte) > size) {
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    for (i = 0; i < pdfi_array_size(Matte); i++) {
        code = pdfi_array_get_number(ctx, Matte, (uint64_t)i, &f);
        if (code < 0)
            goto exit;
        vals[i] = (float)f;
    }
    if (i == pdfi_array_size(Matte))
        code = i;

 exit:
    pdfi_countdown(Matte);
    return code;
}

/* See ztrans.c/zbegintransparencymaskimage() and pdf_draw.ps/doimagesmask */
static int
pdfi_do_image_smask(pdf_context *ctx, pdf_stream *source, pdfi_image_info_t *image_info)
{
    gs_rect bbox = { { 0, 0} , { 1, 1} };
    gs_transparency_mask_params_t params;
    gs_offset_t savedoffset = 0;
    int code, code1;
    pdfi_int_gstate *igs = (pdfi_int_gstate *)ctx->pgs->client_data;

#if DEBUG_IMAGES
    dbgmprintf(ctx->memory, "pdfi_do_image_smask BEGIN\n");
#endif

    gs_trans_mask_params_init(&params, TRANSPARENCY_MASK_Luminosity);

    code = pdfi_image_get_matte(ctx, (pdf_dict *)image_info->SMask, params.Matte,
                                GS_CLIENT_COLOR_MAX_COMPONENTS);
    if (code >= 0)
        params.Matte_components = code;

    //        params.image_with_SMask = true; /* TODO: Not sure... */

    code = gs_begin_transparency_mask(ctx->pgs, &params, &bbox, true);
    if (code < 0)
        goto exit;
    savedoffset = pdfi_tell(ctx->main_stream);
    code = pdfi_gsave(ctx);

    /* Disable SMask for inner image */
    pdfi_gstate_smask_free(igs);

    gs_setstrokeconstantalpha(ctx->pgs, 1.0);
    gs_setfillconstantalpha(ctx->pgs, 1.0);
    gs_setblendmode(ctx->pgs, BLEND_MODE_Compatible);

    pdfi_seek(ctx, ctx->main_stream, ((pdf_dict *)image_info->SMask)->stream_offset, SEEK_SET);
    code = pdfi_do_image_or_form(ctx, image_info->stream_dict,
                                 image_info->page_dict, (pdf_dict *)image_info->SMask);
    pdfi_seek(ctx, ctx->main_stream, savedoffset, SEEK_SET);

    code1 = pdfi_grestore(ctx);
    if (code < 0)
        code = code1;
    code1 = gs_end_transparency_mask(ctx->pgs, TRANSPARENCY_CHANNEL_Opacity);
    if (code < 0)
        code = code1;

 exit:
#if DEBUG_IMAGES
    dbgmprintf(ctx->memory, "pdfi_do_image_smask END\n");
#endif
    return code;
}


/* Setup for transparency (see pdf_draw.ps/doimage) */
static int
pdfi_image_setup_trans(pdf_context *ctx, pdfi_trans_state_t *state)
{
    int code;

    code = pdfi_gsave(ctx);
    if (code < 0)
        return code;
    code = gs_moveto(ctx->pgs, 1.0, 1.0);
    if (code < 0)
        goto exit;
    code = gs_lineto(ctx->pgs, 0., 0.);
    if (code < 0)
        goto exit;
    code = pdfi_trans_setup(ctx, state, TRANSPARENCY_Caller_Image);
 exit:
    pdfi_grestore(ctx);
    return code;
}


/* Setup a type 4 image, particularly the MaskColor array.
 * Handles error situations like pdf_draw.ps/makemaskimage
 */
static int
pdfi_image_setup_type4(pdf_context *ctx, pdfi_image_info_t *image_info,
                       gs_image4_t *t4image,
                       pdf_array *mask_array, gs_color_space *pcs)
{
    int i;
    double num;
    int code = 0;
    int bpc = image_info->BPC;
    uint mask = (1 << bpc) - 1;
    uint maxval = mask;
    int64_t intval;
    bool had_range_error = false;
    bool had_float_error = false;
    /* Check for special case of Indexed and BPC=1 (to match AR)
     * See bugs: 692852, 697919, 689717
     */
    bool indexed_case = (pcs && pcs->type == &gs_color_space_type_Indexed && bpc == 1);

    memset(t4image, 0, sizeof(t4image));
    gs_image4_t_init(t4image, NULL);

    if (pdfi_array_size(mask_array) > GS_IMAGE_MAX_COMPONENTS * 2) {
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    for (i=0; i<pdfi_array_size(mask_array); i++) {
        code = pdfi_array_get_int(ctx, mask_array, i, &intval);
        if (code == gs_error_typecheck) {
            code = pdfi_array_get_number(ctx, mask_array, i, &num);
            if (code == 0) {
                intval = (int64_t)(num + 0.5);
                had_float_error = true;
            }
        }
        if (code < 0)
            goto exit;
        if (intval > maxval) {
            had_range_error = true;
            if (indexed_case) {
                if (i == 0) {
                    /* If first component is invalid, AR9 ignores the mask. */
                    code = gs_note_error(gs_error_rangecheck);
                    goto exit;
                } else {
                    /* If second component is invalid, AR9 replace it with 1. */
                    intval = 1;
                }
            } else {
                if (bpc != 1) {
                    /* If not special handling, just mask it off to be in range */
                    intval &= mask;
                }
            }
        }
        t4image->MaskColor[i] = intval;
    }
    t4image->MaskColor_is_range = true;

    /* Another special handling (see Bug701468)
     * If 1 BPC and the two entries are not the same, ignore the mask
     */
    if (!indexed_case && bpc == 1 && had_range_error) {
        if (t4image->MaskColor[0] != t4image->MaskColor[1]) {
            code = gs_note_error(gs_error_rangecheck);
            goto exit;
        } else {
            t4image->MaskColor[0] &= mask;
            t4image->MaskColor[1] &= mask;
        }
    }

    code = 0;

 exit:
    if (had_float_error) {
        dmprintf(ctx->memory, "*** Error: Some elements of Mask array are not integers\n");
        ctx->pdf_warnings |= W_PDF_IMAGE_ERROR;
    }
    if (had_range_error) {
        dmprintf(ctx->memory, "*** Error: Some elements of Mask array are out of range\n");
        ctx->pdf_warnings |= W_PDF_IMAGE_ERROR;
    }
    return code;
}

/* Setup a type 3x image
 */
static int
pdfi_image_setup_type3x(pdf_context *ctx, pdfi_image_info_t *image_info,
                        gs_image3x_t *t3ximage, pdfi_image_info_t *smask_info, int comps)
{
    int code = 0;
    gs_image3x_mask_t *mask;

    memset(t3ximage, 0, sizeof(*t3ximage));
    gs_image3x_t_init(t3ximage, NULL);
    if (gs_getalphaisshape(ctx->pgs))
        mask = &t3ximage->Shape;
    else
        mask = &t3ximage->Opacity;
    mask->InterleaveType = 3;

    code = pdfi_image_get_matte(ctx, (pdf_dict *)image_info->SMask, mask->Matte,
                                GS_CLIENT_COLOR_MAX_COMPONENTS);
    if (code > 0)
        mask->has_Matte = true;

    code = pdfi_data_image_params(ctx, smask_info, &mask->MaskDict, comps, NULL);
    if (code < 0)
        goto exit;
 exit:
    return code;
}

static int
pdfi_image_get_color(pdf_context *ctx, pdf_stream *source, pdfi_image_info_t *image_info,
                     int *comps, gs_color_space **pcs)
{
    int code = 0;
    pdfi_jpx_info_t *jpx_info = &image_info->jpx_info;
    pdf_obj *ColorSpace = NULL;

    /* NOTE: Spec says ImageMask and ColorSpace mutually exclusive */
    if (image_info->ImageMask) {
        *comps = 1;
        *pcs = NULL;
        return 0;
    }

    ColorSpace = image_info->ColorSpace;
    if (ColorSpace)
        pdfi_countup(ColorSpace);
    if (ColorSpace == NULL) {
        if (image_info->is_JPXDecode) {
            /* The graphics library doesn't support 12-bit images, so the openjpeg layer
             * (see sjpx_openjpeg.c/decode_image()) is going to translate the 12-bits up to 16-bits.
             * That means we just treat it as 16-bit when rendering, so force the value
             * to 16 here.
             */
            if (jpx_info->bpc == 12) {
                jpx_info->bpc = 16;
            }
            image_info->BPC = jpx_info->bpc;

            if (jpx_info->iccbased) {
                int dummy; /* Holds number of components read from the ICC profile, we ignore this here */

                code = pdfi_create_icc_colorspace_from_stream(ctx, source, jpx_info->icc_offset,
                                                              jpx_info->icc_length, jpx_info->comps, &dummy,
                                                              pcs);
                if (code < 0) {
                    dmprintf2(ctx->memory,
                              "WARNING JPXDecode: Error setting icc colorspace (offset=%d,len=%d)\n",
                              jpx_info->icc_offset, jpx_info->icc_length);
                    goto cleanupExit;
                }
                *comps = gs_color_space_num_components(*pcs);
                goto cleanupExit;
            } else {
                char *color_str;

                /* TODO: Hackity BS here, just trying to pull out a reasonable color for now */
                switch(jpx_info->cs_enum) {
                case 12:
                    color_str = (char *)"DeviceCMYK";
                    break;
                case 16:
                case 18:
                    color_str = (char *)"DeviceRGB";
                    break;
                case 17:
                    color_str = (char *)"DeviceGray";
                    break;
                case 20:
                case 24:
                    /* TODO: gs Implementation assumes these are DeviceRGB.
                     * We can do same and get matching output (but is it correct?)
                     * (should probably look at num comps, but gs code doesn't)
                     */
                    if (ctx->pdfdebug)
                        dmprintf1(ctx->memory,
                                  "WARNING JPXDecode: Unsupported EnumCS %d, assuming DeviceRGB\n",
                                  jpx_info->cs_enum);
                    color_str = (char *)"DeviceRGB";
                    break;
                default:
                    dmprintf1(ctx->memory,
                              "WARNING JPXDecode: Unsupported EnumCS %d\n", jpx_info->cs_enum);
                    goto cleanupExit;
                }

                /* Make a ColorSpace for the name */
                code = pdfi_make_name(ctx, (byte *)color_str, strlen(color_str), &ColorSpace);
                if (code < 0)
                    goto cleanupExit;
            }
        } else {
            /* Assume DeviceRGB colorspace */
            dmprintf(ctx->memory, "**** Error: image has no /ColorSpace key; assuming /DeviceRGB.\n");
            ctx->pdf_warnings |= W_PDF_BAD_IMAGEDICT;
            code = pdfi_make_name(ctx, (byte *)"DeviceRGB", strlen("DeviceRGB"), &ColorSpace);
            if (code < 0)
                goto cleanupExit;
        }
    }

    /* At this point ColorSpace is either a string we just made, or the one from the Image */
    code = pdfi_create_colorspace(ctx, ColorSpace,
                                  image_info->page_dict, image_info->stream_dict,
                                  pcs, image_info->inline_image);
    /* TODO: image_2bpp.pdf has an image in there somewhere that fails on this call (probably ColorN) */
    if (code < 0) {
        dmprintf(ctx->memory, "WARNING: Image has unsupported ColorSpace ");
        if (image_info->ColorSpace->type == PDF_NAME) {
            pdf_name *name = (pdf_name *)image_info->ColorSpace;
            char str[100];
            memcpy(str, (const char *)name->data, name->length);
            str[name->length] = '\0';
            dmprintf1(ctx->memory, "NAME:%s\n", str);
        } else {
            dmprintf(ctx->memory, "(not a name)\n");
        }
        goto cleanupExit;
    }
    *comps = gs_color_space_num_components(*pcs);

 cleanupExit:
    pdfi_countdown(ColorSpace);
    return code;
}

/* Make a fake SMask dict from a JPX SMaskInData */
static int
pdfi_make_smask_dict(pdf_context *ctx, pdf_dict *image_dict, pdfi_image_info_t *image_info,
                     int comps)
{
    int code = 0;
    pdf_dict *smask_dict = NULL;
    pdf_array *array = NULL;
    pdf_array *matte = NULL;

    if (image_info->SMask != NULL) {
        dmprintf(ctx->memory, "ERROR SMaskInData when there is already an SMask?\n");
        goto exit;
    }

    code = pdfi_alloc_object(ctx, PDF_DICT, 32, (pdf_obj **)&smask_dict);
    if (code < 0) goto exit;
    pdfi_countup(&smask_dict);

    /* Copy everything from the image_dict */
    code = pdfi_dict_copy(smask_dict, image_dict);
    smask_dict->stream_offset = image_dict->stream_offset;

    code = pdfi_dict_put_int(ctx, smask_dict, "SMaskInData", 0);
    if (code < 0) goto exit;

    code = pdfi_dict_put_name(ctx, smask_dict, "ColorSpace", "DeviceGray");
    if (code < 0) goto exit;

    /* BPC needs to come from the jpxinfo */
    code = pdfi_dict_put_int(ctx, smask_dict, "BitsPerComponent", image_info->jpx_info.bpc);
    if (code < 0) goto exit;

    /* "Alpha" is a non-standard thing that gs code uses to tell
     * the jpxfilter that it is doing an SMask.  I guess we can do
     * the same, since we're making this dictionary anyway.
     */
    code = pdfi_dict_put_bool(ctx, smask_dict, "Alpha", true);
    if (code < 0) goto exit;

    /* Make an array [0,1] */
    code = pdfi_array_alloc(ctx, 2, &array);
    if (code < 0) goto exit;
    pdfi_countup(array);
    code = pdfi_array_put_int(ctx, array, 0, 0);
    if (code < 0) goto exit;
    code = pdfi_array_put_int(ctx, array, 1, 1);
    if (code < 0) goto exit;
    code = pdfi_dict_put(ctx, smask_dict, "Decode", (pdf_obj *)array);
    if (code < 0) goto exit;

    /* Make Matte array if needed */
    /* This just makes an array [0,0,0...] of size 'comps'
     * See pdf_draw.ps/makeimagekeys
     * TODO: The only sample in our test suite that triggers this path is fts_17_1718.pdf
     * and this code being there or not makes no difference on that sample, so.. ???
     */
    if (image_info->SMaskInData == 2) {
        int i;
        code = pdfi_array_alloc(ctx, comps, &matte);
        if (code < 0) goto exit;
        pdfi_countup(matte);
        for (i=0; i<comps; i++) {
            code = pdfi_array_put_int(ctx, matte, i, 0);
            if (code < 0) goto exit;
        }
        code = pdfi_dict_put(ctx, smask_dict, "Matte", (pdf_obj *)matte);
        if (code < 0) goto exit;
    }

    image_info->SMask = (pdf_obj *)smask_dict;

 exit:
    if (code < 0)
        pdfi_countdown(smask_dict);
    pdfi_countdown(array);
    pdfi_countdown(matte);
    return code;
}

/* NOTE: "source" is the current input stream.
 * on exit:
 *  inline_image = TRUE, stream it will point to after the image data.
 *  inline_image = FALSE, stream position undefined.
 */
static int
pdfi_do_image(pdf_context *ctx, pdf_dict *page_dict, pdf_dict *stream_dict, pdf_dict *image_dict,
              pdf_stream *source, bool inline_image)
{
    pdf_stream *new_stream = NULL;
    int code = 0, code1 = 0;
    int comps = 0;
    gs_color_space  *pcs = NULL;
    gs_image1_t t1image;
    gs_image4_t t4image;
    gs_image3_t t3image;
    gs_image3x_t t3ximage;
    gs_pixel_image_t *pim = NULL;
    pdf_dict *alt_dict = NULL;
    pdfi_image_info_t image_info, mask_info, smask_info;
    pdf_dict *mask_dict = NULL;
    pdf_dict *smask_dict = NULL; /* only non-null for imagetype 3x (PreserveSMask) */
    pdf_array *mask_array = NULL;
    unsigned char *mask_buffer = NULL;
    uint64_t mask_size = 0;
    pdfi_int_gstate *igs = (pdfi_int_gstate *)ctx->pgs->client_data;
    bool transparency_group = false;
    bool need_smask_cleanup = false;
    bool maybe_jpxdecode = false;
    pdfi_trans_state_t trans_state;
    int saved_intent;

#if DEBUG_IMAGES
    dbgmprintf(ctx->memory, "pdfi_do_image BEGIN\n");
#endif
    memset(&mask_info, 0, sizeof(mask_info));

    if (!inline_image) {
        /* If we are not processing an inline image, check to see if any of the abbreviated
         * keys are present in the image dictionary. If they are, and we need to abort, we'll
         * get an error return, otherwise we can continue.
         */
        code = pdfi_check_inline_image_keys(ctx, image_dict);
        if (code < 0)
            return code;
    }

    /* Save current rendering intent so we can put it back if it is modified */
    saved_intent = gs_currentrenderingintent(ctx->pgs);

    code = pdfi_get_image_info(ctx, image_dict, page_dict, stream_dict, inline_image, &image_info);
    if (code < 0)
        goto cleanupExit;

    /* Don't render this if turned off */
    if (pdfi_oc_is_off(ctx))
        goto cleanupExit;
    /* If there is an OC dictionary, see if we even need to render this */
    if (image_info.OC) {
        if (!pdfi_oc_is_ocg_visible(ctx, image_info.OC))
            goto cleanupExit;
    }

    /* If there is an alternate, swap it in */
    /* If image_info.Alternates, look in the array, see if any of them are flagged as "DefaultForPrinting"
     * and if so, substitute that one for the image we are processing.
     * (it can probably be either an array, or a reference to an array, need an example to test/implement)
     * see p.274 of PDFReference.pdf
     */

    if (image_info.Alternates != NULL) {
        alt_dict = pdfi_find_alternate(ctx, image_info.Alternates);
        if (alt_dict != NULL) {
            image_dict = alt_dict;
            pdfi_free_image_info_components(&image_info);
            code = pdfi_get_image_info(ctx, image_dict, page_dict, stream_dict, inline_image, &image_info);
            if (code < 0)
                goto cleanupExit;
        }
    }

    /* See if it might be a JPXDecode image even though not in the header */
    if (image_info.ColorSpace == NULL && !image_info.ImageMask)
        maybe_jpxdecode = true;

    /* Handle JPXDecode filter pre-scan of header */
    if ((maybe_jpxdecode || image_info.is_JPXDecode) && !inline_image) {
        pdfi_seek(ctx, source, image_dict->stream_offset, SEEK_SET);
        code = pdfi_scan_jpxfilter(ctx, source, image_info.Length, &image_info.jpx_info);
        if (code < 0 && image_info.is_JPXDecode)
            goto cleanupExit;
        if (code == 0 && maybe_jpxdecode)
            image_info.is_JPXDecode = true;
    }

    /* Set the rendering intent if applicable */
    if (image_info.Intent) {
        code = pdfi_setrenderingintent(ctx, image_info.Intent);
        if (code < 0) {
            /* TODO: Flag a warning on this?  Sample fts_17_1706.pdf has misspelled Intent
               which gs renders without flagging an error */
#if DEBUG_IMAGES
            dbgmprintf(ctx->memory, "WARNING: Image with unexpected Intent\n");
#endif
        }
    }

    /* Get the color for this image */
    code = pdfi_image_get_color(ctx, source, &image_info, &comps, &pcs);
    if (code < 0)
        goto cleanupExit;

    /* Set the colorspace */
    if (pcs) {
        code = pdfi_gs_setcolorspace(ctx, pcs);
        if (code < 0)
            goto cleanupExit;
    }

    /* Make a fake SMask dict if needed for JPXDecode */
    if (ctx->page_has_transparency && image_info.is_JPXDecode && image_info.SMaskInData != 0) {
        code = pdfi_make_smask_dict(ctx, image_dict, &image_info, comps);
        if (code < 0)
            goto cleanupExit;
    }

    if (ctx->page_has_transparency == true && image_info.SMask != NULL) {
        /* If this flag is set, then device will process the SMask and we need do nothing
         * here (e.g. pdfwrite).
         */
        if (!ctx->preserve_smask) {
            code = pdfi_do_image_smask(ctx, source, &image_info);
            if (code < 0)
                goto cleanupExit;
            need_smask_cleanup = true;
        }
        code = pdfi_trans_begin_isolated_group(ctx, true);
        if (code < 0)
            goto cleanupExit;
        transparency_group = true;
    } else if (igs->SMask) {
        code = pdfi_trans_begin_isolated_group(ctx, false);
        if (code < 0)
            goto cleanupExit;
        transparency_group = true;
    }

    /* Get the Mask data either as an array or a dict, if present */
    if (image_info.SMask == NULL && image_info.Mask != NULL) {
        if (image_info.Mask->type == PDF_ARRAY) {
            mask_array = (pdf_array *)image_info.Mask;
        } else if (image_info.Mask->type == PDF_DICT) {
            mask_dict = (pdf_dict *)image_info.Mask;
            code = pdfi_get_image_info(ctx, mask_dict, page_dict, stream_dict, inline_image, &mask_info);
            if (code < 0)
                goto cleanupExit;
        } else {
            code = gs_note_error(gs_error_typecheck);
            goto cleanupExit;
        }
    }

    /* Get the SMask info if we will need it (Type 3x images) */
    if (image_info.SMask && ctx->preserve_smask) {
        /* smask_dict non-NULL is used to flag a Type 3x image below */
        smask_dict = (pdf_dict *)image_info.SMask;
        code = pdfi_get_image_info(ctx, smask_dict, page_dict, stream_dict, inline_image, &smask_info);
        if (code < 0)
            goto cleanupExit;
    }

    /* Get the image into a supported gs type (type1, type3, type4, type3x) */
    if (!image_info.Mask && !smask_dict) { /* Type 1 and ImageMask */
        memset(&t1image, 0, sizeof(t1image));
        pim = (gs_pixel_image_t *)&t1image;

        if (image_info.ImageMask) {
            /* Sets up timage.ImageMask, amongst other things */
            gs_image_t_init_adjust(&t1image, NULL, false);
        } else {
            gs_image_t_init_adjust(&t1image, pcs, true);
        }
    } else if (smask_dict) { /* Type 3x */
        code = pdfi_image_setup_type3x(ctx, &image_info, &t3ximage, &smask_info, comps);
        if (code < 0) {
            /* If this got an error, setup as a Type 1 image */
            /* NOTE: I did this error-handling the same as for Type 4 image below.
             * Dunno if it's better to do this or to just abort the whole image?
             */
            memset(&t1image, 0, sizeof(t1image));
            pim = (gs_pixel_image_t *)&t1image;
            gs_image_t_init_adjust(&t1image, pcs, true);
        } else {
            pim = (gs_pixel_image_t *)&t3ximage;
        }
    } else {
        if (mask_array) { /* Type 4 */
            code = pdfi_image_setup_type4(ctx, &image_info, &t4image, mask_array, pcs);
            if (code < 0) {
                /* If this got an error, setup as a Type 1 image */
                memset(&t1image, 0, sizeof(t1image));
                pim = (gs_pixel_image_t *)&t1image;
                gs_image_t_init_adjust(&t1image, pcs, true);
            } else {
                pim = (gs_pixel_image_t *)&t4image;
            }
        } else { /* Type 3 */
            memset(&t3image, 0, sizeof(t3image));
            pim = (gs_pixel_image_t *)&t3image;
            gs_image3_t_init(&t3image, NULL, interleave_separate_source);
            code = pdfi_data_image_params(ctx, &mask_info, &t3image.MaskDict, 1, NULL);
            if (code < 0)
                goto cleanupExit;
        }
    }


    /* At this point pim points to a structure containing the specific type
     * of image, and then we can handle it generically from here.
     * The underlying gs image functions will do different things for different
     * types of images.
     */

    /* Setup the common params */
    pim->ColorSpace = pcs;
    code = pdfi_data_image_params(ctx, &image_info, (gs_data_image_t *)pim, comps, pcs);
    if (code < 0)
        goto cleanupExit;

    /* Grab the mask_image data buffer in advance.
     * Doing it this way because I don't want to muck with reading from
     * two streams simultaneously -- not even sure that is feasible?
     */
    if (mask_dict) {
        code = pdfi_stream_to_buffer(ctx, mask_dict, &mask_buffer, (int64_t *)&mask_size);
        if (code < 0)
            goto cleanupExit;
    } else if (smask_dict) {
        /* This will happen only in case of PreserveSMask (Type 3x) */
        code = pdfi_stream_to_buffer(ctx, smask_dict, &mask_buffer, (int64_t *)&mask_size);
        if (code < 0)
            goto cleanupExit;
    }

    /* Setup the data stream for the image data */
    if (!inline_image)
        pdfi_seek(ctx, source, image_dict->stream_offset, SEEK_SET);
    code = pdfi_filter(ctx, image_dict, source, &new_stream, inline_image);
    if (code < 0)
        goto cleanupExit;

    code = pdfi_image_setup_trans(ctx, &trans_state);
    if (code < 0)
        goto cleanupExit;

    /* Render the image */
    code = pdfi_render_image(ctx, pim, new_stream,
                             mask_buffer, mask_size,
                             comps, image_info.ImageMask);
    if (code < 0) {
        if (ctx->pdfdebug)
            dmprintf1(ctx->memory, "WARNING: pdfi_do_image: error %d from pdfi_render_image\n", code);
    }

    code1 = pdfi_trans_teardown(ctx, &trans_state);
    if (code == 0)
        code = code1;

 cleanupExit:
    if (code < 0)
        ctx->pdf_warnings |= W_PDF_IMAGE_ERROR;

    code = 0;  /* suppress errors */

    if (transparency_group) {
        pdfi_trans_end_isolated_group(ctx);
        if (need_smask_cleanup)
            pdfi_trans_end_smask_notify(ctx);
    }

    if (new_stream)
        pdfi_close_file(ctx, new_stream);
    if (mask_buffer)
        gs_free_object(ctx->memory, mask_buffer, "pdfi_do_image (mask_buffer)");
    if (alt_dict) {
        pdfi_countdown(alt_dict);
    }

    pdfi_free_image_info_components(&image_info);
    pdfi_free_image_info_components(&mask_info);

    if (pcs != NULL)
        rc_decrement_only_cs(pcs, "pdfi_do_image");

    /* Restore the rendering intent */
    gs_setrenderingintent(ctx->pgs, saved_intent);

#if DEBUG_IMAGES
    dbgmprintf(ctx->memory, "pdfi_do_image END\n");
#endif
    return code;
}

int pdfi_ID(pdf_context *ctx, pdf_dict *stream_dict, pdf_dict *page_dict, pdf_stream *source)
{
    pdf_dict *d = NULL;
    int code;

    if (ctx->TextBlockDepth != 0)
        ctx->pdf_warnings |= W_PDF_OPINVALIDINTEXT;

    /* we want to have the indirect_num and indirect_gen of the created dictionary
     * be 0, because we are reading from a stream, and the stream has already
     * been decrypted, we don't need to decrypt any strings contained in the
     * inline dictionary.
     */
    code = pdfi_dict_from_stack(ctx, 0, 0);
    if (code < 0)
        /* pdfi_dict_from_stack cleans up the stack so we don't need to in case of an error */
        return code;

    d = (pdf_dict *)ctx->stack_top[-1];
    pdfi_countup(d);
    pdfi_pop(ctx, 1);

    code = pdfi_do_image(ctx, stream_dict, page_dict, d, source, true);
    pdfi_countdown(d);
    if (code < 0 && ctx->pdfstoponerror)
        return code;
    return 0;
}

int pdfi_EI(pdf_context *ctx)
{
    if (ctx->TextBlockDepth != 0)
        ctx->pdf_warnings |= W_PDF_OPINVALIDINTEXT;

/*    pdfi_clearstack(ctx);*/
    return 0;
}

/* see .execgroup */
int pdfi_form_execgroup(pdf_context *ctx, pdf_dict *page_dict, pdf_dict *xobject_dict,
                        gs_gstate *GroupGState, gs_matrix *matrix)
{
    int code;
    pdfi_int_gstate *igs = (pdfi_int_gstate *)ctx->pgs->client_data;

    code = pdfi_gsave(ctx);
    if (code < 0)
        goto exit;

    if (GroupGState) {
        code = gs_setgstate(ctx->pgs, GroupGState);
        if (code < 0)
            goto exit2;
    }

    /* Disable the SMask */
    pdfi_gstate_smask_free(igs);

    gs_setblendmode(ctx->pgs, BLEND_MODE_Compatible);
    gs_setstrokeconstantalpha(ctx->pgs, 1.0);
    gs_setfillconstantalpha(ctx->pgs, 1.0);

    if (matrix)
        code = gs_concat(ctx->pgs, matrix);
    if (code < 0) {
        goto exit2;
    }
    code = pdfi_run_context(ctx, xobject_dict, page_dict, false, "FORM");

 exit2:
    if (code != 0)
        (void)pdfi_grestore(ctx);
    else
        code = pdfi_grestore(ctx);
 exit:
    return code;
}

static int pdfi_do_form(pdf_context *ctx, pdf_dict *page_dict, pdf_dict *form_dict)
{
    int code, code1 = 0;
    bool group_known = false;
    bool do_group = false;
    pdf_array *FormMatrix = NULL;
    gs_matrix m;
    gs_rect bbox;
    pdf_array *BBox;

    if (!pdfi_dict_is_stream(ctx, form_dict)) {
        /* See bug #702560. The original file has a Form XObject which is not a stream. Instead
         * the Form XObject has a /Contents key which points to a stream dictionary. This is plainly
         * illegal but, as always, Acrobat can open it....
         * If PDFSTOPONERROR is true then we just exit. Otherwise we look for a /Contents key in the stream
         * dictionary. If we find one we dereference the object to get a stream dictionary, then merge the
         * two dictionaries, ensuring the stream offset is correct, and proceed as if that's what we'd
         * always had. If we don't have a /Contents key then exit with a typecheck error.
         */
        pdf_dict *stream_dict = NULL;

        if (!ctx->pdfstoponerror) {
            code = pdfi_dict_knownget_type(ctx, form_dict, "Contents", PDF_DICT, (pdf_obj **)&stream_dict);
            if (code < 0 || stream_dict == NULL || !pdfi_dict_is_stream(ctx, stream_dict)) {
                pdfi_countdown(stream_dict);
                ctx->pdf_errors |= E_PDF_BADSTREAMDICT;
                return_error(gs_error_typecheck);
            }
            ctx->pdf_warnings |= W_PDF_STREAM_HAS_CONTENTS;
            code = pdfi_merge_dicts(form_dict, stream_dict);
            form_dict->stream_offset = stream_dict->stream_offset;
            pdfi_countdown(stream_dict);
            if (code < 0)
                return code;
        } else {
            ctx->pdf_errors |= E_PDF_BADSTREAMDICT;
            return_error(gs_error_typecheck);
        }
    }

#if DEBUG_IMAGES
    dbgmprintf(ctx->memory, "pdfi_do_form BEGIN\n");
#endif
    code = pdfi_dict_known(form_dict, "Group", &group_known);
    if (code < 0)
        goto exit;
    if (group_known && ctx->page_has_transparency)
        do_group = true;

    code = pdfi_op_q(ctx);
    if (code < 0)
        goto exit1;

    code = pdfi_dict_knownget_type(ctx, form_dict, "Matrix", PDF_ARRAY, (pdf_obj **)&FormMatrix);
    if (code < 0)
        goto exit1;
    code = pdfi_array_to_gs_matrix(ctx, FormMatrix, &m);
    if (code < 0)
        goto exit1;

    code = pdfi_dict_knownget_type(ctx, form_dict, "BBox", PDF_ARRAY, (pdf_obj **)&BBox);
    if (code < 0)
        goto exit1;
    code = pdfi_array_to_gs_rect(ctx, BBox, &bbox);
    if (code < 0)
        goto exit1;

    code = gs_concat(ctx->pgs, &m);
    if (code < 0) {
        goto exit1;
    }

    code = gs_rectclip(ctx->pgs, &bbox, 1);
    if (code < 0)
        goto exit1;

    if (do_group) {
        code = pdfi_loop_detector_mark(ctx);
        if (code < 0)
            goto exit1;

        code = pdfi_trans_begin_form_group(ctx, page_dict, form_dict);
        (void)pdfi_loop_detector_cleartomark(ctx);
        if (code < 0)
            goto exit1;
        code = pdfi_form_execgroup(ctx, page_dict, form_dict, NULL, NULL);
        code1 = pdfi_trans_end_group(ctx);
        if (code < 0)
            code = code1;
    } else {
        bool saved_decrypt_strings = ctx->decrypt_strings;

        /* We can run a Form even when we aren't running a page content stresm,
         * eg for an annotation, and we need to *not* decrypt strings in that
         * case (the content stream will be decrypted and strings in content
         * streams are not additionally encrypted).
         */
        ctx->decrypt_strings = false;
        code = pdfi_interpret_inner_content_stream(ctx, form_dict, page_dict, false, "FORM");
        ctx->decrypt_strings = saved_decrypt_strings;
    }

 exit1:
    if (code != 0)
        (void)pdfi_op_Q(ctx);
    else
        code = pdfi_op_Q(ctx);

 exit:
    pdfi_countdown(FormMatrix);
    pdfi_countdown(BBox);
#if DEBUG_IMAGES
    dbgmprintf(ctx->memory, "pdfi_do_form END\n");
#endif
    if (code < 0)
        return code;
    return 0;
}

int pdfi_do_image_or_form(pdf_context *ctx, pdf_dict *stream_dict,
                                 pdf_dict *page_dict, pdf_dict *xobject_dict)
{
    int code;
    pdf_name *n = NULL;

#if DEBUG_IMAGES
    dbgmprintf1(ctx->memory, "pdfi_do_image_or_form BEGIN (OBJ = %d)\n", xobject_dict->object_num);
#endif
    code = pdfi_trans_set_params(ctx);
    if (code < 0)
        return code;

    code = pdfi_dict_get(ctx, (pdf_dict *)xobject_dict, "Subtype", (pdf_obj **)&n);
    if (code == 0) {
        if (pdfi_name_is(n, "Image")) {
            gs_offset_t savedoffset;

            pdfi_countdown(n);
            savedoffset = pdfi_tell(ctx->main_stream);
            code = pdfi_do_image(ctx, page_dict, stream_dict, xobject_dict, ctx->main_stream, false);
            pdfi_seek(ctx, ctx->main_stream, savedoffset, SEEK_SET);
        } else if (pdfi_name_is(n, "Form")) {
            pdfi_countdown(n);
            code = pdfi_do_form(ctx, page_dict, xobject_dict);
            if (code < 0) {
                return code;
            }
        } else if (pdfi_name_is(n, "PS")) {
            pdfi_countdown(n);
            dmprintf(ctx->memory, "*** WARNING: PostScript XObjects are deprecated (SubType 'PS')\n");
            code = 0; /* Swallow silently */
        } else {
            pdfi_countdown(n);
            code = gs_error_typecheck;
        }
    }
#if DEBUG_IMAGES
    dbgmprintf(ctx->memory, "pdfi_do_image_or_form END\n");
#endif
    return 0;
}

int pdfi_Do(pdf_context *ctx, pdf_dict *stream_dict, pdf_dict *page_dict)
{
    int code = 0;
    pdf_name *n = NULL;
    pdf_obj *o = NULL;
    bool clear_loop_detect = false;

    if (pdfi_count_stack(ctx) < 1) {
        code = gs_note_error(gs_error_stackunderflow);
        goto exit;
    }
    n = (pdf_name *)ctx->stack_top[-1];
    if (n->type != PDF_NAME) {
        code = gs_note_error(gs_error_typecheck);
        goto exit;
    }

    if (ctx->TextBlockDepth != 0)
        ctx->pdf_warnings |= W_PDF_OPINVALIDINTEXT;

    code = pdfi_loop_detector_mark(ctx);
    if (code < 0)
        goto exit;
    clear_loop_detect = true;
    code = pdfi_find_resource(ctx, (unsigned char *)"XObject", n, stream_dict, page_dict, &o);
    if (code < 0)
        goto exit;

    if (o->type != PDF_DICT) {
        code = gs_note_error(gs_error_typecheck);
        goto exit;
    }

    /* NOTE: Used to have a pdfi_gsave/pdfi_grestore around this, but it actually makes
     * things render incorrectly (and isn't in the PS code).
     * It also causes demo.ai.pdf to crash.
     * I don't really understand... (all transparency related, though, so nothing surprises me...)
     * (there are some q/Q and gsave/grestore in the code under this)
     *
     * Original Comment:
     * The image or form might change the colour space (or indeed other aspects
     * of the graphics state, if its a Form XObject. So gsave/grestore round it
     * to prevent unexpected changes.
     */
    //    pdfi_gsave(ctx);
    code = pdfi_do_image_or_form(ctx, stream_dict, page_dict, (pdf_dict *)o);
    //    pdfi_grestore(ctx);
    if (code < 0)
        goto exit;

 exit:
    if (clear_loop_detect) {
        if (code < 0)
            (void)pdfi_loop_detector_cleartomark(ctx);
        else
            code = pdfi_loop_detector_cleartomark(ctx);
    }
    /* No need to countdown 'n' because that points to the stack object, and we're going to pop that */
    pdfi_countdown(o);
    pdfi_pop(ctx, 1);
    if (code < 0 && ctx->pdfstoponerror)
        return code;
    return 0;
}