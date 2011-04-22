/*
 * Copyright © 2009 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Zhou Chang <chang.zhou@intel.com>
 *
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <va/va_backend.h>

#include "intel_batchbuffer.h"
#include "intel_driver.h"

#include "i965_defines.h"
#include "i965_drv_video.h"
#include "gen6_vme.h"

#define VME_INTRA_SHADER	0	
#define VME_INTER_SHADER	1

#define CURBE_ALLOCATION_SIZE   37              /* in 256-bit */
#define CURBE_TOTAL_DATA_LENGTH (4 * 32)        /* in byte, it should be less than or equal to CURBE_ALLOCATION_SIZE * 32 */
#define CURBE_URB_ENTRY_LENGTH  4               /* in 256-bit, it should be less than or equal to CURBE_TOTAL_DATA_LENGTH / 32 */
  
static uint32_t gen6_vme_intra_frame[][4] = {
#include "shaders/vme/intra_frame.g6b"
    {0,0,0,0}
};

static uint32_t gen6_vme_inter_frame[][4] = {
#include "shaders/vme/inter_frame.g6b"
    {0,0,0,0}
};

static struct media_kernel gen6_vme_kernels[] = {
    {
        "VME Intra Frame",
        VME_INTRA_SHADER,										/*index*/
        gen6_vme_intra_frame, 			
        sizeof(gen6_vme_intra_frame),		
        NULL
    },
    {
        "VME inter Frame",
        VME_INTER_SHADER,
        gen6_vme_inter_frame,
        sizeof(gen6_vme_inter_frame),
        NULL
    }
};

#define	GEN6_VME_KERNEL_NUMBER ARRAY_ELEMS(gen6_vme_kernels)

static void
gen6_vme_set_common_surface_tiling(struct i965_surface_state *ss, unsigned int tiling)
{
    switch (tiling) {
    case I915_TILING_NONE:
        ss->ss3.tiled_surface = 0;
        ss->ss3.tile_walk = 0;
        break;
    case I915_TILING_X:
        ss->ss3.tiled_surface = 1;
        ss->ss3.tile_walk = I965_TILEWALK_XMAJOR;
        break;
    case I915_TILING_Y:
        ss->ss3.tiled_surface = 1;
        ss->ss3.tile_walk = I965_TILEWALK_YMAJOR;
        break;
    }
}

static void
gen6_vme_set_source_surface_tiling(struct i965_surface_state2 *ss, unsigned int tiling)
{
    switch (tiling) {
    case I915_TILING_NONE:
        ss->ss2.tiled_surface = 0;
        ss->ss2.tile_walk = 0;
        break;
    case I915_TILING_X:
        ss->ss2.tiled_surface = 1;
        ss->ss2.tile_walk = I965_TILEWALK_XMAJOR;
        break;
    case I915_TILING_Y:
        ss->ss2.tiled_surface = 1;
        ss->ss2.tile_walk = I965_TILEWALK_YMAJOR;
        break;
    }
}

/* only used for VME source surface state */
static void gen6_vme_source_surface_state(VADriverContextP ctx,
                                          int index,
                                          struct object_surface *obj_surface)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);  
    struct gen6_media_state *media_state = &i965->gen6_media_state;
    struct i965_surface_state2 *ss;
    dri_bo *bo;
    int w, h, w_pitch, h_pitch;
    unsigned int tiling, swizzle;

    assert(obj_surface->bo);
    dri_bo_get_tiling(obj_surface->bo, &tiling, &swizzle);

    w = obj_surface->orig_width;
    h = obj_surface->orig_height;
    w_pitch = obj_surface->width;
    h_pitch = obj_surface->height;

    bo = dri_bo_alloc(i965->intel.bufmgr, 
                      "VME surface state", 
                      sizeof(struct i965_surface_state2), 
                      0x1000);
    assert(bo);
    dri_bo_map(bo, 1);
    assert(bo->virtual);
    ss = bo->virtual;
    memset(ss, 0, sizeof(*ss));

    ss->ss0.surface_base_address = obj_surface->bo->offset;

    ss->ss1.cbcr_pixel_offset_v_direction = 2;
    ss->ss1.width = w - 1;
    ss->ss1.height = h - 1;

    ss->ss2.surface_format = MFX_SURFACE_PLANAR_420_8;
    ss->ss2.interleave_chroma = 1;
    ss->ss2.pitch = w_pitch - 1;
    ss->ss2.half_pitch_for_chroma = 0;

    gen6_vme_set_source_surface_tiling(ss, tiling);

    /* UV offset for interleave mode */
    ss->ss3.x_offset_for_cb = 0;
    ss->ss3.y_offset_for_cb = h_pitch;

    dri_bo_unmap(bo);

    dri_bo_emit_reloc(bo,
                      I915_GEM_DOMAIN_RENDER, 0,
                      0,
                      offsetof(struct i965_surface_state2, ss0),
                      obj_surface->bo);

    assert(index < MAX_MEDIA_SURFACES_GEN6);
    media_state->surface_state[index].bo = bo;
}

static void
gen6_vme_media_source_surface_state(VADriverContextP ctx,
                                    int index,
                                    struct object_surface *obj_surface)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);  
    struct gen6_media_state *media_state = &i965->gen6_media_state;
    struct i965_surface_state *ss;
    dri_bo *bo;
    int w, h, w_pitch;
    unsigned int tiling, swizzle;

    w = obj_surface->orig_width;
    h = obj_surface->orig_height;
    w_pitch = obj_surface->width;

    /* Y plane */
    dri_bo_get_tiling(obj_surface->bo, &tiling, &swizzle);
    bo = dri_bo_alloc(i965->intel.bufmgr, 
                      "surface state", 
                      sizeof(struct i965_surface_state), 
                      0x1000);
    assert(bo);

    dri_bo_map(bo, True);
    assert(bo->virtual);
    ss = bo->virtual;
    memset(ss, 0, sizeof(*ss));
    ss->ss0.surface_type = I965_SURFACE_2D;
    ss->ss0.surface_format = I965_SURFACEFORMAT_R8_UNORM;
    ss->ss1.base_addr = obj_surface->bo->offset;
    ss->ss2.width = w / 4 - 1;
    ss->ss2.height = h - 1;
    ss->ss3.pitch = w_pitch - 1;
    gen6_vme_set_common_surface_tiling(ss, tiling);
    dri_bo_emit_reloc(bo,
                      I915_GEM_DOMAIN_RENDER, 
                      0,
                      0,
                      offsetof(struct i965_surface_state, ss1),
                      obj_surface->bo);
    dri_bo_unmap(bo);

    assert(index < MAX_MEDIA_SURFACES_GEN6);
    media_state->surface_state[index].bo = bo;
}

static VAStatus
gen6_vme_output_buffer_setup(VADriverContextP ctx,
                             VAContextID context,
                             struct mfc_encode_state *encode_state,
                             int index)

{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_media_state *media_state = &i965->gen6_media_state;
    struct object_context *obj_context = CONTEXT(context);
    struct i965_surface_state *ss;
    dri_bo *bo;
    int width_in_mbs = ALIGN(obj_context->picture_width, 16) / 16;
    int height_in_mbs = ALIGN(obj_context->picture_height, 16) / 16;
    int num_entries;
    VAEncSliceParameterBuffer *pSliceParameter = (VAEncSliceParameterBuffer *)encode_state->slice_params[0]->buffer;
    int is_intra = pSliceParameter->slice_flags.bits.is_intra;

    if ( is_intra ) {
        media_state->vme_output.num_blocks = width_in_mbs * height_in_mbs;
    } else {
        media_state->vme_output.num_blocks = width_in_mbs * height_in_mbs * 4;
    }
    media_state->vme_output.size_block = 16; /* an OWORD */
    media_state->vme_output.pitch = ALIGN(media_state->vme_output.size_block, 16);
    bo = dri_bo_alloc(i965->intel.bufmgr, 
                      "VME output buffer",
                      media_state->vme_output.num_blocks * media_state->vme_output.pitch,
                      0x1000);
    assert(bo);
    media_state->vme_output.bo = bo;

    bo = dri_bo_alloc(i965->intel.bufmgr, 
                      "VME output buffer state", 
                      sizeof(struct i965_surface_state), 
                      0x1000);
    assert(bo);
    dri_bo_map(bo, 1);
    assert(bo->virtual);
    ss = bo->virtual;
    memset(ss, 0, sizeof(*ss));

    /* always use 16 bytes as pitch on Sandy Bridge */
    num_entries = media_state->vme_output.num_blocks * media_state->vme_output.pitch / 16;
    ss->ss0.render_cache_read_mode = 1;
    ss->ss0.surface_type = I965_SURFACE_BUFFER;
    ss->ss1.base_addr = media_state->vme_output.bo->offset;
    ss->ss2.width = ((num_entries - 1) & 0x7f);
    ss->ss2.height = (((num_entries - 1) >> 7) & 0x1fff);
    ss->ss3.depth = (((num_entries - 1) >> 20) & 0x7f);
    ss->ss3.pitch = media_state->vme_output.pitch - 1;
    dri_bo_emit_reloc(bo,
                      I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                      0,
                      offsetof(struct i965_surface_state, ss1),
                      media_state->vme_output.bo);

    dri_bo_unmap(bo);

    assert(index < MAX_MEDIA_SURFACES_GEN6);
    media_state->surface_state[index].bo = bo;
    return VA_STATUS_SUCCESS;
}

static VAStatus gen6_vme_surface_setup(VADriverContextP ctx, 
                                       VAContextID context,                              
                                       struct mfc_encode_state *encode_state,
                                       int is_intra)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_media_state *media_state = &i965->gen6_media_state;
    struct object_surface *obj_surface;
    unsigned int *binding_table;
    dri_bo *bo = media_state->binding_table.bo;
    int i;
    VAEncPictureParameterBufferH264 *pPicParameter = (VAEncPictureParameterBufferH264 *)encode_state->pic_param->buffer;

    /*Setup surfaces state*/
    /* current picture for encoding */
    obj_surface = SURFACE(encode_state->current_render_target);
    assert(obj_surface);
    gen6_vme_source_surface_state(ctx, 0, obj_surface);
    gen6_vme_media_source_surface_state(ctx, 4, obj_surface);

    if ( ! is_intra ) {
        /* reference 0 */
        obj_surface = SURFACE(pPicParameter->reference_picture);
        assert(obj_surface);
        gen6_vme_source_surface_state(ctx, 1, obj_surface);
        /* reference 1, FIXME: */
        // obj_surface = SURFACE(pPicParameter->reference_picture);
        // assert(obj_surface);
        //gen6_vme_source_surface_state(ctx, 2, obj_surface);
    }

    /* VME output */
    gen6_vme_output_buffer_setup(ctx, context, encode_state, 3);

    /*Building binding table*/
    dri_bo_map(bo, 1); 
    assert(bo->virtual);
    binding_table = bo->virtual;
    memset(binding_table, 0, bo->size);

    for (i = 0; i < MAX_MEDIA_SURFACES_GEN6; i++) {
        if (media_state->surface_state[i].bo) {
            binding_table[i] = media_state->surface_state[i].bo->offset;
            dri_bo_emit_reloc(bo,
                              I915_GEM_DOMAIN_INSTRUCTION, 0,
                              0,  
                              i * sizeof(*binding_table),
                              media_state->surface_state[i].bo);
        }   
    }   

    dri_bo_unmap(media_state->binding_table.bo);

    return VA_STATUS_SUCCESS;
}

static VAStatus gen6_vme_interface_setup(VADriverContextP ctx, 
                                         VAContextID context,                              
                                         struct mfc_encode_state *encode_state)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_media_state *media_state = &i965->gen6_media_state;
    struct gen6_interface_descriptor_data *desc;   
    int i;
    dri_bo *bo;

    bo = media_state->idrt.bo;
    dri_bo_map(bo, 1);
    assert(bo->virtual);
    desc = bo->virtual;

    for (i = 0; i < GEN6_VME_KERNEL_NUMBER; i++) {
        struct media_kernel *kernel;
        kernel = &gen6_vme_kernels[i];
        assert(sizeof(*desc) == 32);
        /*Setup the descritor table*/
        memset(desc, 0, sizeof(*desc));
        desc->desc0.kernel_start_pointer = (kernel->bo->offset >> 6);
        desc->desc2.sampler_count = 1; /* FIXME: */
        desc->desc2.sampler_state_pointer = (media_state->vme_state.bo->offset >> 5);
        desc->desc3.binding_table_entry_count = 1; /* FIXME: */
        desc->desc3.binding_table_pointer = (media_state->binding_table.bo->offset >> 5);
        desc->desc4.constant_urb_entry_read_offset = 0;
        desc->desc4.constant_urb_entry_read_length = CURBE_URB_ENTRY_LENGTH;
 		
        /*kernel start*/
        dri_bo_emit_reloc(bo,	
                          I915_GEM_DOMAIN_INSTRUCTION, 0,
                          0,
                          i * sizeof(*desc) + offsetof(struct gen6_interface_descriptor_data, desc0),
                          kernel->bo);
        /*Sampler State(VME state pointer)*/
        dri_bo_emit_reloc(bo,
                          I915_GEM_DOMAIN_INSTRUCTION, 0,
                          (1 << 2),									//
                          i * sizeof(*desc) + offsetof(struct gen6_interface_descriptor_data, desc2),
                          media_state->vme_state.bo);
        /*binding table*/
        dri_bo_emit_reloc(bo,
                          I915_GEM_DOMAIN_INSTRUCTION, 0,
                          4,									//One Entry
                          i * sizeof(*desc) + offsetof(struct gen6_interface_descriptor_data, desc3),
                          media_state->binding_table.bo);
        desc++;
    }
    dri_bo_unmap(bo);

    return VA_STATUS_SUCCESS;
}

static VAStatus gen6_vme_constant_setup(VADriverContextP ctx, 
                                        VAContextID context,                              
                                        struct mfc_encode_state *encode_state)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_media_state *media_state = &i965->gen6_media_state;
    unsigned char *constant_buffer;

    dri_bo_map(media_state->curbe.bo, 1);
    assert(media_state->curbe.bo->virtual);
    constant_buffer = media_state->curbe.bo->virtual;
	
    /*TODO copy buffer into CURB*/

    dri_bo_unmap( media_state->curbe.bo);

    return VA_STATUS_SUCCESS;
}

static VAStatus gen6_vme_vme_state_setup(VADriverContextP ctx, VAContextID context, struct mfc_encode_state *encode_state, int is_intra)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_media_state *media_state = &i965->gen6_media_state;
    unsigned int *vme_state_message;
    int i;
	
    //building VME state message
    dri_bo_map(media_state->vme_state.bo, 1);
    assert(media_state->vme_state.bo->virtual);
    vme_state_message = (unsigned int *)media_state->vme_state.bo->virtual;
	
    for(i = 0;i < 32; i++) {
        vme_state_message[i] = 0x11;
    }		
    vme_state_message[16] = 0x42424242;			//cost function LUT set 0 for Intra

    dri_bo_unmap( media_state->vme_state.bo);
    return VA_STATUS_SUCCESS;
}

static void gen6_vme_pipeline_select(VADriverContextP ctx)
{
    BEGIN_BATCH(ctx, 1);
    OUT_BATCH(ctx, CMD_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
    ADVANCE_BATCH(ctx);
}

static void gen6_vme_state_base_address(VADriverContextP ctx)
{
    BEGIN_BATCH(ctx, 10);

    OUT_BATCH(ctx, CMD_STATE_BASE_ADDRESS | 8);

    OUT_BATCH(ctx, 0 | BASE_ADDRESS_MODIFY);				//General State Base Address
    OUT_BATCH(ctx, 0 | BASE_ADDRESS_MODIFY);				//Surface State Base Address	
    OUT_BATCH(ctx, 0 | BASE_ADDRESS_MODIFY);				//Dynamic State Base Address
    OUT_BATCH(ctx, 0 | BASE_ADDRESS_MODIFY);				//Indirect Object Base Address
    OUT_BATCH(ctx, 0 | BASE_ADDRESS_MODIFY);				//Instruction Base Address

    OUT_BATCH(ctx, 0xFFFFF000 | BASE_ADDRESS_MODIFY);		//General State Access Upper Bound	
    OUT_BATCH(ctx, 0xFFFFF000 | BASE_ADDRESS_MODIFY);		//Dynamic State Access Upper Bound
    OUT_BATCH(ctx, 0xFFFFF000 | BASE_ADDRESS_MODIFY);		//Indirect Object Access Upper Bound
    OUT_BATCH(ctx, 0xFFFFF000 | BASE_ADDRESS_MODIFY);		//Instruction Access Upper Bound

    /*
      OUT_BATCH(ctx, 0 | BASE_ADDRESS_MODIFY);				//LLC Coherent Base Address
      OUT_BATCH(ctx, 0xFFFFF000 | BASE_ADDRESS_MODIFY );		//LLC Coherent Upper Bound
    */

    ADVANCE_BATCH(ctx);
}

static void gen6_vme_vfe_state(VADriverContextP ctx)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct gen6_media_state *media_state = &i965->gen6_media_state;

    BEGIN_BATCH(ctx, 8);

    OUT_BATCH(ctx, CMD_MEDIA_VFE_STATE | 6);					/*Gen6 CMD_MEDIA_STATE_POINTERS = CMD_MEDIA_STATE */
    OUT_BATCH(ctx, 0);												/*Scratch Space Base Pointer and Space*/
    OUT_BATCH(ctx, (media_state->vfe_state.max_num_threads << 16) 
              | (media_state->vfe_state.num_urb_entries << 8) 
              | (media_state->vfe_state.gpgpu_mode << 2) );	/*Maximum Number of Threads , Number of URB Entries, MEDIA Mode*/
    OUT_BATCH(ctx, 0);												/*Debug: Object ID*/
    OUT_BATCH(ctx, (media_state->vfe_state.urb_entry_size << 16) 
              | media_state->vfe_state.curbe_allocation_size);				/*URB Entry Allocation Size , CURBE Allocation Size*/
    OUT_BATCH(ctx, 0);											/*Disable Scoreboard*/
    OUT_BATCH(ctx, 0);											/*Disable Scoreboard*/
    OUT_BATCH(ctx, 0);											/*Disable Scoreboard*/
	
    ADVANCE_BATCH(ctx);

}

static void gen6_vme_curbe_load(VADriverContextP ctx)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct gen6_media_state *media_state = &i965->gen6_media_state;

    BEGIN_BATCH(ctx, 4);

    OUT_BATCH(ctx, CMD_MEDIA_CURBE_LOAD | 2);
    OUT_BATCH(ctx, 0);

    OUT_BATCH(ctx, CURBE_TOTAL_DATA_LENGTH);
    OUT_RELOC(ctx, media_state->curbe.bo, I915_GEM_DOMAIN_INSTRUCTION, 0, 0);

    ADVANCE_BATCH(ctx);
}

static void gen6_vme_idrt(VADriverContextP ctx)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct gen6_media_state *media_state = &i965->gen6_media_state;

    BEGIN_BATCH(ctx, 4);

    OUT_BATCH(ctx, CMD_MEDIA_INTERFACE_LOAD | 2);	
    OUT_BATCH(ctx, 0);
    OUT_BATCH(ctx, GEN6_VME_KERNEL_NUMBER * sizeof(struct gen6_interface_descriptor_data));
    OUT_RELOC(ctx, media_state->idrt.bo, I915_GEM_DOMAIN_INSTRUCTION, 0, 0);

    ADVANCE_BATCH(ctx);
}

static int gen6_vme_media_object_intra(VADriverContextP ctx, 
                                  VAContextID context, 
                                  struct mfc_encode_state *encode_state,
                                  int mb_x, int mb_y)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface = SURFACE( encode_state->current_render_target);
    int mb_width = ALIGN(obj_surface->orig_width, 16) / 16;
    int len_in_dowrds = 6 + 1;

    BEGIN_BATCH(ctx, len_in_dowrds);
    
    OUT_BATCH(ctx, CMD_MEDIA_OBJECT | (len_in_dowrds - 2));
    OUT_BATCH(ctx, VME_INTRA_SHADER);		/*Interface Descriptor Offset*/	
    OUT_BATCH(ctx, 0);
    OUT_BATCH(ctx, 0);
    OUT_BATCH(ctx, 0);
    OUT_BATCH(ctx, 0);
   
    /*inline data */
    OUT_BATCH(ctx, mb_width << 16 | mb_y << 8 | mb_x);			/*M0.0 Refrence0 X,Y, not used in Intra*/
    ADVANCE_BATCH(ctx);

    return len_in_dowrds * 4;
}

static int gen6_vme_media_object_inter(VADriverContextP ctx, 
                                  VAContextID context, 
                                  struct mfc_encode_state *encode_state,
                                  int mb_x, int mb_y)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface = SURFACE( encode_state->current_render_target);
    int i;
    unsigned char *pPixel[17];	
    int pitch = obj_surface->width;
    int mb_width = ALIGN(obj_surface->orig_width, 16) / 16;
    int len_in_dowrds = 6 + 32 + 8;

    BEGIN_BATCH(ctx, len_in_dowrds);
    
    OUT_BATCH(ctx, CMD_MEDIA_OBJECT | (len_in_dowrds - 2));
    OUT_BATCH(ctx, VME_INTER_SHADER);		/*Interface Descriptor Offset*/	
    OUT_BATCH(ctx, 0);
    OUT_BATCH(ctx, 0);
    OUT_BATCH(ctx, 0);
    OUT_BATCH(ctx, 0);
   
    /*inline data */
    OUT_BATCH(ctx, mb_width << 16 | mb_y << 8 | mb_x);
    OUT_BATCH(ctx, 0x00000000);			/*M0.1 Refrence1 X,Y, not used in P frame*/
    OUT_BATCH(ctx, (mb_y<<20) |
              (mb_x<<4));			    /*M0.2 Source X,Y*/
    OUT_BATCH(ctx, 0x00A03000);			/*M0.3 16x16 Source, 1/4 pixel, harr*/
    OUT_BATCH(ctx, 0x00000000);			/*M0.4 Ignored*/
    OUT_BATCH(ctx, 0x20200000);			/*M0.5 Reference Width&Height, 32x32*/
    OUT_BATCH(ctx, 0x00000000);			/*M0.6 Debug*/
    OUT_BATCH(ctx, 0x00000000);			/*M0.7 Debug*/
										
    OUT_BATCH(ctx, 0x00000000);			/*M1.0 Default value*/
    OUT_BATCH(ctx, 0x0C000020);			/*M1.1 Default value MAX 32 MVs*/
    OUT_BATCH(ctx, 0x00000000);			/*M1.2 Default value*/
    OUT_BATCH(ctx, 0x00000000);			/*M1.3 Default value*/
    OUT_BATCH(ctx, 0x00000000);			/*M1.4 Default value*/
    OUT_BATCH(ctx, 0x00000000);			/*M1.5 Default value*/
    OUT_BATCH(ctx, 0x00000000);			/*M1.6 Default value*/

    i = 0;
    if ( mb_x > 0)
        i |= 0x60;
    if ( mb_y > 0)
        i |= 0x10;
    if ( mb_x > 0 && mb_y > 0)
        i |= 0x04;
    if ( mb_y > 0 && mb_x < (mb_width - 1) )
        i |= 0x08;
    OUT_BATCH(ctx, (i << 8) | 6 );		/*M1.7 Neighbor MBS and Intra mode masks*/

    drm_intel_gem_bo_map_gtt( obj_surface->bo );
    for(i = 0; i < 17; i++){
        pPixel[i] = (unsigned char *) ( obj_surface->bo->virtual + mb_x * 16 - 1  + ( mb_y * 16 - 1 + i) * pitch);
    }

    OUT_BATCH(ctx, 0);							/*M2.0 MBZ*/
    OUT_BATCH(ctx, pPixel[0][0] << 24);			/*M2.1 Corner Neighbor*/
	
    OUT_BATCH(ctx, ( (pPixel[0][4] << 24) 
                     | (pPixel[0][3] << 16)
                     | (pPixel[0][2] << 8)
                     | (pPixel[0][1] ) ));		/*M2.2 */
	
    OUT_BATCH(ctx, ( (pPixel[0][8]	<< 24) 
                     | (pPixel[0][7] << 16)
                     | (pPixel[0][6] << 8)
                     | (pPixel[0][5] ) ));		/*M2.3 */

    OUT_BATCH(ctx, ( (pPixel[0][12]	<< 24) 
                     | (pPixel[0][11] << 16)
                     | (pPixel[0][10] << 8)		
                     | (pPixel[0][9] ) ));		/*M2.4 */

    OUT_BATCH(ctx, ( (pPixel[0][16]	<< 24) 
                     | (pPixel[0][15] << 16)
                     | (pPixel[0][14] << 8)	
                     | (pPixel[0][13] ) ));		/*M2.5 */

    OUT_BATCH(ctx, ( (pPixel[0][20]	<< 24) 
                     | (pPixel[0][19] << 16)
                     | (pPixel[0][18] << 8)		
                     | (pPixel[0][17] ) ));		/*M2.6 */

    OUT_BATCH(ctx, ( (pPixel[0][24]	<< 24) 
                     | (pPixel[0][23] << 16)
                     | (pPixel[0][22] << 8)		
                     | (pPixel[0][21] ) ));		/*M2.7 */

    OUT_BATCH(ctx, ( (pPixel[4][0]	<< 24) 
                     | (pPixel[3][0] << 16)
                     | (pPixel[2][0] << 8)		
                     | (pPixel[1][0] ) ));		/*M3.0 */

    OUT_BATCH(ctx, ( (pPixel[8][0]	<< 24) 
                     | (pPixel[7][0] << 16)
                     | (pPixel[6][0] << 8)		
                     | (pPixel[5][0] ) ));		/*M3.1 */

    OUT_BATCH(ctx, ( (pPixel[12][0]	<< 24) 
                     | (pPixel[11][0] << 16)
                     | (pPixel[10][0] << 8)		
                     | (pPixel[9][0] ) ));		/*M3.2 */

    OUT_BATCH(ctx, ( (pPixel[16][0]	<< 24) 
                     | (pPixel[15][0] << 16)
                     | (pPixel[14][0] << 8)		
                     | (pPixel[13][0] ) ));		/*M3.3 */

    OUT_BATCH(ctx, 0x11111111);			/*M3.4*/    
    OUT_BATCH(ctx, 0x00000000);			/*M3.5*/
    OUT_BATCH(ctx, 0x00000000);			/*M3.6*/
    OUT_BATCH(ctx, 0x00000000);			/*M3.7*/

    OUT_BATCH(ctx, 0);                      /*Write Message Header M0.0*/
    OUT_BATCH(ctx, 0);                      /*Write Message Header M0.1*/

    OUT_BATCH(ctx, (mb_y * mb_width + mb_x) * 4); /*Write Message Header M0.2*/
    OUT_BATCH(ctx, 0x00000000);				/*Write Message Header M0.3*/

    OUT_BATCH(ctx, 0x00000000);
    OUT_BATCH(ctx, 0x00000000);
    OUT_BATCH(ctx, 0x00000000);
    OUT_BATCH(ctx, 0x00000000);	

    drm_intel_gem_bo_unmap_gtt( obj_surface->bo );

    ADVANCE_BATCH(ctx);

    return len_in_dowrds * 4;
}

static void gen6_vme_media_init(VADriverContextP ctx)
{
    int i;
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_media_state *media_state = &i965->gen6_media_state;
    dri_bo *bo;

    /* constant buffer */
    dri_bo_unreference(media_state->curbe.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr,
                      "Buffer",
                      CURBE_TOTAL_DATA_LENGTH, 64);
    assert(bo);
    media_state->curbe.bo = bo;

    /* surface state */
    for (i = 0; i < MAX_MEDIA_SURFACES_GEN6; i++) {
        dri_bo_unreference(media_state->surface_state[i].bo);
        media_state->surface_state[i].bo = NULL;
    }

    /* binding table */
    dri_bo_unreference(media_state->binding_table.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr, 
                      "Buffer",
                      MAX_MEDIA_SURFACES_GEN6 * sizeof(unsigned int), 32);
    assert(bo);
    media_state->binding_table.bo = bo;

    /* interface descriptor remapping table */
    dri_bo_unreference(media_state->idrt.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr, 
                      "Buffer", 
                      MAX_INTERFACE_DESC_GEN6 * sizeof(struct gen6_interface_descriptor_data), 16);
    assert(bo);
    media_state->idrt.bo = bo;

    /* VME output buffer */
    dri_bo_unreference(media_state->vme_output.bo);
    media_state->vme_output.bo = NULL;

    /* VME state */
    dri_bo_unreference(media_state->vme_state.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr,
                      "Buffer",
                      1024*16, 64);
    assert(bo);
    media_state->vme_state.bo = bo;

    media_state->vfe_state.max_num_threads = 60 - 1;
    media_state->vfe_state.num_urb_entries = 16;
    media_state->vfe_state.gpgpu_mode = 0;
    media_state->vfe_state.urb_entry_size = 59 - 1;
    media_state->vfe_state.curbe_allocation_size = CURBE_ALLOCATION_SIZE - 1;
}

static void gen6_vme_pipeline_programing(VADriverContextP ctx, 
                                         VAContextID context, 
                                         struct mfc_encode_state *encode_state)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx); 
    struct object_context *obj_context = CONTEXT(context);
    int width_in_mbs = (obj_context->picture_width + 15) / 16;
    int height_in_mbs = (obj_context->picture_height + 15) / 16;
    int x, y;
    int emit_new_state = 1, object_len_in_bytes;
    VAEncSliceParameterBuffer *pSliceParameter = (VAEncSliceParameterBuffer *)encode_state->slice_params[0]->buffer;
    int is_intra = pSliceParameter->slice_flags.bits.is_intra;

    intel_batchbuffer_start_atomic(ctx, 0x1000);

    for(y = 0; y < height_in_mbs; y++){
        for(x = 0; x < width_in_mbs; x++){	

            if (emit_new_state) {
                /*Step1: MI_FLUSH/PIPE_CONTROL*/
                BEGIN_BATCH(ctx, 4);
                OUT_BATCH(ctx, CMD_PIPE_CONTROL | 0x02);
                OUT_BATCH(ctx, 0);
                OUT_BATCH(ctx, 0);
                OUT_BATCH(ctx, 0);
                ADVANCE_BATCH(ctx);

                /*Step2: State command PIPELINE_SELECT*/
                gen6_vme_pipeline_select(ctx);

                /*Step3: State commands configuring pipeline states*/
                gen6_vme_state_base_address(ctx);
                gen6_vme_vfe_state(ctx);
                gen6_vme_curbe_load(ctx);
                gen6_vme_idrt(ctx);

                emit_new_state = 0;
            }

            /*Step4: Primitive commands*/
            if ( is_intra ) {
                object_len_in_bytes = gen6_vme_media_object_intra(ctx, context, encode_state, x, y);
            } else {
                object_len_in_bytes = gen6_vme_media_object_inter(ctx, context, encode_state, x, y);
            }

            if (intel_batchbuffer_check_free_space(ctx, object_len_in_bytes) == 0) {
                intel_batchbuffer_end_atomic(ctx);	
                intel_batchbuffer_flush(ctx);
                emit_new_state = 1;
                intel_batchbuffer_start_atomic(ctx, 0x1000);
            }
        }
    }

    intel_batchbuffer_end_atomic(ctx);	
}

static VAStatus gen6_vme_prepare(VADriverContextP ctx, 
                                 VAContextID context,                              
                                 struct mfc_encode_state *encode_state)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VAEncSliceParameterBuffer *pSliceParameter = (VAEncSliceParameterBuffer *)encode_state->slice_params[0]->buffer;
    int is_intra = pSliceParameter->slice_flags.bits.is_intra;
	
    /*Setup all the memory object*/
    gen6_vme_surface_setup(ctx, context, encode_state, is_intra);
    gen6_vme_interface_setup(ctx, context, encode_state);
    gen6_vme_constant_setup(ctx, context, encode_state);
    gen6_vme_vme_state_setup(ctx, context, encode_state, is_intra);

    /*Programing media pipeline*/
    gen6_vme_pipeline_programing(ctx, context, encode_state);

    return vaStatus;
}

static VAStatus gen6_vme_run(VADriverContextP ctx, 
                             VAContextID context,                              
                             struct mfc_encode_state *encode_state)
{
    intel_batchbuffer_flush(ctx);

    return VA_STATUS_SUCCESS;
}

static VAStatus gen6_vme_stop(VADriverContextP ctx, 
                              VAContextID context,                              
                              struct mfc_encode_state *encode_state)
{
    return VA_STATUS_SUCCESS;
}

VAStatus gen6_vme_media_pipeline(VADriverContextP ctx,
                                 VAContextID context,                              
                                 struct mfc_encode_state *encode_state)
{
    gen6_vme_media_init(ctx);	
    gen6_vme_prepare(ctx, context, encode_state);
    gen6_vme_run(ctx, context, encode_state);	
    gen6_vme_stop(ctx, context, encode_state);

    return VA_STATUS_SUCCESS;
}

Bool gen6_vme_init(VADriverContextP ctx)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    int i;

    for (i = 0; i < GEN6_VME_KERNEL_NUMBER; i++) {
        /*Load kernel into GPU memory*/	
        struct media_kernel *kernel = &gen6_vme_kernels[i];

        kernel->bo = dri_bo_alloc(i965->intel.bufmgr, 
                                  kernel->name, 
                                  kernel->size,
                                  0x1000);
        assert(kernel->bo);
        dri_bo_subdata(kernel->bo, 0, kernel->size, kernel->bin);
    }
    
    return True;
}

Bool gen6_vme_terminate(VADriverContextP ctx)
{
    int i;

    for (i = 0; i < GEN6_VME_KERNEL_NUMBER; i++) {
        /*Load kernel into GPU memory*/	
        struct media_kernel *kernel = &gen6_vme_kernels[i];

        dri_bo_unreference(kernel->bo);
        kernel->bo = NULL;
    }

    return True;
}
