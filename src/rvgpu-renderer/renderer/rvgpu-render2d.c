// SPDX-License-Identifier: Apache-2.0
/**
 * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <rvgpu-renderer/renderer/rvgpu-render2d.h>

/* ------------------------------------------------------ *
 *  shader for FillColor
 * ------------------------------------------------------ */
static char vs_fill[] = "                             \n\
                                                      \n\
attribute    vec4    a_Vertex;                        \n\
uniform      mat4    u_PMVMatrix;                     \n\
void main (void)                                      \n\
{                                                     \n\
    gl_Position = u_PMVMatrix * a_Vertex;             \n\
}                                                     ";

static char fs_fill[] = "                             \n\
                                                      \n\
precision mediump float;                              \n\
uniform      vec4    u_Color;                         \n\
                                                      \n\
void main (void)                                      \n\
{                                                     \n\
    gl_FragColor = u_Color;                           \n\
}                                                       ";

/* ------------------------------------------------------ *
 *  shader for Texture
 * ------------------------------------------------------ */
static char vs_tex[] = "                              \n\
attribute    vec4    a_Vertex;                        \n\
attribute    vec2    a_TexCoord;                      \n\
varying      vec2    v_TexCoord;                      \n\
uniform      mat4    u_PMVMatrix;                     \n\
                                                      \n\
void main (void)                                      \n\
{                                                     \n\
    gl_Position = u_PMVMatrix * a_Vertex;             \n\
    v_TexCoord  = a_TexCoord;                         \n\
}                                                     \n";

static char fs_tex[] = "                              \n\
precision mediump float;                              \n\
varying     vec2      v_TexCoord;                     \n\
uniform     sampler2D u_sampler;                      \n\
uniform     vec4      u_Color;                        \n\
                                                      \n\
void main (void)                                      \n\
{                                                     \n\
    gl_FragColor = texture2D (u_sampler, v_TexCoord); \n\
    gl_FragColor *= u_Color;                          \n\
}                                                     \n";

typedef struct shader_obj_t {
	GLuint program;
	GLint loc_vtx;
	GLint loc_nrm;
	GLint loc_clr;
	GLint loc_uv;
	GLint loc_tex;
	GLint loc_mtx;
	GLint loc_mtx_nrm;
} shader_obj_t;

/* ----------------------------------------------------------- *
 *   create & compile shader
 * ----------------------------------------------------------- */

GLuint compile_shader_text(GLenum shaderType, const char *text)
{
	GLuint shader;
	GLint stat;

	shader = glCreateShader(shaderType);
	glShaderSource(shader, 1, (const char **)&text, NULL);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &stat);
	if (!stat) {
		GLsizei len;
		char *lpBuf;

		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
		lpBuf = (char *)malloc(len);

		glGetShaderInfoLog(shader, len, &len, lpBuf);

		free(lpBuf);

		return 0;
	}

	return shader;
}

/* ----------------------------------------------------------- *
 *    link shaders
 * ----------------------------------------------------------- */
GLuint link_shaders(GLuint vertShader, GLuint fragShader)
{
	GLuint program = glCreateProgram();

	if (fragShader)
		glAttachShader(program, fragShader);
	if (vertShader)
		glAttachShader(program, vertShader);

	glLinkProgram(program);

	{
		GLint stat;
		glGetProgramiv(program, GL_LINK_STATUS, &stat);
		if (!stat) {
			GLsizei len;
			char *lpBuf;

			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
			lpBuf = (char *)malloc(len);

			glGetProgramInfoLog(program, len, &len, lpBuf);
			free(lpBuf);

			return 0;
		}
	}

	return program;
}

int generate_shader(shader_obj_t *sobj, char *str_vs, char *str_fs)
{
	GLuint fs, vs, program;

	vs = compile_shader_text(GL_VERTEX_SHADER, str_vs);
	fs = compile_shader_text(GL_FRAGMENT_SHADER, str_fs);
	if (vs == 0 || fs == 0) {
		return -1;
	}

	program = link_shaders(vs, fs);
	if (program == 0) {
		return -1;
	}

	glDeleteShader(vs);
	glDeleteShader(fs);

	sobj->program = program;
	sobj->loc_vtx = glGetAttribLocation(program, "a_Vertex");
	sobj->loc_nrm = glGetAttribLocation(program, "a_Normal");
	sobj->loc_clr = glGetAttribLocation(program, "a_Color");
	sobj->loc_uv = glGetAttribLocation(program, "a_TexCoord");
	sobj->loc_tex = glGetUniformLocation(program, "u_sampler");
	sobj->loc_mtx = glGetUniformLocation(program, "u_PMVMatrix");
	sobj->loc_mtx_nrm = glGetUniformLocation(program, "u_NrmMatrix");

	return 0;
}

enum shader_type {
	SHADER_TYPE_FILL = 0, // 0
	SHADER_TYPE_TEX, // 1

	SHADER_TYPE_MAX
};

#define SHADER_NUM SHADER_TYPE_MAX
static char *s_shader[SHADER_NUM * 2] = {
	vs_fill,
	fs_fill,
	vs_tex,
	fs_tex,
};

static shader_obj_t s_sobj[SHADER_NUM];
static int s_loc_mtx[SHADER_NUM];
static int s_loc_color[SHADER_NUM];
static int s_loc_texdim[SHADER_NUM];

void matrix_identity(float *m)
{
	m[0] = 1.0f;
	m[4] = 0.0f;
	m[8] = 0.0f;
	m[12] = 0.0f;
	m[1] = 0.0f;
	m[5] = 1.0f;
	m[9] = 0.0f;
	m[13] = 0.0f;
	m[2] = 0.0f;
	m[6] = 0.0f;
	m[10] = 1.0f;
	m[14] = 0.0f;
	m[3] = 0.0f;
	m[7] = 0.0f;
	m[11] = 0.0f;
	m[15] = 1.0f;
}

/************************************************************               Translate Matrix
   M = M * T                                                                                                                                          | m00  m04  m08  m12 |   | 1  0  0  x |   | m00  m04  m08  (m00*x + m04*y + m08*z + m12) |
    | m01  m05  m09  m13 | * | 0  1  0  y | = | m01  m05  m09  (m01*x + m05*y + m09*z + m13) |
    | m02  m06  m10  m14 |   | 0  0  1  z |   | m02  m06  m10  (m02*x + m06*y + m10*z + m14) |
    | m03  m07  m11  m15 |   | 0  0  0  1 |   | m03  m07  m11  (m03*x + m07*y + m11*z + m15) |
***********************************************************/
void matrix_translate(float *m, float x, float y, float z)
{
	float m00, m01, m02, m03;
	float m04, m05, m06, m07;
	float m08, m09, m10, m11;
	float m12, m13, m14, m15;

	m00 = m[0];
	m04 = m[4];
	m08 = m[8]; /* m12 = m[12]; */
	m01 = m[1];
	m05 = m[5];
	m09 = m[9]; /* m13 = m[13]; */
	m02 = m[2];
	m06 = m[6];
	m10 = m[10]; /* m14 = m[14]; */
	m03 = m[3];
	m07 = m[7];
	m11 = m[11]; /* m15 = m[15]; */

	m12 = m[12];
	m13 = m[13];
	m14 = m[14];
	m15 = m[15];

	m12 += m08 * z;
	m13 += m09 * z;
	m14 += m10 * z;
	m15 += m11 * z;

	m12 += m04 * y;
	m13 += m05 * y;
	m14 += m06 * y;
	m15 += m07 * y;

	m12 += m00 * x;
	m13 += m01 * x;
	m14 += m02 * x;
	m15 += m03 * x;

	m[12] = m12;
	m[13] = m13;
	m[14] = m14;
	m[15] = m15;
}

static void turn_x(float *m, float cosA, float sinA)
{
	float m01, m02;
	float m11, m12;
	float m21, m22;
	float m31, m32;
	float mx01, mx02;
	float mx11, mx12;
	float mx21, mx22;
	float mx31, mx32;

	m01 = m[4]; //m->m01;
	m02 = m[8]; //m->m02;
	m11 = m[5]; //m->m11;
	m12 = m[9]; //m->m12;
	m21 = m[6]; //m->m21;
	m22 = m[10]; //m->m22;
	m31 = m[7]; //m->m31;
	m32 = m[11]; //m->m32;

	mx01 = cosA * m01;
	mx02 = sinA * m01;

	mx11 = cosA * m11;
	mx12 = sinA * m11;

	mx21 = cosA * m21;
	mx22 = sinA * m21;

	mx31 = cosA * m31;
	mx32 = sinA * m31;

	mx01 = sinA * m02 + mx01;
	mx02 = cosA * m02 - mx02;

	mx11 = sinA * m12 + mx11;
	mx12 = cosA * m12 - mx12;

	mx21 = sinA * m22 + mx21;
	mx22 = cosA * m22 - mx22;

	mx31 = sinA * m32 + mx31;
	mx32 = cosA * m32 - mx32;

	m[4] = mx01;
	m[8] = mx02;

	m[5] = mx11;
	m[9] = mx12;

	m[6] = mx21;
	m[10] = mx22;

	m[7] = mx31;
	m[11] = mx32;
}

/*                                                                        * void turn_y(float *m, float cosA, float cosB)
 * local rotation around Y-axis
 * M = M * Ry
 *
 * | m00 m01 m02 m03 |   | m00 m01 m02 m03 |   |  cosA 0 sinA 0 |
 * | m10 m11 m12 m13 | = | m10 m11 m12 m13 | * |   0   1   0  0 |
 * | m20 m21 m22 m23 |   | m20 m21 m22 m23 |   | -sinA 0 cosA 0 |
 * | m30 m31 m32 m33 |   | m30 m31 m32 m33 |   |   0     0  0 1 |
 */
static void turn_y(float *m, float cosA, float sinA)
{
	float m00, m02;
	float m10, m12;
	float m20, m22;
	float m30, m32;
	float mx00, mx02;
	float mx10, mx12;
	float mx20, mx22;
	float mx30, mx32;

	m00 = m[0]; //m->m00;
	m02 = m[8]; //m->m02;
	m10 = m[1]; //m->m10;
	m12 = m[9]; //m->m12;
	m20 = m[2]; //m->m20;
	m22 = m[10]; //m->m22;
	m30 = m[3]; //m->m30;
	m32 = m[11]; //m->m32;

	mx00 = cosA * m00;
	mx02 = sinA * m00;

	mx10 = cosA * m10;
	mx12 = sinA * m10;

	mx20 = cosA * m20;
	mx22 = sinA * m20;

	mx30 = cosA * m30;
	mx32 = sinA * m30;

	mx00 = -sinA * m02 + mx00;
	mx02 = cosA * m02 + mx02;

	mx10 = -sinA * m12 + mx10;
	mx12 = cosA * m12 + mx12;

	mx20 = -sinA * m22 + mx20;
	mx22 = cosA * m22 + mx22;

	mx30 = -sinA * m32 + mx30;
	mx32 = cosA * m32 + mx32;

	m[0] = mx00;
	m[8] = mx02;

	m[1] = mx10;
	m[9] = mx12;

	m[2] = mx20;
	m[10] = mx22;

	m[3] = mx30;
	m[11] = mx32;
}

/*
 * void turn_z(float *m, float cosA, float sinA)
 * local rotation around Z-axis
 * M = M * Rz
 *
 * | m00 m01 m02 m03 |   | m00 m01 m02 m03 |   | cosA -sinA 0 0 |
 * | m10 m11 m12 m13 | = | m10 m11 m12 m13 | * | sinA  cosA 0 0 |
 * | m20 m21 m22 m23 |   | m20 m21 m22 m23 |   |   0     0  1 0 |
 * | m30 m31 m32 m33 |   | m30 m31 m32 m33 |   |   0     0  0 1 |
 *
 */
static void turn_z(float *m, float cosA, float sinA)
{
	float m00, m01;
	float m10, m11;
	float m20, m21;
	float m30, m31;
	float mx00, mx01;
	float mx10, mx11;
	float mx20, mx21;
	float mx30, mx31;

	m00 = m[0]; //m->m00;
	m01 = m[4]; //m->m01;
	m10 = m[1]; //m->m10;
	m11 = m[5]; //m->m11;
	m20 = m[2]; //m->m20;
	m21 = m[6]; //m->m21;
	m30 = m[3]; //m->m30;
	m31 = m[7]; //m->m31;

	mx00 = cosA * m00;
	mx01 = sinA * m00;

	mx10 = cosA * m10;
	mx11 = sinA * m10;

	mx20 = cosA * m20;
	mx21 = sinA * m20;

	mx30 = cosA * m30;
	mx31 = sinA * m30;

	mx00 = sinA * m01 + mx00;
	mx01 = cosA * m01 - mx01;

	mx10 = sinA * m11 + mx10;
	mx11 = cosA * m11 - mx11;

	mx20 = sinA * m21 + mx20;
	mx21 = cosA * m21 - mx21;

	mx30 = sinA * m31 + mx30;
	mx31 = cosA * m31 - mx31;

	m[0] = mx00;
	m[4] = mx01;
	m[1] = mx10;
	m[5] = mx11;
	m[2] = mx20;
	m[6] = mx21;
	m[3] = mx30;
	m[7] = mx31;
}

float vec3_normalize(float *v)
{
	float len, invLen;

	len = (float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len == 0.0f) {
		return 0.0f;
	}
	invLen = 1.0f / len;

	v[0] *= invLen;
	v[1] *= invLen;
	v[2] *= invLen;

	return len;
}

/************************************************************
  Rotate Matrix
    | m00  m04  m08  m12 |   | r00  r04  r08   0 |
    | m01  m05  m09  m13 | * | r01  r05  r09   0 |
    | m02  m06  m10  m14 |   | r02  r06  r10   0 |
    | m03  m07  m11  m15 |   |   0    0    0   1 |

    m00 = m00*r00 + m04*r01 + m08*r02
    m01 = m01*r00 + m05*r01 + m09*r02
    m02 = m02*r00 + m06*r01 + m10*r02
    m03 = m03*r00 + m07*r01 + m11*r02

    m04 = m00*r04 + m04*r05 + m08*r06
    m05 = m01*r04 + m05*r05 + m09*r06
    m06 = m02*r04 + m06*r05 + m10*r06
    m07 = m03*r04 + m07*r05 + m11*r06

    m08 = m00*r08 + m04*r09 + m08*r10
    m09 = m01*r08 + m05*r09 + m09*r10
    m10 = m02*r08 + m06*r09 + m10*r10
    m11 = m03*r08 + m07*r09 + m11*r10

    m12 = m12
    m13 = m13
    m14 = m14
    m15 = m15
***********************************************************/
void matrix_rotate(float *m, float angle, float x, float y, float z)
{
	float v[3], angleRadian;
	float sinA, cosA, cosA2;
	float xcosA2, ycosA2, zcosA2;
	float xsinA, ysinA, zsinA;
	angleRadian = angle * M_PId180f;
	sinA = (float)sin(angleRadian);
	cosA = (float)cos(angleRadian);

	/* for fast rotation around X-Axis/Y-Axis,and Z-Axis */
	if (x == 0.0f && y == 0.0f && z != 0.0f) {
		if (z < 0.0f) {
			/* If the Axis of the Rotation is minus, Rotate Backwords */
			sinA = -sinA;
		}
		/* Z Axis Rotateion */
		turn_z(m, cosA, sinA);
		return;
	} else if (x == 0.0f && y != 0.0f && z == 0.0f) {
		if (y < 0.0f) {
			/* If the Axis of the Rotation is minus, Rotate Backwords */
			sinA = -sinA;
		}
		/* Y Axis Rotation */
		turn_y(m, cosA, sinA);
		return;
	} else if (x != 0.0f && y == 0.0f && z == 0.0f) {
		if (x < 0.0f) {
			/* If the Axis of the Rotation is minus, Rotate Backwords */
			sinA = -sinA;
		}
		/* X Axis Rotation */
		turn_x(m, cosA, sinA);
		return;
	}

	{
		float r00, r01, r02;
		float r10, r11, r12;
		float r20, r21, r22;

		/* normalization of 3D-vector */
		v[0] = x;
		v[1] = y;
		v[2] = z;
		vec3_normalize(v);

		x = v[0];
		y = v[1];
		z = v[2];

		/* making rotation matrix */
		cosA2 = 1.0f - cosA;
		xsinA = x * sinA;
		ysinA = y * sinA;
		zsinA = z * sinA;
		xcosA2 = x * cosA2;
		ycosA2 = y * cosA2;
		zcosA2 = z * cosA2;

		r00 = x * xcosA2 + cosA;
		r10 = y * xcosA2 + zsinA;
		r20 = z * xcosA2 - ysinA;

		r01 = x * ycosA2 - zsinA;
		r11 = y * ycosA2 + cosA;
		r21 = z * ycosA2 + xsinA;

		r02 = x * zcosA2 + ysinA;
		r12 = y * zcosA2 - xsinA;
		r22 = z * zcosA2 + cosA;

		/* multing with 3x3 rotating matrix. */
		{
			float fm0, fm1, fm2;
			float mx, my, mz;

			/* load 0th low of "m" */
			fm0 = m[0];
			fm1 = m[4];
			fm2 = m[8]; /* fm3 = m[12]; */

			mx = fm0 * r00;
			my = fm0 * r01;
			mz = fm0 * r02;

			mx += fm1 * r10;
			my += fm1 * r11;
			mz += fm1 * r12;

			mx += fm2 * r20;
			my += fm2 * r21;
			mz += fm2 * r22;

			fm0 = m[1];
			fm1 = m[5];
			fm2 = m[9]; /* fm3 = m[13]; */

			m[0] = mx;
			m[4] = my;
			m[8] = mz;

			/* *************************** */
			mx = fm0 * r00;
			my = fm0 * r01;
			mz = fm0 * r02;

			mx += fm1 * r10;
			my += fm1 * r11;
			mz += fm1 * r12;

			mx += fm2 * r20;
			my += fm2 * r21;
			mz += fm2 * r22;

			fm0 = m[2];
			fm1 = m[6];
			fm2 = m[10]; /* m23 = m[14]; */

			m[1] = mx;
			m[5] = my;
			m[9] = mz;

			/* *************************** */
			mx = fm0 * r00;
			my = fm0 * r01;
			mz = fm0 * r02;

			mx += fm1 * r10;
			my += fm1 * r11;
			mz += fm1 * r12;

			mx += fm2 * r20;
			my += fm2 * r21;
			mz += fm2 * r22;

			fm0 = m[3];
			fm1 = m[7];
			fm2 = m[11]; /* m33 = m[15]; */

			m[2] = mx;
			m[6] = my;
			m[10] = mz;

			/* *************************** */
			mx = fm0 * r00;
			my = fm0 * r01;
			mz = fm0 * r02;

			mx += fm1 * r10;
			my += fm1 * r11;
			mz += fm1 * r12;

			mx += fm2 * r20;
			my += fm2 * r21;
			mz += fm2 * r22;

			m[3] = mx;
			m[7] = my;
			m[11] = mz;
		}
	}
}

/******************************************
   Scale Matrix
    | m00  m04  m08  m12 |   | x  0  0  0 |
    | m01  m05  m09  m13 | * | 0  y  0  0 |
    | m02  m06  m10  m14 |   | 0  0  z  0 |
    | m03  m07  m11  m15 |   | 0  0  0  1 |
*******************************************/
void matrix_scale(float *m, float x, float y, float z)
{
	float m00, m01, m02, m03;
	float m04, m05, m06, m07;
	float m08, m09, m10, m11;
	/* float m12, m13, m14, m15; */

	m00 = m[0];
	m04 = m[4];
	m08 = m[8]; /* m12 = m[12]; */
	m01 = m[1];
	m05 = m[5];
	m09 = m[9]; /* m13 = m[13]; */
	m02 = m[2];
	m06 = m[6];
	m10 = m[10]; /* m14 = m[14]; */
	m03 = m[3];
	m07 = m[7];
	m11 = m[11]; /* m15 = m[15]; */

	m00 = m00 * x;
	m04 = m04 * y;
	m08 = m08 * z;

	m01 = m01 * x;
	m05 = m05 * y;
	m09 = m09 * z;

	m02 = m02 * x;
	m06 = m06 * y;
	m10 = m10 * z;

	m03 = m03 * x;
	m07 = m07 * y;
	m11 = m11 * z;

	m[0] = m00;
	m[4] = m04;
	m[8] = m08;

	m[1] = m01;
	m[5] = m05;
	m[9] = m09;

	m[2] = m02;
	m[6] = m06;
	m[10] = m10;

	m[3] = m03;
	m[7] = m07;
	m[11] = m11;
}

/******************************************
   Multiply Matrix
     M = M1 * M2
*******************************************/
void matrix_mult(float *m, float *m1, float *m2)
{
	float fm0, fm1, fm2, fm3;
	float fpm00, fpm01, fpm02, fpm03;
	float fpm10, fpm11, fpm12, fpm13;
	float fpm20, fpm21, fpm22, fpm23;
	float fpm30, fpm31, fpm32, fpm33;
	float x, y, z, w;

	/* load pMb */
	fpm00 = m2[0];
	fpm01 = m2[4];
	fpm02 = m2[8];
	fpm03 = m2[12];

	fpm10 = m2[1];
	fpm11 = m2[5];
	fpm12 = m2[9];
	fpm13 = m2[13];

	fpm20 = m2[2];
	fpm21 = m2[6];
	fpm22 = m2[10];
	fpm23 = m2[14];

	fpm30 = m2[3];
	fpm31 = m2[7];
	fpm32 = m2[11];
	fpm33 = m2[15];

	/*  process 0-line of "m1" */
	fm0 = m1[0];
	fm1 = m1[4];
	fm2 = m1[8];
	fm3 = m1[12];

	x = fm0 * fpm00;
	y = fm0 * fpm01;
	z = fm0 * fpm02;
	w = fm0 * fpm03;

	x += fm1 * fpm10;
	y += fm1 * fpm11;
	z += fm1 * fpm12;
	w += fm1 * fpm13;

	x += fm2 * fpm20;
	y += fm2 * fpm21;
	z += fm2 * fpm22;
	w += fm2 * fpm23;

	x += fm3 * fpm30;
	y += fm3 * fpm31;
	z += fm3 * fpm32;
	w += fm3 * fpm33;

	fm0 = m1[1];
	fm1 = m1[5];
	fm2 = m1[9];
	fm3 = m1[13];

	m[0] = x;
	m[4] = y;
	m[8] = z;
	m[12] = w;

	/* *************************** */
	x = fm0 * fpm00;
	y = fm0 * fpm01;
	z = fm0 * fpm02;
	w = fm0 * fpm03;

	x += fm1 * fpm10;
	y += fm1 * fpm11;
	z += fm1 * fpm12;
	w += fm1 * fpm13;

	x += fm2 * fpm20;
	y += fm2 * fpm21;
	z += fm2 * fpm22;
	w += fm2 * fpm23;

	x += fm3 * fpm30;
	y += fm3 * fpm31;
	z += fm3 * fpm32;
	w += fm3 * fpm33;

	fm0 = m1[2];
	fm1 = m1[6];
	fm2 = m1[10];
	fm3 = m1[14];

	m[1] = x;
	m[5] = y;
	m[9] = z;
	m[13] = w;

	/* *************************** */
	x = fm0 * fpm00;
	y = fm0 * fpm01;
	z = fm0 * fpm02;
	w = fm0 * fpm03;

	x += fm1 * fpm10;
	y += fm1 * fpm11;
	z += fm1 * fpm12;
	w += fm1 * fpm13;

	x += fm2 * fpm20;
	y += fm2 * fpm21;
	z += fm2 * fpm22;
	w += fm2 * fpm23;

	x += fm3 * fpm30;
	y += fm3 * fpm31;
	z += fm3 * fpm32;
	w += fm3 * fpm33;

	fm0 = m1[3];
	fm1 = m1[7];
	fm2 = m1[11];
	fm3 = m1[15];

	m[2] = x;
	m[6] = y;
	m[10] = z;
	m[14] = w;

	/* *************************** */
	x = fm0 * fpm00;
	y = fm0 * fpm01;
	z = fm0 * fpm02;
	w = fm0 * fpm03;

	x += fm1 * fpm10;
	y += fm1 * fpm11;
	z += fm1 * fpm12;
	w += fm1 * fpm13;

	x += fm2 * fpm20;
	y += fm2 * fpm21;
	z += fm2 * fpm22;
	w += fm2 * fpm23;

	x += fm3 * fpm30;
	y += fm3 * fpm31;
	z += fm3 * fpm32;
	w += fm3 * fpm33;

	m[3] = x;
	m[7] = y;
	m[11] = z;
	m[15] = w;
}

static float varray[] = { 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0, 1.0 };

static float s_matprj[16];
int set_2d_projection_matrix(int w, int h)
{
	float mat_proj[] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f,
			     0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 1.0f };

	mat_proj[0] = 2.0f / (float)w;
	mat_proj[5] = -2.0f / (float)h;

	memcpy(s_matprj, mat_proj, 16 * sizeof(float));

	return 0;
}

int init_2d_renderer(int w, int h)
{
	int i;

	for (i = 0; i < SHADER_NUM; i++) {
		if (generate_shader(&s_sobj[i], s_shader[2 * i],
				    s_shader[2 * i + 1]) < 0) {
			fprintf(stderr, "%s\n", __FUNCTION__);
			return -1;
		}

		s_loc_mtx[i] =
			glGetUniformLocation(s_sobj[i].program, "u_PMVMatrix");
		s_loc_color[i] =
			glGetUniformLocation(s_sobj[i].program, "u_Color");
		s_loc_texdim[i] =
			glGetUniformLocation(s_sobj[i].program, "u_TexDim");
	}

	set_2d_projection_matrix(w, h);

	return 0;
}

typedef struct _texparam {
	int textype;
	int texid;
	double x, y, w, h;
	int texw, texh;
	int upsidedown;
	float color[4];
	float rot; /* degree */
	float px, py; /* pivot */
	int blendfunc_en;
	unsigned int blendfunc[4]; /* src_rgb, dst_rgb, src_alpha, dst_alpha */
	float *user_texcoord;
} texparam_t;

static void flip_texcoord(float *uv, unsigned int flip_mode)
{
	if (flip_mode & RENDER2D_FLIP_V) {
		uv[1] = 1.0f - uv[1];
		uv[3] = 1.0f - uv[3];
		uv[5] = 1.0f - uv[5];
		uv[7] = 1.0f - uv[7];
	}

	if (flip_mode & RENDER2D_FLIP_H) {
		uv[0] = 1.0f - uv[0];
		uv[2] = 1.0f - uv[2];
		uv[4] = 1.0f - uv[4];
		uv[6] = 1.0f - uv[6];
	}
}

static int draw_2d_texture_in(texparam_t *tparam)
{
	int ttype = tparam->textype;
	int texid = tparam->texid;
	float x = tparam->x;
	float y = tparam->y;
	float w = tparam->w;
	float h = tparam->h;
	float rot = tparam->rot;
	shader_obj_t *sobj = &s_sobj[ttype];
	float matrix[16];
	float tarray[] = { 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0, 1.0 };
	float *uv = tarray;

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	glUseProgram(sobj->program);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(sobj->loc_tex, 0);

	switch (ttype) {
	case SHADER_TYPE_FILL:
		break;
	case SHADER_TYPE_TEX:
		glBindTexture(GL_TEXTURE_2D, texid);
		break;
	default:
		break;
	}

	flip_texcoord(uv, tparam->upsidedown);

	if (tparam->user_texcoord) {
		uv = tparam->user_texcoord;
	}

	if (sobj->loc_uv >= 0) {
		glEnableVertexAttribArray(sobj->loc_uv);
		glVertexAttribPointer(sobj->loc_uv, 2, GL_FLOAT, GL_FALSE, 0,
				      uv);
	}

	glEnable(GL_BLEND);

	if (tparam->blendfunc_en) {
		glBlendFuncSeparate(tparam->blendfunc[0], tparam->blendfunc[1],
				    tparam->blendfunc[2], tparam->blendfunc[3]);
	} else {
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
				    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}

	matrix_identity(matrix);
	matrix_translate(matrix, x, y, 0.0f);
	if (rot != 0) {
		float px = tparam->px;
		float py = tparam->py;
		matrix_translate(matrix, px, py, 0.0f);
		matrix_rotate(matrix, rot, 0.0f, 0.0f, 1.0f);
		matrix_translate(matrix, -px, -py, 0.0f);
	}
	matrix_scale(matrix, w, h, 1.0f);
	matrix_mult(matrix, s_matprj, matrix);

	glUniformMatrix4fv(s_loc_mtx[ttype], 1, GL_FALSE, matrix);
	glUniform4fv(s_loc_color[ttype], 1, tparam->color);

	if (s_loc_texdim[ttype] >= 0) {
		float texdim[2];
		texdim[0] = tparam->texw;
		texdim[1] = tparam->texh;
		glUniform2fv(s_loc_texdim[ttype], 1, texdim);
	}

	if (sobj->loc_vtx >= 0) {
		glEnableVertexAttribArray(sobj->loc_vtx);
		glVertexAttribPointer(sobj->loc_vtx, 2, GL_FLOAT, GL_FALSE, 0,
				      varray);
	}

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisable(GL_BLEND);

	return 0;
}

int draw_2d_texture(int texid, int x, int y, int w, int h, int upsidedown)
{
	texparam_t tparam = { 0 };
	tparam.x = x;
	tparam.y = y;
	tparam.w = w;
	tparam.h = h;
	tparam.texid = texid;
	tparam.textype = SHADER_TYPE_TEX;
	tparam.color[0] = 1.0f;
	tparam.color[1] = 1.0f;
	tparam.color[2] = 1.0f;
	tparam.color[3] = 1.0f;
	tparam.upsidedown = upsidedown;

	draw_2d_texture_in(&tparam);

	return 0;
}

int draw_2d_texture_layout(int texid, int width, int height, double src_x,
			   double src_y, double src_w, double src_h,
			   double dst_x, double dst_y, double dst_w,
			   double dst_h, int upsidedown)
{
	texparam_t tparam = { 0 };
	tparam.x = dst_x;
	tparam.y = dst_y;
	tparam.w = dst_w;
	tparam.h = dst_h;
	tparam.texid = texid;
	tparam.textype = SHADER_TYPE_TEX;
	tparam.color[0] = 1.0f;
	tparam.color[1] = 1.0f;
	tparam.color[2] = 1.0f;
	tparam.color[3] = 1.0f;

	float tarray[] = {
		(float)src_x / width,		(float)src_y / height,
		(float)src_x / width,		(float)(src_y + src_h) / height,
		(float)(src_x + src_w) / width, (float)src_y / height,
		(float)(src_x + src_w) / width, (float)(src_y + src_h) / height
	};
	tparam.user_texcoord = tarray;
	tparam.upsidedown = upsidedown;

	draw_2d_texture_in(&tparam);
	return 0;
}
