/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016 Axel Gneiting

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_world.c: world model rendering

#include "quakedef.h"

extern cvar_t gl_fullbrights, r_drawflat, r_oldskyleaf, r_showtris; //johnfitz

extern glpoly_t	*lightmap_polys[MAX_LIGHTMAPS];

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);
extern byte mod_novis[MAX_MAP_LEAFS/8];
int vis_changed; //if true, force pvs to be refreshed

extern VkBuffer bmodel_vertex_buffer;

//==============================================================================
//
// SETUP CHAINS
//
//==============================================================================

/*
================
R_ClearTextureChains -- ericw 

clears texture chains for all textures used by the given model, and also
clears the lightmap chains
================
*/
void R_ClearTextureChains (qmodel_t *mod, texchain_t chain)
{
	int i;

	// set all chains to null
	for (i=0 ; i<mod->numtextures ; i++)
		if (mod->textures[i])
			mod->textures[i]->texturechains[chain] = NULL;
			
	// clear lightmap chains
	memset (lightmap_polys, 0, sizeof(lightmap_polys));
}

/*
================
R_ChainSurface -- ericw -- adds the given surface to its texture chain
================
*/
void R_ChainSurface (msurface_t *surf, texchain_t chain)
{
	surf->texturechain = surf->texinfo->texture->texturechains[chain];
	surf->texinfo->texture->texturechains[chain] = surf;
}

/*
===============
R_MarkSurfaces -- johnfitz -- mark surfaces based on PVS and rebuild texture chains
===============
*/
void R_MarkSurfaces (void)
{
	byte		*vis;
	mleaf_t		*leaf;
	mnode_t		*node;
	msurface_t	*surf, **mark;
	int			i, j;
	qboolean	nearwaterportal;

	// clear lightmap chains
	memset (lightmap_polys, 0, sizeof(lightmap_polys));

	// check this leaf for water portals
	// TODO: loop through all water surfs and use distance to leaf cullbox
	nearwaterportal = false;
	for (i=0, mark = r_viewleaf->firstmarksurface; i < r_viewleaf->nummarksurfaces; i++, mark++)
		if ((*mark)->flags & SURF_DRAWTURB)
			nearwaterportal = true;

	// choose vis data
	if (r_novis.value || r_viewleaf->contents == CONTENTS_SOLID || r_viewleaf->contents == CONTENTS_SKY)
		vis = &mod_novis[0];
	else if (nearwaterportal)
		vis = SV_FatPVS (r_origin, cl.worldmodel);
	else
		vis = Mod_LeafPVS (r_viewleaf, cl.worldmodel);

	// if surface chains don't need regenerating, just add static entities and return
	if (r_oldviewleaf == r_viewleaf && !vis_changed && !nearwaterportal)
	{
		leaf = &cl.worldmodel->leafs[1];
		for (i=0 ; i<cl.worldmodel->numleafs ; i++, leaf++)
			if (vis[i>>3] & (1<<(i&7)))
				if (leaf->efrags)
					R_StoreEfrags (&leaf->efrags);
		return;
	}

	vis_changed = false;
	r_visframecount++;
	r_oldviewleaf = r_viewleaf;

	// iterate through leaves, marking surfaces
	leaf = &cl.worldmodel->leafs[1];
	for (i=0 ; i<cl.worldmodel->numleafs ; i++, leaf++)
	{
		if (vis[i>>3] & (1<<(i&7)))
		{
			if (r_oldskyleaf.value || leaf->contents != CONTENTS_SKY)
				for (j=0, mark = leaf->firstmarksurface; j<leaf->nummarksurfaces; j++, mark++)
					(*mark)->visframe = r_visframecount;

			// add static models
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
		}
	}

	// set all chains to null
	for (i=0 ; i<cl.worldmodel->numtextures ; i++)
		if (cl.worldmodel->textures[i])
			cl.worldmodel->textures[i]->texturechains[chain_world] = NULL;

	// rebuild chains

#if 1
	//iterate through surfaces one node at a time to rebuild chains
	//need to do it this way if we want to work with tyrann's skip removal tool
	//becuase his tool doesn't actually remove the surfaces from the bsp surfaces lump
	//nor does it remove references to them in each leaf's marksurfaces list
	for (i=0, node = cl.worldmodel->nodes ; i<cl.worldmodel->numnodes ; i++, node++)
		for (j=0, surf=&cl.worldmodel->surfaces[node->firstsurface] ; j<(int)node->numsurfaces ; j++, surf++)
			if (surf->visframe == r_visframecount)
			{
				R_ChainSurface(surf, chain_world);
			}
#else
	//the old way
	surf = &cl.worldmodel->surfaces[cl.worldmodel->firstmodelsurface];
	for (i=0 ; i<cl.worldmodel->nummodelsurfaces ; i++, surf++)
	{
		if (surf->visframe == r_visframecount)
		{
			R_ChainSurface(surf, chain_world);
		}
	}
#endif
}

/*
================
R_BackFaceCull -- johnfitz -- returns true if the surface is facing away from vieworg
================
*/
qboolean R_BackFaceCull (msurface_t *surf)
{
	double dot;

	switch (surf->plane->type)
	{
	case PLANE_X:
		dot = r_refdef.vieworg[0] - surf->plane->dist;
		break;
	case PLANE_Y:
		dot = r_refdef.vieworg[1] - surf->plane->dist;
		break;
	case PLANE_Z:
		dot = r_refdef.vieworg[2] - surf->plane->dist;
		break;
	default:
		dot = DotProduct (r_refdef.vieworg, surf->plane->normal) - surf->plane->dist;
		break;
	}

	if ((dot < 0) ^ !!(surf->flags & SURF_PLANEBACK))
		return true;

	return false;
}

/*
================
R_CullSurfaces -- johnfitz
================
*/
void R_CullSurfaces (void)
{
	msurface_t *s;
	int i;
	texture_t *t;

	if (!r_drawworld_cheatsafe)
		return;

// ericw -- instead of testing (s->visframe == r_visframecount) on all world
// surfaces, use the chained surfaces, which is exactly the same set of sufaces
	for (i=0 ; i<cl.worldmodel->numtextures ; i++)
	{
		t = cl.worldmodel->textures[i];

		if (!t || !t->texturechains[chain_world])
			continue;

		for (s = t->texturechains[chain_world]; s; s = s->texturechain)
		{
			if (R_CullBox(s->mins, s->maxs) || R_BackFaceCull (s))
				s->culled = true;
			else
			{
				s->culled = false;
				rs_brushpolys++; //count wpolys here
				if (s->texinfo->texture->warpimage)
					s->texinfo->texture->update_warp = true;
			}
		}
	}
}

/*
================
R_BuildLightmapChains -- johnfitz -- used for r_lightmap 1

ericw -- now always used at the start of R_DrawTextureChains for the 
mh dynamic lighting speedup
================
*/
void R_BuildLightmapChains (qmodel_t *model, texchain_t chain)
{
	texture_t *t;
	msurface_t *s;
	int i;

	// now rebuild them
	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain])
			continue;

		for (s = t->texturechains[chain]; s; s = s->texturechain)
			if (!s->culled)
				R_RenderDynamicLightmaps (s);
	}
}

//==============================================================================
//
// DRAW CHAINS
//
//==============================================================================

/*
=============
R_BeginTransparentDrawing -- ericw
=============
*/
static void R_BeginTransparentDrawing (float entalpha)
{
	/*if (entalpha < 1.0f)
	{
		glDepthMask (GL_FALSE);
		glEnable (GL_BLEND);
		glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glColor4f (1,1,1,entalpha);
	}*/
}

/*
=============
R_EndTransparentDrawing -- ericw
=============
*/
static void R_EndTransparentDrawing (float entalpha)
{
	//if (entalpha < 1.0f)
	//{
	//	glDepthMask (GL_TRUE);
	//	glDisable (GL_BLEND);
	//	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	//	glColor3f (1, 1, 1);
	//}
}

/*
================
R_DrawTextureChains_ShowTris -- johnfitz
================
*/
void R_DrawTextureChains_ShowTris (qmodel_t *model, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];
		if (!t)
			continue;

		for (s = t->texturechains[chain]; s; s = s->texturechain)
			if (!s->culled)
			{
				DrawGLTriangleFan (s->polys);
			}
	}
}

/*
================
R_DrawTextureChains_Drawflat -- johnfitz
================
*/
void R_DrawTextureChains_Drawflat (qmodel_t *model, texchain_t chain)
{
	//int			i;
	//msurface_t	*s;
	//texture_t	*t;
	//glpoly_t	*p;

	//for (i=0 ; i<model->numtextures ; i++)
	//{
	//	t = model->textures[i];
	//	if (!t)
	//		continue;

	//	{
	//		for (s = t->texturechains[chain]; s; s = s->texturechain)
	//			if (!s->culled)
	//			{
	//				srand((unsigned int) (uintptr_t) s->polys);
	//				glColor3f (rand()%256/255.0, rand()%256/255.0, rand()%256/255.0);
	//				DrawGLPoly (s->polys);
	//				rs_brushpasses++;
	//			}
	//	}
	//}
	//glColor3f (1,1,1);
	//srand ((int) (cl.time * 1000));
}

/*
================
R_DrawTextureChains_Glow -- johnfitz
================
*/
void R_DrawTextureChains_Glow (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	//int			i;
	//msurface_t	*s;
	//texture_t	*t;
	//gltexture_t	*glt;
	//qboolean	bound;

	//for (i=0 ; i<model->numtextures ; i++)
	//{
	//	t = model->textures[i];

	//	if (!t || !t->texturechains[chain] || !(glt = R_TextureAnimation(t, ent != NULL ? ent->frame : 0)->fullbright))
	//		continue;

	//	bound = false;

	//	for (s = t->texturechains[chain]; s; s = s->texturechain)
	//		if (!s->culled)
	//		{
	//			if (!bound) //only bind once we are sure we need this texture
	//			{
	//				GL_Bind (glt);
	//				bound = true;
	//			}
	//			DrawGLPoly (s->polys);
	//			rs_brushpasses++;
	//		}
	//}
}

//==============================================================================
//
// VBO SUPPORT
//
//==============================================================================

static unsigned int R_NumTriangleIndicesForSurf (msurface_t *s)
{
	return 3 * (s->numedges - 2);
}

/*
================
R_TriangleIndicesForSurf

Writes out the triangle indices needed to draw s as a triangle list.
The number of indices it will write is given by R_NumTriangleIndicesForSurf.
================
*/
static void R_TriangleIndicesForSurf (msurface_t *s, uint32_t *dest)
{
	int i;
	for (i=2; i<s->numedges; i++)
	{
		*dest++ = s->vbo_firstvert;
		*dest++ = s->vbo_firstvert + i - 1;
		*dest++ = s->vbo_firstvert + i;
	}
}

#define MAX_BATCH_SIZE 4096

static uint32_t vbo_indices[MAX_BATCH_SIZE];
static unsigned int num_vbo_indices;

/*
================
R_ClearBatch
================
*/
static void R_ClearBatch ()
{
	num_vbo_indices = 0;
}

/*
================
R_FlushBatch

Draw the current batch if non-empty and clears it, ready for more R_BatchSurface calls.
================
*/
static void R_FlushBatch ()
{
	if (num_vbo_indices > 0)
	{
		VkBuffer buffer;
		VkDeviceSize buffer_offset;
		byte * indices = R_IndexAllocate(num_vbo_indices * sizeof(uint32_t), &buffer, &buffer_offset);
		memcpy(indices, vbo_indices, num_vbo_indices * sizeof(uint32_t));

		vkCmdBindIndexBuffer(vulkan_globals.command_buffer, buffer, buffer_offset, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(vulkan_globals.command_buffer, num_vbo_indices, 1, 0, 0, 0);

		num_vbo_indices = 0;
	}
}

/*
================
R_BatchSurface

Add the surface to the current batch, or just draw it immediately if we're not
using VBOs.
================
*/
static void R_BatchSurface (msurface_t *s)
{
	int num_surf_indices;

	num_surf_indices = R_NumTriangleIndicesForSurf (s);
	
	if (num_vbo_indices + num_surf_indices > MAX_BATCH_SIZE)
		R_FlushBatch();
	
	R_TriangleIndicesForSurf (s, &vbo_indices[num_vbo_indices]);
	num_vbo_indices += num_surf_indices;
}

/*
================
R_DrawTextureChains_NoTexture -- johnfitz

draws surfs whose textures were missing from the BSP
================
*/
void R_DrawTextureChains_NoTexture (qmodel_t *model, texchain_t chain)
{
	//int			i;
	//msurface_t	*s;
	//texture_t	*t;
	//qboolean	bound;

	//for (i=0 ; i<model->numtextures ; i++)
	//{
	//	t = model->textures[i];

	//	if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_NOTEXTURE))
	//		continue;

	//	bound = false;

	//	for (s = t->texturechains[chain]; s; s = s->texturechain)
	//		if (!s->culled)
	//		{
	//			if (!bound) //only bind once we are sure we need this texture
	//			{
	//				GL_Bind (t->gltexture);
	//				bound = true;
	//			}
	//			DrawGLPoly (s->polys);
	//			rs_brushpasses++;
	//		}
	//}
}

/*
================
R_DrawTextureChains_TextureOnly -- johnfitz
================
*/
void R_DrawTextureChains_TextureOnly (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	//int			i;
	//msurface_t	*s;
	//texture_t	*t;
	//qboolean	bound;

	//for (i=0 ; i<model->numtextures ; i++)
	//{
	//	t = model->textures[i];

	//	if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTURB | SURF_DRAWSKY))
	//		continue;

	//	bound = false;

	//	for (s = t->texturechains[chain]; s; s = s->texturechain)
	//		if (!s->culled)
	//		{
	//			if (!bound) //only bind once we are sure we need this texture
	//			{
	//				GL_Bind ((R_TextureAnimation(t, ent != NULL ? ent->frame : 0))->gltexture);
	//				
	//				if (t->texturechains[chain]->flags & SURF_DRAWFENCE)
	//					glEnable (GL_ALPHA_TEST); // Flip alpha test back on
	//				
	//				bound = true;
	//			}
	//			DrawGLPoly (s->polys);
	//			rs_brushpasses++;
	//		}
	//		
	//	if (bound && t->texturechains[chain]->flags & SURF_DRAWFENCE)
	//		glDisable (GL_ALPHA_TEST); // Flip alpha test back off
	//}
}

/*
================
GL_WaterAlphaForEntitySurface -- ericw
 
Returns the water alpha to use for the entity and surface combination.
================
*/
float GL_WaterAlphaForEntitySurface (entity_t *ent, msurface_t *s)
{
	float entalpha;
	if (ent == NULL || ent->alpha == ENTALPHA_DEFAULT)
		entalpha = GL_WaterAlphaForSurface(s);
	else
		entalpha = ENTALPHA_DECODE(ent->alpha);
	return entalpha;
}

/*
================
R_DrawTextureChains_Water -- johnfitz
================
*/
void R_DrawTextureChains_Water (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	bound;
	float entalpha;

	if (r_drawflat_cheatsafe || r_lightmap_cheatsafe) // ericw -- !r_drawworld_cheatsafe check moved to R_DrawWorld_Water ()
		return;

	float color[3] = { 1.0f, 1.0f, 1.0f };

	vkCmdBindPipeline(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.water_pipeline);

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];
		if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTURB))
			continue;
		bound = false;
		entalpha = 1.0f;
		for (s = t->texturechains[chain]; s; s = s->texturechain)
			if (!s->culled)
			{
				if (!bound) //only bind once we are sure we need this texture
				{
					entalpha = GL_WaterAlphaForEntitySurface (ent, s);
					R_BeginTransparentDrawing (entalpha);
					vkCmdBindDescriptorSets(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.basic_pipeline_layout, 0, 1, t->warpimage->sampler_set, 0, NULL);
					vkCmdBindDescriptorSets(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.basic_pipeline_layout, 1, 1, &t->warpimage->descriptor_set, 0, NULL);

					if (model != cl.worldmodel)
					{
						// ericw -- this is copied from R_DrawSequentialPoly.
						// If the poly is not part of the world we have to
						// set this flag
						t->update_warp = true; // FIXME: one frame too late!
					}

					bound = true;
				}
				DrawGLPoly (s->polys, color, entalpha);
				rs_brushpasses++;
			}
		R_EndTransparentDrawing (entalpha);
	}
}

/*
================
R_DrawTextureChains_White -- johnfitz -- draw sky and water as white polys when r_lightmap is 1
================
*/
void R_DrawTextureChains_White (qmodel_t *model, texchain_t chain)
{
	//int			i;
	//msurface_t	*s;
	//texture_t	*t;

	//glDisable (GL_TEXTURE_2D);
	//for (i=0 ; i<model->numtextures ; i++)
	//{
	//	t = model->textures[i];

	//	if (!t || !t->texturechains[chain] || !(t->texturechains[chain]->flags & SURF_DRAWTILED))
	//		continue;

	//	for (s = t->texturechains[chain]; s; s = s->texturechain)
	//		if (!s->culled)
	//		{
	//			DrawGLPoly (s->polys);
	//			rs_brushpasses++;
	//		}
	//}
	//glEnable (GL_TEXTURE_2D);
}

/*
================
R_DrawLightmapChains -- johnfitz -- R_BlendLightmaps stripped down to almost nothing
================
*/
void R_DrawLightmapChains (void)
{
	//int			i, j;
	//glpoly_t	*p;
	//float		*v;

	//for (i=0 ; i<MAX_LIGHTMAPS ; i++)
	//{
	//	if (!lightmap_polys[i])
	//		continue;

	//	GL_Bind (lightmap_textures[i]);
	//	for (p = lightmap_polys[i]; p; p=p->chain)
	//	{
	//		glBegin (GL_POLYGON);
	//		v = p->verts[0];
	//		for (j=0 ; j<p->numverts ; j++, v+= VERTEXSIZE)
	//		{
	//			glTexCoord2f (v[5], v[6]);
	//			glVertex3fv (v);
	//		}
	//		glEnd ();
	//		rs_brushpasses++;
	//	}
	//}
}

/*
================
R_DrawTextureChains_Multitexture
================
*/
void R_DrawTextureChains_Multitexture (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;
	qboolean	bound;
	int		lastlightmap;
	gltexture_t	*fullbright = NULL;
	
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(vulkan_globals.command_buffer, 0, 1, &bmodel_vertex_buffer, &offset);
	vkCmdBindPipeline(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline);
	VkPipeline current_pipeline = vulkan_globals.world_pipeline;

	for (i=0 ; i<model->numtextures ; i++)
	{
		t = model->textures[i];

		if (!t || !t->texturechains[chain] || t->texturechains[chain]->flags & (SURF_DRAWTILED | SURF_NOTEXTURE))
			continue;

	// Enable/disable TMU 2 (fullbrights)
		if (gl_fullbrights.value && (fullbright = R_TextureAnimation(t, ent != NULL ? ent->frame : 0)->fullbright))
		{
			if (current_pipeline != vulkan_globals.world_fullbright_pipeline)
			{
				vkCmdBindPipeline(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_fullbright_pipeline);
				current_pipeline = vulkan_globals.world_fullbright_pipeline;
			}

			vkCmdBindDescriptorSets(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout, 3, 1, &fullbright->descriptor_set, 0, NULL);
		}
		else if (current_pipeline != vulkan_globals.world_pipeline)
		{
			vkCmdBindPipeline(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline);
			current_pipeline = vulkan_globals.world_pipeline;
		}

		R_ClearBatch ();

		bound = false;
		lastlightmap = 0; // avoid compiler warning
		for (s = t->texturechains[chain]; s; s = s->texturechain)
			if (!s->culled)
			{
				if (!bound) //only bind once we are sure we need this texture
				{
					texture_t * texture = R_TextureAnimation(t, ent != NULL ? ent->frame : 0);
					gltexture_t * gl_texture = texture->gltexture;
					vkCmdBindDescriptorSets(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout, 0, 1, gl_texture->sampler_set, 0, NULL);
					vkCmdBindDescriptorSets(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout, 1, 1, &gl_texture->descriptor_set, 0, NULL);

					//if (t->texturechains[chain]->flags & SURF_DRAWFENCE)
					//	glEnable (GL_ALPHA_TEST); // Flip alpha test back on
										
					bound = true;
					lastlightmap = s->lightmaptexturenum;
				}
				
				if (s->lightmaptexturenum != lastlightmap)
					R_FlushBatch ();

				gltexture_t * lightmap_texture = lightmap_textures[s->lightmaptexturenum];
				vkCmdBindDescriptorSets(vulkan_globals.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.world_pipeline_layout, 2, 1, &lightmap_texture->descriptor_set, 0, NULL);

				lastlightmap = s->lightmaptexturenum;
				R_BatchSurface (s);

				rs_brushpasses++;
			}

		R_FlushBatch ();

		//if (bound && t->texturechains[chain]->flags & SURF_DRAWFENCE)
		//	glDisable (GL_ALPHA_TEST); // Flip alpha test back off
	}
}

/*
=============
R_DrawWorld -- johnfitz -- rewritten
=============
*/
void R_DrawTextureChains (qmodel_t *model, entity_t *ent, texchain_t chain)
{
	float entalpha;
	
	if (ent != NULL)
		entalpha = ENTALPHA_DECODE(ent->alpha);
	else
		entalpha = 1;

	// ericw -- the mh dynamic lightmap speedup: make a first pass through all
	// surfaces we are going to draw, and rebuild any lightmaps that need it.
	// this also chains surfaces by lightmap which is used by r_lightmap 1.
	// the previous implementation of the speedup uploaded lightmaps one frame
	// late which was visible under some conditions, this method avoids that.
	R_BuildLightmapChains (model, chain);
	R_UploadLightmaps ();

	if (r_drawflat_cheatsafe)
	{
		//glDisable (GL_TEXTURE_2D);
		//R_DrawTextureChains_Drawflat (model, chain);
		//glEnable (GL_TEXTURE_2D);
		return;
	}

	if (r_fullbright_cheatsafe)
	{
		//R_BeginTransparentDrawing (entalpha);
		//R_DrawTextureChains_TextureOnly (model, ent, chain);
		//R_EndTransparentDrawing (entalpha);
		//goto fullbrights;
	}

	if (r_lightmap_cheatsafe)
	{
		//R_DrawLightmapChains ();
		//R_DrawTextureChains_White (model, chain);
		//return;
	}

	R_BeginTransparentDrawing (entalpha);

	R_DrawTextureChains_NoTexture (model, chain);
	R_DrawTextureChains_Multitexture (model, ent, chain);

	R_EndTransparentDrawing (entalpha);

/*fullbrights:
	if (gl_fullbrights.value)
	{
		glDepthMask (GL_FALSE);
		glEnable (GL_BLEND);
		glBlendFunc (GL_ONE, GL_ONE);
		glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glColor3f (entalpha, entalpha, entalpha);
		Fog_StartAdditive ();
		R_DrawTextureChains_Glow (model, ent, chain);
		Fog_StopAdditive ();
		glColor3f (1, 1, 1);
		glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable (GL_BLEND);
		glDepthMask (GL_TRUE);
	}*/
}

/*
=============
R_DrawWorld -- ericw -- moved from R_DrawTextureChains, which is no longer specific to the world.
=============
*/
void R_DrawWorld (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_DrawTextureChains (cl.worldmodel, NULL, chain_world);
}

/*
=============
R_DrawWorld_Water -- ericw -- moved from R_DrawTextureChains_Water, which is no longer specific to the world.
=============
*/
void R_DrawWorld_Water (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_DrawTextureChains_Water (cl.worldmodel, NULL, chain_world);
}

/*
=============
R_DrawWorld_ShowTris -- ericw -- moved from R_DrawTextureChains_ShowTris, which is no longer specific to the world.
=============
*/
void R_DrawWorld_ShowTris (void)
{
	if (!r_drawworld_cheatsafe)
		return;

	R_DrawTextureChains_ShowTris (cl.worldmodel, chain_world);
}
