// Copyright (C) 2003-2008 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#ifndef GCOGL_PIXELSHADER
#define GCOGL_PIXELSHADER

#include "Render.h"
#include "BPStructs.h"

#define I_COLORS "color"
#define I_KCOLORS "k"
#define I_ALPHA "alphaRef"
#define I_TEXDIMS "texdim"
#define I_ZBIAS "czbias"
#define I_INDTEXSCALE "cindscale"
#define I_INDTEXMTX "cindmtx"

#define C_COLORS 0
#define C_KCOLORS (C_COLORS+4)
#define C_ALPHA (C_KCOLORS+4)
#define C_TEXDIMS (C_ALPHA+1)
#define C_ZBIAS (C_TEXDIMS+8)
#define C_INDTEXSCALE (C_ZBIAS+2)
#define C_INDTEXMTX (C_INDTEXSCALE+2)
#define C_ENVCONST_END (C_INDTEXMTX+6)

#define C_COLORMATRIX (C_INDTEXMTX+6)

struct FRAGMENTSHADER
{
    FRAGMENTSHADER() : glprogid(0) { }
    GLuint glprogid; // opengl program id

#ifdef _DEBUG
	std::string strprog;
#endif
};

bool GeneratePixelShader(FRAGMENTSHADER& ps);

#endif
