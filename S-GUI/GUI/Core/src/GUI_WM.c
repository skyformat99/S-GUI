#include "GUI_WM.h"
#include "GUI.h"
#include "RootWindow.h"

/* 找到窗口Z序最大的子窗口（包括它自己）的下一个窗口，注意窗口h一定不能是NULL */
#define WM__FindChildEnd(h) \
    (((WM_Obj *)WM_GetTopChildWindow(h))->hNextLine)

#if GUI_USE_MEMORY
static GUI_RECT __PaintArea;
#endif

static int __InvalidWindowNum = 0;

/* 窗口管理器初始化 */
GUI_RESULT WM_Init(void)
{
    GUI_HWIN *p = &_hRootWin;
    
    *p = GUI_fastmalloc(sizeof(WM_Obj));
    if (*p == NULL) {
        return GUI_ERR;
    }
    WM_RootWindowInit(*p);
    return GUI_OK;
}

/*
* 向窗口管理器的指定窗口发送消息。
* hWin:要指定窗口的句柄,为NULL时将直接返回。
**/
void WM__SendMessage(WM_HWIN hWin, WM_MESSAGE *pMsg)
{
    static u_8 __MseeageCount;

    if (hWin == NULL) { /* hWin不能为NULL */
        return;
    }
    GUI_LOCK();
    if (__MseeageCount < GUI_MAX_MSG_NEST) {
        ++__MseeageCount;
        pMsg->hWin = hWin;
        if (((WM_Obj *)hWin)->WinCb) {
            ((WM_Obj *)hWin)->WinCb(pMsg);
        }
        --__MseeageCount;
    }
#if GUI_DEBUG_MODE
    else {
        GUI_DEBUG_OUT("GUI message nesting exceeded.");
    }
#endif
    GUI_UNLOCK();
}

/* 向窗口管理器的指定窗口发送消息 */
void WM_SendMessage(WM_HWIN hWin, u_16 MsgId, GUI_PARAM Param)
{
    WM_MESSAGE Msg;

    Msg.MsgId = MsgId;
    Msg.Param = Param;
    WM__SendMessage(hWin, &Msg);
}

/* 向窗口管理器的指定窗口的父窗口发送消息 */
void WM_SendMessageToParent(WM_HWIN hWin, GUI_MESSAGE *pMsg)
{
    pMsg->hWinSrc = hWin;
    WM__SendMessage(((WM_Obj *)hWin)->hParent, pMsg);
}

/* 向消息队列发送消息 */
GUI_RESULT WM_PostMessage(WM_HWIN hWin, u_16 MsgId, GUI_PARAM Param)
{
    WM_MESSAGE Msg;

    Msg.hWin = hWin;
    Msg.MsgId = MsgId;
    Msg.Param = Param;
    return GUI_PostMessage(&Msg);
}

/* 裁剪透明窗口的子窗口 */
static GUI_AREA _ClipTransChildren(GUI_AREA L, WM_Obj *pWin)
{
    for (pWin = pWin->hFirstChild; pWin && L; pWin = pWin->hNext) {
        if (pWin->Status & WM_WS_TRANS) {
            L = _ClipTransChildren(L, pWin); /* 裁剪子窗口 */
        } else {
            L = GUI_ReCalcRectList(L, &pWin->Rect);  /* 裁剪透明窗口的孩子 */
        }
    }
    return L;
}

/* 为一个窗口计算裁剪矩形链表 */
static GUI_AREA WM__ClipWindowArea(WM_HWIN hWin)
{
    WM_Obj *pWin = hWin, *pObj;
    GUI_RECT Rect1;
    GUI_AREA Area;

    /* 在考虑遮挡之前,窗口就只有一个裁剪矩形 */
    Area = GUI_GetRectList(1);
    if (Area) {
        Rect1 = WM_GetTaliorInvalidRect(pWin);
        Area->Rect = Rect1;
    }
    /* 窗口会被它的儿子们或者右边的兄弟们(如果有的话)裁剪,也就是遮挡，遍历它的孩子和兄弟们，
       逐个计算窗口的裁剪矩形链表，最后就能得到这个窗口被它们遮挡后的裁剪矩形链表. */
    /* 再遍历它右边的同属窗口及祖先的同属窗口 */
    pObj = pWin;
    while (pObj != _pRootWin && Area) {
        while (pObj->hNext && Area) {
            pObj = pObj->hNext; /* 向右遍历 */
            /* 如果是普通窗口就直接计算裁剪，如果是透明窗口就用它的孩子来计算裁剪 */
            if (pObj->Status & WM_WS_TRANS) {
                Area = _ClipTransChildren(Area, pObj);
            } else {
                Area = GUI_ReCalcRectList(Area, &pObj->Rect);
            }
        }
        pObj = pObj->hParent; /* 向上遍历 */
    }
    if (pWin->hFirstChild && Area) {
        /* 先遍历子窗口 */
        pObj = pWin->hFirstChild;
        while (pObj && Area) {
            /* 如果是普通窗口就直接计算裁剪，如果是透明窗口就用它的孩子来计算裁剪 */
            if (pObj->Status & WM_WS_TRANS) {
                Area = _ClipTransChildren(Area, pObj);
            } else {
                Area = GUI_ReCalcRectList(Area, &pObj->Rect);
            }
            pObj = pObj->hNext; /* 向右遍历 */
        }
    }
    return Area;
}

/*
* 将一个矩形裁剪到窗口的可见区域中。
* 返回值:FALSE,该矩形与窗口的可见区域不相交。
*       TRUE,裁剪成功。
**/
static GUI_BOOL WM__ClipAtParent(GUI_RECT *pr, WM_Obj *pWin)
{
    for (; pWin; pWin = pWin->hParent) {
        if (GUI_RectOverlay(pr, pr, &pWin->Rect) == FALSE) {
            return FALSE;
        }
    }
    return TRUE;
}

/* 窗口部分区域无效化，内部调用 */
static void _Invalidate1Abs(WM_Obj *pWin, GUI_RECT *pr)
{
    if (pWin->Status & WM_WS_INVAILD) {
        GUI_RectSum(&pWin->InvalidRect, &pWin->InvalidRect, pr);
    } else {
        pWin->InvalidRect = *pr;
        pWin->Status |= WM_WS_INVAILD;
        ++__InvalidWindowNum;
    }
}

/* 重绘一个窗口 */
static void _PaintOne(WM_Obj *pWin)
{
    GUI_AREA Area;

    /* 计算窗口裁剪矩形,并设置为当前的绘制链表 */
    Area = WM__ClipWindowArea(pWin);
    if (Area != NULL) {
        /* 重绘窗口 */
        GUI_SetNowRectList(Area, &pWin->InvalidRect);
        WM_SendMessage(pWin, WM_PAINT, 0);
        GUI_FreeRectList(Area);
#if GUI_USE_MEMORY
        /* 需要刷新的矩形 */
        __PaintArea = GUI_RectOrCalc(&pWin->Rect, &__PaintArea);
#endif
    }
}

/* 绘制窗口的透明子窗口 */
static void _PaintTransChild(WM_Obj *pWin)
{
    GUI_RECT r = pWin->InvalidRect;

    for (pWin = pWin->hFirstChild; pWin; pWin = pWin->hNext) {
        if ((pWin->Status & WM_WS_TRANS)
            && GUI_RectOverlay(&pWin->InvalidRect, &pWin->Rect, &r)) {
            _PaintOne(pWin);
            _PaintTransChild(pWin); /* 在绘制它的透明子窗口 */
        }
    }
}

/* 绘制窗口之上的所有透明窗口 */
static void _PaintTransTop(WM_Obj *pWin)
{
    WM_Obj *pParent;
    GUI_RECT r = pWin->InvalidRect;

    for (pParent = pWin; pParent; pParent = pParent->hParent) {
        for (pWin = pParent->hNext; pWin; pWin = pWin->hNext) {
            if ((pWin->Status & WM_WS_TRANS)
                && GUI_RectOverlay(&pWin->InvalidRect, &pWin->Rect, &r)) {
                _PaintOne(pWin);
                _PaintTransChild(pWin);
            }
        }
    }
}

/* 重绘所有窗口 */
static void _PaintAll(void)
{
    WM_Obj *pWin = _pRootWin;

    if (__InvalidWindowNum) {
        /* 遍历并重绘窗口 */
        while (__InvalidWindowNum && pWin) {
            GUI_LOCK();
            if (pWin->Status & WM_WS_INVAILD) { /* 窗口需要重绘 */
                _PaintOne(pWin);
                pWin->Status &= ~(WM_WS_INVAILD); /* 清除窗口无效标志 */
                --__InvalidWindowNum;
                _PaintTransChild(pWin);
                _PaintTransTop(pWin);
            }
            pWin = pWin->hNextLine;
            GUI_UNLOCK();
        }
    }
}

/* 派发消息 */
static void WM__DispatchMessage(WM_MESSAGE *pMsg)
{
    GUI_RESULT res = GUI_ERR;

    /* GUI_ERR为1，只要下面出现GUI_OK res在与赋值之后就会变成0 */
    res &= GUI_TouchPadMessageProc(pMsg); /* 处理触摸事件 */
    res &= GUI_KeyMessageProc(pMsg);      /* 处理键盘事件 */
    if (res == GUI_ERR) {
        WM__SendMessage(pMsg->hWin, pMsg); /* 直接发送到窗口 */
    }
}

/* 通过执行回调重绘无效窗口(所有工作) */
void WM_Exec(void)
{
    WM_MESSAGE Msg;

    if (GUI_Data == NULL) {
        return;
    }
    /* WM消息环 */
    while (GUI_GetMessage(&Msg) == GUI_OK) {
        WM__DispatchMessage(&Msg); /* 派发消息 */
    }
    GUI_TimerHandle();
    _PaintAll(); /* 重绘所有窗口 */
#if GUI_USE_MEMORY
    GUI_LOCK();
    /* 将内存数据更新到LCD上 */
    if (__PaintArea.x1 || __PaintArea.y1) {
        __PaintArea = GUI_RectAndCalc(&_pRootWin->Rect, &__PaintArea);
        GUI_DrawFromMem(__PaintArea.x0, __PaintArea.y0,
            __PaintArea.x1 - __PaintArea.x0 + 1,
            __PaintArea.y1 - __PaintArea.y0 + 1);
        __PaintArea.x0 = 240;
        __PaintArea.y0 = 320;
        __PaintArea.x1 = 0;
        __PaintArea.y1 = 0;
    }
    GUI_UNLOCK();
#endif
}

/*
 * 获取窗口有效的区域大小
 * -该函数通过将目标窗口的与它所有的祖先窗口的用户区取并集得到有效区域
 * -hWin不能是NULL
 **/
GUI_RECT WM_GetWindowAreaRect(WM_HWIN hWin)
{
    GUI_RECT Rect;

    GUI_LOCK();
    Rect = ((WM_Obj*)hWin)->Rect;
    while (((WM_Obj*)hWin) != _pRootWin) {
        hWin = ((WM_Obj*)hWin)->hParent;
        GUI_RectOverlay(&Rect, &Rect, &((WM_Obj*)hWin)->Rect);
    }
    GUI_UNLOCK();
    return Rect;
}

/* 获得裁剪后的窗口无效区域大小，hWin不能是NULL */
GUI_RECT WM_GetTaliorInvalidRect(WM_HWIN hWin)
{
    GUI_RECT Rect;

    GUI_LOCK();
    Rect = WM_GetWindowAreaRect(hWin);
    Rect = GUI_RectAndCalc(&Rect, &((WM_Obj*)hWin)->InvalidRect);
    GUI_UNLOCK();
    return Rect;
}

/* 获得比某个窗口Z序小1的窗口 */
WM_HWIN WM_GetFrontWindow(WM_HWIN hWin)
{
    WM_Obj *pWin = _pRootWin;

    if (hWin == _hRootWin) { /* 根窗口Z序最小 */
        return NULL;
    }
    GUI_LOCK();
    while (pWin && pWin->hNextLine != hWin) {
        pWin = pWin->hNextLine;
    }
    GUI_UNLOCK();
    return pWin;
}

/* 获得某个窗口Z序最高的子窗口 */
WM_HWIN WM_GetTopChildWindow(WM_HWIN hWin)
{
    WM_Obj *pWin = hWin;

    GUI_LOCK();
    if (pWin && pWin->hFirstChild) {
        pWin = pWin->hFirstChild;
        while (pWin->hFirstChild || pWin->hNext) {
            if (pWin->hNext) {
                pWin = pWin->hNext;
            } else {
                pWin = pWin->hFirstChild;
            }
        }
    }
    GUI_UNLOCK();
    return pWin;
}

/* 将一个窗口添加到父窗口下的最顶层 */
void WM_AttachWindow(WM_HWIN hWin, WM_HWIN hParent)
{
    WM_Obj *pWin = hWin, *pObj, *ptr;

    if (hWin == NULL) {
        return;
    }
    GUI_LOCK();
    if (hParent == NULL) { /* hParent为NULL时作为根窗口的子窗口 */
        pObj = _pRootWin;
    } else {
        pObj = hParent;
    }
    pWin->hNext = NULL;
    pWin->hParent = pObj;
    if (pObj->hFirstChild == NULL) { /* 父窗口没有子窗口 */
        pObj->hFirstChild = pWin;
    } else { /* 父窗口已经有子窗口 */
        pObj = pObj->hFirstChild;
        while (pObj->hNext) { /* 直到窗口的左兄弟 */
            pObj = pObj->hNext;
        }
        pObj->hNext = pWin;
        /* 取这个窗口Z序最高的子窗口 */
        pObj = WM_GetTopChildWindow(pObj);
    }
    /* 连接链表 */
    ptr = WM_GetTopChildWindow(pWin);
    ptr->hNextLine = pObj->hNextLine;
    pObj->hNextLine = pWin;
    GUI_UNLOCK();
}

/* 将一个窗口从窗口树中移除 */
void WM_RemoveWindow(WM_HWIN hWin)
{
    WM_Obj *pWin = hWin, *pObj;
    
    /* 窗口不能为NULL,根窗口无需移除 */
    if (hWin == NULL || hWin == _pRootWin) {
        return;
    }
    pObj = pWin->hParent;
    GUI_LOCK();
    if (pObj->hFirstChild == pWin) { /* 它是父窗口的第一个子窗口 */
        pObj->hFirstChild = pWin->hNext;   
    } else {
        pObj = pObj->hFirstChild;
        while (pObj->hNext != pWin) { /* 直到目标窗口的左兄弟 */
            pObj = pObj->hNext;
        }
        pObj->hNext = pWin->hNext;
        pObj = WM_GetFrontWindow(pWin);
    }
    /* 孤立节点 */
    pWin->hParent = NULL;
    pWin->hNext = NULL;
    /* 连接链表 */
    pWin = WM_GetTopChildWindow(pWin);
    pObj->hNextLine = pWin->hNextLine;
    pWin->hNextLine = NULL;
    GUI_UNLOCK();
}

/* 删除窗口 */
void WM_DeleteWindow(WM_HWIN hWin)
{
    WM_Obj *pWin = hWin;

    if (hWin == NULL) { /* hWin不能为NULL */
        return;
    }
    GUI_LOCK();
    /* 被遮挡的窗口无效化 */
    WM_InvalidCoverWindow(pWin, &pWin->Rect);
    WM_RemoveWindow(pWin); /* 先移除窗口 */
    while (pWin) {
        hWin = pWin;
        pWin = pWin->hNextLine;
        /* 删除焦点 */
        if (GUI_Context.hFocus == hWin) {
            GUI_Context.hFocus = NULL;
        }
        /* 删除窗口的定时器 */
        GUI_SetWindowTimer(hWin, 0);
        /* 调用回调函数删除节点 */
        WM_SendMessage(hWin, WM_DELETE, (GUI_PARAM)NULL);
        GUI_fastfree(hWin); /* 释放空间 */
    }
    GUI_UNLOCK();
}

/* 设置焦点窗口 */
GUI_RESULT WM_SetFocusWindow(WM_HWIN hWin)
{
    WM_MESSAGE Msg;

    if (hWin == NULL) {
        return GUI_ERR;
    }
    /* 设置当前窗口为输入焦点 */
    Msg.hWinSrc = NULL;
    Msg.MsgId = WM_SET_FOCUS;
    Msg.Param = 1;
    WM__SendMessage(hWin, &Msg);
    return GUI_OK;
}

/*
 * 设置前景窗口
 * hWin:要设置的窗口句柄
 * 返回值:GUI_OK,正常;GUI_ERR,错误,没有该窗口或窗口为根窗口
 **/
GUI_RESULT WM_SetForegroundWindow(WM_HWIN hWin)
{
    WM_Obj *pObj = hWin;

    if (hWin == NULL || hWin == _hRootWin) {
        return GUI_ERR;
    }
    GUI_LOCK();
    WM_SetFocusWindow(hWin); /* 设置输入焦点 */
    /* 先找到它位于根窗口下的祖先 */
    while (pObj && pObj->hParent != _pRootWin) {
        pObj = pObj->hParent;
    }
    if (WM_GetForegroundWindow() != pObj) {
        WM_RemoveWindow(pObj); /* 先移除窗口 */
        WM_AttachWindow(pObj, NULL); /* 插入窗口到最后 */
        WM_InvalidTree(pObj); /* 窗口及其所有的子窗口无效化 */
    }
    GUI_UNLOCK();
    return GUI_OK;
}

/* 获取当前活动窗口的句柄 */
WM_HWIN WM_GetForegroundWindow(void)
{
    WM_Obj *pWin = _RootWin.hFirstChild;

    GUI_LOCK();
    while (pWin->hNext != NULL) { /* 遍历同属节点 */
        pWin = pWin->hNext;
    }
    GUI_UNLOCK();
    return pWin;
}

/*
 * 创建一个窗口作为指定窗口的子窗口
 * 当hParent为NULL时,窗口将是RootWindow的子窗口
 **/
WM_HWIN WM_CreateWindowAsChild(i_16 x0,             /* 橫坐标 */
                               i_16 y0,             /* 纵坐标 */
                               u_16 xSize,          /* 宽度 */
                               u_16 ySize,          /* 高度 */
                               WM_HWIN hParent,     /* 父窗口句柄 */
                               u_16 Style,          /* 窗口风格 */
                               u_8 Sign,            /* 窗口标识符 */
                               u_16 Id,             /* 窗口ID */
                               WM_CALLBACK *WinCb,  /* 窗口功能回调函数 */
                               u_32 bytes)          /* 窗口结构体多出的字节 */
{
    WM_Obj *pObj, *pParent = hParent;

    GUI_LOCK();
    /* 检查窗口ID是否已经注册 */
    pObj = WM_GetWindowHandle(Id);
    if (pObj != NULL) {
        WM_SetForegroundWindow(pObj); /* 将已经注册的窗口放到最前 */
        GUI_UNLOCK();
        return NULL;
    }
    pObj = GUI_fastmalloc(sizeof(WM_Obj) + bytes);
    if (pObj == NULL) {
        GUI_UNLOCK();
        return NULL;
    }
    pObj->hFirstChild = NULL;
    pObj->hNextLine = NULL;
    WM_AttachWindow(pObj, pParent); /* 注册到父窗口 */
    pObj->Status = Style;
    pObj->Sign = Sign;
    pObj->Id = Id;
    pObj->WinCb = WinCb;
    if (pParent) {
        x0 += pParent->Rect.x0;
        y0 += pParent->Rect.y0;
    }
    pObj->Rect.x0 = x0;
    pObj->Rect.y0 = y0;
    pObj->Rect.x1 = x0 + xSize - 1;
    pObj->Rect.y1 = y0 + ySize - 1;
    WM_Invalidate(pObj); /* 整个窗口失效 */
    GUI_UNLOCK();
    return pObj;
}

/* 
 * 检查窗口标识符 
 * 返回值:0,标识符匹配;1,标识符不匹配
 **/
GUI_RESULT WM_CheckWindowSign(WM_HWIN hWin, u_16 Sign)
{
    if (hWin && ((WM_Obj*)hWin)->Sign == Sign) {
        return GUI_OK;
    }
    return GUI_ERR;
}

/* 透明窗口无效化 */
static void _InvalidateTrans(WM_Obj *pWin, GUI_RECT *pr)
{
    int Status;
    WM_Obj *pParent = pWin->hParent;
    WM_Obj *pStop = pWin;
    GUI_RECT r, ri;

    if (pParent == NULL) {
        return;
    }
    Status = pParent->Status;
    if (GUI_RectOverlay(&r, pr, &pParent->Rect) == FALSE) {
        return;
    }
    if (Status & WM_WS_TRANS) {
        _InvalidateTrans(pParent, pr); /* 父窗口是透明窗口 */
    } else {
        _Invalidate1Abs(pParent, pr); /* 先把父窗口无效化 */
    }
    /* 父窗口所有的子窗口无效化 */
    for (pWin = pParent->hFirstChild; pWin && pWin != pStop;) {
        ri = r;
        if (WM__ClipAtParent(&ri, pWin)) {
            if (!(pWin->Status & WM_WS_TRANS)) { /* 透明窗口不管 */
                _Invalidate1Abs(pWin, &ri);
            }
            pWin = pWin->hNextLine;
        } else { /* 该窗口与无效区域不相交，无需计算它的子窗口 */
            if (pWin->hNext) {
                pWin = pWin->hNext;
            } else {
                pWin = pWin->hNextLine;
            }
        }
    }
}

/* 窗口部分区域无效化 */
void WM_InvalidateRect(WM_HWIN hWin, GUI_RECT *pRect)
{
    WM_Obj *pWin;
    GUI_RECT r;

    if (hWin) {
        pWin = hWin;
        GUI_LOCK();
        if (pRect == NULL) {
            r = pWin->Rect;
        } else {
            r = *pRect;
        }
        if (WM__ClipAtParent(&r, pWin)) { /* 无效区域裁剪为窗口可见区域 */
            if (pWin->Status & WM_WS_TRANS) {
                _InvalidateTrans(pWin, &r);
            } else {
                _Invalidate1Abs(pWin, &r);
            }
        }
        GUI_UNLOCK();
    }
}

/* 整个窗口无效化 */
void WM_Invalidate(WM_HWIN hWin)
{
    WM_InvalidateRect(hWin, NULL);
}

/* 将一个窗口及其子窗口无效化 */
GUI_RESULT WM_InvalidTree(WM_HWIN hWin)
{
    WM_Obj *pWin = hWin;

    if (hWin == NULL) {
        return GUI_ERR;
    }
    GUI_LOCK();
    /* 找到遍历子窗口的终点 */
    hWin = WM__FindChildEnd(pWin);
    while (pWin && pWin != hWin) {
        WM_InvalidateRect(pWin, NULL);
        pWin = pWin->hNextLine;
    }
    GUI_UNLOCK();
    return GUI_OK;
}

/* 获取窗口的尺寸 */
GUI_RECT WM_GetWindowRect(WM_HWIN hWin)
{
    return ((WM_Obj*)hWin)->Rect;
}

/* 获取窗口无效区域 */
GUI_RECT WM_GetWindowInvalidRect(WM_HWIN hWin)
{
    return ((WM_Obj*)hWin)->InvalidRect;
}

/*
 * 在窗口树中寻找一个窗口
 * 返回值:0,没有这个窗口;1,有这个窗口
 * 外部调用
 **/
GUI_RESULT WM_FindWindow(WM_HWIN hWin)
{
    WM_Obj *pWin = _hRootWin;
    
    if (pWin) {
        GUI_LOCK();
        while (pWin && pWin != hWin) { /* 遍历窗口树 */
            pWin = pWin->hNextLine;
        }
        GUI_UNLOCK();
        if (pWin == hWin) {
            return GUI_OK; /* 找到了窗口 */
        }
    }
    return GUI_ERR; /* 不存在该窗口 */
}

/* 获得指定ID的子窗口句柄 */
WM_HWIN WM_GetDialogItem(WM_HWIN hWin, u_16 Id)
{
    WM_Obj *pWin = hWin;

    /* WM_NULL_ID不可寻找 */
    if (Id == WM_NULL_ID) {
        return NULL;
    }
    GUI_LOCK();
    if (pWin && pWin->hFirstChild) {
        pWin = pWin->hFirstChild; /* 从子窗口开始寻找 */
        /* 找到遍历子窗口的终点 */
        hWin = WM__FindChildEnd(hWin);
        while (pWin && pWin->Id != Id && pWin != hWin) {
            pWin = pWin->hNextLine;
        }
    } else {
        pWin = NULL;
    }
    GUI_UNLOCK();
    return pWin;
}

/* 获得指定ID的窗口句柄 */
WM_HWIN WM_GetWindowHandle(u_16 Id)
{
    WM_Obj *pWin = _pRootWin;

    /* WM_NULL_ID不可寻找 */
    if (Id == WM_NULL_ID) {
        return NULL;
    }
    GUI_LOCK();
    while (pWin && pWin->Id != Id) { /* 遍历窗口 */
        pWin = pWin->hNextLine;
    }
    GUI_UNLOCK();
    return pWin;
}

/* 获取控件ID */
u_16 WM_GetDialogId(WM_HWIN hWin)
{
    if (hWin) {
        return ((WM_Obj*)(hWin))->Id;
    }
    return 0;
}

/*
 * 获得在输入坐标下暴露的窗口(被指定坐标选中的窗口).
 * 返回值:选中窗口的句柄.
 **/
WM_HWIN WM_GetExposedWindow(u_16 x, u_16 y)
{
    WM_Obj *p1 = _pRootWin, *p2 = NULL, *pWin = NULL;
    GUI_RECT r, rParent = p1->Rect; /* 根窗口的矩形 */

    GUI_LOCK(); 
    /* 找到同级别中Z序最高的选中窗口，然后检查它的是否有孩子或孩子中是否有选中窗口，
       如果它没有孩子或孩子中没有选中窗口则返回这个窗口，有则继续向窗口树深处寻找. */
    do {
        while (p1) { /* 遍历同级窗口 */
            /* 计算窗口在被祖先窗口裁剪之后的矩形（暴露矩形），
               然后检查这个窗口是否被输入坐标选中. */
            if (GUI_RectOverlay(&r, &p1->Rect, &rParent)) {
                if (GUI_CheckPointAtRect(x, y, &r)) {
                    p2 = p1; /* 记录选中窗口 */
                }
            }
            p1 = p1->hNext;
        } /* 循环结束之后p2即为Z序最高的被选中窗口 */
        if (p2 == pWin) { /* 子窗口没中没有选中窗口 */
            break;
        }
        pWin = p2; /* 记录目前Z序最高的选中窗口 */
        p1 = pWin->hFirstChild; /* 接下来检查选中窗口的子窗口 */
        /* 下一级窗口的暴露矩形用当前选中窗口的暴露矩形来计算 */
        GUI_RectOverlay(&rParent, &rParent, &pWin->Rect);
    } while (p1); /* 选中窗口没有子窗口，结束循环 */
    GUI_UNLOCK();
    return pWin;
}

/* 获取父窗口句柄 */
WM_HWIN WM_GetParentHandle(WM_HWIN hWin)
{
    return ((WM_Obj*)hWin)->hParent;
}

/* 将被一个窗口遮盖的窗口及其子窗口无效化，
 * 例如在删除窗口时需要把将要被删除的窗口遮挡的窗口无效化 
 **/
void WM_InvalidCoverWindow(WM_HWIN hWin, GUI_RECT *pRect)
{
    WM_Obj *p = hWin;

    /* hWin不能为NULL,也不能是RootWindow */
    if (hWin == NULL && p->hParent == NULL) { 
        return;
    }
    /* 空矩形直接返回 */
    if (GUI_CheckRectNull(pRect) == FALSE) {
        return;
    }
    GUI_LOCK();
    /*这个窗口最多只能遮盖到它的父窗口*/
    for (p = p->hParent;  p && p != hWin; p = p->hNextLine) {
            WM_InvalidateRect(p, pRect);
    }
    GUI_UNLOCK();
}

/* 移动窗口及它所有的子窗口 */
void WM_MoveWindow(WM_HWIN hWin, i_16 dX, i_16 dY)
{
    WM_Obj *p = hWin;
    
    GUI_LOCK();
    if (p && p->Status & WM_WS_MOVE && (dX || dY)) {
        /* 先将被遮挡的窗口无效化 */
        WM_InvalidCoverWindow(p, &p->Rect);
        /* 找到遍历子窗口的终点 */
        hWin = WM__FindChildEnd(p);
        for (; p != hWin; p = p->hNextLine) { /* 遍历子窗口 */
            GUI_MoveRect(&p->Rect, dX, dY);
            WM_Invalidate(p); /* 窗口无效化 */
        }
    }
    GUI_UNLOCK();
}

/* 设置可移动的窗口 */
void WM_SetMoveWindow(WM_HWIN hWin)
{
    ((WM_Obj*)hWin)->Status |= WM_WS_MOVE;
}

/* 获取窗口客户区句柄 */
WM_HWIN WM_GetClientWindow(WM_HWIN hWin)
{
    GUI_MESSAGE Msg;

    Msg.MsgId = WM_GET_CLIENT;
    Msg.Param = 0;
    WM__SendMessage(hWin, &Msg);
    return (WM_HWIN)(Msg.Param);
}

/* 获取窗口客户区矩形 */
GUI_RECT * WM_GetClientRect(WM_HWIN hWin)
{
    WM_Obj *pClient;

    pClient = WM_GetClientWindow(hWin);
    return &pClient->Rect;
}

/* GUI按键默认处理函数 */
GUI_BOOL WM_DefaultKeyProc(GUI_MESSAGE *pMsg)
{
    if (pMsg->MsgId == WM_KEYDOWN) {
        switch (pMsg->Param) {
        case KEY_TAB: /* TAB键切换焦点 */
            pMsg->MsgId = WM_SET_FOCUS;
            pMsg->Param = 0;
            WM_SendMessage(pMsg->hWin, WM_SET_FOCUS, 0);
            break;
        default:
            return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

/* 默认消息处理函数 */
void WM_DefaultProc(GUI_MESSAGE *pMsg)
{
    /* 处理消息 */
    switch (pMsg->MsgId) {
    case WM_DELETE:
        return;
    case WM_TP_CHECKED:
        WM_SetForegroundWindow(pMsg->hWin); /* 设置为前景窗口 */
        return;
    case WM_TP_PRESS: /* 移动窗口 */
        WM_MoveWindow(pMsg->hWin,
            ((GUI_POINT*)pMsg->Param)[1].x,
            ((GUI_POINT*)pMsg->Param)[1].y);
        return;
    case WM_TP_LEAVE: /* 移动窗口 */
        WM_MoveWindow(pMsg->hWin,
            ((GUI_POINT*)pMsg->Param)[1].x,
            ((GUI_POINT*)pMsg->Param)[1].y);
        return;
    case WM_SET_FOCUS: /* 设置窗口焦点 */
        WM_SendMessageToParent(pMsg->hWin, pMsg);
        return;
    case WM_KEYDOWN:
        if (pMsg->Param == KEY_TAB) { /* TAB键切换焦点 */
            pMsg->MsgId = WM_SET_FOCUS;
            pMsg->Param = 0;
            WM__SendMessage(pMsg->hWin, pMsg);
        }
        return;
    }
    /* 设置默认参数 */
    pMsg->Param = 0;
}
