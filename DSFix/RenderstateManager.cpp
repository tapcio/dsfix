

#include "RenderstateManager.h"

#include <time.h>
#include <intsafe.h>
#include <io.h>
#include <fstream>
#include <string>

#include "d3dutil.h"
#include "Settings.h"
#include "Hash.h"
#include "Detouring.h"
#include "WindowManager.h"
#include "SaveManager.h"
#include "KeyActions.h"
#include "FPS.h"

#include "WinUtil.h"

RSManager RSManager::instance;

void RSManager::initResources()
{
	SDLOG(0, "RenderstateManager resource initialization started");
	unsigned rw = Settings::get().getRenderWidth(), rh = Settings::get().getRenderHeight();
	unsigned dofRes = Settings::get().getDOFOverrideResolution();
	if (Settings::get().getAAQuality())
	{
		if (Settings::get().getAAType() == "SMAA")
		{
			smaa.reset(new SMAA(d3ddev, rw, rh, (SMAA::Preset)(Settings::get().getAAQuality() - 1)));
		}
		else
		{
			fxaa.reset(new FXAA(d3ddev, rw, rh, (FXAA::Quality)(Settings::get().getAAQuality() - 1)));
		}
	}
	if (Settings::get().getSsaoStrength()) ssao.reset(new SSAO(d3ddev, rw, rh, Settings::get().getSsaoStrength() - 1,
		(Settings::get().getSsaoType() == "VSSAO") ? SSAO::VSSAO : ((Settings::get().getSsaoType() == "HBAO") ? SSAO::HBAO : SSAO::SCAO)));
	if (Settings::get().getDOFBlurAmount()) gauss.reset(new GAUSS(d3ddev, dofRes * 16 / 9, dofRes));
	if (Settings::get().getEnableHudMod()) hud.reset(new HUD(d3ddev, rw, rh));
	d3ddev->CreateTexture(rw, rh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rgbaBuffer1Tex, NULL);
	rgbaBuffer1Tex->GetSurfaceLevel(0, &rgbaBuffer1Surf);
	d3ddev->CreateDepthStencilSurface(rw, rh, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, false, &depthStencilSurf, NULL);
	d3ddev->CreateStateBlock(D3DSBT_ALL, &prevStateBlock);
	if (Settings::get().getEnableTextureOverride() && Settings::get().getEnableTexturePrefetch())
		prefetchTextures();

	SDLOG(0, "RenderstateManager resource initialization completed");
}

void RSManager::prefetchTextures()
{
	SDLOG(0, "Prefetch overwrite textures to memory started\n");
	double startTime = getElapsedTime();
	WIN32_FIND_DATA textureFile;
	const char* texturePath = "dsfix\\tex_override\\";
	char tmp[128];
	sprintf_s(tmp, 128, "%s*", texturePath);
	HANDLE searchHandle = FindFirstFile(tmp, &textureFile);
	if (searchHandle != INVALID_HANDLE_VALUE) {
		do {
			SDLOG(0, "initTexture: %s ", textureFile.cFileName);
			sprintf_s(tmp, 128, "%s%s", texturePath, textureFile.cFileName);
			FILE* fp = 0;
			errno_t err = fopen_s(&fp, tmp, "rb");
			if (!err)
			{
				std::string filename = textureFile.cFileName;
				const size_t period_idx = filename.rfind('.');
				std::string extension = "";
				if (std::string::npos != period_idx)
				{
					extension = filename.substr(period_idx);
					filename = filename.erase(period_idx);
					SDLOG(0, "filename: %s extension: %s ", filename.c_str(), extension.c_str());
				}
				if (extension == ".bmp" ||
					extension == ".dds" ||
					extension == ".dib" ||
					extension == ".hdr" ||
					extension == ".jpg" ||
					extension == ".pfm" ||
					extension == ".png" ||
					extension == ".ppm" ||
					extension == ".tga")
				{
					MemData data;
					data.size = textureFile.nFileSizeLow;
					SDLOG(0, "size: %ld ", data.size);
					data.buffer = new char[data.size];
					fread(&data.buffer[0], 1, data.size, fp);
					fclose(fp);
					UINT32 decimalValue;
					sscanf_s(filename.c_str(), "%08x", &decimalValue);
					SDLOG(0, "texture hash: %s hex: %08x\n", filename.c_str(), decimalValue);
					cachedTexFiles.insert(std::pair<UINT32, MemData>(decimalValue, data));
				}
			}
		} while (FindNextFile(searchHandle, &textureFile));
	}
	SDLOG(0, "Prefetch overwrite textures to memory ended, time: %f\n", getElapsedTime() - startTime);
}

RSManager::~RSManager()
{
	for (auto texData : cachedTexFiles)
	{
		delete[] texData.second.buffer;
	}
}

void RSManager::releaseResources()
{
	SDLOG(0, "RenderstateManager releasing resources");

	rgbaBuffer1Surf = nullptr;
	rgbaBuffer1Tex = nullptr;
	depthStencilSurf = nullptr;
	prevStateBlock = nullptr;
	smaa = nullptr;
	fxaa = nullptr;
	ssao = nullptr;
	gauss = nullptr;
	hud = nullptr;

	SDLOG(0, "RenderstateManager resource release completed");
}

HRESULT RSManager::redirectPresent(CONST RECT *pSourceRect, CONST RECT *pDestRect, HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion)
{
	while (paused)
	{
		Sleep(1);
		KeyActions::get().processIO();
	}

	// tick SaveManager
	SaveManager::get().tick();

	capturing = false;
	if (captureNextFrame)
	{
		capturing = true;
		captureNextFrame = false;
		SDLOG(0, "== CAPTURING FRAME ==");
	}
	if (timingIntroMode)
	{
		skippedPresents++;
		if (skippedPresents >= 1200u && !Settings::get().getUnlockFPS())
		{
			SDLOG(1, "Intro mode ended (timeout)!");
			timingIntroMode = false;
		}
		if (skippedPresents >= 3000u)
		{
			SDLOG(1, "Intro mode ended (full timeout)!");
			timingIntroMode = false;
		}
		return S_OK;
	}
	skippedPresents = 0;
	hudStarted = false;
	nrts = 0;
	doft[0] = 0;
	doft[1] = 0;
	doft[2] = 0;
	mainRT = NULL;
	mainRTuses = 0;
	zSurf = NULL;

	frameTimeManagement();
	return d3ddev->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

D3DPRESENT_PARAMETERS RSManager::adjustPresentationParameters(const D3DPRESENT_PARAMETERS *pPresentationParameters)
{
	D3DPRESENT_PARAMETERS ret;
	memcpy(&ret, pPresentationParameters, sizeof(D3DPRESENT_PARAMETERS));
	SDLOG(0, " - requested mode:");
	SDLOG(0, " - - Backbuffer(s): %4u x %4u %16s *%d ", pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight, D3DFormatToString(pPresentationParameters->BackBufferFormat), pPresentationParameters->BackBufferCount);
	SDLOG(0, " - - PresentationInterval: %2u   Windowed: %5s    Refresh: %3u Hz", pPresentationParameters->PresentationInterval, pPresentationParameters->Windowed ? "true" : "false", pPresentationParameters->FullScreen_RefreshRateInHz);
	if (Settings::get().getForceWindowed())
	{
		SDLOG(0, " - OVERRIDING to user-specified windowed mode:");
		WindowManager::get().resize(Settings::get().getPresentWidth(), Settings::get().getPresentHeight());
		ret.Windowed = TRUE;
		ret.FullScreen_RefreshRateInHz = 0;
	}
	else if (Settings::get().getForceFullscreen())
	{
		SDLOG(0, " - OVERRIDING to user-specified fullscreen mode:");
		ret.Windowed = FALSE;
		ret.FullScreen_RefreshRateInHz = Settings::get().getFullscreenHz();
	}

	if (Settings::get().getForceFullscreen() || Settings::get().getForceWindowed())
	{
		ret.BackBufferWidth = Settings::get().getPresentWidth();
		ret.BackBufferHeight = Settings::get().getPresentHeight();
		SDLOG(0, " - - Backbuffer(s): %4u x %4u %16s *%d ", ret.BackBufferWidth, ret.BackBufferHeight, D3DFormatToString(ret.BackBufferFormat), ret.BackBufferCount);
		SDLOG(0, " - - PresentationInterval: %2u   Windowed: %5s    Refresh: %3u Hz", ret.PresentationInterval, ret.Windowed ? "true" : "false", ret.FullScreen_RefreshRateInHz);
	}

	if (Settings::get().getEnableVsync())
		ret.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

	return ret;
}

void RSManager::dumpSurface(const char* name, IDirect3DSurface9* surface)
{
	char fullname[128];
	sprintf_s(fullname, 128, "dump%03d_%s.tga", dumpCaptureIndex++, name);
	D3DXSaveSurfaceToFile(fullname, D3DXIFF_TGA, surface, NULL, NULL);
}

void getDofRes(UINT inW, UINT inH, UINT& outW, UINT& outH)
{
	if (Settings::get().getDOFOverrideResolution() == 0)
	{
		outW = inW;
		outH = inH;
		return;
	}
	else
	{
		UINT divFactor = Settings::get().getDisableDofScaling() ? 1 : 360 / inH;
		UINT topWidth = Settings::get().getDOFOverrideResolution() * 16 / 9, topHeight = Settings::get().getDOFOverrideResolution();
		outW = topWidth / divFactor;
		outH = topHeight / divFactor;
	}
}

void RSManager::registerMainRenderTexture(IDirect3DTexture9* pTexture)
{
	if (pTexture)
	{
		mainRenderTexIndices.insert(std::make_pair(pTexture, mainRenderTexIndex));
		SDLOG(4, "Registering main render tex: %p as #%d", pTexture, mainRenderTexIndex);
		mainRenderTexIndex++;
	}
}

void RSManager::registerMainRenderSurface(IDirect3DSurface9* pSurface)
{
	if (pSurface)
	{
		mainRenderSurfIndices.insert(std::make_pair(pSurface, mainRenderSurfIndex));
		SDLOG(4, "Registering main render surface: %p as #%d", pSurface, mainRenderSurfIndex);
		mainRenderSurfIndex++;
	}
}

HRESULT RSManager::redirectCreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
{
	SDLOG(1, "CreateTexture w/h: %4u/%4u    format: %s    RENDERTARGET=%d", Width, Height, D3DFormatToString(Format), Usage & D3DUSAGE_RENDERTARGET);
	if (Width == 1024 && Height == 720)
	{
		SDLOG(1, " - OVERRIDE to %4u/%4u!", Settings::get().getRenderWidth(), Settings::get().getRenderHeight());
		HRESULT res = d3ddev->CreateTexture(Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
		if (res == D3D_OK && (Usage & D3DUSAGE_RENDERTARGET)) RSManager::get().registerMainRenderTexture(*ppTexture);
		return res;
	}
	if ((Width == 512 && Height == 360) || (Width == 256 && Height == 180))
	{
		UINT w, h;
		getDofRes(Width, Height, w, h);
		SDLOG(1, " - OVERRIDE DoF to %4u/%4u!", w, h);
		return d3ddev->CreateTexture(w, h, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
	}
	if (Width == 1280 && Height == 720)
	{
		SDLOG(1, " - OVERRIDE to %4u/%4u!", Settings::get().getPresentWidth(), Settings::get().getPresentHeight());
		return d3ddev->CreateTexture(Settings::get().getPresentWidth(), Settings::get().getPresentHeight(), Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
	}
	HRESULT res = d3ddev->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
	return res;
}

HRESULT RSManager::redirectSetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
{
	nrts++;
	if (capturing)
	{
		CComPtr<IDirect3DSurface9> oldRenderTarget, depthStencilSurface;
		d3ddev->GetRenderTarget(0, &oldRenderTarget);
		d3ddev->GetDepthStencilSurface(&depthStencilSurface);
		char buffer[64];
		sprintf_s(buffer, "%03d_oldRenderTarget_%p_.tga", nrts, oldRenderTarget.p);
		SDLOG(0, "Capturing surface %p as %s", oldRenderTarget, buffer);
		D3DXSaveSurfaceToFile(buffer, D3DXIFF_TGA, oldRenderTarget, NULL, NULL);
		if (depthStencilSurface)
		{
			sprintf_s(buffer, "%03d_oldRenderTargetDepth_%p_.tga", nrts, oldRenderTarget.p);
			SDLOG(0, "Capturing depth surface %p as %s", depthStencilSurface, buffer);
			D3DXSaveSurfaceToFile(buffer, D3DXIFF_TGA, depthStencilSurface, NULL, NULL);
		}
	}

	if (nrts == 1)   // we are switching to the RT that will be the main rendering target for this frame
	{
		// store it for later use
		mainRT = pRenderTarget;
		SDLOG(0, "Storing RT as main RT: %p", mainRT);
	}

	if (nrts == 11)   // we are switching to the RT used to store the Z value in the 24 RGB bits (among other things)
	{
		// lets store it for later use
		zSurf = pRenderTarget;
		SDLOG(0, "Storing RT as Z buffer RT: %p", zSurf);
	}

	if (mainRT && pRenderTarget == mainRT)
	{
		SDLOG(0, "MainRT uses: %d + 1", mainRTuses);
		++mainRTuses;
	}

	// we are switching away from the initial 3D-rendered image, do AA and SSAO
	if (mainRTuses == 2 && mainRT && zSurf && ((ssao && doSsao) || (doAA && (smaa || fxaa))))
	{
		CComPtr<IDirect3DSurface9> oldRenderTarget;
		d3ddev->GetRenderTarget(0, &oldRenderTarget);
		if (oldRenderTarget == mainRT)
		{
			// final renderbuffer has to be from texture, just making sure here
			CComPtr<IDirect3DTexture9> tex = getSurfTexture(oldRenderTarget);
			if (tex)
			{
				// check size just to make even more sure
				D3DSURFACE_DESC desc;
				oldRenderTarget->GetDesc(&desc);
				if (desc.Width == Settings::get().getRenderWidth() && desc.Height == Settings::get().getRenderHeight())
				{
					CComPtr<IDirect3DTexture9> zTex = getSurfTexture(zSurf);
					//if(takeScreenshot) D3DXSaveTextureToFile("0effect_pre.bmp", D3DXIFF_BMP, tex, NULL);
					//if(takeScreenshot) D3DXSaveTextureToFile("0effect_z.bmp", D3DXIFF_BMP, zTex, NULL);
					storeRenderState();
					d3ddev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
					d3ddev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
					d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
					// perform AA processing
					if (!lowFPSmode && doAA && (smaa || fxaa))
					{
						if (smaa) smaa->go(tex, tex, rgbaBuffer1Surf, SMAA::INPUT_COLOR);
						else fxaa->go(tex, rgbaBuffer1Surf);
						d3ddev->StretchRect(rgbaBuffer1Surf, NULL, oldRenderTarget, NULL, D3DTEXF_NONE);
					}
					// perform SSAO
					if (ssao && doSsao)
					{
						ssao->go(tex, zTex, rgbaBuffer1Surf);
						d3ddev->StretchRect(rgbaBuffer1Surf, NULL, oldRenderTarget, NULL, D3DTEXF_NONE);
					}
					restoreRenderState();
					//if(takeScreenshot) D3DXSaveSurfaceToFile("1effect_buff.bmp", D3DXIFF_BMP, rgbaBuffer1Surf, NULL, NULL);
					//if(takeScreenshot) D3DXSaveSurfaceToFile("1effect_post.bmp", D3DXIFF_BMP, oldRenderTarget, NULL, NULL);
				}
			}
		}
	}

	// DoF blur stuff
	if (gauss && doDofGauss)
	{
		CComPtr<IDirect3DSurface9> oldRenderTarget;
		d3ddev->GetRenderTarget(0, &oldRenderTarget);
		D3DSURFACE_DESC desc;
		oldRenderTarget->GetDesc(&desc);
		unsigned dofIndex = isDof(desc.Width, desc.Height);
		if (dofIndex)
		{
			doft[dofIndex]++;
			SDLOG(6, "DOF index: %u, doft: %u", dofIndex, doft[dofIndex]);
			//if(takeScreenshot) {
			//	char buffer[256];
			//	sprintf(buffer, "dof%1u_doft%1u.bmp", dofIndex, doft[dofIndex]);
			//	D3DXSaveSurfaceToFile(buffer, D3DXIFF_BMP, oldRenderTarget, NULL, NULL);
			//}
			if (dofIndex == 1 && doft[1] == 4)
			{
				CComPtr<IDirect3DTexture9> oldRTtex = getSurfTexture(oldRenderTarget);
				if (oldRTtex)
				{
					storeRenderState();
					for (size_t i = 0; i < Settings::get().getDOFBlurAmount(); ++i) gauss->go(oldRTtex, oldRenderTarget);
					restoreRenderState();
				}
			}
		}
	}

	// Timing for hudless screenshots
	if (mainRTuses == 11 && takeScreenshot)
	{
		CComPtr<IDirect3DSurface9> oldRenderTarget;
		d3ddev->GetRenderTarget(0, &oldRenderTarget);
		if (oldRenderTarget != mainRT)
		{
			static int toggleSS = 0;
			toggleSS = (toggleSS + 1) % 2;
			if (!toggleSS)
			{
				takeScreenshot = false;
				SDLOG(0, "Capturing screenshot");
				char timebuf[128], buffer[512];
				time_t ltime;
				time(&ltime);
				struct tm timeinfo;
				localtime_s(&timeinfo, &ltime);

				strftime(timebuf, 128, "screenshot_%Y-%m-%d_%H-%M-%S.png", &timeinfo);
				sprintf_s(buffer, "%s\\%s", Settings::get().getScreenshotDir().c_str(), timebuf);
				SDLOG(0, " - to %s", buffer);

				D3DSURFACE_DESC desc;
				oldRenderTarget->GetDesc(&desc);
				CComPtr<IDirect3DSurface9> convertedSurface;
				d3ddev->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, true, &convertedSurface, NULL);
				D3DXLoadSurfaceFromSurface(convertedSurface, NULL, NULL, oldRenderTarget, NULL, NULL, D3DX_FILTER_POINT, 0);
				D3DXSaveSurfaceToFile(buffer, D3DXIFF_PNG, convertedSurface, NULL, NULL);
			}
		}
	}

	if (rddp >= 4)   // we just finished rendering the frame (pre-HUD)
	{
		CComPtr<IDirect3DSurface9>oldRenderTarget;
		d3ddev->GetRenderTarget(0, &oldRenderTarget);
		// final renderbuffer has to be from texture, just making sure here
		CComPtr <IDirect3DTexture9> tex = getSurfTexture(oldRenderTarget);
		if (tex)
		{
			// check size just to make even more sure
			D3DSURFACE_DESC desc;
			oldRenderTarget->GetDesc(&desc);
			if (desc.Width == Settings::get().getRenderWidth() && desc.Height == Settings::get().getRenderHeight())
			{
				// HUD stuff
				if (hud && doHud && rddp == 9)
				{
					SDLOG(0, "Starting HUD rendering");
					hddp = 0;
					onHudRT = true;
					d3ddev->SetRenderTarget(0, rgbaBuffer1Surf);
					d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_RGBA(0, 0, 0, 0), 0.0f, 0);
					prevRenderTex = tex;
					prevRenderTarget = pRenderTarget;

					d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_ADD);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
					return S_OK;
				}
			}
		}
	}
	if (onHudRT)
	{
		finishHudRendering();
	}
	if (rddp < 4 || rddp > 8) rddp = 0;
	else rddp++;
	return d3ddev->SetRenderTarget(RenderTargetIndex, pRenderTarget);
}

HRESULT RSManager::redirectStretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
{
	//SurfSurfMap::iterator it = renderTexSurfTargets.find(pSourceSurface);
	//if(it != renderTexSurfTargets.end()) {
	//	SDLOG(1, "Redirected StretchRect %p -> %p to %p -> %p", pSourceSurface, pDestSurface, it->second, pDestSurface);
	//	if(capturing) dumpSurface("redirectStretchRect", it->second);
	//	return d3ddev->StretchRect(it->second, pSourceRect, pDestSurface, pDestRect, Filter);
	//}
	return d3ddev->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, D3DTEXF_LINEAR);
}

HRESULT RSManager::redirectSetTexture(DWORD Stage, IDirect3DBaseTexture9 * pTexture)
{
	if (pTexture == NULL) return d3ddev->SetTexture(Stage, pTexture);
	//TexIntMap::iterator it = renderTexIndices.find((IDirect3DTexture9*)pTexture);
	//if(it != renderTexIndices.end() && it->second == 2) {
	//	IDirect3DSurface9* surf0;
	//	((IDirect3DTexture9*)pTexture)->GetSurfaceLevel(0, &surf0);
	//	SDLOG(1, "Redirected SetTexture(%d, %p) -- StretchRect from %p to %p", Stage, pTexture, renderTexSurfTargets[surf0], surf0);
	//	if(capturing) dumpSurface("redirectSetTexture", renderTexSurfTargets[surf0]);
	//	d3ddev->StretchRect(renderTexSurfTargets[surf0], NULL, surf0, NULL, D3DTEXF_LINEAR);
	//}
	//int index = getTextureIndex((IDirect3DTexture9*)pTexture);
	//if(dumpedTextures.find(index) == dumpedTextures.end()) {
	//	dumpedTextures.insert(index);
	//	IDirect3DSurface9* surf;
	//	((IDirect3DTexture9*)pTexture)->GetSurfaceLevel(0, &surf);
	//	char buffer[16];
	//	sprintf(buffer, "tex%4d__", index);
	//	dumpSurface(buffer, surf);
	//}
	if (Settings::get().getSkipIntro() && !timingIntroMode && isTextureBandainamcoLogo(pTexture))
	{
		SDLOG(1, "Intro mode started!");
		timingIntroMode = true;
	}
	if (timingIntroMode && (isTextureGuiElements1(pTexture) || isTextureMenuscreenLogo(pTexture) || isTextureText(pTexture)))
	{
		SDLOG(1, "Intro mode ended due to texture!");
		timingIntroMode = false;
	}
	if (!hudStarted && isTextureHudHealthbar(pTexture))
	{
		SDLOG(1, "HUD started!");
		hudStarted = true;
	}

	if (pTexture && rddp == 0 && Stage == 0)
	{
		++rddp;
	}
	else if (pTexture && rddp == 1 && Stage == 1)
	{
		++rddp;
	}
	else if (pTexture && rddp == 2 && Stage == 2)
	{
		++rddp;
	}
	else if (pTexture && rddp == 3 && Stage == 3)
	{
		++rddp;
	}
	else rddp = 0;
	return d3ddev->SetTexture(Stage, pTexture);
}

HRESULT RSManager::redirectSetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
{
	//if(lastReplacement >= 0) {
	//	SDLOG(1, "Redirected SetDepthStencilSurface(%p) to %p", pNewZStencil, renderTexDSBuffers[lastReplacement]);
	//	d3ddev->SetDepthStencilSurface(renderTexDSBuffers[lastReplacement]);
	//}
	//lastReplacement = -1;
	return d3ddev->SetDepthStencilSurface(pNewZStencil);
}

unsigned RSManager::getTextureIndex(IDirect3DTexture9* ppTexture)
{
	TexIntMap::iterator it = texIndices.find(ppTexture);
	if (it != texIndices.end()) return it->second;
	return UINT_MAX;
}

void RSManager::registerD3DXCreateTextureFromFileInMemory(LPCVOID pSrcData, UINT SrcDataSize, LPDIRECT3DTEXTURE9 pTexture)
{
	SDLOG(1, "RenderstateManager: registerD3DXCreateTextureFromFileInMemory %p", pTexture);
	if (Settings::get().getEnableTextureDumping())
	{
		UINT32 hash = SuperFastHash((char*)const_cast<void*>(pSrcData), SrcDataSize);
		SDLOG(1, " - size: %8u, hash: %8x", SrcDataSize, hash);

		CComPtr<IDirect3DSurface9> surf;
		pTexture->GetSurfaceLevel(0, &surf);
		char buffer[128];
		sprintf_s(buffer, "dsfix/tex_dump/%08x.tga", hash);
		D3DXSaveSurfaceToFile(GetDirectoryFile(buffer), D3DXIFF_TGA, surf, NULL, NULL);
	}
	registerKnowTexture(pSrcData, SrcDataSize, pTexture);
}

void RSManager::registerKnowTexture(LPCVOID pSrcData, UINT SrcDataSize, LPDIRECT3DTEXTURE9 pTexture)
{
	if (foundKnownTextures < numKnownTextures)
	{
		UINT32 hash = SuperFastHash((char*)const_cast<void*>(pSrcData), SrcDataSize);
#define TEXTURE(_name, _hash) \
		if(hash == _hash) { \
			texture##_name = pTexture; \
			++foundKnownTextures; \
			SDLOG(1, "RenderstateManager: recognized known texture %s at %u", #_name, pTexture); \
		}
#include "Textures.def"
#undef TEXTURE
		if (foundKnownTextures == numKnownTextures)
		{
			SDLOG(1, "RenderstateManager: all known textures found!");
		}
	}
}

void RSManager::registerD3DXCompileShader(LPCSTR pSrcData, UINT srcDataLen, const D3DXMACRO* pDefines, LPD3DXINCLUDE pInclude, LPCSTR pFunctionName, LPCSTR pProfile, DWORD Flags, LPD3DXBUFFER * ppShader, LPD3DXBUFFER * ppErrorMsgs, LPD3DXCONSTANTTABLE * ppConstantTable)
{
	SDLOG(0, "RenderstateManager: registerD3DXCompileShader %p, fun: %s, profile: %s", *ppShader, pFunctionName, pProfile);
	SDLOG(0, "============= source:\n%s\n====================", pSrcData);
}

IDirect3DTexture9* RSManager::getSurfTexture(IDirect3DSurface9* pSurface)
{
	CComPtr<IUnknown> pContainer;
	HRESULT hr = pSurface->GetContainer(IID_IDirect3DTexture9, (void**)&pContainer);
	if (D3D_OK == hr)
		return (IDirect3DTexture9*)pContainer.Detach();
	return NULL;
}

void RSManager::enableSingleFrameCapture()
{
	captureNextFrame = true;
}

void RSManager::enableTakeScreenshot()
{
	takeScreenshot = true;
	SDLOG(0, "takeScreenshot: %s", takeScreenshot ? "true" : "false");
}

void RSManager::reloadVssao()
{
	ssao.reset(new SSAO(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), Settings::get().getSsaoStrength() - 1, SSAO::VSSAO));
	SDLOG(0, "Reloaded SSAO");
}
void RSManager::reloadHbao()
{
	ssao.reset(new SSAO(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), Settings::get().getSsaoStrength() - 1, SSAO::HBAO));
	SDLOG(0, "Reloaded SSAO");
}
void RSManager::reloadScao()
{
	ssao.reset(new SSAO(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), Settings::get().getSsaoStrength() - 1, SSAO::SCAO));
	SDLOG(0, "Reloaded SSAO");
}

void RSManager::reloadGauss()
{
	gauss.reset(new GAUSS(d3ddev, Settings::get().getDOFOverrideResolution() * 16 / 9, Settings::get().getDOFOverrideResolution()));
	SDLOG(0, "Reloaded GAUSS");
}

void RSManager::reloadAA()
{
	if (Settings::get().getAAType() == "SMAA")
	{
		smaa.reset(new SMAA(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (SMAA::Preset)(Settings::get().getAAQuality() - 1)));
	}
	else
	{
		fxaa.reset(new FXAA(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (FXAA::Quality)(Settings::get().getAAQuality() - 1)));
	}
	SDLOG(0, "Reloaded AA");
}

HRESULT RSManager::redirectDrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	if (hudStarted && hideHud)
	{
		return D3D_OK;
	}
	bool isTargetIndicator = false;
	if (pausedHudRT)
	{
		CComPtr<IDirect3DBaseTexture9> t;
		d3ddev->GetTexture(0, &t);
		// check for target indicator
		if (isTextureHudHealthbar(t))
		{
			INT16 *vertices = (INT16*)pVertexStreamZeroData;
			if (vertices[3] > -2000)
			{
				resumeHudRendering();
			}
		}
		else
		{
			resumeHudRendering();
		}
	}
	if (onHudRT)
	{
		CComPtr<IDirect3DBaseTexture9> t;
		d3ddev->GetTexture(0, &t);
		SDLOG(4, "On HUD, redirectDrawIndexedPrimitiveUP texture: %s", getTextureName(t));
		if ((hddp < 5 && isTextureHudHealthbar(t)) || (hddp >= 5 && hddp < 7 && isTextureCategoryIconsHumanityCount(t)) || (hddp >= 7 && !isTextureCategoryIconsHumanityCount(t))) hddp++;
		// check for target indicator
		if (isTextureHudHealthbar(t))
		{
			INT16 *vertices = (INT16*)pVertexStreamZeroData;
			if (vertices[3] < -2000)
			{
				isTargetIndicator = true;
				pauseHudRendering();
			}
		}
		if (hddp == 8)
		{
			finishHudRendering();
		}
		else if (!isTargetIndicator)
		{
			//d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
			//d3ddev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
			//d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			//d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
		}
	}
	HRESULT hr = d3ddev->DrawIndexedPrimitiveUP(PrimitiveType, MinIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
	//if(onHudRT) {
	//	if(takeScreenshot) dumpSurface("HUD_IndexPrimUP", rgbaBuffer1Surf);
	//}
	return hr;
}

HRESULT RSManager::redirectDrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
	if (hudStarted && hideHud)
	{
		CComPtr<IDirect3DBaseTexture9> t;
		d3ddev->GetTexture(0, &t);
		bool hide = isTextureText(t);
		hide = hide || isTextureButtonsEffects(t);
		hide = hide || isTextureHudEffectIcons(t);
		if (hide) return D3D_OK;
	}
	if (pausedHudRT)
	{
		CComPtr<IDirect3DBaseTexture9> t;
		d3ddev->GetTexture(0, &t);
		bool isText = isTextureText(t);
		SDLOG(4, "On HUD, PAUSED, redirectDrawPrimitiveUP texture: %s", getTextureName(t));
		//// Print vertices
		//SDLOG(0, "Vertices: ");
		//INT16 *values = (INT16*)pVertexStreamZeroData;
		//for(size_t i=0; i<PrimitiveCount+2; ++i) {
		//	SDLOG(0, "%8hd, ", values[i]);
		//	if((i+1)%2 == 0) SDLOG(0, "; ");
		//	if((i+1)%8 == 0) SDLOG(0, "");
		//}
		if (isText && PrimitiveCount >= 12) resumeHudRendering();
	}
	bool subbed = false;
	if (onHudRT)
	{
		CComPtr<IDirect3DBaseTexture9> t;
		d3ddev->GetTexture(0, &t);
		bool isText = isTextureText(t);
		bool isSub = isTextureText00(t);
		SDLOG(4, "On HUD, redirectDrawPrimitiveUP texture: %s", getTextureName(t));
		//if(isText && PrimitiveCount <= 10) pauseHudRendering();
		if (isSub)
		{
			pauseHudRendering();
			subbed = true;
		}
	}
	HRESULT hr = d3ddev->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
	if (subbed) resumeHudRendering();
	//if(onHudRT) {
	//	if(takeScreenshot) dumpSurface("HUD_PrimUP", rgbaBuffer1Surf);
	//}
	return hr;
}

void RSManager::reloadHudVertices()
{
	SDLOG(0, "Reloading HUD vertices");
	std::ifstream vfile;
	vfile.open(GetDirectoryFile("hudvertices.txt"), std::ios::in);
	if (vfile.is_open())
	{
		char buffer[256];
		size_t index = 0;
		SDLOG(0, "- starting");
		while (!vfile.eof())
		{
			vfile.getline(buffer, 256);
			if (buffer[0] == '#') continue;
			if (vfile.gcount() <= 4) continue;

			sscanf_s(buffer, "%hd %hd", &hudVertices[index], &hudVertices[index + 1]);
			SDLOG(0, "- read %hd, %hd", hudVertices[index], hudVertices[index + 1]);
			index += 2;
		}
		vfile.close();
	}
}

bool RSManager::isTextureText(IDirect3DBaseTexture9* t)
{
	return isTextureText00(t) || isTextureText01(t) || isTextureText02(t) || isTextureText03(t)
		|| isTextureText04(t) || isTextureText05(t) || isTextureText06(t) || isTextureText07(t)
		|| isTextureText08(t) || isTextureText09(t) || isTextureText10(t) || isTextureText11(t)
		|| isTextureText12(t);
}

unsigned RSManager::isDof(unsigned width, unsigned height)
{
	unsigned topWidth = Settings::get().getDOFOverrideResolution() * 16 / 9, topHeight = Settings::get().getDOFOverrideResolution();
	if (width == topWidth && height == topHeight) return 1;
	if (width == topWidth / 2 && height == topHeight / 2) return 2;
	return 0;
}

HRESULT RSManager::redirectD3DXCreateTextureFromFileInMemoryEx(LPDIRECT3DDEVICE9 pDevice, LPCVOID pSrcData, UINT SrcDataSize, UINT Width, UINT Height, UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, D3DXIMAGE_INFO* pSrcInfo, PALETTEENTRY* pPalette, LPDIRECT3DTEXTURE9* ppTexture)
{
	if (Settings::get().getEnableTextureOverride())
	{
		UINT32 hash = SuperFastHash((char*)const_cast<void*>(pSrcData), SrcDataSize);
		SDLOG(4, "Trying texture override size: %8u, hash: %8x", SrcDataSize, hash);

		if (Settings::get().getEnableTexturePrefetch() && cachedTexFiles.find(hash) != cachedTexFiles.end())
		{
			SDLOG(4, "Cached texture file found! size: %ld, hash: %8x \n", cachedTexFiles[hash].size, hash);
			return TrueD3DXCreateTextureFromFileInMemoryEx(pDevice, cachedTexFiles[hash].buffer, cachedTexFiles[hash].size, D3DX_DEFAULT, D3DX_DEFAULT, MipLevels, Usage, D3DFMT_FROM_FILE, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
		}
		else
		{
			char buffer[128];
			sprintf_s(buffer, "dsfix/tex_override/%08x.png", hash);
			if (FileExists(buffer)) {
				SDLOG(4, "Texture override (png)! hash: %08x\n", hash);
				return D3DXCreateTextureFromFileEx(pDevice, buffer, D3DX_DEFAULT, D3DX_DEFAULT, MipLevels, Usage, D3DFMT_FROM_FILE, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
			}
			sprintf_s(buffer, "dsfix/tex_override/%08x.dds", hash);
			if (FileExists(buffer)) {
				SDLOG(4, "Texture override (dds)! hash: %08x\n", hash);
				return D3DXCreateTextureFromFileEx(pDevice, buffer, D3DX_DEFAULT, D3DX_DEFAULT, MipLevels, Usage, D3DFMT_FROM_FILE, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
			}
		}
	}
	return TrueD3DXCreateTextureFromFileInMemoryEx(pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
}

void RSManager::storeRenderState()
{
	prevStateBlock->Capture();
	prevVDecl = nullptr;
	prevDepthStencilSurf = nullptr;
	d3ddev->GetVertexDeclaration(&prevVDecl);
	d3ddev->GetDepthStencilSurface(&prevDepthStencilSurf);
	d3ddev->SetDepthStencilSurface(depthStencilSurf);
}

void RSManager::restoreRenderState()
{
	if (prevVDecl)
	{
		d3ddev->SetVertexDeclaration(prevVDecl);
		prevVDecl = nullptr;
	}
	d3ddev->SetDepthStencilSurface(prevDepthStencilSurf); // also restore NULL!
	if (prevDepthStencilSurf)
	{
		prevDepthStencilSurf = nullptr;
	}
	prevStateBlock->Apply();
}

const char* RSManager::getTextureName(IDirect3DBaseTexture9* pTexture)
{
#define TEXTURE(_name, _hash) \
	if(texture##_name == pTexture) return #_name;
#include "Textures.def"
#undef TEXTURE
	return "Unknown";
}

void RSManager::finishHudRendering()
{
	SDLOG(2, "FinishHudRendering");
	if (takeScreenshot) dumpSurface("HUD_end", rgbaBuffer1Surf);
	d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	d3ddev->SetRenderTarget(0, prevRenderTarget);
	onHudRT = false;
	// draw HUD to screen
	storeRenderState();
	hud->go(rgbaBuffer1Tex, prevRenderTarget);
	restoreRenderState();
}

void RSManager::pauseHudRendering()
{
	SDLOG(3, "PauseHudRendering");
	d3ddev->SetRenderTarget(0, prevRenderTarget);
	d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	d3ddev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
	onHudRT = false;
	pausedHudRT = true;
}

void RSManager::resumeHudRendering()
{
	SDLOG(3, "ResumeHudRendering");
	d3ddev->SetRenderTarget(0, rgbaBuffer1Surf);
	d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
	d3ddev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	onHudRT = true;
	pausedHudRT = false;
}

HRESULT RSManager::redirectSetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
	//if(allowStateChanges()) {
	return d3ddev->SetTextureStageState(Stage, Type, Value);
	//} else {
	//	SDLOG(3, "SetTextureStageState suppressed: %u  -  %u  -  %u", Stage, Type, Value);
	//}
	//return D3D_OK;
}

HRESULT RSManager::redirectSetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
	if (State == D3DRS_COLORWRITEENABLE && !allowStateChanges()) return D3D_OK;
	//if(allowStateChanges()) {
	return d3ddev->SetRenderState(State, Value);
	//} else {
	//	SDLOG(3, "SetRenderState suppressed: %u  -  %u", State, Value);
	//}
	//return D3D_OK;
}

void RSManager::frameTimeManagement()
{
	double renderTime = getElapsedTime() - lastPresentTime;

	// implement FPS threshold
	double thresholdRenderTime = (1000.0f / Settings::get().getFPSThreshold()) + 0.2;
	if (renderTime > thresholdRenderTime) lowFPSmode = true;
	else if (renderTime < thresholdRenderTime - 1.0f) lowFPSmode = false;

	// implement FPS cap
	if (Settings::get().getUnlockFPS())
	{
		double desiredRenderTime = (1000.0 / Settings::get().getCurrentFPSLimit()) - 0.1;
		while (renderTime < desiredRenderTime)
		{
			SwitchToThread();
			renderTime = getElapsedTime() - lastPresentTime;
		}
		lastPresentTime = getElapsedTime();
	}
}
