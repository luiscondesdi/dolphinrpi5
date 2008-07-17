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

#ifdef _WIN32
#include <intrin.h>
#endif

#include "Globals.h"

#include "Render.h"

#include "MemoryUtil.h"
#include "BPStructs.h"
#include "TextureDecoder.h"
#include "TextureMngr.h"
#include "PixelShaderManager.h"
#include "VertexShaderManager.h"

u8 *TextureMngr::temp = NULL;
TextureMngr::TexCache TextureMngr::textures;
std::map<u32, TextureMngr::DEPTHTARGET> TextureMngr::mapDepthTargets;
int TextureMngr::nTex2DEnabled, TextureMngr::nTexRECTEnabled;

extern int frameCount;
static u32 s_TempFramebuffer = 0;
#define TEMP_SIZE (1024*1024*4)

const GLint c_MinLinearFilter[8] = {
    GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST,
    GL_LINEAR, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR};

const GLint c_WrapSettings[4] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT };

void TextureMngr::TCacheEntry::SetTextureParameters(TexMode0& newmode)
{
    mode = newmode;
    if( isNonPow2 ) {
        // very limited!
        glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_MAG_FILTER, (newmode.mag_filter||g_Config.bForceFiltering)?GL_LINEAR:GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_MIN_FILTER, (g_Config.bForceFiltering||newmode.min_filter>=4)?GL_LINEAR:GL_NEAREST);
		if( newmode.wrap_s == 2 || newmode.wrap_t == 2 ) {
            DEBUG_LOG("cannot support mirrorred repeat mode\n");
		}
    }
    else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (newmode.mag_filter||g_Config.bForceFiltering)?GL_LINEAR:GL_NEAREST);

        if( bHaveMipMaps ) {
            int filt = newmode.min_filter;
            if( g_Config.bForceFiltering && newmode.min_filter < 4 )
                newmode.min_filter += 4; // take equivalent forced linear
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, c_MinLinearFilter[filt]);
        }
        else
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (g_Config.bForceFiltering||newmode.min_filter>=4)?GL_LINEAR:GL_NEAREST);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, c_WrapSettings[newmode.wrap_s]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, c_WrapSettings[newmode.wrap_t]);
    }
    
    if (g_Config.bForceMaxAniso)
    {
        // not used for now, check out GL_EXT_texture_filter_anisotropic
    }
}

void TextureMngr::TCacheEntry::Destroy()
{
    SAFE_RELEASE_TEX(texture);
}

void TextureMngr::Init()
{
    temp = (u8*)AllocateMemoryPages(TEMP_SIZE);
    nTex2DEnabled = nTexRECTEnabled = 0;
}

void TextureMngr::Invalidate()
{
    TexCache::iterator iter = textures.begin();
    for (;iter!=textures.end();iter++)
        iter->second.Destroy();
    textures.clear();
}

void TextureMngr::Shutdown()
{
    Invalidate();
    std::map<u32, DEPTHTARGET>::iterator itdepth = mapDepthTargets.begin();
	for (itdepth = mapDepthTargets.begin(); itdepth != mapDepthTargets.end(); ++itdepth) {
		glDeleteRenderbuffersEXT(1, &itdepth->second.targ);
	}
    mapDepthTargets.clear();

    if( s_TempFramebuffer ) {
        glDeleteFramebuffersEXT(1, &s_TempFramebuffer);
        s_TempFramebuffer = 0;
    }

    FreeMemoryPages(temp, TEMP_SIZE);	
    temp = NULL;
}

void TextureMngr::Cleanup()
{
    TexCache::iterator iter = textures.begin();

    while(iter!=textures.end()) {
        if (frameCount > 20 + iter->second.frameCount) {
            if (!iter->second.isRenderTarget) {
                u32 *ptr = (u32*)g_VideoInitialize.pGetMemoryPointer(iter->second.addr + iter->second.hashoffset*4);
                if (*ptr == iter->second.hash)
                    *ptr = iter->second.oldpixel;
                iter->second.Destroy();
#ifdef _WIN32
                iter = textures.erase(iter);
#else
				textures.erase(iter++);
#endif
            }
            else {
                iter->second.Destroy();
#ifdef _WIN32
                iter = textures.erase(iter);
#else
				textures.erase(iter++);
#endif
            }
        }
        else
            iter++;
    }

    std::map<u32, DEPTHTARGET>::iterator itdepth = mapDepthTargets.begin();
    while(itdepth != mapDepthTargets.end()) {
        if( frameCount > 20 + itdepth->second.framecount) {
#ifdef _WIN32
            itdepth = mapDepthTargets.erase(itdepth);
#else
            mapDepthTargets.erase(itdepth++);
#endif
        }
        else ++itdepth;
    }
}

#ifndef _WIN32
inline u32 _rotl(u32 x, int shift) {
	return (x << shift) | (x >> (32 - shift));
}
#endif
TextureMngr::TCacheEntry* TextureMngr::Load(int texstage, u32 address, int width, int height, int format, int tlutaddr, int tlutfmt)
{
    if (address == 0 )
        return NULL;

    TexCache::iterator iter = textures.find(address);
    TexMode0 &tm0 = bpmem.tex[texstage>3].texMode0[texstage&3];
    u8 *ptr = g_VideoInitialize.pGetMemoryPointer(address);

    int palSize = TexDecoder_GetPaletteSize(format);
    u32 palhash = 0xc0debabe;
    
    if (palSize) {
        if (palSize>16) 
            palSize = 16; //let's not do excessive amount of checking
        u8 *pal = g_VideoInitialize.pGetMemoryPointer(tlutaddr);
        if (pal != 0) {
            for (int i=0; i<palSize; i++) {
                palhash = _rotl(palhash,13);
                palhash ^= pal[i];
                palhash += 31;
            }
        }
    }

    if (iter != textures.end()) {
        TCacheEntry &entry = iter->second;

        if( entry.isRenderTarget || ((u32 *)ptr)[entry.hashoffset] == entry.hash && palhash == entry.paletteHash) { //stupid, improve
            entry.frameCount = frameCount;
            //glEnable(entry.isNonPow2?GL_TEXTURE_RECTANGLE_NV:GL_TEXTURE_2D);
            glBindTexture(entry.isNonPow2?GL_TEXTURE_RECTANGLE_NV:GL_TEXTURE_2D, entry.texture);
            if (entry.mode.hex != tm0.hex)
                entry.SetTextureParameters(tm0);
            return &entry;
        }
        else
        {
            // can potentially do some caching

            //TCacheEntry &entry = entry;
			/*if (width == entry.w && height==entry.h && format==entry.fmt)
            {
                LPDIRECT3DTEXTURE9 tex = entry.texture;
                int bs = TexDecoder_GetBlockWidthInTexels(format)-1; //TexelSizeInNibbles(format)*width*height/16;
                int expandedWidth = (width+bs) & (~bs);
                D3DFORMAT dfmt = TexDecoder_Decode(temp,ptr,expandedWidth,height,format, tlutaddr, tlutfmt);
                ReplaceTexture2D(tex,temp,width,height,expandedWidth,dfmt);
                dev->SetTexture(texstage, stage,tex);
                return;
            }
            else
            {*/
                entry.Destroy();
                textures.erase(iter);
            //}
        }
    }
    
    int bs = TexDecoder_GetBlockWidthInTexels(format)-1; //TexelSizeInNibbles(format)*width*height/16;
    int expandedWidth = (width+bs) & (~bs);
    TEXTUREFMT dfmt = TexDecoder_Decode(temp,ptr,expandedWidth,height,format, tlutaddr, tlutfmt);

    //Make an entry in the table
    TCacheEntry& entry = textures[address];

    entry.hashoffset = 0; 
    entry.hash = (u32)(((double)rand() / RAND_MAX) * 0xFFFFFFFF);
    entry.paletteHash = palhash;
    entry.oldpixel = ((u32 *)ptr)[entry.hashoffset];
    ((u32 *)ptr)[entry.hashoffset] = entry.hash;

    entry.addr = address;
    entry.isRenderTarget=false;
    
    entry.isNonPow2 = ((width&(width-1)) || (height&(height-1)));
    
    glGenTextures(1, &entry.texture);
    GLenum target = entry.isNonPow2 ? GL_TEXTURE_RECTANGLE_NV : GL_TEXTURE_2D;
    glBindTexture(target, entry.texture);

    if (expandedWidth != width)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, expandedWidth);

    if( !entry.isNonPow2 && ((tm0.min_filter&3)==1||(tm0.min_filter&3)==2) ) {
        gluBuild2DMipmaps(GL_TEXTURE_2D, 4, width, height, GL_BGRA, GL_UNSIGNED_BYTE, temp);
        entry.bHaveMipMaps = true;
    }
    else
        glTexImage2D(target, 0, 4, width, height, 0, dfmt.format, dfmt.type, temp);

    if (expandedWidth != width) // reset
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    entry.frameCount = frameCount;
    entry.w=width;
    entry.h=height;
    entry.fmt=format;
    entry.SetTextureParameters(tm0);

    if (g_Config.bDumpTextures) { // dump texture to file
        static int counter = 0;
        char szTemp[MAX_PATH];
        sprintf(szTemp, "%s\\txt_%04i_%i.png", g_Config.texDumpPath, counter++, format);
        
        SaveTexture(szTemp,target, entry.texture, width, height);
    }

    INCSTAT(stats.numTexturesCreated);
    SETSTAT(stats.numTexturesAlive,textures.size());

    //glEnable(entry.isNonPow2?GL_TEXTURE_RECTANGLE_NV:GL_TEXTURE_2D);

    //SaveTexture("tex.tga", target, entry.texture, entry.w, entry.h);
    return &entry;
}

void TextureMngr::CopyRenderTargetToTexture(u32 address, bool bFromZBuffer, bool bIsIntensityFmt, u32 copyfmt, bool bScaleByHalf, TRectangle *source)
{
    DVSTARTPROFILE();
    GL_REPORT_ERRORD();

    // for intensity values, use Y of YUV format!
    // for all purposes, treat 4bit equivalents as 8bit (probably just used for compression)
    // RGBA8 - RGBA8
    // RGB565 - RGB565
    // RGB5A3 - RGB5A3
    // I4,R4,Z4 - I4
    // IA4,RA4 - IA4
    // Z8M,G8,I8,A8,Z8,R8,B8,Z8L - I8
    // Z16,GB8,RG8,Z16L,IA8,RA8 - IA8
    bool bIsInit = textures.find(address) != textures.end();

    PRIM_LOG("copytarg: addr=0x%x, fromz=%d, intfmt=%d, copyfmt=%d\n", address, (int)bFromZBuffer,(int)bIsIntensityFmt,copyfmt);

    TCacheEntry& entry = textures[address];
    entry.isNonPow2 = true;
    entry.hash = 0;
    entry.hashoffset = 0;
    entry.frameCount = frameCount;
    
    int mult = bScaleByHalf?2:1;
    int w = (abs(source->right-source->left)/mult+7)&~7;
    int h = (abs(source->bottom-source->top)/mult+7)&~7;

    GL_REPORT_ERRORD();

    if( !bIsInit ) {
        glGenTextures(1, &entry.texture);
        glBindTexture(GL_TEXTURE_RECTANGLE_NV, entry.texture);
        glTexImage2D(GL_TEXTURE_RECTANGLE_NV, 0, 4, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        GL_REPORT_ERRORD();
    }
    else {
        _assert_(entry.texture);
        bool bReInit = true;

        if( entry.w == w && entry.h == h ) {
            glBindTexture(GL_TEXTURE_RECTANGLE_NV, entry.texture);
            // for some reason mario sunshine errors here...
            GLenum err = GL_NO_ERROR;
            GL_REPORT_ERROR();
            if( err == GL_NO_ERROR )
                bReInit = false;
        }

        if( bReInit ) {
            // necessary, for some reason opengl gives errors when texture isn't deleted
            glDeleteTextures(1,&entry.texture);
            glGenTextures(1, &entry.texture);
            glBindTexture(GL_TEXTURE_RECTANGLE_NV, entry.texture);
            glTexImage2D(GL_TEXTURE_RECTANGLE_NV, 0, 4, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            GL_REPORT_ERRORD();
        }
    }

    if( !bIsInit || !entry.isRenderTarget ) {
        glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

        glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if( glGetError() != GL_NO_ERROR) {
            glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_RECTANGLE_NV, GL_TEXTURE_WRAP_T, GL_CLAMP);
            GL_REPORT_ERRORD();
        }
    }
    
    entry.w = w;
    entry.h = h;
    entry.isRenderTarget=true;
    entry.fmt = copyfmt;

    float colmat[16];
    float fConstAdd[4] = {0};
    memset(colmat, 0, sizeof(colmat));

    if( bFromZBuffer ) {
        switch(copyfmt) {
            case 0: // Z4
            case 1: // Z8
                colmat[2] = colmat[6] = colmat[10] = colmat[14] = 1;
                break;
            
            case 3: // Z16 //?
            case 11: // Z16
                colmat[1] = colmat[5] = colmat[9] = colmat[14] = 1;
                break;
            case 6: // Z24X8
                colmat[0] = 1;
                colmat[5] = 1;
                colmat[10] = 1;
                break;
            case 9: // Z8M
                colmat[1] = colmat[5] = colmat[9] = colmat[13] = 1;
                break;
            case 10: // Z8L
                colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1;
                break;
            case 12: // Z16L
                colmat[0] = colmat[4] = colmat[8] = colmat[13] = 1;
                break;
            default:
                ERROR_LOG("Unknown copy zbuf format: 0x%x\n", copyfmt);
                colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1;
                break;
        }
    }
    else if( bIsIntensityFmt ) {
        fConstAdd[0] = fConstAdd[1] = fConstAdd[2] = 16.0f/255.0f;
        switch(copyfmt) {
            case 0: // I4
            case 1: // I8
            case 2: // IA4
            case 3: // IA8
                colmat[0] = 0.257f; colmat[1] = 0.504f; colmat[2] = 0.098f;
                colmat[4] = 0.257f; colmat[5] = 0.504f; colmat[6] = 0.098f;
                colmat[8] = 0.257f; colmat[9] = 0.504f; colmat[10] = 0.098f;
                if( copyfmt < 2 ) {
                    fConstAdd[3] = 16.0f/255.0f;
                    colmat[12] = 0.257f; colmat[13] = 0.504f; colmat[14] = 0.098f;
                }
                else { // alpha
                    colmat[15] = 1;
                }
                break;
            default:
                ERROR_LOG("Unknown copy intensity format: 0x%x\n", copyfmt);
                colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1;
                break;
        }
    }
    else {
        switch(copyfmt) {
            case 0: // R4
            case 8: // R8
                colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1;
                break;
            case 2: // RA4
            case 3: // RA8
                colmat[0] = colmat[4] = colmat[8] = colmat[15] = 1;
                break;

            case 7: // A8
                colmat[3] = colmat[7] = colmat[11] = colmat[15] = 1; 
                break;
            case 9: // G8
                colmat[1] = colmat[5] = colmat[9] = colmat[13] = 1;
                break;
            case 10: // B8
                colmat[2] = colmat[6] = colmat[10] = colmat[14] = 1;
                break;
            case 11: // RG8
                colmat[0] = colmat[4] = colmat[8] = colmat[13] = 1;
                break;
            case 12: // GB8
                colmat[1] = colmat[5] = colmat[9] = colmat[14] = 1;
                break;

            case 4: // RGB565
                colmat[0] = colmat[5] = colmat[10] = 1;
                fConstAdd[3] = 1; // set alpha to 1
                break;
            case 5: // RGB5A3
            case 6: // RGBA8
                colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1;
                break;

            default:
                ERROR_LOG("Unknown copy color format: 0x%x\n", copyfmt);
                colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1;
                break;
        }
    }

//    if( bCopyToTarget ) {
//        _assert_( glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT );
//        glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
//        GL_REPORT_ERRORD();
//        glCopyTexSubImage2D(GL_TEXTURE_RECTANGLE_NV, 0, 0, 0, source->left, source->top, source->right-source->left, source->bottom-source->top);
//        entry.isUpsideDown = true; // note that the copy is upside down!!
//        GL_REPORT_ERRORD(); 
//        return;
//    }
    
    Renderer::SetRenderMode(Renderer::RM_Normal); // set back to normal
    GL_REPORT_ERRORD();

    // have to run a pixel shader

    Renderer::ResetGLState(); // reset any game specific settings

    if( s_TempFramebuffer == 0 )
        glGenFramebuffersEXT( 1, &s_TempFramebuffer);

    Renderer::SetFramebuffer(s_TempFramebuffer);
    Renderer::SetRenderTarget(entry.texture);
    GL_REPORT_ERRORD();

    // create and attach the render target
    std::map<u32, DEPTHTARGET>::iterator itdepth = mapDepthTargets.find((h<<16)|w);
    
    if( itdepth == mapDepthTargets.end() ) {
        DEPTHTARGET& depth = mapDepthTargets[(h<<16)|w];
        depth.framecount = frameCount;
        glGenRenderbuffersEXT( 1, &depth.targ);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, depth.targ);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT/*GL_DEPTH24_STENCIL8_EXT*/, w, h);
        GL_REPORT_ERRORD();
        Renderer::SetDepthTarget(depth.targ);
        GL_REPORT_ERRORD();
    }
    else {
        itdepth->second.framecount = frameCount;
        Renderer::SetDepthTarget(itdepth->second.targ);
        GL_REPORT_ERRORD();
    }

    glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_RECTANGLE_NV, bFromZBuffer?Renderer::GetZBufferTarget():Renderer::GetRenderTarget());    
    TextureMngr::EnableTexRECT(0);
    
    glViewport(0, 0, w, h);

    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    glBindProgramARB( GL_FRAGMENT_PROGRAM_ARB, PixelShaderMngr::GetColorMatrixProgram());
    PixelShaderMngr::SetColorMatrix(colmat, fConstAdd); // set transformation
    GL_REPORT_ERRORD();
    
    glBegin(GL_QUADS);
    glTexCoord2f((float)source->left, Renderer::GetTargetHeight()-(float)source->bottom); glVertex2f(-1,1);
    glTexCoord2f((float)source->left, Renderer::GetTargetHeight()-(float)source->top); glVertex2f(-1,-1);
    glTexCoord2f((float)source->right, Renderer::GetTargetHeight()-(float)source->top); glVertex2f(1,-1);
    glTexCoord2f((float)source->right, Renderer::GetTargetHeight()-(float)source->bottom); glVertex2f(1,1);
    glEnd();

    GL_REPORT_ERRORD();

    Renderer::SetFramebuffer(0);
    Renderer::RestoreGLState();
    VertexShaderMngr::SetViewportChanged();

    TextureMngr::DisableStage(0);

    if( bFromZBuffer )
        Renderer::SetZBufferRender(); // notify for future settings

    GL_REPORT_ERRORD();
    //SaveTexture("frame.tga", GL_TEXTURE_RECTANGLE_NV, entry.texture, entry.w, entry.h);
    //SaveTexture("tex.tga", GL_TEXTURE_RECTANGLE_NV, Renderer::GetZBufferTarget(), Renderer::GetTargetWidth(), Renderer::GetTargetHeight());
}

void TextureMngr::EnableTex2D(int stage)
{
    if( !(nTex2DEnabled & (1<<stage)) ) {
        nTex2DEnabled |= (1<<stage);
        glEnable(GL_TEXTURE_2D);
    }
    if( nTexRECTEnabled & (1<<stage) ) {
        nTexRECTEnabled &= ~(1<<stage);
        glDisable(GL_TEXTURE_RECTANGLE_NV);
    }
}

void TextureMngr::EnableTexRECT(int stage)
{
    if( (nTex2DEnabled & (1<<stage)) ) {
        nTex2DEnabled &= ~(1<<stage);
        glDisable(GL_TEXTURE_2D);
    }
    if( !(nTexRECTEnabled & (1<<stage)) ) {
        nTexRECTEnabled |= (1<<stage);
        glEnable(GL_TEXTURE_RECTANGLE_NV);
    }
}

void TextureMngr::DisableStage(int stage)
{
    bool bset = false;
    if( nTex2DEnabled & (1<<stage) ) {
        nTex2DEnabled &= ~(1<<stage);
        glActiveTexture(GL_TEXTURE0+stage);
        glDisable(GL_TEXTURE_2D);
        bset = true;
    }
    if( nTexRECTEnabled & (1<<stage) ) {
        nTexRECTEnabled &= ~(1<<stage);
        if( !bset ) glActiveTexture(GL_TEXTURE0+stage);
        glDisable(GL_TEXTURE_RECTANGLE_NV);
    }
}
