/*
 * Mesa 3-D graphics library
 * Version:  6.1
 *
 * Copyright (C) 1999-2004  Brian Paul   All Rights Reserved.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Keith Whitwell <keith@tungstengraphics.com>
 */


#include "glheader.h"
#include "colormac.h"
#include "context.h"
#include "macros.h"
#include "imports.h"
#include "mtypes.h"

#include "math/m_xform.h"

#include "t_context.h"
#include "t_pipeline.h"



struct vertex_stage_data {
   GLvector4f eye;
   GLvector4f clip;
   GLvector4f proj;
   GLubyte *clipmask;
   GLubyte ormask;
   GLubyte andmask;


   /* Need these because it's difficult to replay the sideeffects
    * analytically.
    */
   GLvector4f *save_eyeptr;
   GLvector4f *save_clipptr;
   GLvector4f *save_ndcptr;
};

#define VERTEX_STAGE_DATA(stage) ((struct vertex_stage_data *)stage->privatePtr)




/* This function implements cliptesting for user-defined clip planes.
 * The clipping of primitives to these planes is implemented in
 * t_render_clip.h.
 */
#define USER_CLIPTEST(NAME, SZ)					\
static void NAME( GLcontext *ctx,				\
		  GLvector4f *clip,				\
		  GLubyte *clipmask,				\
		  GLubyte *clipormask,				\
		  GLubyte *clipandmask )			\
{								\
   GLuint p;							\
								\
   for (p = 0; p < ctx->Const.MaxClipPlanes; p++)		\
      if (ctx->Transform.ClipPlanesEnabled & (1 << p)) {	\
	 GLuint nr, i;						\
	 const GLfloat a = ctx->Transform._ClipUserPlane[p][0];	\
	 const GLfloat b = ctx->Transform._ClipUserPlane[p][1];	\
	 const GLfloat c = ctx->Transform._ClipUserPlane[p][2];	\
	 const GLfloat d = ctx->Transform._ClipUserPlane[p][3];	\
         GLfloat *coord = (GLfloat *)clip->data;		\
         GLuint stride = clip->stride;				\
         GLuint count = clip->count;				\
								\
	 for (nr = 0, i = 0 ; i < count ; i++) {		\
	    GLfloat dp = coord[0] * a + coord[1] * b;		\
	    if (SZ > 2) dp += coord[2] * c;			\
	    if (SZ > 3) dp += coord[3] * d; else dp += d;	\
								\
	    if (dp < 0) {					\
	       nr++;						\
	       clipmask[i] |= CLIP_USER_BIT;			\
	    }							\
								\
	    STRIDE_F(coord, stride);				\
	 }							\
								\
	 if (nr > 0) {						\
	    *clipormask |= CLIP_USER_BIT;			\
	    if (nr == count) {					\
	       *clipandmask |= CLIP_USER_BIT;			\
	       return;						\
	    }							\
	 }							\
      }								\
}


USER_CLIPTEST(userclip2, 2)
USER_CLIPTEST(userclip3, 3)
USER_CLIPTEST(userclip4, 4)

static void (*(usercliptab[5]))( GLcontext *,
				 GLvector4f *, GLubyte *,
				 GLubyte *, GLubyte * ) =
{
   0,
   0,
   userclip2,
   userclip3,
   userclip4
};



static GLboolean run_vertex_stage( GLcontext *ctx,
				   struct tnl_pipeline_stage *stage )
{
   struct vertex_stage_data *store = (struct vertex_stage_data *)stage->privatePtr;
   TNLcontext *tnl = TNL_CONTEXT(ctx);
   struct vertex_buffer *VB = &tnl->vb;

   ASSERT(!ctx->VertexProgram._Enabled);

   if (stage->changed_inputs) {

      if (ctx->_NeedEyeCoords) {
	 /* Separate modelview transformation:
	  * Use combined ModelProject to avoid some depth artifacts
	  */
	 if (ctx->ModelviewMatrixStack.Top->type == MATRIX_IDENTITY)
	    VB->EyePtr = VB->ObjPtr;
	 else
	    VB->EyePtr = TransformRaw( &store->eye,
                                       ctx->ModelviewMatrixStack.Top,
				       VB->ObjPtr);
#if 0
         /* examine some eye coordinates */
         {
            GLuint i;
            GLfloat *v = VB->EyePtr->start;
            for (i = 0; i < 4; i++) {
               _mesa_printf("eye[%d] = %g, %g, %g, %g\n",
                            i, v[0], v[1], v[2], v[3]);
               v += 4;
            }
         }
#endif
      }

      VB->ClipPtr = TransformRaw( &store->clip,
				  &ctx->_ModelProjectMatrix,
				  VB->ObjPtr );

      /* Drivers expect this to be clean to element 4...
       */
      switch (VB->ClipPtr->size) {
      case 1:			
	 /* impossible */
      case 2:
	 _mesa_vector4f_clean_elem( VB->ClipPtr, VB->Count, 2 );
         /* fall-through */
      case 3:
	 _mesa_vector4f_clean_elem( VB->ClipPtr, VB->Count, 3 );
         /* fall-through */
      case 4:
	 break;
      }

#if 0
      /* examine some clip coordinates */
      {
         GLuint i;
         GLfloat *v = VB->ClipPtr->start;
         for (i = 0; i < 4; i++) {
            _mesa_printf("clip[%d] = %g, %g, %g, %g\n",
                         i, v[0], v[1], v[2], v[3]);
            v += 4;
         }
      }
#endif

      /* Cliptest and perspective divide.  Clip functions must clear
       * the clipmask.
       */
      store->ormask = 0;
      store->andmask = CLIP_ALL_BITS;

      if (tnl->NeedNdcCoords) {
	 VB->NdcPtr =
	    _mesa_clip_tab[VB->ClipPtr->size]( VB->ClipPtr,
                                               &store->proj,
                                               store->clipmask,
                                               &store->ormask,
                                               &store->andmask );
      }
      else {
	 VB->NdcPtr = 0;
	 _mesa_clip_np_tab[VB->ClipPtr->size]( VB->ClipPtr,
                                               0,
                                               store->clipmask,
                                               &store->ormask,
                                               &store->andmask );
      }

      if (store->andmask)
	 return GL_FALSE;


      /* Test userclip planes.  This contributes to VB->ClipMask, so
       * is essentially required to be in this stage.
       */
      if (ctx->Transform.ClipPlanesEnabled) {
	 usercliptab[VB->ClipPtr->size]( ctx,
					 VB->ClipPtr,
					 store->clipmask,
					 &store->ormask,
					 &store->andmask );

	 if (store->andmask)
	    return GL_FALSE;
      }

      VB->ClipAndMask = store->andmask;
      VB->ClipOrMask = store->ormask;
      VB->ClipMask = store->clipmask;

      store->save_eyeptr = VB->EyePtr;
      store->save_clipptr = VB->ClipPtr;
      store->save_ndcptr = VB->NdcPtr;
   }
   else {
      /* Replay the sideeffects.
       */
      VB->EyePtr = store->save_eyeptr;
      VB->ClipPtr = store->save_clipptr;
      VB->NdcPtr = store->save_ndcptr;
      VB->ClipMask = store->clipmask;
      VB->ClipAndMask = store->andmask;
      VB->ClipOrMask = store->ormask;
      if (store->andmask)
	 return GL_FALSE;
   }

   return GL_TRUE;
}


static void check_vertex( GLcontext *ctx, struct tnl_pipeline_stage *stage )
{
   stage->active = !ctx->VertexProgram._Enabled;
}

static GLboolean init_vertex_stage( GLcontext *ctx,
				    struct tnl_pipeline_stage *stage )
{
   struct vertex_buffer *VB = &TNL_CONTEXT(ctx)->vb;
   struct vertex_stage_data *store;
   GLuint size = VB->Size;

   stage->privatePtr = CALLOC(sizeof(*store));
   store = VERTEX_STAGE_DATA(stage);
   if (!store)
      return GL_FALSE;

   _mesa_vector4f_alloc( &store->eye, 0, size, 32 );
   _mesa_vector4f_alloc( &store->clip, 0, size, 32 );
   _mesa_vector4f_alloc( &store->proj, 0, size, 32 );

   store->clipmask = (GLubyte *) ALIGN_MALLOC(sizeof(GLubyte)*size, 32 );

   if (!store->clipmask ||
       !store->eye.data ||
       !store->clip.data ||
       !store->proj.data)
      return GL_FALSE;

   /* Now run the stage.
    */
   stage->run = run_vertex_stage;
   return stage->run( ctx, stage );
}

static void dtr( struct tnl_pipeline_stage *stage )
{
   struct vertex_stage_data *store = VERTEX_STAGE_DATA(stage);

   if (store) {
      _mesa_vector4f_free( &store->eye );
      _mesa_vector4f_free( &store->clip );
      _mesa_vector4f_free( &store->proj );
      ALIGN_FREE( store->clipmask );
      FREE(store);
      stage->privatePtr = NULL;
      stage->run = init_vertex_stage;
   }
}


const struct tnl_pipeline_stage _tnl_vertex_transform_stage =
{
   "modelview/project/cliptest/divide",
   _NEW_PROGRAM,                /* check_state: only care about vertex prog */
   _MESA_NEW_NEED_EYE_COORDS |  /* run_state: when to invalidate / re-run */
   _NEW_MODELVIEW|
   _NEW_PROJECTION|
   _NEW_PROGRAM|
   _NEW_TRANSFORM,
   GL_TRUE,			/* active */
   _TNL_BIT_POS,		/* inputs */
   _TNL_BIT_POS,		/* outputs */
   0,				/* changed_inputs */
   NULL,			/* private data */
   dtr,				/* destructor */
   check_vertex,		/* check */
   init_vertex_stage		/* run -- initially set to init */
};
