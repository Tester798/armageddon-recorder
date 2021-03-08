/* Armageddon Recorder
 * Copyright (C) 2013 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <windows.h>
#include <dsound.h>
#include <stdio.h>

#include "ds-capture.h"

typedef struct IDirectSound_hook IDirectSound_hook;

struct IDirectSound_hook
{
	IDirectSound obj;
	IDirectSoundVtbl vtable;
	
	IDirectSound *real;
};

static ULONG   __stdcall IDirectSound_hook_AddRef(IDirectSound_hook *self);
static HRESULT __stdcall IDirectSound_hook_QueryInterface(IDirectSound_hook *self, REFIID riid, void **ppvObject);
static ULONG   __stdcall IDirectSound_hook_Release(IDirectSound_hook *self);
static HRESULT __stdcall IDirectSound_hook_Initialize(IDirectSound_hook *self, LPCGUID lpcGuid);
static HRESULT __stdcall IDirectSound_hook_SetCooperativeLevel(IDirectSound_hook *self, HWND hwnd, DWORD dwLevel);
static HRESULT __stdcall IDirectSound_hook_CreateSoundBuffer(struct IDirectSound_hook *self, const DSBUFFERDESC *lpcDSBufferDesc, IDirectSoundBuffer **lplpDirectSoundBuffer, IUnknown FAR *pUnkOuter);
static HRESULT __stdcall IDirectSound_hook_DuplicateSoundBuffer(IDirectSound_hook *self, LPDIRECTSOUNDBUFFER lpDsbOriginal, LPLPDIRECTSOUNDBUFFER lplpDsbDuplicate);
static HRESULT __stdcall IDirectSound_hook_GetCaps(IDirectSound_hook *self, LPDSCAPS lpDSCaps);
static HRESULT __stdcall IDirectSound_hook_Compact(IDirectSound_hook *self);
static HRESULT __stdcall IDirectSound_hook_GetSpeakerConfig(IDirectSound_hook *self, LPDWORD lpdwSpeakerConfig);
static HRESULT __stdcall IDirectSound_hook_SetSpeakerConfig(IDirectSound_hook *self, DWORD dwSpeakerConfig);

typedef struct IDirectSoundBuffer_hook IDirectSoundBuffer_hook;

struct IDirectSoundBuffer_hook
{
	IDirectSoundBuffer obj;
	IDirectSoundBufferVtbl vtable;
	
	IDirectSoundBuffer *real;
	
	unsigned int buf_id;
	
	void *lock_buf;
	DWORD lock_offset;
};

static ULONG   __stdcall IDirectSoundBuffer_hook_AddRef(IDirectSoundBuffer_hook *self);
static HRESULT __stdcall IDirectSoundBuffer_hook_QueryInterface(IDirectSoundBuffer_hook *self, REFIID riid, void **ppvObject);
static ULONG   __stdcall IDirectSoundBuffer_hook_Release(IDirectSoundBuffer_hook *self);
static HRESULT __stdcall IDirectSoundBuffer_hook_GetCaps(IDirectSoundBuffer_hook *self, LPDSBCAPS lpDSBufferCaps);
static HRESULT __stdcall IDirectSoundBuffer_hook_GetFormat(IDirectSoundBuffer_hook *self, LPWAVEFORMATEX lpwfxFormat, DWORD dwSizeAllocated, LPDWORD lpdwSizeWritten);
static HRESULT __stdcall IDirectSoundBuffer_hook_GetStatus(IDirectSoundBuffer_hook *self, LPDWORD lpdwStatus);
static HRESULT __stdcall IDirectSoundBuffer_hook_SetFormat(IDirectSoundBuffer_hook *self, LPCWAVEFORMATEX lpcfxFormat);
static HRESULT __stdcall IDirectSoundBuffer_hook_Initialize(IDirectSoundBuffer_hook *self, LPDIRECTSOUND lpDirectSound, LPCDSBUFFERDESC lpcDSBufferDesc);
static HRESULT __stdcall IDirectSoundBuffer_hook_Restore(IDirectSoundBuffer_hook *self);
static HRESULT __stdcall IDirectSoundBuffer_hook_GetCurrentPosition(IDirectSoundBuffer_hook *self, LPDWORD lpdwCurrentPlayCursor, LPDWORD lpdwCurrentWriteCursor);
static HRESULT __stdcall IDirectSoundBuffer_hook_Lock(IDirectSoundBuffer_hook *self, DWORD dwWriteCursor, DWORD dwWriteBytes, LPVOID lplpvAudioPtr1, LPDWORD lpdwAudioBytes1, LPVOID lplpvAudioPtr2, LPDWORD lpdwAudioBytes2, DWORD dwFlags);
static HRESULT __stdcall IDirectSoundBuffer_hook_Unlock(IDirectSoundBuffer_hook *self, LPVOID lpvAudioPtr1, DWORD dwAudioBytes1, LPVOID lpvAudioPtr2, DWORD dwAudioBytes2);
static HRESULT __stdcall IDirectSoundBuffer_hook_SetCurrentPosition(IDirectSoundBuffer_hook *self, DWORD dwNewPosition);
static HRESULT __stdcall IDirectSoundBuffer_hook_Play(IDirectSoundBuffer_hook *self, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags);
static HRESULT __stdcall IDirectSoundBuffer_hook_Stop(IDirectSoundBuffer_hook *self);
static HRESULT __stdcall IDirectSoundBuffer_hook_GetFrequency(IDirectSoundBuffer_hook *self, LPDWORD lpdwFrequency);
static HRESULT __stdcall IDirectSoundBuffer_hook_GetPan(IDirectSoundBuffer_hook *self, LPLONG lplPan);
static HRESULT __stdcall IDirectSoundBuffer_hook_GetVolume(IDirectSoundBuffer_hook *self, LPLONG lplVolume);
static HRESULT __stdcall IDirectSoundBuffer_hook_SetFrequency(IDirectSoundBuffer_hook *self, DWORD dwFrequency);
static HRESULT __stdcall IDirectSoundBuffer_hook_SetPan(IDirectSoundBuffer_hook *self, LONG lPan);
static HRESULT __stdcall IDirectSoundBuffer_hook_SetVolume(IDirectSoundBuffer_hook *self, LONG lVolume);

/* Function pointer to DirectSoundCreate() */
typedef HRESULT(__stdcall *DirectSoundCreate_t)(LPCGUID, IDirectSound**, LPUNKNOWN);

static HMODULE sys_dsound = NULL;
static FILE *capture_fh   = NULL;

/* Get number of frames exported so far */
unsigned int get_frames(void)
{
	static unsigned int frame_count = 0;
	
	char *frame_prefix = getenv("AREC_FRAME_PREFIX");
	if(!frame_prefix)
	{
		abort();
	}
	
	char *path = malloc(strlen(frame_prefix) + 11);
	if(!path)
	{
		abort();
	}
	
	while(1)
	{
		sprintf(path, "%s%06u.png", frame_prefix, frame_count);
		
		if(GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES)
		{
			++frame_count;
		}
		else{
			break;
		}
	}
	
	free(path);
	
	return frame_count;
}

/* Wrap an IDirectSoundBuffer instance within a hook instance.
 * 
 * Replaces the IDirectSoundBuffer at *obj and returns a pointer to the
 * IDirectSoundBuffer_hook structure.
*/
static IDirectSoundBuffer_hook *wrap_IDirectSoundBuffer(IDirectSoundBuffer **obj, DWORD flags)
{
	IDirectSoundBuffer_hook *hook = malloc(sizeof(IDirectSoundBuffer_hook));
	if(!hook)
	{
		abort();
	}
	
	hook->real = *obj;
	
	hook->vtable.AddRef         = IDirectSoundBuffer_hook_AddRef;
	hook->vtable.QueryInterface = IDirectSoundBuffer_hook_QueryInterface;
	hook->vtable.Release        = IDirectSoundBuffer_hook_Release;
	
	hook->vtable.GetCaps            = IDirectSoundBuffer_hook_GetCaps;
	hook->vtable.GetFormat          = IDirectSoundBuffer_hook_GetFormat;
	hook->vtable.GetStatus          = IDirectSoundBuffer_hook_GetStatus;
	hook->vtable.SetFormat          = IDirectSoundBuffer_hook_SetFormat;
	hook->vtable.Initialize         = IDirectSoundBuffer_hook_Initialize;
	hook->vtable.Restore            = IDirectSoundBuffer_hook_Restore;
	hook->vtable.GetCurrentPosition = IDirectSoundBuffer_hook_GetCurrentPosition;
	hook->vtable.Lock               = IDirectSoundBuffer_hook_Lock;
	hook->vtable.Unlock             = IDirectSoundBuffer_hook_Unlock;
	hook->vtable.SetCurrentPosition = IDirectSoundBuffer_hook_SetCurrentPosition;
	hook->vtable.Play               = IDirectSoundBuffer_hook_Play;
	hook->vtable.Stop               = IDirectSoundBuffer_hook_Stop;
	hook->vtable.GetFrequency       = IDirectSoundBuffer_hook_GetFrequency;
	hook->vtable.GetPan             = IDirectSoundBuffer_hook_GetPan;
	hook->vtable.GetVolume          = IDirectSoundBuffer_hook_GetVolume;
	hook->vtable.SetFrequency       = IDirectSoundBuffer_hook_SetFrequency;
	hook->vtable.SetPan             = IDirectSoundBuffer_hook_SetPan;
	hook->vtable.SetVolume          = IDirectSoundBuffer_hook_SetVolume;
	
	hook->obj.lpVtbl = &(hook->vtable);
	
	/* Each IDirectSoundBuffer_hook instance is given a serial number to
	 * differentiate buffers in the log.
	 * 
	 * The primary buffer is given a serial of zero which keeps it out of
	 * the log.
	 * 
	 * BUG: Assumes less than 2^32 buffers over the program lifetime.
	*/
	
	static unsigned int buf_id = 0;
	hook->buf_id = (flags & DSBCAPS_PRIMARYBUFFER) ? 0 : ++buf_id;
	
	hook->lock_buf    = NULL;
	hook->lock_offset = 0;
	
	*obj = &(hook->obj);
	
	return hook;
}

/* --- IDirectSound hook methods --- */

static ULONG __stdcall IDirectSound_hook_AddRef(IDirectSound_hook *self)
{
	return IDirectSound_AddRef(self->real);
}

static HRESULT __stdcall IDirectSound_hook_QueryInterface(IDirectSound_hook *self, REFIID riid, void **ppvObject)
{
	return IDirectSound_QueryInterface(self->real, riid, ppvObject);
}

static ULONG __stdcall IDirectSound_hook_Release(IDirectSound_hook *self)
{
	ULONG refcount = IDirectSound_Release(self->real);
	
	if(refcount == 0)
	{
		free(self);
	}
	
	return refcount;
}

static HRESULT __stdcall IDirectSound_hook_Initialize(IDirectSound_hook *self, LPCGUID lpcGuid)
{
	return IDirectSound_Initialize(self->real, lpcGuid);
}

static HRESULT __stdcall IDirectSound_hook_SetCooperativeLevel(IDirectSound_hook *self, HWND hwnd, DWORD dwLevel)
{
	return IDirectSound_SetCooperativeLevel(self->real, hwnd, dwLevel);
}

static HRESULT __stdcall IDirectSound_hook_CreateSoundBuffer(struct IDirectSound_hook *self, const DSBUFFERDESC *lpcDSBufferDesc, IDirectSoundBuffer **lplpDirectSoundBuffer, IUnknown FAR *pUnkOuter)
{
	HRESULT result = IDirectSound_CreateSoundBuffer(self->real, lpcDSBufferDesc, lplpDirectSoundBuffer, pUnkOuter);
	
	if(result == DS_OK)
	{
		IDirectSoundBuffer_hook *hook = wrap_IDirectSoundBuffer(lplpDirectSoundBuffer, lpcDSBufferDesc->dwFlags);
		
		if(capture_fh && hook->buf_id)
		{
			audio_event event;
			
			event.check = 0x12345678;
			event.frame = get_frames();
			event.op    = AUDIO_OP_INIT;
			
			event.e.init.buf_id = hook->buf_id;
			event.e.init.size   = lpcDSBufferDesc->dwBufferBytes;
			
			event.e.init.sample_rate = lpcDSBufferDesc->lpwfxFormat->nSamplesPerSec;
			event.e.init.sample_bits = lpcDSBufferDesc->lpwfxFormat->wBitsPerSample;
			event.e.init.channels    = lpcDSBufferDesc->lpwfxFormat->nChannels;
			
			fwrite(&event, sizeof(event), 1, capture_fh);
		}
	}
	
	return result;
}

static HRESULT __stdcall IDirectSound_hook_DuplicateSoundBuffer(IDirectSound_hook *self, LPDIRECTSOUNDBUFFER lpDsbOriginal, LPLPDIRECTSOUNDBUFFER lplpDsbDuplicate)
{
	IDirectSoundBuffer_hook *old_hook = (IDirectSoundBuffer_hook*)(lpDsbOriginal);
	
	HRESULT result = IDirectSound_DuplicateSoundBuffer(self->real, old_hook->real, lplpDsbDuplicate);
	
	if(result == DS_OK)
	{
		DSBCAPS caps;
		IDirectSoundBuffer_GetCaps(*lplpDsbDuplicate, &caps);
		
		IDirectSoundBuffer_hook *new_hook = wrap_IDirectSoundBuffer(lplpDsbDuplicate, caps.dwFlags);
		
		if(capture_fh && new_hook->buf_id)
		{
			audio_event event;
			
			event.check = 0x12345678;
			event.frame = get_frames();
			event.op    = AUDIO_OP_CLONE;
			
			event.e.clone.src_buf_id = old_hook->buf_id;
			event.e.clone.new_buf_id = new_hook->buf_id;
			
			fwrite(&event, sizeof(event), 1, capture_fh);
		}
	}
	
	return result;
}

static HRESULT __stdcall IDirectSound_hook_GetCaps(IDirectSound_hook *self, LPDSCAPS lpDSCaps)
{
	return IDirectSound_GetCaps(self->real, lpDSCaps);
}

static HRESULT __stdcall IDirectSound_hook_Compact(IDirectSound_hook *self)
{
	return IDirectSound_Compact(self->real);
}

static HRESULT __stdcall IDirectSound_hook_GetSpeakerConfig(IDirectSound_hook *self, LPDWORD lpdwSpeakerConfig)
{
	return IDirectSound_GetSpeakerConfig(self->real, lpdwSpeakerConfig);
}

static HRESULT __stdcall IDirectSound_hook_SetSpeakerConfig(IDirectSound_hook *self, DWORD dwSpeakerConfig)
{
	return IDirectSound_SetSpeakerConfig(self->real, dwSpeakerConfig);
}

/* --- IDirectSoundBuffer hook methods */

static ULONG __stdcall IDirectSoundBuffer_hook_AddRef(IDirectSoundBuffer_hook *self)
{
	return IDirectSoundBuffer_AddRef(self->real);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_QueryInterface(IDirectSoundBuffer_hook *self, REFIID riid, void **ppvObject)
{
	return IDirectSoundBuffer_QueryInterface(self->real, riid, ppvObject);
}

static ULONG __stdcall IDirectSoundBuffer_hook_Release(IDirectSoundBuffer_hook *self)
{
	ULONG refcount = IDirectSoundBuffer_Release(self->real);
	
	if(refcount == 0)
	{
		if(capture_fh && self->buf_id)
		{
			audio_event event;
			
			event.check = 0x12345678;
			event.frame = get_frames();
			event.op    = AUDIO_OP_FREE;
			
			event.e.free.buf_id = self->buf_id;
			
			fwrite(&event, sizeof(event), 1, capture_fh);
		}
		
		free(self);
	}
	
	return refcount;
}

static HRESULT __stdcall IDirectSoundBuffer_hook_GetCaps(IDirectSoundBuffer_hook *self, LPDSBCAPS lpDSBufferCaps)
{
	return IDirectSoundBuffer_GetCaps(self->real, lpDSBufferCaps);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_GetFormat(IDirectSoundBuffer_hook *self, LPWAVEFORMATEX lpwfxFormat, DWORD dwSizeAllocated, LPDWORD lpdwSizeWritten)
{
	return IDirectSoundBuffer_GetFormat(self->real, lpwfxFormat, dwSizeAllocated, lpdwSizeWritten);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_GetStatus(IDirectSoundBuffer_hook *self, LPDWORD lpdwStatus)
{
	return IDirectSoundBuffer_GetStatus(self->real, lpdwStatus);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_SetFormat(IDirectSoundBuffer_hook *self, LPCWAVEFORMATEX lpcfxFormat)
{
	return IDirectSoundBuffer_SetFormat(self->real, lpcfxFormat);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_Initialize(IDirectSoundBuffer_hook *self, LPDIRECTSOUND lpDirectSound, LPCDSBUFFERDESC lpcDSBufferDesc)
{
	return IDirectSoundBuffer_Initialize(self->real, lpDirectSound, lpcDSBufferDesc);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_Restore(IDirectSoundBuffer_hook *self)
{
	return IDirectSoundBuffer_Restore(self->real);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_GetCurrentPosition(IDirectSoundBuffer_hook *self, LPDWORD lpdwCurrentPlayCursor, LPDWORD lpdwCurrentWriteCursor)
{
	return IDirectSoundBuffer_GetCurrentPosition(self->real, lpdwCurrentPlayCursor, lpdwCurrentWriteCursor);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_Lock(IDirectSoundBuffer_hook *self, DWORD dwWriteCursor, DWORD dwWriteBytes, LPVOID lplpvAudioPtr1, LPDWORD lpdwAudioBytes1, LPVOID lplpvAudioPtr2, LPDWORD lpdwAudioBytes2, DWORD dwFlags)
{
	HRESULT result = IDirectSoundBuffer_Lock(self->real, dwWriteCursor, dwWriteBytes, lplpvAudioPtr1, lpdwAudioBytes1, lplpvAudioPtr2, lpdwAudioBytes2, dwFlags);
	
	if(result == DS_OK)
	{
		if(dwFlags & DSBLOCK_FROMWRITECURSOR)
		{
			IDirectSoundBuffer_GetCurrentPosition(self->real, NULL, &dwWriteCursor);
		}
		
		self->lock_buf    = *(void**)(lplpvAudioPtr1);
		self->lock_offset = dwWriteCursor;
	}
	
	return result;
}

static HRESULT __stdcall IDirectSoundBuffer_hook_Unlock(IDirectSoundBuffer_hook *self, LPVOID lpvAudioPtr1, DWORD dwAudioBytes1, LPVOID lpvAudioPtr2, DWORD dwAudioBytes2)
{
	if(self->lock_buf == lpvAudioPtr1)
	{
		if(capture_fh && self->buf_id)
		{
			audio_event event;
			
			event.check = 0x12345678;
			event.frame = get_frames();
			event.op    = AUDIO_OP_LOAD;
			
			event.e.load.buf_id = self->buf_id;
			event.e.load.offset = self->lock_offset;
			event.e.load.size   = dwAudioBytes1;
			
			fwrite(&event, sizeof(event), 1, capture_fh);
			fwrite(lpvAudioPtr1, 1, dwAudioBytes1, capture_fh);
			
			if(lpvAudioPtr2)
			{
				/* NOTE: Always assumes lpvAudioPtr2 points to
				 * the start of the buffer. Is this guarenteed?
				*/
				
				event.e.load.offset = 0;
				event.e.load.size   = dwAudioBytes2;
				
				fwrite(&event, sizeof(event), 1, capture_fh);
				fwrite(lpvAudioPtr2, 1, dwAudioBytes2, capture_fh);
			}
		}
		
		self->lock_buf    = NULL;
		self->lock_offset = 0;
	}
	
	return IDirectSoundBuffer_Unlock(self->real, lpvAudioPtr1, dwAudioBytes1, lpvAudioPtr2, dwAudioBytes2);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_SetCurrentPosition(IDirectSoundBuffer_hook *self, DWORD dwNewPosition)
{
	if(capture_fh && self->buf_id)
	{
		audio_event event;
		
		event.check = 0x12345678;
		event.frame = get_frames();
		event.op    = AUDIO_OP_JMP;
		
		event.e.jmp.buf_id = self->buf_id;
		event.e.jmp.offset = dwNewPosition;
		
		fwrite(&event, sizeof(event), 1, capture_fh);
	}
	
	return IDirectSoundBuffer_SetCurrentPosition(self->real, dwNewPosition);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_Play(IDirectSoundBuffer_hook *self, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags)
{
	if(capture_fh && self->buf_id)
	{
		audio_event event;
		
		event.check = 0x12345678;
		event.frame = get_frames();
		event.op    = AUDIO_OP_START;
		
		event.e.start.buf_id = self->buf_id;
		event.e.start.loop   = !!(dwFlags & DSBPLAY_LOOPING);
		
		fwrite(&event, sizeof(event), 1, capture_fh);
	}
	
	return IDirectSoundBuffer_Play(self->real, dwReserved1, dwPriority, dwFlags);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_Stop(IDirectSoundBuffer_hook *self)
{
	if(capture_fh && self->buf_id)
	{
		audio_event event;
		
		event.check = 0x12345678;
		event.frame = get_frames();
		event.op    = AUDIO_OP_STOP;
		
		event.e.stop.buf_id = self->buf_id;
		
		fwrite(&event, sizeof(event), 1, capture_fh);
	}
	
	return IDirectSoundBuffer_Stop(self->real);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_GetFrequency(IDirectSoundBuffer_hook *self, LPDWORD lpdwFrequency)
{
	return IDirectSoundBuffer_GetFrequency(self->real, lpdwFrequency);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_GetPan(IDirectSoundBuffer_hook *self, LPLONG lplPan)
{
	return IDirectSoundBuffer_GetPan(self->real, lplPan);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_GetVolume(IDirectSoundBuffer_hook *self, LPLONG lplVolume)
{
	return IDirectSoundBuffer_GetVolume(self->real, lplVolume);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_SetFrequency(IDirectSoundBuffer_hook *self, DWORD dwFrequency)
{
	if(capture_fh && self->buf_id)
	{
		struct audio_event event;
		
		event.check = 0x12345678;
		event.frame = get_frames();
		event.op    = AUDIO_OP_FREQ;
		
		event.e.freq.buf_id      = self->buf_id;
		event.e.freq.sample_rate = dwFrequency;
		
		fwrite(&event, sizeof(event), 1, capture_fh);
	}
	
	return IDirectSoundBuffer_SetFrequency(self->real, dwFrequency);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_SetPan(IDirectSoundBuffer_hook *self, LONG lPan)
{
	return IDirectSoundBuffer_SetPan(self->real, lPan);
}

static HRESULT __stdcall IDirectSoundBuffer_hook_SetVolume(IDirectSoundBuffer_hook *self, LONG lVolume)
{
	if(capture_fh && self->buf_id)
	{
		struct audio_event event;
		
		event.check = 0x12345678;
		event.frame = get_frames();
		event.op    = AUDIO_OP_GAIN;
		
		event.e.gain.buf_id = self->buf_id;
		event.e.gain.gain   = (double)(lVolume + 10000) / 10000;
		
		fwrite(&event, sizeof(event), 1, capture_fh);
	}
	
	return IDirectSoundBuffer_SetVolume(self->real, lVolume);
}

/* --- DLL functions --- */

HRESULT __stdcall DirectSoundCreate(LPCGUID lpcGuid, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter)
{
	DirectSoundCreate_t sys_DirectSoundCreate = (DirectSoundCreate_t)(GetProcAddress(sys_dsound, "DirectSoundCreate"));
	if(!sys_DirectSoundCreate)
	{
		/* No DirectSoundCreate symbol in the system dsound.dll */
		abort();
	}
	
	HRESULT result = sys_DirectSoundCreate(lpcGuid, ppDS, pUnkOuter);
	
	if(result == DS_OK)
	{
		struct IDirectSound_hook *hook = malloc(sizeof(struct IDirectSound_hook));
		
		hook->real = *ppDS;
		
		hook->vtable.AddRef         = &IDirectSound_hook_AddRef;
		hook->vtable.QueryInterface = &IDirectSound_hook_QueryInterface;
		hook->vtable.Release        = &IDirectSound_hook_Release;
		
		hook->vtable.Initialize           = &IDirectSound_hook_Initialize;
		hook->vtable.SetCooperativeLevel  = &IDirectSound_hook_SetCooperativeLevel;
		hook->vtable.CreateSoundBuffer    = &IDirectSound_hook_CreateSoundBuffer;
		hook->vtable.DuplicateSoundBuffer = &IDirectSound_hook_DuplicateSoundBuffer;
		hook->vtable.GetCaps              = &IDirectSound_hook_GetCaps;
		hook->vtable.Compact              = &IDirectSound_hook_Compact;
		hook->vtable.GetSpeakerConfig     = &IDirectSound_hook_GetSpeakerConfig;
		hook->vtable.SetSpeakerConfig     = &IDirectSound_hook_SetSpeakerConfig;
		
		hook->obj.lpVtbl = &(hook->vtable);
		
		*ppDS = &(hook->obj);
	}
	
	return result;
}

BOOL WINAPI DllMain(HINSTANCE me, DWORD why, LPVOID res)
{
	if(why == DLL_PROCESS_ATTACH)
	{
		char *capture_folder_main = getenv("WA_CAPTURE_FOLDER_MAIN");
		_mkdir(capture_folder_main);

		char *capture_folder_current = getenv("WA_CAPTURE_FOLDER_CURRENT");
		_mkdir(capture_folder_current);

		char path[1024];
		
		/* Load the system dsound.dll and get the real DirectSoundCreate
		 * function from it.
		*/
		
		GetSystemDirectory(path, sizeof(path));
		strcat(path, "\\dsound.dll");
		
		if(!(sys_dsound = LoadLibrary(path)))
		{
			/* Couldn't load the system dsound.dll */
			abort();
		}
		
		char *capture_file = getenv("DSOUND_CAPTURE_FILE");
		if(capture_file)
		{
			if(!(capture_fh = fopen(capture_file, "wb")))
			{
				/* Couldn't open capture output file */
				abort();
			}
		}
		
		if(getenv("AREC_LOAD_WORMKIT"))
		{
			LoadLibrary("HookLib.dll");
		}
	}
	else if(why == DLL_PROCESS_DETACH)
	{
		if(capture_fh)
		{
			fclose(capture_fh);
		}
		
		FreeLibrary(sys_dsound);
	}
	
	return TRUE;
}

void is_dsound_wrapper(void)
{
	
}
