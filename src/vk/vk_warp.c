/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 * Copyright (C) 2018-2019 Krzysztof Kondrak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * Warps. Used on water surfaces und for skybox rotation.
 *
 * =======================================================================
 */

#include "header/local.h"

static float	skyrotate;
static vec3_t	skyaxis;
static image_t	*sky_images[6];

static msurface_t	*warpface;

#define	SUBDIVIDE_SIZE	64

static void BoundPoly (int numverts, float *verts, vec3_t mins, vec3_t maxs)
{
	int		i, j;
	float	*v;

	mins[0] = mins[1] = mins[2] = 9999;
	maxs[0] = maxs[1] = maxs[2] = -9999;
	v = verts;
	for (i=0 ; i<numverts ; i++)
		for (j=0 ; j<3 ; j++, v++)
		{
			if (*v < mins[j])
				mins[j] = *v;
			if (*v > maxs[j])
				maxs[j] = *v;
		}
}

static void SubdividePolygon (int numverts, float *verts)
{
	int		i, j, k;
	vec3_t	mins, maxs;
	float	*v;
	vec3_t	front[64], back[64];
	int		f, b;
	float	dist[64];
	float	frac;
	vkpoly_t	*poly;
	vec3_t	total;
	float	total_s, total_t;

	if (numverts > 60)
		ri.Sys_Error (ERR_DROP, "%s: numverts = %i", __func__, numverts);

	BoundPoly (numverts, verts, mins, maxs);

	for (i=0 ; i<3 ; i++)
	{
		float	m;

		m = (mins[i] + maxs[i]) * 0.5;
		m = SUBDIVIDE_SIZE * floor (m/SUBDIVIDE_SIZE + 0.5);
		if (maxs[i] - m < 8)
			continue;
		if (m - mins[i] < 8)
			continue;

		// cut it
		v = verts + i;
		for (j=0 ; j<numverts ; j++, v+= 3)
			dist[j] = *v - m;

		// wrap cases
		dist[j] = dist[0];
		v-=i;
		VectorCopy(verts, v);

		f = b = 0;
		v = verts;
		for (j=0 ; j<numverts ; j++, v+= 3)
		{
			if (dist[j] >= 0)
			{
				VectorCopy(v, front[f]);
				f++;
			}
			if (dist[j] <= 0)
			{
				VectorCopy(v, back[b]);
				b++;
			}
			if (dist[j] == 0 || dist[j+1] == 0)
				continue;
			if ( (dist[j] > 0) != (dist[j+1] > 0) )
			{
				// clip point
				frac = dist[j] / (dist[j] - dist[j+1]);
				for (k=0 ; k<3 ; k++)
					front[f][k] = back[b][k] = v[k] + frac*(v[3+k] - v[k]);
				f++;
				b++;
			}
		}

		SubdividePolygon (f, front[0]);
		SubdividePolygon (b, back[0]);
		return;
	}

	// add a point in the center to help keep warp valid
	poly = Hunk_Alloc (sizeof(vkpoly_t) + ((numverts-4)+2) * VERTEXSIZE*sizeof(float));
	poly->next = warpface->polys;
	warpface->polys = poly;
	poly->numverts = numverts+2;
	VectorClear (total);
	total_s = 0;
	total_t = 0;
	for (i=0 ; i<numverts ; i++, verts+= 3)
	{
		float s, t;

		VectorCopy(verts, poly->verts[i+1]);
		s = DotProduct(verts, warpface->texinfo->vecs[0]);
		t = DotProduct(verts, warpface->texinfo->vecs[1]);

		total_s += s;
		total_t += t;
		VectorAdd (total, verts, total);

		poly->verts[i+1][3] = s;
		poly->verts[i+1][4] = t;
	}

	VectorScale(total, (1.0/numverts), poly->verts[0]);
	poly->verts[0][3] = total_s/numverts;
	poly->verts[0][4] = total_t/numverts;

	// copy first vertex to last
	memmove (poly->verts[i+1], poly->verts[1], sizeof(poly->verts[0]));
}

/*
================
Vk_SubdivideSurface

Breaks a polygon up along axial 64 unit
boundaries so that turbulent and sky warps
can be done reasonably.
================
*/
void Vk_SubdivideSurface (msurface_t *fa, model_t *loadmodel)
{
	vec3_t		verts[64];
	int			numverts;
	int			i;
	float		*vec;

	warpface = fa;

	//
	// convert edges back to a normal polygon
	//
	numverts = 0;
	for (i=0 ; i<fa->numedges ; i++)
	{
		int	lindex;

		lindex = loadmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
			vec = loadmodel->vertexes[loadmodel->edges[lindex].v[0]].position;
		else
			vec = loadmodel->vertexes[loadmodel->edges[-lindex].v[1]].position;
		VectorCopy(vec, verts[numverts]);
		numverts++;
	}

	SubdividePolygon (numverts, verts[0]);
}

//=========================================================

/*
=============
EmitWaterPolys

Does a water warp on the pre-fragmented vkpoly_t chain
=============
*/
void
EmitWaterPolys (msurface_t *fa, image_t *texture, const float *modelMatrix,
			  const float *color, qboolean solid_surface)
{
	vkpoly_t	*p, *bp;
	float		*v;
	int			i;

	struct {
		float model[16];
		float color[4];
		float time;
		float scroll;
	} polyUbo;

	polyUbo.color[0] = color[0];
	polyUbo.color[1] = color[1];
	polyUbo.color[2] = color[2];
	polyUbo.color[3] = color[3];
	polyUbo.time = r_newrefdef.time;

	if (fa->texinfo->flags & SURF_FLOWING)
		polyUbo.scroll = (-64 * ((r_newrefdef.time*0.5) - (int)(r_newrefdef.time*0.5))) / 64.f;
	else
		polyUbo.scroll = 0;

	if (modelMatrix)
	{
		memcpy(polyUbo.model, modelMatrix, sizeof(float) * 16);
	}
	else
	{
		Mat_Identity(polyUbo.model);
	}

	if (solid_surface)
	{
		// Solid surface
		QVk_BindPipeline(&vk_drawPolySolidWarpPipeline);
	}
	else
	{
		// Blend surface
		QVk_BindPipeline(&vk_drawPolyWarpPipeline);
	}

	uint32_t uboOffset;
	VkDescriptorSet uboDescriptorSet;
	uint8_t *uboData = QVk_GetUniformBuffer(sizeof(polyUbo), &uboOffset, &uboDescriptorSet);
	memcpy(uboData, &polyUbo, sizeof(polyUbo));

	VkBuffer vbo;
	VkDeviceSize vboOffset;
	VkDescriptorSet descriptorSets[] = { texture->vk_texture.descriptorSet, uboDescriptorSet };

	float gamma = 2.1F - vid_gamma->value;

	vkCmdPushConstants(vk_activeCmdbuffer, vk_drawTexQuadPipeline[vk_state.current_renderpass].layout,
		VK_SHADER_STAGE_FRAGMENT_BIT, 17 * sizeof(float), sizeof(gamma), &gamma);

	if (solid_surface)
	{
		// Solid surface
		vkCmdBindDescriptorSets(
			vk_activeCmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			vk_drawPolySolidWarpPipeline.layout, 0, 2,
			descriptorSets, 1, &uboOffset);
	}
	else
	{
		// Blend surface
		vkCmdBindDescriptorSets(
			vk_activeCmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			vk_drawPolyWarpPipeline.layout, 0, 2,
			descriptorSets, 1, &uboOffset);
	}

	for (bp = fa->polys; bp; bp = bp->next)
	{
		p = bp;

		if (Mesh_VertsRealloc(p->numverts))
		{
			ri.Sys_Error(ERR_FATAL, "%s: can't allocate memory", __func__);
		}

		for (i = 0, v = p->verts[0]; i < p->numverts; i++, v += VERTEXSIZE)
		{
			verts_buffer[i].vertex[0] = v[0];
			verts_buffer[i].vertex[1] = v[1];
			verts_buffer[i].vertex[2] = v[2];
			verts_buffer[i].texCoord[0] = v[3] / 64.f;
			verts_buffer[i].texCoord[1] = v[4] / 64.f;
		}

		uint8_t *vertData = QVk_GetVertexBuffer(sizeof(polyvert_t) * p->numverts, &vbo, &vboOffset);
		memcpy(vertData, verts_buffer, sizeof(polyvert_t) * p->numverts);

		vkCmdBindVertexBuffers(vk_activeCmdbuffer, 0, 1, &vbo, &vboOffset);
		vkCmdBindIndexBuffer(vk_activeCmdbuffer, QVk_GetTriangleFanIbo((p->numverts - 2) * 3), 0, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(vk_activeCmdbuffer, (p->numverts - 2) * 3, 1, 0, 0, 0);
	}
}


//===================================================================


static vec3_t	skyclip[6] = {
	{1,1,0},
	{1,-1,0},
	{0,-1,1},
	{0,1,1},
	{1,0,1},
	{-1,0,1}
};

// 1 = s, 2 = t, 3 = 2048
static int	st_to_vec[6][3] =
{
	{3,-1,2},
	{-3,1,2},

	{1,3,2},
	{-1,-3,2},

	{-2,-1,3},		// 0 degrees yaw, look straight up
	{2,-1,-3}		// look straight down

//	{-1,2,3},
//	{1,2,-3}
};

// s = [0]/[2], t = [1]/[2]
static int	vec_to_st[6][3] =
{
	{-2,3,1},
	{2,3,-1},

	{1,3,2},
	{-1,3,-2},

	{-2,-1,3},
	{-2,1,-3}

//	{-1,2,3},
//	{1,2,-3}
};

static float	skymins[2][6], skymaxs[2][6];
static float	sky_min, sky_max;

static void DrawSkyPolygon (int nump, vec3_t vecs)
{
	int		i;
	vec3_t	v, av;
	float	s, t, dv;
	int		axis;
	float	*vp;

	// decide which face it maps to
	VectorCopy(vec3_origin, v);
	for (i=0, vp=vecs ; i<nump ; i++, vp+=3)
	{
		VectorAdd (vp, v, v);
	}
	av[0] = fabs(v[0]);
	av[1] = fabs(v[1]);
	av[2] = fabs(v[2]);
	if (av[0] > av[1] && av[0] > av[2])
	{
		if (v[0] < 0)
			axis = 1;
		else
			axis = 0;
	}
	else if (av[1] > av[2] && av[1] > av[0])
	{
		if (v[1] < 0)
			axis = 3;
		else
			axis = 2;
	}
	else
	{
		if (v[2] < 0)
			axis = 5;
		else
			axis = 4;
	}

	// project new texture coords
	for (i=0 ; i<nump ; i++, vecs+=3)
	{
		int j;

		j = vec_to_st[axis][2];
		if (j > 0)
			dv = vecs[j - 1];
		else
			dv = -vecs[-j - 1];
		if (dv < 0.001)
			continue;	// don't divide by zero
		j = vec_to_st[axis][0];
		if (j < 0)
			s = -vecs[-j -1] / dv;
		else
			s = vecs[j-1] / dv;
		j = vec_to_st[axis][1];
		if (j < 0)
			t = -vecs[-j -1] / dv;
		else
			t = vecs[j-1] / dv;

		if (s < skymins[0][axis])
			skymins[0][axis] = s;
		if (t < skymins[1][axis])
			skymins[1][axis] = t;
		if (s > skymaxs[0][axis])
			skymaxs[0][axis] = s;
		if (t > skymaxs[1][axis])
			skymaxs[1][axis] = t;
	}
}

#define	ON_EPSILON		0.1			// point on plane side epsilon
#define	MAX_CLIP_VERTS	64
static void ClipSkyPolygon (int nump, vec3_t vecs, int stage)
{
	float	*norm;
	float	*v;
	qboolean	front, back;
	float	d, e;
	float	dists[MAX_CLIP_VERTS];
	int		sides[MAX_CLIP_VERTS];
	vec3_t	newv[2][MAX_CLIP_VERTS];
	int		newc[2];
	int		i, j;

	if (nump > MAX_CLIP_VERTS-2)
		ri.Sys_Error (ERR_DROP, "%s: MAX_CLIP_VERTS", __func__);
	if (stage == 6)
	{	// fully clipped, so draw it
		DrawSkyPolygon (nump, vecs);
		return;
	}

	front = back = false;
	norm = skyclip[stage];
	for (i=0, v = vecs ; i<nump ; i++, v+=3)
	{
		d = DotProduct(v, norm);
		if (d > ON_EPSILON)
		{
			front = true;
			sides[i] = SIDE_FRONT;
		}
		else if (d < -ON_EPSILON)
		{
			back = true;
			sides[i] = SIDE_BACK;
		}
		else
			sides[i] = SIDE_ON;
		dists[i] = d;
	}

	if (!front || !back)
	{	// not clipped
		ClipSkyPolygon (nump, vecs, stage+1);
		return;
	}

	// clip it
	sides[i] = sides[0];
	dists[i] = dists[0];
	VectorCopy(vecs, (vecs+(i*3)) );
	newc[0] = newc[1] = 0;

	for (i=0, v = vecs ; i<nump ; i++, v+=3)
	{
		switch (sides[i])
		{
		case SIDE_FRONT:
			VectorCopy(v, newv[0][newc[0]]);
			newc[0]++;
			break;
		case SIDE_BACK:
			VectorCopy(v, newv[1][newc[1]]);
			newc[1]++;
			break;
		case SIDE_ON:
			VectorCopy(v, newv[0][newc[0]]);
			newc[0]++;
			VectorCopy(v, newv[1][newc[1]]);
			newc[1]++;
			break;
		}

		if (sides[i] == SIDE_ON || sides[i+1] == SIDE_ON || sides[i+1] == sides[i])
			continue;

		d = dists[i] / (dists[i] - dists[i+1]);
		for (j=0 ; j<3 ; j++)
		{
			e = v[j] + d*(v[j+3] - v[j]);
			newv[0][newc[0]][j] = e;
			newv[1][newc[1]][j] = e;
		}
		newc[0]++;
		newc[1]++;
	}

	// continue
	ClipSkyPolygon (newc[0], newv[0][0], stage+1);
	ClipSkyPolygon (newc[1], newv[1][0], stage+1);
}

/*
=================
R_AddSkySurface
=================
*/
void R_AddSkySurface (msurface_t *fa)
{
	int			i;
	vec3_t		verts[MAX_CLIP_VERTS];
	vkpoly_t	*p;

	// calculate vertex values for sky box
	for (p=fa->polys ; p ; p=p->next)
	{
		for (i=0 ; i<p->numverts ; i++)
		{
			VectorSubtract(p->verts[i], r_origin, verts[i]);
		}
		ClipSkyPolygon (p->numverts, verts[0], 0);
	}
}


/*
==============
R_ClearSkyBox
==============
*/
void R_ClearSkyBox (void)
{
	int		i;

	for (i=0 ; i<6 ; i++)
	{
		skymins[0][i] = skymins[1][i] = 9999;
		skymaxs[0][i] = skymaxs[1][i] = -9999;
	}
}


static void MakeSkyVec (float s, float t, int axis, float *vertexData)
{
	vec3_t		v, b;
	int			j;

	float dist = (r_farsee->value == 0) ? 2300.0f : 4096.0f;

	b[0] = s * dist;
	b[1] = t * dist;
	b[2] = dist;

	for (j = 0; j<3; j++)
	{
		int k;

		k = st_to_vec[axis][j];
		if (k < 0)
			v[j] = -b[-k - 1];
		else
			v[j] = b[k - 1];
	}

	// avoid bilerp seam
	s = (s + 1)*0.5;
	t = (t + 1)*0.5;

	if (s < sky_min)
		s = sky_min;
	else if (s > sky_max)
		s = sky_max;
	if (t < sky_min)
		t = sky_min;
	else if (t > sky_max)
		t = sky_max;

	t = 1.0 - t;

	vertexData[0] = v[0];
	vertexData[1] = v[1];
	vertexData[2] = v[2];
	vertexData[3] = s;
	vertexData[4] = t;
}

/*
==============
R_DrawSkyBox
==============
*/
static int	skytexorder[6] = {0,2,1,3,4,5};
void R_DrawSkyBox (void)
{
	int		i;

	if (skyrotate)
	{	// check for no sky at all
		for (i = 0; i<6; i++)
			if (skymins[0][i] < skymaxs[0][i]
				&& skymins[1][i] < skymaxs[1][i])
				break;
		if (i == 6)
			return;		// nothing visible
	}

	float model[16];
	Mat_Identity(model);
	Mat_Rotate(model, r_newrefdef.time * skyrotate, skyaxis[0], skyaxis[1], skyaxis[2]);
	Mat_Translate(model, r_origin[0], r_origin[1], r_origin[2]);

	struct {
		float data[5];
	} skyVerts[4];

	QVk_BindPipeline(&vk_drawSkyboxPipeline);
	uint32_t uboOffset;
	VkDescriptorSet uboDescriptorSet;
	uint8_t *uboData = QVk_GetUniformBuffer(sizeof(model), &uboOffset, &uboDescriptorSet);
	memcpy(uboData, model, sizeof(model));

	for (i = 0; i<6; i++)
	{
		if (skyrotate)
		{	// hack, forces full sky to draw when rotating
			skymins[0][i] = -1;
			skymins[1][i] = -1;
			skymaxs[0][i] = 1;
			skymaxs[1][i] = 1;
		}

		if (skymins[0][i] >= skymaxs[0][i]
			|| skymins[1][i] >= skymaxs[1][i])
			continue;

		MakeSkyVec(skymins[0][i], skymins[1][i], i, skyVerts[0].data);
		MakeSkyVec(skymins[0][i], skymaxs[1][i], i, skyVerts[1].data);
		MakeSkyVec(skymaxs[0][i], skymaxs[1][i], i, skyVerts[2].data);
		MakeSkyVec(skymaxs[0][i], skymins[1][i], i, skyVerts[3].data);

		float verts[] = {
			skyVerts[0].data[0], skyVerts[0].data[1], skyVerts[0].data[2], skyVerts[0].data[3], skyVerts[0].data[4],
			skyVerts[1].data[0], skyVerts[1].data[1], skyVerts[1].data[2], skyVerts[1].data[3], skyVerts[1].data[4],
			skyVerts[2].data[0], skyVerts[2].data[1], skyVerts[2].data[2], skyVerts[2].data[3], skyVerts[2].data[4],
			skyVerts[0].data[0], skyVerts[0].data[1], skyVerts[0].data[2], skyVerts[0].data[3], skyVerts[0].data[4],
			skyVerts[2].data[0], skyVerts[2].data[1], skyVerts[2].data[2], skyVerts[2].data[3], skyVerts[2].data[4],
			skyVerts[3].data[0], skyVerts[3].data[1], skyVerts[3].data[2], skyVerts[3].data[3], skyVerts[3].data[4]
		};

		VkBuffer vbo;
		VkDeviceSize vboOffset;
		uint8_t *vertData = QVk_GetVertexBuffer(sizeof(verts), &vbo, &vboOffset);
		memcpy(vertData, verts, sizeof(verts));

		VkDescriptorSet descriptorSets[] = { sky_images[skytexorder[i]]->vk_texture.descriptorSet, uboDescriptorSet };

		float gamma = 2.1F - vid_gamma->value;

		vkCmdPushConstants(vk_activeCmdbuffer, vk_drawTexQuadPipeline[vk_state.current_renderpass].layout,
			VK_SHADER_STAGE_FRAGMENT_BIT, 17 * sizeof(float), sizeof(gamma), &gamma);

		vkCmdBindDescriptorSets(vk_activeCmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			vk_drawSkyboxPipeline.layout, 0, 2, descriptorSets, 1, &uboOffset);
		vkCmdBindVertexBuffers(vk_activeCmdbuffer, 0, 1, &vbo, &vboOffset);
		vkCmdDraw(vk_activeCmdbuffer, 6, 1, 0, 0);
	}
}

/*
============
RE_SetSky
============
*/
// 3dstudio environment map names
static char	*suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};

void
RE_SetSky(const char *name, float rotate, vec3_t axis)
{
	char	skyname[MAX_QPATH];
	int		i;

	strncpy(skyname, name, sizeof(skyname) - 1);
	skyrotate = rotate;
	VectorCopy(axis, skyaxis);

	for (i = 0; i<6; i++)
	{
		image_t	*image;

		image = (image_t *)GetSkyImage(skyname, suf[i],
			r_palettedtexture->value, (findimage_t)Vk_FindImage);

		if (!image)
		{
			R_Printf(PRINT_ALL, "%s: can't load %s:%s sky\n",
				__func__, skyname, suf[i]);
			image = r_notexture;
		}

		sky_images[i] = image;
	}

	sky_min = 1.0 / 512;
	sky_max = 511.0 / 512;
}
