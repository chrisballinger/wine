/* IDirectMusicPerformance Implementation
 *
 * Copyright (C) 2003-2004 Rok Mandeljc
 * Copyright (C) 2003-2004 Raphael Junqueira
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "dmime_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dmime);

typedef struct DMUS_PMSGItem DMUS_PMSGItem;
struct DMUS_PMSGItem {
  DMUS_PMSGItem* next;
  DMUS_PMSGItem* prev;

  REFERENCE_TIME rtItemTime;
  BOOL bInUse;
  DWORD cb;
  DMUS_PMSG pMsg;
};

#define DMUS_PMSGToItem(pMSG)   ((DMUS_PMSGItem*) (((unsigned char*) pPMSG) -  offsetof(DMUS_PMSGItem, pMsg)))
#define DMUS_ItemToPMSG(pItem)  (&(pItem->pMsg))
#define DMUS_ItemRemoveFromQueue(This,pItem) \
{\
  if (pItem->prev) pItem->prev->next = pItem->next;\
  if (pItem->next) pItem->next->prev = pItem->prev;\
  if (This->head == pItem) This->head = pItem->next;\
  if (This->imm_head == pItem) This->imm_head = pItem->next;\
  pItem->bInUse = FALSE;\
}

#define PROCESSMSG_START           (WM_APP + 0)
#define PROCESSMSG_EXIT            (WM_APP + 1)
#define PROCESSMSG_REMOVE          (WM_APP + 2)
#define PROCESSMSG_ADD             (WM_APP + 4)


static DMUS_PMSGItem* ProceedMsg(IDirectMusicPerformance8Impl* This, DMUS_PMSGItem* cur) {
  if (cur->pMsg.dwType == DMUS_PMSGT_NOTIFICATION) {
    SetEvent(This->hNotification);
  }	
  DMUS_ItemRemoveFromQueue(This, cur);
  switch (cur->pMsg.dwType) {
  case DMUS_PMSGT_WAVE:
  case DMUS_PMSGT_TEMPO:   
  case DMUS_PMSGT_STOP:
  default:
    FIXME("Unhandled PMsg Type: 0x%lx\n", cur->pMsg.dwType);
    break;
  }
  return cur;
}

static DWORD WINAPI ProcessMsgThread(LPVOID lpParam) {
  IDirectMusicPerformance8Impl* This = (IDirectMusicPerformance8Impl*) lpParam;
  DWORD timeOut = INFINITE;
  MSG msg;
  HRESULT hr;
  REFERENCE_TIME rtLastTime;
  REFERENCE_TIME rtCurTime;
  DMUS_PMSGItem* it = NULL;
  DMUS_PMSGItem* cur = NULL;
  DMUS_PMSGItem* it_next = NULL;

  while (TRUE) {
    DWORD dwDec = This->rtLatencyTime + This->dwBumperLength;

    if (timeOut > 0) MsgWaitForMultipleObjects(0, NULL, FALSE, timeOut, QS_POSTMESSAGE|QS_SENDMESSAGE|QS_TIMER);
    timeOut = INFINITE;

    EnterCriticalSection(&This->safe);
    rtLastTime = rtCurTime;
    hr = IDirectMusicPerformance8Impl_GetTime((IDirectMusicPerformance8*) This, &rtCurTime, NULL);
    if (FAILED(hr)) {
      goto outrefresh;
    }
    
    for (it = This->imm_head; NULL != it; ) {
      it_next = it->next;
      cur = ProceedMsg(This, it);  
      if (NULL != cur) {
	HeapFree(GetProcessHeap(), 0, cur); 
      }
      it = it_next;
    }

    for (it = This->head; NULL != it && it->rtItemTime < rtCurTime + dwDec; ) {
      it_next = it->next;
      cur = ProceedMsg(This, it);
      if (NULL != cur) {
	HeapFree(GetProcessHeap(), 0, cur);
      }
      it = it_next;
    }
    if (NULL != it) {
      timeOut = ( it->rtItemTime - rtCurTime ) + This->rtLatencyTime;
    }

outrefresh:
    LeaveCriticalSection(&This->safe);
    
    while (TRUE == PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      /** if hwnd we suppose that is a windows event ... */
      if  (NULL != msg.hwnd) {
	TranslateMessage(&msg);
	DispatchMessageA(&msg);
      } else {
	switch (msg.message) {	    
	case WM_QUIT:
	case PROCESSMSG_EXIT:
	  goto outofthread;
	  break;
	case PROCESSMSG_START:
	  break;
	case PROCESSMSG_ADD:
	  break;
	case PROCESSMSG_REMOVE:
	  break;
	default:
	  ERR("Unhandled message %u. Critical Path\n", msg.message);
	  break;
	}
      }
    }

    /** here we should run a little of current AudioPath */

  }

outofthread:
  TRACE("(%p): Exiting\n", This);
  
  return 0;
}

static BOOL PostMessageToProcessMsgThread(IDirectMusicPerformance8Impl* This, UINT iMsg) {
  if (FALSE == This->procThreadTicStarted && PROCESSMSG_EXIT != iMsg) {
    This->procThread = CreateThread(NULL, 0, ProcessMsgThread, This, 0, &This->procThreadId);
    if (NULL == This->procThread) return FALSE;
    SetThreadPriority(This->procThread, THREAD_PRIORITY_TIME_CRITICAL);
    This->procThreadTicStarted = TRUE;
  }
  return PostThreadMessageA(This->procThreadId, iMsg, 0, 0);
}

/* IDirectMusicPerformance8 IUnknown part: */
HRESULT WINAPI IDirectMusicPerformance8Impl_QueryInterface (LPDIRECTMUSICPERFORMANCE8 iface, REFIID riid, LPVOID *ppobj) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  TRACE("(%p, %s,%p)\n", This, debugstr_dmguid(riid), ppobj);

  if (IsEqualIID (riid, &IID_IUnknown) || 
      IsEqualIID (riid, &IID_IDirectMusicPerformance) ||
      IsEqualIID (riid, &IID_IDirectMusicPerformance8)) {
    IDirectMusicPerformance8Impl_AddRef(iface);
    *ppobj = This;
    return S_OK;
  }
	
  WARN("(%p, %s,%p): not found\n", This, debugstr_dmguid(riid), ppobj);
  return E_NOINTERFACE;
}

ULONG WINAPI IDirectMusicPerformance8Impl_AddRef (LPDIRECTMUSICPERFORMANCE8 iface) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  TRACE("(%p): AddRef from %ld\n", This, This->ref);
  return ++(This->ref);
}

ULONG WINAPI IDirectMusicPerformance8Impl_Release (LPDIRECTMUSICPERFORMANCE8 iface) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  ULONG ref = --This->ref;
  TRACE("(%p): ReleaseRef to %ld\n", This, This->ref);
  if (ref == 0) {
    DeleteCriticalSection(&This->safe);
    HeapFree(GetProcessHeap(), 0, This);
  }
  return ref;
}

/* IDirectMusicPerformanceImpl IDirectMusicPerformance Interface part: */
HRESULT WINAPI IDirectMusicPerformance8Impl_Init (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusic** ppDirectMusic, LPDIRECTSOUND pDirectSound, HWND hWnd) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);

	FIXME("(iface = %p, dmusic = %p, dsound = %p, hwnd = %p)\n", This, ppDirectMusic, pDirectSound, hWnd);
 	if (This->pDirectMusic || This->pDirectSound)
	  return DMUS_E_ALREADY_INITED;
	
	if (NULL == hWnd) {
	  hWnd = GetForegroundWindow();
	}

	if (NULL != pDirectSound) {
	  This->pDirectSound = (IDirectSound*) pDirectSound;
	  IDirectSound_AddRef((LPDIRECTSOUND) This->pDirectSound);
	} else {
	  DirectSoundCreate8(NULL, (LPDIRECTSOUND8*) &This->pDirectSound, NULL);
	  /** 
	   * as seen in msdn
	   * 
	   *  http://msdn.microsoft.com/library/default.asp?url=/library/en-us/directx9_c/directX/htm/idirectmusicperformance8initaudio.asp
	   */
	  if (NULL != hWnd) {
	    IDirectSound8_SetCooperativeLevel(This->pDirectSound, hWnd, DSSCL_PRIORITY);
	  } else {
	    /* how to get the ForeGround window handle ? */
            /*IDirectSound8_SetCooperativeLevel(This->pDirectSound, hWnd, DSSCL_PRIORITY);*/
	  }
	  if (!This->pDirectSound)
	    return DSERR_NODRIVER;
	}

	if (NULL != ppDirectMusic && NULL != *ppDirectMusic) {
	  /* app creates it's own dmusic object and gives it to performance */
	  This->pDirectMusic = (IDirectMusic8*) *ppDirectMusic;
	  IDirectMusic8_AddRef((LPDIRECTMUSIC8) This->pDirectMusic);
	} else {
	  /* app allows the performance to initialise itfself and needs a pointer to object*/
          CoCreateInstance (&CLSID_DirectMusic, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectMusic8, (void**)&This->pDirectMusic);
	  if (ppDirectMusic) {
	    *ppDirectMusic = (LPDIRECTMUSIC) This->pDirectMusic;
	    IDirectMusic8_AddRef((LPDIRECTMUSIC8) *ppDirectMusic);
	  }
	}
	
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_PlaySegment (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicSegment* pSegment, DWORD dwFlags, __int64 i64StartTime, IDirectMusicSegmentState** ppSegmentState) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p, %ld, %lli, %p): stub\n", This, pSegment, dwFlags, i64StartTime, ppSegmentState);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_Stop (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicSegment* pSegment, IDirectMusicSegmentState* pSegmentState, MUSIC_TIME mtTime, DWORD dwFlags) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p, %p, %ld, %ld): stub\n", This, pSegment, pSegmentState, mtTime, dwFlags);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetSegmentState (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicSegmentState** ppSegmentState, MUSIC_TIME mtTime) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p,%p, %ld): stub\n", This, ppSegmentState, mtTime);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_SetPrepareTime (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwMilliSeconds) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  TRACE("(%p, %ld)\n", This, dwMilliSeconds);
  This->dwPrepareTime = dwMilliSeconds;
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetPrepareTime (LPDIRECTMUSICPERFORMANCE8 iface, DWORD* pdwMilliSeconds) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  TRACE("(%p, %p)\n", This, pdwMilliSeconds);
  if (NULL == pdwMilliSeconds) {
    return E_POINTER;
  }
  *pdwMilliSeconds = This->dwPrepareTime;
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_SetBumperLength (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwMilliSeconds) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  TRACE("(%p, %ld)\n", This, dwMilliSeconds);
  This->dwBumperLength =  dwMilliSeconds;
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetBumperLength (LPDIRECTMUSICPERFORMANCE8 iface, DWORD* pdwMilliSeconds) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  TRACE("(%p, %p)\n", This, pdwMilliSeconds);
  if (NULL == pdwMilliSeconds) {
    return E_POINTER;
  }
  *pdwMilliSeconds = This->dwBumperLength;
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_SendPMsg (LPDIRECTMUSICPERFORMANCE8 iface, DMUS_PMSG* pPMSG) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  DMUS_PMSGItem* pItem = NULL;
  DMUS_PMSGItem* it = NULL;
  DMUS_PMSGItem* prev_it = NULL;
  DMUS_PMSGItem** queue = NULL;

  FIXME("(%p, %p): stub\n", This, pPMSG);
	 
  if (NULL == pPMSG) {
    return E_POINTER;
  }
  pItem = DMUS_PMSGToItem(pPMSG);
  if (NULL == pItem) {
    return E_POINTER;
  }
  if (TRUE == pItem->bInUse) {
    return DMUS_E_ALREADY_SENT;
  }
  
  /* TODO: Valid Flags */
  /* TODO: DMUS_PMSGF_MUSICTIME */
  pItem->rtItemTime = pPMSG->rtTime;

  if (pPMSG->dwFlags & DMUS_PMSGF_TOOL_IMMEDIATE) {
    queue = &This->imm_head;
  } else {
    queue = &This->head;
  }

  EnterCriticalSection(&This->safe);
  for (it = *queue; NULL != it && it->rtItemTime < pItem->rtItemTime; it = it->next) {
    prev_it = it;
  }
  if (NULL == prev_it) {
    pItem->prev = NULL;
    if (NULL != *queue) pItem->next = (*queue)->next;
    /*assert( NULL == pItem->next->prev );*/
    if (NULL != pItem->next) pItem->next->prev = pItem;
    *queue = pItem;
  } else {
    pItem->prev = prev_it;
    pItem->next = prev_it->next;
    prev_it->next = pItem;
    if (NULL != pItem->next) pItem->next->prev = pItem;
  } 
  LeaveCriticalSection(&This->safe);

  /** now in use, prevent from stupid Frees */
  pItem->bInUse = TRUE;
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_MusicToReferenceTime (LPDIRECTMUSICPERFORMANCE8 iface, MUSIC_TIME mtTime, REFERENCE_TIME* prtTime) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %ld, %p): stub\n", This, mtTime, prtTime);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_ReferenceToMusicTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME rtTime, MUSIC_TIME* pmtTime) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %lli, %p): stub\n", This, rtTime, pmtTime);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_IsPlaying (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicSegment* pSegment, IDirectMusicSegmentState* pSegState) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p, %p): stub\n", This, pSegment, pSegState);
	return S_FALSE;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME* prtNow, MUSIC_TIME* pmtNow) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  HRESULT hr = S_OK;
  REFERENCE_TIME rtCur = 0;

  /*TRACE("(%p, %p, %p)\n", This, prtNow, pmtNow); */
  if (TRUE == This->procThreadTicStarted) {
    rtCur = ((REFERENCE_TIME) GetTickCount() * 10000) - This->procThreadStartTime;
  } else {
    /*return DMUS_E_NO_MASTER_CLOCK;*/
  }
  if (NULL != prtNow) {
    *prtNow = rtCur;
  }
  if (NULL != pmtNow) {
    hr = IDirectMusicPerformance8_ReferenceToMusicTime(iface, rtCur, pmtNow);
  }
  return hr;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_AllocPMsg (LPDIRECTMUSICPERFORMANCE8 iface, ULONG cb, DMUS_PMSG** ppPMSG) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  DMUS_PMSGItem* pItem = NULL;
  
  FIXME("(%p, %ld, %p): stub\n", This, cb, ppPMSG);
	
  if (sizeof(DMUS_PMSG) > cb) {
    return E_INVALIDARG;
  }
  if (NULL == ppPMSG) {
    return E_POINTER;
  }
  pItem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cb - sizeof(DMUS_PMSG)  + sizeof(DMUS_PMSGItem));
  if (NULL == pItem) {
    return E_OUTOFMEMORY;
  }
  pItem->pMsg.dwSize = cb;
  *ppPMSG = DMUS_ItemToPMSG(pItem);
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_FreePMsg (LPDIRECTMUSICPERFORMANCE8 iface, DMUS_PMSG* pPMSG) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  DMUS_PMSGItem* pItem = NULL;
  
  FIXME("(%p, %p): stub\n", This, pPMSG);
  
  if (NULL == pPMSG) {
    return E_POINTER;
  }
  pItem = DMUS_PMSGToItem(pPMSG);
  if (NULL == pItem) {
    return E_POINTER;
  }
  if (TRUE == pItem->bInUse) {
    /** prevent for freeing PMsg in queue (ie to be processed) */
    return DMUS_E_CANNOT_FREE;
  }
  /** now we can remove it safely */
  EnterCriticalSection(&This->safe);
  DMUS_ItemRemoveFromQueue( This, pItem );
  LeaveCriticalSection(&This->safe);

  /** TODO: see if we should Release the pItem->pMsg->punkUser and others Interfaces */
  HeapFree(GetProcessHeap(), 0, pItem);  
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetGraph (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicGraph** ppGraph) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  FIXME("(%p, %p): to check\n", This, ppGraph);
  if (NULL != This->pToolGraph) {
    *ppGraph = (LPDIRECTMUSICGRAPH) This->pToolGraph; 
    IDirectMusicGraph_AddRef((LPDIRECTMUSICGRAPH) *ppGraph);
  } else {
    return E_FAIL;
  }
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_SetGraph (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicGraph* pGraph) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  
  FIXME("(%p, %p): to check\n", This, pGraph);
  
  if (NULL != This->pToolGraph) {
    /* Todo clean buffers and tools before */
    IDirectMusicGraph_Release((LPDIRECTMUSICGRAPH) This->pToolGraph);
  }
  This->pToolGraph = pGraph;
  if (NULL != This->pToolGraph) {
    IDirectMusicGraph_AddRef((LPDIRECTMUSICGRAPH) This->pToolGraph);
  }
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_SetNotificationHandle (LPDIRECTMUSICPERFORMANCE8 iface, HANDLE hNotification, REFERENCE_TIME rtMinimum) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  FIXME("(%p, %p, %lli): stub\n", This, hNotification, rtMinimum);
  This->hNotification = hNotification;
  if (rtMinimum) This->rtMinimum = rtMinimum;
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetNotificationPMsg (LPDIRECTMUSICPERFORMANCE8 iface, DMUS_NOTIFICATION_PMSG** ppNotificationPMsg) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  
  
  FIXME("(%p, %p): stub\n", This, ppNotificationPMsg);
  if (NULL == ppNotificationPMsg) {
    return E_POINTER;
  }
  
  

  return S_FALSE;
  /*return S_OK;*/
}

HRESULT WINAPI IDirectMusicPerformance8Impl_AddNotificationType (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidNotificationType) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %s): stub\n", This, debugstr_dmguid(rguidNotificationType));
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_RemoveNotificationType (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidNotificationType) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %s): stub\n", This, debugstr_dmguid(rguidNotificationType));
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_AddPort (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicPort* pPort) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p): stub\n", This, pPort);
	IDirectMusicPort_AddRef (pPort);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_RemovePort (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicPort* pPort) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p): stub\n", This, pPort);
	IDirectMusicPort_Release (pPort);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_AssignPChannelBlock (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwBlockNum, IDirectMusicPort* pPort, DWORD dwGroup) {
	int i, j, range /* min value in range */;
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);

	FIXME("(%p, %ld, %p, %ld): semi-stub\n", This, dwBlockNum, pPort, dwGroup-1);
	range = 16 * dwBlockNum;
	j = 0;
                
	for (i = range; i < range+16; i++) {
		/*TRACE("Setting PChannel[%i] to port %p, group %ld, MIDI port %i\n", i, pPort, dwGroup-1, j); */
		This->PChannel[i].port = pPort; 
		This->PChannel[i].group = dwGroup - 1; /* first index is always zero */
		This->PChannel[i].channel = j; /* FIXME: should this be assigned? */
		j++;
	}

	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_AssignPChannel (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwPChannel, IDirectMusicPort* pPort, DWORD dwGroup, DWORD dwMChannel) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);

	TRACE("(%p, %ld, %p, %ld, %ld)\n", This, dwPChannel, pPort, dwGroup, dwMChannel);
	This->PChannel[dwPChannel].port = pPort; 
	This->PChannel[dwPChannel].group = dwGroup; 
	This->PChannel[dwPChannel].channel = dwMChannel;

	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_PChannelInfo (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwPChannel, IDirectMusicPort** ppPort, DWORD* pdwGroup, DWORD* pdwMChannel) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %ld, %p, %p, %p): stub\n", This, dwPChannel, ppPort, pdwGroup, pdwMChannel);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_DownloadInstrument (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicInstrument* pInst, DWORD dwPChannel, IDirectMusicDownloadedInstrument** ppDownInst, DMUS_NOTERANGE* pNoteRanges, DWORD dwNumNoteRanges, IDirectMusicPort** ppPort, DWORD* pdwGroup, DWORD* pdwMChannel) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p, %ld, %p, %p, %ld, %p, %p, %p): stub\n", This, pInst, dwPChannel, ppDownInst, pNoteRanges, dwNumNoteRanges, ppPort, pdwGroup, pdwMChannel);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_Invalidate (LPDIRECTMUSICPERFORMANCE8 iface, MUSIC_TIME mtTime, DWORD dwFlags) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %ld, %ld): stub\n", This, mtTime, dwFlags);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetParam (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, DWORD dwGroupBits, DWORD dwIndex, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %s, %ld, %ld, %ld, %p, %p): stub\n", This, debugstr_dmguid(rguidType), dwGroupBits, dwIndex, mtTime, pmtNext, pParam);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_SetParam (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, DWORD dwGroupBits, DWORD dwIndex, MUSIC_TIME mtTime, void* pParam) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %s, %ld, %ld, %ld, %p): stub\n", This, debugstr_dmguid(rguidType), dwGroupBits, dwIndex, mtTime, pParam);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetGlobalParam (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, void* pParam, DWORD dwSize) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);

	TRACE("(%p, %s, %p, %ld): stub\n", This, debugstr_dmguid(rguidType), pParam, dwSize);
	
	if (IsEqualGUID (rguidType, &GUID_PerfAutoDownload))
		memcpy(pParam, &This->fAutoDownload, sizeof(&This->fAutoDownload));
	if (IsEqualGUID (rguidType, &GUID_PerfMasterGrooveLevel))
		memcpy(pParam, &This->cMasterGrooveLevel, sizeof(&This->cMasterGrooveLevel));
	if (IsEqualGUID (rguidType, &GUID_PerfMasterTempo))
		memcpy(pParam, &This->fMasterTempo, sizeof(&This->fMasterTempo));
	if (IsEqualGUID (rguidType, &GUID_PerfMasterVolume))
		memcpy(pParam, &This->lMasterVolume, sizeof(&This->lMasterVolume));

	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_SetGlobalParam (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, void* pParam, DWORD dwSize) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);

	TRACE("(%p, %s, %p, %ld)\n", This, debugstr_dmguid(rguidType), pParam, dwSize);
	
	if (IsEqualGUID (rguidType, &GUID_PerfAutoDownload)) {
		memcpy(&This->fAutoDownload, pParam, dwSize);
		TRACE("=> AutoDownload set to %d\n", This->fAutoDownload);
	}
	if (IsEqualGUID (rguidType, &GUID_PerfMasterGrooveLevel)) {
		memcpy(&This->cMasterGrooveLevel, pParam, dwSize);
		TRACE("=> MasterGrooveLevel set to %i\n", This->cMasterGrooveLevel);
	}
	if (IsEqualGUID (rguidType, &GUID_PerfMasterTempo)) {
		memcpy(&This->fMasterTempo, pParam, dwSize);
		TRACE("=> MasterTempo set to %f\n", This->fMasterTempo);
	}
	if (IsEqualGUID (rguidType, &GUID_PerfMasterVolume)) {
		memcpy(&This->lMasterVolume, pParam, dwSize);
		TRACE("=> MasterVolume set to %li\n", This->lMasterVolume);
	}

	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetLatencyTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME* prtTime) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	TRACE("(%p, %p): stub\n", This, prtTime);
	*prtTime = This->rtLatencyTime;
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetQueueTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME* prtTime) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p): stub\n", This, prtTime);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_AdjustTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME rtAmount) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %lli): stub\n", This, rtAmount);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_CloseDown (LPDIRECTMUSICPERFORMANCE8 iface) {
  ICOM_THIS(IDirectMusicPerformance8Impl,iface);
  FIXME("(%p): stub\n", This);
  if (PostMessageToProcessMsgThread(This, PROCESSMSG_EXIT)) {
    WaitForSingleObject(This->procThread, INFINITE);
    This->procThreadTicStarted = FALSE;
    CloseHandle(This->procThread);
  }
  if (NULL != This->pDirectSound) {
    IDirectSound_Release((LPDIRECTSOUND) This->pDirectSound);
    This->pDirectSound = NULL;
  }
  if (NULL != This->pDirectMusic) {
    IDirectMusic8_Release((LPDIRECTMUSIC8) This->pDirectMusic);
    This->pDirectMusic = NULL;
  }
  return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetResolvedTime (LPDIRECTMUSICPERFORMANCE8 iface, REFERENCE_TIME rtTime, REFERENCE_TIME* prtResolved, DWORD dwTimeResolveFlags) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %lli, %p, %ld): stub\n", This, rtTime, prtResolved, dwTimeResolveFlags);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_MIDIToMusic (LPDIRECTMUSICPERFORMANCE8 iface, BYTE bMIDIValue, DMUS_CHORD_KEY* pChord, BYTE bPlayMode, BYTE bChordLevel, WORD* pwMusicValue) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %d, %p, %d, %d, %p): stub\n", This, bMIDIValue, pChord, bPlayMode, bChordLevel, pwMusicValue);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_MusicToMIDI (LPDIRECTMUSICPERFORMANCE8 iface, WORD wMusicValue, DMUS_CHORD_KEY* pChord, BYTE bPlayMode, BYTE bChordLevel, BYTE* pbMIDIValue) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %d, %p, %d, %d, %p): stub\n", This, wMusicValue, pChord, bPlayMode, bChordLevel, pbMIDIValue);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_TimeToRhythm (LPDIRECTMUSICPERFORMANCE8 iface, MUSIC_TIME mtTime, DMUS_TIMESIGNATURE* pTimeSig, WORD* pwMeasure, BYTE* pbBeat, BYTE* pbGrid, short* pnOffset) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %ld, %p, %p, %p, %p, %p): stub\n", This, mtTime, pTimeSig, pwMeasure, pbBeat, pbGrid, pnOffset);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_RhythmToTime (LPDIRECTMUSICPERFORMANCE8 iface, WORD wMeasure, BYTE bBeat, BYTE bGrid, short nOffset, DMUS_TIMESIGNATURE* pTimeSig, MUSIC_TIME* pmtTime) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %d, %d, %d, %i, %p, %p): stub\n", This, wMeasure, bBeat, bGrid, nOffset, pTimeSig, pmtTime);
	return S_OK;
}

/* IDirectMusicPerformance8 Interface part follow: */
HRESULT WINAPI IDirectMusicPerformance8Impl_InitAudio (LPDIRECTMUSICPERFORMANCE8 iface, 
						      IDirectMusic** ppDirectMusic, 
						      IDirectSound** ppDirectSound, 
						      HWND hWnd, 
						      DWORD dwDefaultPathType, 
						      DWORD dwPChannelCount, 
						      DWORD dwFlags, 
						      DMUS_AUDIOPARAMS* pParams) {

	IDirectSound* dsound = NULL;
	HRESULT hr = S_OK;
	
        ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p, %p, %p, %lx, %lu, %lx, %p): to check\n", This, ppDirectMusic, ppDirectSound, hWnd, dwDefaultPathType, dwPChannelCount, dwFlags, pParams);

	if (This->pDirectMusic || This->pDirectSound)
	  return DMUS_E_ALREADY_INITED;

	if (NULL != ppDirectSound && NULL != *ppDirectSound) {
	  dsound = *ppDirectSound;
	} else {
	  hr = DirectSoundCreate8 (NULL, (LPDIRECTSOUND8*) &dsound, NULL);
	  FIXME("return dsound(%p,%ld)\n", dsound, hr);
	  if (FAILED(hr) || !dsound)
	    return DSERR_NODRIVER;
	  if (ppDirectSound)
	    *ppDirectSound = dsound;  
	}
	
	IDirectMusicPerformance8Impl_Init(iface, ppDirectMusic, dsound, hWnd);

	/* Init increases the ref count of the dsound object. Decremente it if the app don't want a pointer to the object. */
	if (NULL == ppDirectSound) {
	  IDirectSound_Release(This->pDirectSound);
	}

	/* as seen in msdn we need params init before audio path creation */
	if (NULL != pParams) {
	  memcpy(&This->pParams, pParams, sizeof(DMUS_AUDIOPARAMS));
	} else {
	  /**
	   * TODO, how can i fill the struct 
	   * as seen at http://msdn.microsoft.com/library/default.asp?url=/library/en-us/directx9_c/directX/htm/dmusaudioparams.asp
	   */
	  memset(&This->pParams, 0, sizeof(DMUS_AUDIOPARAMS));
	  This->pParams.dwSize = sizeof(DMUS_AUDIOPARAMS);
	  This->pParams.fInitNow = FALSE;
	  This->pParams.dwValidData = DMUS_AUDIOPARAMS_FEATURES | DMUS_AUDIOPARAMS_VOICES | DMUS_AUDIOPARAMS_SAMPLERATE | DMUS_AUDIOPARAMS_DEFAULTSYNTH;
	  This->pParams.dwVoices = 64;
	  This->pParams.dwSampleRate = (DWORD) 22.050; 
	  This->pParams.dwFeatures = dwFlags;
	  This->pParams.clsidDefaultSynth = CLSID_DirectMusicSynthSink;
	}
	hr = IDirectMusicPerformance8Impl_CreateStandardAudioPath(iface, dwDefaultPathType, dwPChannelCount, FALSE, (IDirectMusicAudioPath**) &This->pDefaultPath);

	PostMessageToProcessMsgThread(This, PROCESSMSG_START);

	return hr;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_PlaySegmentEx (LPDIRECTMUSICPERFORMANCE8 iface, IUnknown* pSource, WCHAR* pwzSegmentName, IUnknown* pTransition, DWORD dwFlags, __int64 i64StartTime, IDirectMusicSegmentState** ppSegmentState, IUnknown* pFrom, IUnknown* pAudioPath) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p, %p, %p, %ld, %lli, %p, %p, %p): stub\n", This, pSource, pwzSegmentName, pTransition, dwFlags, i64StartTime, ppSegmentState, pFrom, pAudioPath);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_StopEx (LPDIRECTMUSICPERFORMANCE8 iface, IUnknown* pObjectToStop, __int64 i64StopTime, DWORD dwFlags) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p, %lli, %ld): stub\n", This, pObjectToStop, i64StopTime, dwFlags);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_ClonePMsg (LPDIRECTMUSICPERFORMANCE8 iface, DMUS_PMSG* pSourcePMSG, DMUS_PMSG** ppCopyPMSG) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p, %p): stub\n", This, pSourcePMSG, ppCopyPMSG);
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_CreateAudioPath (LPDIRECTMUSICPERFORMANCE8 iface, IUnknown* pSourceConfig, BOOL fActivate, IDirectMusicAudioPath** ppNewPath) {
	IDirectMusicAudioPathImpl *default_path;
	IDirectMusicAudioPath *pPath;

	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	FIXME("(%p, %p, %d, %p): stub\n", This, pSourceConfig, fActivate, ppNewPath);

	if (NULL == ppNewPath) {
	  return E_POINTER;
	}

	DMUSIC_CreateDirectMusicAudioPathImpl (&IID_IDirectMusicAudioPath, (LPVOID*)&pPath, NULL);
	default_path = (IDirectMusicAudioPathImpl*)((char*)(pPath) - offsetof(IDirectMusicAudioPathImpl,AudioPathVtbl));
	default_path->pPerf = (IDirectMusicPerformance8*) This;

	/** TODO */
	
	*ppNewPath = (LPDIRECTMUSICAUDIOPATH) pPath;

	return IDirectMusicAudioPathImpl_IDirectMusicAudioPath_Activate(*ppNewPath, fActivate);
}

/**
 * see  http://msdn.microsoft.com/library/default.asp?url=/library/en-us/directx9_c/directX/htm/standardaudiopaths.asp
 */
HRESULT WINAPI IDirectMusicPerformance8Impl_CreateStandardAudioPath (LPDIRECTMUSICPERFORMANCE8 iface, DWORD dwType, DWORD dwPChannelCount, BOOL fActivate, IDirectMusicAudioPath** ppNewPath) {
	IDirectMusicAudioPathImpl *default_path;
	IDirectMusicAudioPath *pPath;
	DSBUFFERDESC desc;
	WAVEFORMATEX format;
	LPDIRECTSOUNDBUFFER buffer;
	HRESULT hr = S_OK;

	ICOM_THIS(IDirectMusicPerformance8Impl,iface);
	
	FIXME("(%p)->(%ld, %ld, %d, %p): semi-stub\n", This, dwType, dwPChannelCount, fActivate, ppNewPath);

	if (NULL == ppNewPath) {
	  return E_POINTER;
	}
	
	DMUSIC_CreateDirectMusicAudioPathImpl (&IID_IDirectMusicAudioPath, (LPVOID*)&pPath, NULL);
	default_path = (IDirectMusicAudioPathImpl*)((char*)(pPath) - offsetof(IDirectMusicAudioPathImpl,AudioPathVtbl));
	default_path->pPerf = (IDirectMusicPerformance8*) This;
	
	/* Secondary buffer description */
	memset(&format, 0, sizeof(format));
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 1;
	format.nSamplesPerSec = 44000;
	format.nAvgBytesPerSec = 44000*2;
	format.nBlockAlign = 2;
	format.wBitsPerSample = 16;
	format.cbSize = 0;
	
	memset(&desc, 0, sizeof(desc));
	desc.dwSize = sizeof(desc);
	desc.dwFlags = DSBCAPS_CTRLFX | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS;
	desc.dwBufferBytes = DSBSIZE_MIN;
	desc.dwReserved = 0;
	desc.lpwfxFormat = &format;
	desc.guid3DAlgorithm = GUID_NULL;
	
	switch(dwType) {
	case DMUS_APATH_DYNAMIC_3D:
                desc.dwFlags |= DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_MUTE3DATMAXDISTANCE;
		break;
	case DMUS_APATH_DYNAMIC_MONO:
	        desc.dwFlags |= DSBCAPS_CTRLFREQUENCY;
		break;
	case DMUS_APATH_SHARED_STEREOPLUSREVERB:
	        /* normally we havet to create 2 buffers (one for music other for reverb) 
		 * in this case. See msdn
                 */
	case DMUS_APATH_DYNAMIC_STEREO:
		desc.dwFlags |= DSBCAPS_CTRLFREQUENCY;
		format.nChannels = 2;
		format.nBlockAlign *= 2;
		format.nAvgBytesPerSec *=2;
		break;
	default:
	        HeapFree(GetProcessHeap(), 0, default_path); 
	        *ppNewPath = NULL;
	        return E_INVALIDARG;
	        break;
	}

	/* FIXME: Should we create one secondary buffer for each PChannel? */
	hr = IDirectSound8_CreateSoundBuffer ((LPDIRECTSOUND8) This->pDirectSound, &desc, &buffer, NULL);
	if (FAILED(hr)) {
	        HeapFree(GetProcessHeap(), 0, default_path); 
	        *ppNewPath = NULL;
	        return DSERR_BUFFERLOST;
	}
	default_path->pDSBuffer = buffer;

	/* Update description for creating primary buffer */
	desc.dwFlags |= DSBCAPS_PRIMARYBUFFER;
	desc.dwBufferBytes = 0;
	desc.lpwfxFormat = NULL;

	hr = IDirectSound8_CreateSoundBuffer ((LPDIRECTSOUND8) This->pDirectSound, &desc, &buffer, NULL);
	if (FAILED(hr)) {
                IDirectSoundBuffer_Release(default_path->pDSBuffer);
	        HeapFree(GetProcessHeap(), 0, default_path); 
	        *ppNewPath = NULL;
	        return DSERR_BUFFERLOST;
	}
	default_path->pPrimary = buffer;

	*ppNewPath = (LPDIRECTMUSICAUDIOPATH) pPath;
	
	TRACE(" returning IDirectMusicPerformance interface at %p.\n", *ppNewPath);

	return IDirectMusicAudioPathImpl_IDirectMusicAudioPath_Activate(*ppNewPath, fActivate);
}

HRESULT WINAPI IDirectMusicPerformance8Impl_SetDefaultAudioPath (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicAudioPath* pAudioPath) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);

	FIXME("(%p, %p): semi-stub\n", This, pAudioPath);
	if (NULL != This->pDefaultPath) {
		IDirectMusicAudioPath_Release((LPDIRECTMUSICAUDIOPATH) This->pDefaultPath);
		((IDirectMusicAudioPathImpl*) This->pDefaultPath)->pPerf = NULL;
		This->pDefaultPath = NULL;
	}
	This->pDefaultPath = pAudioPath;
	if (NULL != This->pDefaultPath) {
		IDirectMusicAudioPath_AddRef((LPDIRECTMUSICAUDIOPATH) This->pDefaultPath);
		((IDirectMusicAudioPathImpl*) This->pDefaultPath)->pPerf = (IDirectMusicPerformance8*) This;	
	}
	
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetDefaultAudioPath (LPDIRECTMUSICPERFORMANCE8 iface, IDirectMusicAudioPath** ppAudioPath) {
    ICOM_THIS(IDirectMusicPerformance8Impl,iface);

	FIXME("(%p, %p): semi-stub (%p)\n", This, ppAudioPath, This->pDefaultPath);

	if (NULL != This->pDefaultPath) {
	  *ppAudioPath = (LPDIRECTMUSICAUDIOPATH) This->pDefaultPath;
          IDirectMusicAudioPath_AddRef(*ppAudioPath);
        } else {
	  *ppAudioPath = NULL;
        }
	return S_OK;
}

HRESULT WINAPI IDirectMusicPerformance8Impl_GetParamEx (LPDIRECTMUSICPERFORMANCE8 iface, REFGUID rguidType, DWORD dwTrackID, DWORD dwGroupBits, DWORD dwIndex, MUSIC_TIME mtTime, MUSIC_TIME* pmtNext, void* pParam) {
	ICOM_THIS(IDirectMusicPerformance8Impl,iface);

	FIXME("(%p, %s, %ld, %ld, %ld, %ld, %p, %p): stub\n", This, debugstr_dmguid(rguidType), dwTrackID, dwGroupBits, dwIndex, mtTime, pmtNext, pParam);

	return S_OK;
}

ICOM_VTABLE(IDirectMusicPerformance8) DirectMusicPerformance8_Vtbl = {
    ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE
	IDirectMusicPerformance8Impl_QueryInterface,
	IDirectMusicPerformance8Impl_AddRef,
	IDirectMusicPerformance8Impl_Release,
	IDirectMusicPerformance8Impl_Init,
	IDirectMusicPerformance8Impl_PlaySegment,
	IDirectMusicPerformance8Impl_Stop,
	IDirectMusicPerformance8Impl_GetSegmentState,
	IDirectMusicPerformance8Impl_SetPrepareTime,
	IDirectMusicPerformance8Impl_GetPrepareTime,
	IDirectMusicPerformance8Impl_SetBumperLength,
	IDirectMusicPerformance8Impl_GetBumperLength,
	IDirectMusicPerformance8Impl_SendPMsg,
	IDirectMusicPerformance8Impl_MusicToReferenceTime,
	IDirectMusicPerformance8Impl_ReferenceToMusicTime,
	IDirectMusicPerformance8Impl_IsPlaying,
	IDirectMusicPerformance8Impl_GetTime,
	IDirectMusicPerformance8Impl_AllocPMsg,
	IDirectMusicPerformance8Impl_FreePMsg,
	IDirectMusicPerformance8Impl_GetGraph,
	IDirectMusicPerformance8Impl_SetGraph,
	IDirectMusicPerformance8Impl_SetNotificationHandle,
	IDirectMusicPerformance8Impl_GetNotificationPMsg,
	IDirectMusicPerformance8Impl_AddNotificationType,
	IDirectMusicPerformance8Impl_RemoveNotificationType,
	IDirectMusicPerformance8Impl_AddPort,
	IDirectMusicPerformance8Impl_RemovePort,
	IDirectMusicPerformance8Impl_AssignPChannelBlock,
	IDirectMusicPerformance8Impl_AssignPChannel,
	IDirectMusicPerformance8Impl_PChannelInfo,
	IDirectMusicPerformance8Impl_DownloadInstrument,
	IDirectMusicPerformance8Impl_Invalidate,
	IDirectMusicPerformance8Impl_GetParam,
	IDirectMusicPerformance8Impl_SetParam,
	IDirectMusicPerformance8Impl_GetGlobalParam,
	IDirectMusicPerformance8Impl_SetGlobalParam,
	IDirectMusicPerformance8Impl_GetLatencyTime,
	IDirectMusicPerformance8Impl_GetQueueTime,
	IDirectMusicPerformance8Impl_AdjustTime,
	IDirectMusicPerformance8Impl_CloseDown,
	IDirectMusicPerformance8Impl_GetResolvedTime,
	IDirectMusicPerformance8Impl_MIDIToMusic,
	IDirectMusicPerformance8Impl_MusicToMIDI,
	IDirectMusicPerformance8Impl_TimeToRhythm,
	IDirectMusicPerformance8Impl_RhythmToTime,
	IDirectMusicPerformance8Impl_InitAudio,
	IDirectMusicPerformance8Impl_PlaySegmentEx,
	IDirectMusicPerformance8Impl_StopEx,
	IDirectMusicPerformance8Impl_ClonePMsg,
	IDirectMusicPerformance8Impl_CreateAudioPath,
	IDirectMusicPerformance8Impl_CreateStandardAudioPath,
	IDirectMusicPerformance8Impl_SetDefaultAudioPath,
	IDirectMusicPerformance8Impl_GetDefaultAudioPath,
	IDirectMusicPerformance8Impl_GetParamEx
};

/* for ClassFactory */
HRESULT WINAPI DMUSIC_CreateDirectMusicPerformanceImpl (LPCGUID lpcGUID, LPVOID *ppobj, LPUNKNOWN pUnkOuter) {
	IDirectMusicPerformance8Impl *obj;

	TRACE("(%p,%p,%p)\n", lpcGUID, ppobj, pUnkOuter);

	obj = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(IDirectMusicPerformance8Impl));
	if (NULL == obj) 	{
		*ppobj = (LPDIRECTMUSICPERFORMANCE8)NULL;
		return E_OUTOFMEMORY;
	}
	obj->lpVtbl = &DirectMusicPerformance8_Vtbl;
	obj->ref = 0;  /* will be inited by QueryInterface */
	obj->pDirectMusic = NULL;
	obj->pDirectSound = NULL;
	obj->pDefaultPath = NULL;
	InitializeCriticalSection(&obj->safe);

	/**
	 * @see http://msdn.microsoft.com/archive/default.asp?url=/archive/en-us/directx9_c/directx/htm/latencyandbumpertime.asp
	 */
	obj->rtLatencyTime  = 100;  /* 100ms TO FIX */
	obj->dwBumperLength =   50; /* 50ms default */
	obj->dwPrepareTime  = 1000; /* 1000ms default */
	return IDirectMusicPerformance8Impl_QueryInterface ((LPDIRECTMUSICPERFORMANCE8)obj, lpcGUID, ppobj);
}
