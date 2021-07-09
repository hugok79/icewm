/*
 * IceWM
 *
 * Copyright (C) 1997-2003 Marko Macek
 */

#include "config.h"
#include "wmframe.h"
#include "wmmgr.h"
#include "yprefs.h"
#include "prefs.h"
#include "atasks.h"
#include "atray.h"
#include "wmcontainer.h"
#include "wmtitle.h"
#include "wmbutton.h"
#include "wmminiicon.h"
#include "wmtaskbar.h"
#include "wmwinlist.h"
#include "wmapp.h"
#include "yrect.h"
#include "wpixmaps.h"
#include "workspaces.h"
#include "yxcontext.h"

#include "intl.h"

       YColorName activeBorderBg(&clrActiveBorder);
static YColorName inactiveBorderBg(&clrInactiveBorder);

lazy<YTimer> YFrameWindow::fAutoRaiseTimer;
lazy<YTimer> YFrameWindow::fDelayFocusTimer;
YArray<YFrameWindow::GroupModal> YFrameWindow::groupModals;

YFrameWindow::YFrameWindow(
    YActionListener *wmActionListener, unsigned dep, Visual* vis, Colormap col)
    : YWindow(nullptr, None, dep ? dep : xapp->depth(),
              vis ? vis : xapp->visual(), col ? col : xapp->colormap()),
    fManaged(false),
    fFocused(false),
    fFrameFunctions(0),
    fFrameDecors(0),
    fFrameOptions(0),
    normalX(0),
    normalY(0),
    normalW(1),
    normalH(1),
    posX(0),
    posY(0),
    posW(1),
    posH(1),
    fClient(nullptr),
    fContainer(nullptr),
    fTitleBar(nullptr),
    fPopupActive(nullptr),
    movingWindow(false),
    sizingWindow(false),
    topSide(None),
    leftSide(None),
    rightSide(None),
    bottomSide(None),
    topLeft(None),
    topRight(None),
    bottomLeft(None),
    bottomRight(None),
    fTaskBarApp(nullptr),
    fTrayApp(nullptr),
    fMiniIcon(nullptr),
    fWinListItem(nullptr),
    fFrameIcon(null),
    fKillMsgBox(nullptr),
    fOwner(nullptr),
    fTransient(nullptr),
    fNextTransient(nullptr),
    wmActionListener(wmActionListener),
    fWinWorkspace(manager->activeWorkspace()),
    fWinRequestedLayer(WinLayerNormal),
    fWinActiveLayer(WinLayerNormal),
    fWinTrayOption(WinTrayIgnore),
    fWinState(0),
    fWinOptionMask(~0),
    fTrayOrder(0),
    fFullscreenMonitorsTop(-1),
    fFullscreenMonitorsBottom(-1),
    fFullscreenMonitorsLeft(-1),
    fFullscreenMonitorsRight(-1),
    fStrutLeft(0),
    fStrutRight(0),
    fStrutTop(0),
    fStrutBottom(0),
    fUserTimeWindow(None),
    fStartManaged(xapp->getEventTime("frame")),
    fShapeWidth(-1),
    fShapeHeight(-1),
    fShapeTitleY(-1),
    fShapeBorderX(-1),
    fShapeBorderY(-1),
    fShapeDecors(0),
    fHaveStruts(false),
    indicatorsCreated(false),
    fWindowType(wtNormal)
{
    setStyle(wsOverrideRedirect);
    setPointer(YXApplication::leftPointer);
    setTitle("Frame");
    setBackground(inactiveBorderBg);
}

YFrameWindow::~YFrameWindow() {
    fManaged = false;
    if (fKillMsgBox) {
        fKillMsgBox->unmanage();
        fKillMsgBox = nullptr;
    }
    if (fWindowType == wtDialog)
        wmapp->signalGuiEvent(geDialogClosed);
    else
        wmapp->signalGuiEvent(geWindowClosed);
    if (fDelayFocusTimer)
        fDelayFocusTimer->disableTimerListener(this);
    if (fAutoRaiseTimer)
        fAutoRaiseTimer->disableTimerListener(this);
    if (movingWindow || sizingWindow)
        endMoveSize();
    if (fPopupActive)
        fPopupActive->cancelPopup();
    removeAppStatus();
    removeFromWindowList();
    if (fMiniIcon) {
        delete fMiniIcon;
        fMiniIcon = nullptr;
    }
    fFrameIcon = null;
#if 1
    fWinState &= ~WinStateFullscreen;
    updateLayer(false);
#endif
    // perhaps should be done another way
    removeTransients();
    removeAsTransient();
    if (fContainer) {
        manager->lockWorkArea();
        manager->removeFocusFrame(this);
        manager->removeCreatedFrame(this);
        removeFrame();
        manager->removeClientFrame(this);
        if (client()) {
            if (!client()->destroyed() && client()->adopted())
                XRemoveFromSaveSet(xapp->display(), client()->handle());
            frameContext.remove(client()->handle());
        }
        if (fUserTimeWindow != None) {
            windowContext.remove(fUserTimeWindow);
        }

        delete fClient; fClient = nullptr;
        delete fContainer; fContainer = nullptr;
        delete fTitleBar; fTitleBar = nullptr;

        manager->unlockWorkArea();
        manager->updateClientList();
    }

    if (taskBar) {
        taskBar->workspacesRepaint();
    }
}

void YFrameWindow::addToWindowList() {
    if (fWinListItem == nullptr && client()->adopted() &&
        windowList && notbit(frameOptions(), foIgnoreWinList))
    {
        fWinListItem = windowList->addWindowListApp(this);
    }
}

void YFrameWindow::removeFromWindowList() {
    if (fWinListItem) {
        if (windowList)
            windowList->removeWindowListApp(fWinListItem);
        delete fWinListItem; fWinListItem = nullptr;
    }
}

inline bool YFrameWindow::setAbove(YFrameWindow *aboveFrame) {
    return manager->setAbove(this, aboveFrame);
}

inline bool YFrameWindow::setBelow(YFrameWindow *belowFrame) {
    return manager->setBelow(this, belowFrame);
}

YFrameTitleBar* YFrameWindow::titlebar() {
    if (fTitleBar == nullptr && titleY() > 0) {
        fTitleBar = new YFrameTitleBar(this, this);
    }
    return fTitleBar;
}

void YFrameWindow::doManage(YFrameClient *clientw, bool &doActivate, bool &requestFocus) {
    PRECONDITION(clientw != 0 && !fContainer && !fClient);

    if (clientw->handle() == None || clientw->destroyed()) {
        return;
    }

    unsigned depth = Elvis(clientw->depth(), xapp->depth());
    bool sameDepth = (depth == xapp->depth());
    Visual* visual = (sameDepth ? xapp->visual() : clientw->visual());
    Colormap clmap = (sameDepth ? xapp->colormap() : clientw->colormap());
    fContainer = new YClientContainer(this, this, depth, visual, clmap);

    fClient = clientw;
    if (hintOptions && hintOptions->nonempty()) {
        getWindowOptions(hintOptions, getHintOption(), true);
    }

    {
        int x = client()->x();
        int y = client()->y();
        int w = client()->width();
        int h = client()->height();

        XSizeHints *sh = client()->sizeHints();
        normalX = x;
        normalY = y;
        normalW = sh ? (w - sh->base_width) / max(1, sh->width_inc) : w;
        normalH = sh ? (h - sh->base_height) / max(1, sh->height_inc) : h;

        if (client()->winGravity() == StaticGravity) {
            normalX += borderXN();
            normalY += borderYN() + titleYN();
        } else {
            int gx, gy;
            client()->gravityOffsets(gx, gy);

            if (gx > 0)
                normalX += 2 * borderXN() - 1 - client()->getBorder();
            if (gy > 0)
                normalY += 2 * borderYN() + titleYN() - 1 - client()->getBorder();

        }

        getNormalGeometryInner(&posX, &posY, &posW, &posH);
    }

    updateIcon();
    manage();
    manager->appendCreatedFrame(this);
    bool isRunning = manager->wmState() == YWindowManager::wmRUNNING;
    insertFrame(!isRunning);
    manager->insertFocusFrame(this, !isRunning);

    if (client()->getNetWMWindowType(&fWindowType)) {
        if (fWindowType == wtDesktop || fWindowType == wtDock) {
            setAllWorkspaces();
        }
    } else if (client()->ownerWindow()) {
        fWindowType = wtDialog;
    }
    int layer = fWinRequestedLayer;
    if (client()->getLayerHint(&layer) &&
        layer != fWinRequestedLayer &&
        inrange<int>(layer, WinLayerDesktop, WinLayerAboveAll))
    {
        setRequestedLayer(layer);
    } else {
        updateLayer(true);
    }
    getFrameHints();

    getDefaultOptions(requestFocus);
    updateNetWMStrut(); /// ? here
    updateNetWMStrutPartial();
    updateNetStartupId();
    updateNetWMUserTime();
    updateNetWMUserTimeWindow();

    int workspace = getWorkspace(), mask(0), state(0);

    MSG(("Map - Frame: %d", visible()));
    MSG(("Map - Client: %d", client()->visible()));

    if (client()->getNetWMStateHint(&mask, &state)) {
        if ((getState() & mask) != state) {
            setState(mask, state);
        }
    }
    if (client()->ownerWindow() == xapp->root() ||
        (client()->mwmHints() && client()->mwmHints()->inputModal())) {
        fWinState |= WinStateModal;
    }

    {
        FrameState st = client()->getFrameState();

        if (st == WithdrawnState) {
            XWMHints *h = client()->hints();
            if (h && (h->flags & StateHint))
                st = h->initial_state;
            else
                st = NormalState;
        }
        MSG(("FRAME state = %d", st));
        if (st == IconicState) {
            fFrameOptions |= foMinimized;
            if (manager->isRunning())
                if (notState(WinStateMinimized))
                    setState(WinStateUnmapped, WinStateMinimized);
        }
    }

    if (client()->getNetWMDesktopHint(&workspace))
        setWorkspace(workspace);

    int tray;
    if (client()->getWinTrayHint(&tray))
        setTrayOption(tray);
    addAsTransient();
    if (owner())
        setWorkspace(mainOwner()->getWorkspace());

    if (isHidden() || isMinimized() || client() == taskBar) {
        doActivate = false;
        requestFocus = false;
    }

    updateFocusOnMap(doActivate);
    addTransients();
    manager->restackWindows();

    afterManage();
}

void YFrameWindow::afterManage() {
    if (affectsWorkArea())
        manager->updateWorkArea();
    manager->updateClientList();
    setShape();
    if ( !frameOption(foFullKeys))
        grabKeys();
    container()->grabButtons();
    addToWindowList();
    if (fWindowType == wtDialog)
        wmapp->signalGuiEvent(geDialogOpened);
    else
        wmapp->signalGuiEvent(geWindowOpened);
}

// create a window to show a resize pointer on the frame border
Window YFrameWindow::createPointerWindow(Cursor cursor, int gravity) {
    unsigned long valuemask = CWEventMask | CWCursor | CWWinGravity;
    XSetWindowAttributes attributes;
    attributes.win_gravity = gravity;
    attributes.event_mask = 0;
    attributes.cursor = cursor;
    Window window = XCreateWindow(xapp->display(), handle(), 0, 0, 1, 1,
                                  0, 0, InputOnly, CopyFromParent,
                                  valuemask, &attributes);
    XMapWindow(xapp->display(), window);
    return window;
}

// create 8 resize pointer indicator windows
void YFrameWindow::createPointerWindows() {

    topSide =
        createPointerWindow(YWMApp::sizeTopPointer, NorthGravity);
    leftSide =
        createPointerWindow(YWMApp::sizeLeftPointer, WestGravity);
    rightSide =
        createPointerWindow(YWMApp::sizeRightPointer, EastGravity);
    bottomSide =
        createPointerWindow(YWMApp::sizeBottomPointer, SouthGravity);

    topLeft =
        createPointerWindow(YWMApp::sizeTopLeftPointer, NorthWestGravity);
    topRight =
        createPointerWindow(YWMApp::sizeTopRightPointer, NorthEastGravity);
    bottomLeft =
        createPointerWindow(YWMApp::sizeBottomLeftPointer, SouthWestGravity);
    bottomRight =
        createPointerWindow(YWMApp::sizeBottomRightPointer, SouthEastGravity);

    indicatorsCreated = true;

    if (fTitleBar) {
        fTitleBar->raise();
    }
    XStoreName(xapp->display(), topSide, "topSide");
    container()->raise();
}

void YFrameWindow::grabKeys() {
    XUngrabKey(xapp->display(), AnyKey, AnyModifier, handle());

    GRAB_WMKEY(gKeyWinRaise);
    GRAB_WMKEY(gKeyWinOccupyAll);
    GRAB_WMKEY(gKeyWinLower);
    GRAB_WMKEY(gKeyWinClose);
    GRAB_WMKEY(gKeyWinRestore);
    GRAB_WMKEY(gKeyWinNext);
    GRAB_WMKEY(gKeyWinPrev);
    GRAB_WMKEY(gKeyWinMove);
    GRAB_WMKEY(gKeyWinSize);
    GRAB_WMKEY(gKeyWinMinimize);
    GRAB_WMKEY(gKeyWinMaximize);
    GRAB_WMKEY(gKeyWinMaximizeVert);
    GRAB_WMKEY(gKeyWinMaximizeHoriz);
    GRAB_WMKEY(gKeyWinHide);
    GRAB_WMKEY(gKeyWinRollup);
    GRAB_WMKEY(gKeyWinFullscreen);
    GRAB_WMKEY(gKeyWinMenu);
    GRAB_WMKEY(gKeyWinArrangeN);
    GRAB_WMKEY(gKeyWinArrangeNE);
    GRAB_WMKEY(gKeyWinArrangeE);
    GRAB_WMKEY(gKeyWinArrangeSE);
    GRAB_WMKEY(gKeyWinArrangeS);
    GRAB_WMKEY(gKeyWinArrangeSW);
    GRAB_WMKEY(gKeyWinArrangeW);
    GRAB_WMKEY(gKeyWinArrangeNW);
    GRAB_WMKEY(gKeyWinArrangeC);
    GRAB_WMKEY(gKeyWinSmartPlace);

    container()->regrabMouse();
}

void YFrameWindow::manage() {
    PRECONDITION(client());

    if (client()->getBorder()) {
        client()->setBorderWidth(0U);
    }
    if (client()->adopted())
        XAddToSaveSet(xapp->display(), client()->handle());
    else
        client()->getPropertiesList();

    client()->reparent(container(), 0, 0);
    client()->setFrame(this);
}

void YFrameWindow::unmanage(bool reparent) {
    PRECONDITION(client());

    if (fMiniIcon) {
        delete fMiniIcon;
        fMiniIcon = nullptr;
    }

    if (!client()->destroyed()) {
        int gx, gy;
        client()->gravityOffsets(gx, gy);

        client()->setBorderWidth(client()->getBorder());

        int posX, posY, posWidth, posHeight;

        getNormalGeometryInner(&posX, &posY, &posWidth, &posHeight);
        if (gx < 0)
            posX -= borderXN();
        else if (gx > 0)
            posX += borderXN() - 2 * client()->getBorder();
        if (gy < 0)
            posY -= borderYN();
        else if (gy > 0)
            posY += borderYN() + titleYN() - 2 * client()->getBorder();

        if (gx == 0 && gy == 0) {
            if (client()->winGravity() == StaticGravity) {
                posY += titleYN();
            }
        }

        if (reparent)
            client()->reparent(desktop, posX, posY);

        client()->setSize(posWidth, posHeight);

        if (manager->wmState() != YWindowManager::wmSHUTDOWN) {
            client()->setFrameState(WithdrawnState);
        }

        if (!client()->destroyed() && client()->adopted())
            XRemoveFromSaveSet(xapp->display(), client()->handle());
    }
    else
        client()->unmanageWindow();

    client()->setFrame(nullptr);
    fClient = nullptr;

    hide();
}

void YFrameWindow::getNewPos(const XConfigureRequestEvent& cr,
                             int& cx, int& cy, int& cw, int& ch)
{
    const int mask = int(cr.value_mask);
    cw = (mask & CWWidth) ? cr.width : client()->width();
    ch = (mask & CWHeight) ? cr.height : client()->height();

    int grav = client()->winGravity();
    int cur_x = x() + container()->x();
    int cur_y = y() + container()->y();

    //msg("%d %d %d %d", cr.x, cr.y, cr.width, cr.height);

    if (mask & CWX) {
        if (grav == StaticGravity)
            cx = cr.x;
        else {
            cx = cr.x + container()->x();
        }
    } else {
        if (grav == NorthGravity ||
            grav == CenterGravity ||
            grav == SouthGravity)
        {
            cx = cur_x + (client()->width() - cw) / 2;
        } else if (grav == NorthEastGravity ||
                   grav == EastGravity ||
                   grav == SouthEastGravity)
        {
            cx = cur_x + (client()->width() - cw);
        } else {
            cx = cur_x;
        }
    }

    if (mask & CWY) {
        if (grav == StaticGravity)
            cy = cr.y;
        else {
            cy = cr.y + container()->y();
        }
    } else {
        if (grav == WestGravity ||
            grav == CenterGravity ||
            grav == EastGravity)
        {
            cy = cur_y + (client()->height() - ch) / 2;
        } else if (grav == SouthEastGravity ||
                   grav == SouthGravity ||
                   grav == SouthWestGravity)
        {
            cy = cur_y + (client()->height() - ch);
        } else {
            cy = cur_y;
        }
    }

    if (affectsWorkArea() == false) {
        int screen = desktop->getScreenForRect(cx, cy, cw, ch);
        int left, top, right, bottom;
        if (taskBar && screen == taskBar->getFrame()->getScreen()) {
            manager->getWorkArea(this, &left, &top, &right, &bottom, screen);
        }
        else {
            left = desktop->x();
            right = left + desktop->width();
            top = desktop->y();
            bottom = top + desktop->height();
        }
        if (cx + cw > right && cx > left && (mask & CWWidth)) {
            cx -= min(cx + cw - right, cx - left);
        }
        if (cy + ch > bottom && cy > top && (mask & CWHeight)) {
            cy -= min(cy + ch - bottom, cy - top);
        }
        if (limitPosition && (mask & CWX) && notbit(mask, CWWidth)) {
            cx = clamp(cx, left, right - cw);
        }
        if (limitPosition && (mask & CWY) && notbit(mask, CWHeight)) {
            cy = clamp(cy, top, bottom - ch);
        }
    }

    // update pager when windows move/resize themselves (like xmms, gmplayer, ...),
    // because this does not call YFrameWindow::endMoveSize()
    if (taskBar) {
        taskBar->workspacesRepaint();
    }
}

void YFrameWindow::configureClient(const XConfigureRequestEvent &configureRequest) {
    unsigned long mask = configureRequest.value_mask;
    if (hasbit(mask, CWBorderWidth))
        client()->setBorder(configureRequest.border_width);

    if (hasbit(mask, CWX | CWY | CWWidth | CWHeight)) {
        int cx, cy, cw, ch;
        getNewPos(configureRequest, cx, cy, cw, ch);
        configureClient(cx, cy, cw, ch);
    }

    if (hasbit(mask, CWStackMode)) {
        Window window = hasbit(mask, CWSibling) ? configureRequest.above : None;
        if (inrange(configureRequest.detail, Above, Opposite)) {
            netRestackWindow(window, configureRequest.detail);
        }
    }
    sendConfigure();
}

void YFrameWindow::netRestackWindow(Window window, int detail) {
    YFrameWindow* sibling = window ? manager->findFrame(window) : nullptr;
    if (sibling) {
        switch (detail) {
        case Above:
            if (setAbove(sibling)) {
                raiseTo(sibling);
            }
            break;
        case Below:
            if (setBelow(sibling)) {
                beneath(sibling);
            }
            break;
        case TopIf:
            if (getActiveLayer() == sibling->getActiveLayer()) {
                for (YFrameWindow* f = prev(); f; f = f->prev()) {
                    if (f == sibling) {
                        if (overlap(sibling) && setAbove(sibling)) {
                            raiseTo(sibling);
                        }
                        break;
                    }
                }
            }
            break;
        case BottomIf:
            if (getActiveLayer() == sibling->getActiveLayer()) {
                YFrameWindow* f;
                for (f = next(); f && f != owner(); f = f->next()) {
                    if (f == sibling) {
                        if (overlap(sibling) && setBelow(sibling)) {
                            beneath(sibling);
                        }
                        break;
                    }
                }
            }
            break;
        case Opposite:
            if (getActiveLayer() == sibling->getActiveLayer()) {
                bool search = true;
                for (YFrameWindow* f = prev(); f; f = f->prev()) {
                    if (f == sibling) {
                        if (overlap(sibling) && setAbove(sibling)) {
                            raiseTo(sibling);
                        }
                        search = false;
                        break;
                    }
                }
                if (search) {
                    YFrameWindow* f;
                    for (f = next(); f && f != owner(); f = f->next()) {
                        if (f == sibling) {
                            if (overlap(sibling) && setBelow(sibling)) {
                                beneath(sibling);
                            }
                            break;
                        }
                    }
                }
            }
            break;
        }
    }
    else {
        switch (detail) {
        case Above:
            if (canRaise()) {
                wmRaise();
                if (focusOnAppRaise) {
                    if ( !frameOption(foNoFocusOnAppRaise) &&
                        (clickFocus || !strongPointerFocus))
                    {
                        if (focusChangesWorkspace ||
                            focusCurrentWorkspace ||
                            visibleNow())
                        {
                            activate();
                        } else {
                            setWmUrgency(true);
                        }
                    }
                }
                else if (requestFocusOnAppRaise) {
                    setWmUrgency(true);
                }
            }
            break;
        case Below:
            if (owner() && getActiveLayer() == owner()->getActiveLayer()) {
                if (setAbove(owner())) {
                    raiseTo(owner());
                }
            }
            else if (focused() || hasState(WinStateModal)) {
                wmLower();
            }
            else {
                doLower();
            }
            break;
        case TopIf:
            for (YFrameWindow* f = prev(); f; f = f->prev()) {
                if (overlap(f)) {
                    while (f->prev()) {
                        f = f->prev();
                    }
                    if (setAbove(f)) {
                        raiseTo(f);
                    }
                    break;
                }
            }
            break;
        case BottomIf:
            for (YFrameWindow* f = next(); f && f != owner(); f = f->next()) {
                if (overlap(f)) {
                    while (f->next() && f->next() != owner()) {
                        f = f->next();
                    }
                    if (setBelow(f)) {
                        beneath(f);
                    }
                    break;
                }
            }
            break;
        case Opposite:
            for (YFrameWindow* f = prev(); ; f = f->prev()) {
                if (f == nullptr) {
                    for (f = next(); f && f != owner(); f = f->next()) {
                        if (overlap(f)) {
                            while (f->next() && f->next() != owner()) {
                                f = f->next();
                            }
                            if (setBelow(f)) {
                                beneath(f);
                            }
                            break;
                        }
                    }
                    break;
                }
                else if (overlap(f)) {
                    while (f->prev()) {
                        f = f->prev();
                    }
                    if (setAbove(f)) {
                        raiseTo(f);
                    }
                    break;
                }
            }
            break;
        }
    }
    manager->updateClientList();
}

void YFrameWindow::configureClient(int cx, int cy, int cwidth, int cheight) {
    MSG(("setting geometry (%d:%d %dx%d)", cx, cy, cwidth, cheight));
    cy -= titleYN();
    if (isFullscreen()) {
        XSizeHints *sh = client()->sizeHints();
        if (sh) {
            normalX = cx;
            normalY = cy;
            normalW = sh
                    ? (cwidth - sh->base_width) / max(1, sh->width_inc)
                    : cwidth;
            normalH = sh
                    ? (cheight - sh->base_height) / max(1, sh->height_inc)
                    : cheight;
        }
    }
    else {
        int posX, posY, posW, posH;
        getNormalGeometryInner(&posX, &posY, &posW, &posH);

        if (isMaximizedVert() || isRollup()) {
            cy = posY;
            cheight = posH;
        }
        if (isMaximizedHoriz()) {
            cx = posX;
            cwidth = posW;
        }

        setNormalGeometryInner(cx, cy, cwidth, cheight);
    }
}

void YFrameWindow::handleClick(const XButtonEvent &up, int /*count*/) {
    if (up.button == 3) {
        popupSystemMenu(this, up.x_root, up.y_root,
                        YPopupWindow::pfCanFlipVertical |
                        YPopupWindow::pfCanFlipHorizontal |
                        YPopupWindow::pfPopupMenu);
    }
}

void YFrameWindow::handleCrossing(const XCrossingEvent &crossing) {
    if (crossing.type == EnterNotify &&
        (crossing.mode == NotifyNormal ||
         (strongPointerFocus && crossing.mode == NotifyUngrab)) &&
        crossing.window == handle() &&
        (strongPointerFocus ||
         (crossing.serial != YWindow::getLastEnterNotifySerial() &&
          crossing.serial != YWindow::getLastEnterNotifySerial() + 1))
#if false
        &&
        (strongPointerFocus ||
         fMouseFocusX != crossing.x_root ||
         fMouseFocusY != crossing.y_root)
#endif
       )
    {
        //msg("xf: %d %d", fMouseFocusX, crossing.x_root, fMouseFocusY, crossing.y_root);

//        fMouseFocusX = crossing.x_root;
//        fMouseFocusY = crossing.y_root;

        if (!clickFocus && visible() && canFocusByMouse()) {
            if (!delayPointerFocus)
                focus(false);
            else {
                fDelayFocusTimer->setTimer(pointerFocusDelay, this, true);
            }
        } else {
            if (fDelayFocusTimer) {
                fDelayFocusTimer->stopTimer();
            }
        }
        if (autoRaise) {
            fAutoRaiseTimer->setTimer(autoRaiseDelay, this, true);
        }
    } else if (crossing.type == LeaveNotify &&
               fFocused &&
               focusRootWindow &&
               crossing.window == handle())
    {
//        fMouseFocusX = crossing.x_root;
//        fMouseFocusY = crossing.y_root;

        if (crossing.detail != NotifyInferior &&
            crossing.mode == NotifyNormal)
        {
            if (fDelayFocusTimer)
                fDelayFocusTimer->disableTimerListener(this);
            if (autoRaise && fAutoRaiseTimer) {
                fAutoRaiseTimer->disableTimerListener(this);
            }
        }
    }
}

void YFrameWindow::handleFocus(const XFocusChangeEvent &focus) {
    if (manager->switchWindowVisible()) {
        return ;
    }
#if 1
    if (focus.type == FocusIn &&
        focus.mode != NotifyGrab &&
        focus.window == handle() &&
        focus.detail != NotifyInferior &&
        focus.detail != NotifyPointer &&
        focus.detail != NotifyPointerRoot)
        manager->switchFocusTo(this);
#endif
#if 0
    else if (focus.type == FocusOut &&
               focus.mode == NotifyNormal &&
               focus.detail != NotifyInferior &&
               focus.detail != NotifyPointer &&
               focus.detail != NotifyPointerRoot &&
               focus.window == handle())
        manager->switchFocusFrom(this);
#endif
    layoutShape();
}

bool YFrameWindow::handleTimer(YTimer *t) {
    if (isMapped() && !client()->destroyed()) {
        if (t == fAutoRaiseTimer) {
            actionPerformed(actionRaise);
        }
        else if (t == fDelayFocusTimer) {
            focus(false);
        }
    }
    return false;
}

void YFrameWindow::raise() {
    if (this != manager->top(getActiveLayer())) {
        setAbove(manager->top(getActiveLayer()));
        raiseTo(prevLayer());
    }
}

void YFrameWindow::lower() {
    if (this != manager->bottom(getActiveLayer())) {
        setAbove(nullptr);
        beneath(nextLayer());
    }
}

void YFrameWindow::removeFrame() {
#ifdef DEBUG
    if (debug_z) dumpZorder("before removing", this);
#endif
    manager->removeLayeredFrame(this);
#ifdef DEBUG
    if (debug_z) dumpZorder("after removing", this);
#endif
}

void YFrameWindow::insertFrame(bool top) {
#ifdef DEBUG
    if (debug_z) dumpZorder("before inserting", this);
#endif
    if (top) {
        manager->setTop(getActiveLayer(), this);
    } else {
        manager->setBottom(getActiveLayer(), this);
    }
#ifdef DEBUG
    if (debug_z) dumpZorder("after inserting", this);
#endif
}

YFrameWindow *YFrameWindow::findWindow(int flags) {
    YFrameWindow *p = this;

    if (flags & fwfNext)
        goto next;

    do {
        if ((flags & fwfMinimized) && !p->isMinimized())
            goto next;
        if ((flags & fwfUnminimized) && p->isMinimized())
            goto next;
        if ((flags & fwfVisible) && !p->visible())
            goto next;
        if ((flags & fwfHidden) && !p->isHidden())
            goto next;
        if ((flags & fwfNotHidden) && p->isHidden())
            goto next;
        if ((flags & fwfFocusable) && !p->canFocus())
            goto next;
        if ((flags & fwfWorkspace) && !p->visibleNow())
            goto next;
        if (!p->client()->adopted() || p->client()->destroyed())
            goto next;

        return p;

    next:
        if (flags & fwfBackward)
            p = (flags & fwfLayers) ? p->prevLayer() : p->prev();
        else
            p = (flags & fwfLayers) ? p->nextLayer() : p->next();
        if (p == nullptr) {
            if (!(flags & fwfCycle))
                return nullptr;
            else if (flags & fwfBackward)
                p = (flags & fwfLayers) ? manager->bottomLayer() : manager->bottom(getActiveLayer());
            else
                p = (flags & fwfLayers) ? manager->topLayer() : manager->top(getActiveLayer());
        }
    } while (p != this);

    if (!(flags & fwfSame))
        return nullptr;
    if ((flags & fwfVisible) && !p->visible())
        return nullptr;
    if ((flags & fwfWorkspace) && !p->visibleNow())
        return nullptr;
    if (!p->client()->adopted() || p->client()->destroyed())
        return nullptr;

    return this;
}

void YFrameWindow::handleConfigure(const XConfigureEvent &/*configure*/) {
}

void YFrameWindow::sendConfigure() {
    XConfigureEvent notify = {
        .type              = ConfigureNotify,
        .serial            = CurrentTime,
        .send_event        = True,
        .display           = nullptr,
        .event             = client()->handle(),
        .window            = client()->handle(),
        .x                 = x() + borderX(),
        .y                 = y() + borderY() + titleY(),
        .width             = int(client()->width()),
        .height            = int(client()->height()),
        .border_width      = client()->getBorder(),
        .above             = None,
        .override_redirect = False,
    };

    XSendEvent(xapp->display(),
               client()->handle(),
               False,
               StructureNotifyMask,
               (XEvent *) &notify);
}

void YFrameWindow::actionPerformed(YAction action, unsigned int modifiers) {
    switch (action.ident()) {
    case actionRestore:
        if (canRestore())
            wmRestore();
        break;
    case actionMinimize:
        if (canMinimize())
            wmMinimize();
        break;
    case actionMaximize:
        if (canMaximize())
            wmMaximize();
        break;
    case actionMaximizeVert:
        if (canMaximize())
            wmMaximizeVert();
        break;
    case actionMaximizeHoriz:
        if (canMaximize())
            wmMaximizeHorz();
        break;
    case actionLower:
        if (canLower())
            wmLower();
        break;
    case actionRaise:
        if (canRaise())
            wmRaise();
        break;
    case actionDepth:
        if (overlaps(bool(Below)) && canRaise()){
            wmRaise();
            manager->setFocus(this, true);
        } else if (overlaps(bool(Above)) && canLower())
            wmLower();
        break;
    case actionRollup:
        if (canRollup())
            wmRollup();
        break;
    case actionClose:
        if (canClose())
            wmClose();
        break;
    case actionKill:
        wmConfirmKill();
        break;
    case actionHide:
        if (canHide())
            wmHide();
        break;
    case actionShow:
        if (canShow())
            wmShow();
        break;
    case actionMove:
        if (canMove())
            wmMove();
        break;
    case actionSize:
        if (canSize())
            wmSize();
        break;
    case actionOccupyAllOrCurrent:
        wmOccupyAllOrCurrent();
        break;
#if DO_NOT_COVER_OLD
    case actionDoNotCover:
        wmToggleDoNotCover();
        break;
#endif
    case actionFullscreen:
        if (canFullscreen())
            wmToggleFullscreen();
        break;
    case actionToggleTray:
        wmToggleTray();
        break;
    case actionLayerDesktop:
    case actionLayerOne:
    case actionLayerBelow:
    case actionLayerThree:
    case actionLayerNormal:
    case actionLayerFive:
    case actionLayerOnTop:
    case actionLayerSeven:
    case actionLayerDock:
    case actionLayerNine:
    case actionLayerAboveDock:
    case actionLayerEleven:
    case actionLayerMenu:
    case actionLayerThirteen:
    case actionLayerFullscreen:
    case actionLayerAboveAll:
        {
            int layer = (action.ident() - actionLayerDesktop) / 2;
            bool isFull = isFullscreen() && manager->fullscreenEnabled();
            if (isFull)
                manager->setFullscreenEnabled(false);
            wmSetLayer(layer);
            if (isFull)
                manager->setFullscreenEnabled(true);
        }
        break;
    default:
        for (int w(0); w < workspaceCount; w++) {
            if (action == workspaceActionMoveTo[w]) {
                wmOccupyWorkspace(w);
                return ;
            }
        }
        wmActionListener->actionPerformed(action, modifiers);
    }
}

void YFrameWindow::wmSetLayer(int layer) {
    int previous = fWinState;
    setRequestedLayer(layer);
    if (hasbit(previous ^ fWinState, WinStateAbove | WinStateBelow)) {
        client()->setStateHint();
    }
}

void YFrameWindow::wmSetTrayOption(int option) {
    setTrayOption(option);
}

#if DO_NOT_COVER_OLD
void YFrameWindow::wmToggleDoNotCover() {
    setDoNotCover(!doNotCover());
}
#endif

void YFrameWindow::wmToggleFullscreen() {
    if (isFullscreen()) {
        setState(WinStateFullscreen, None);
    }
    else if (canFullscreen()) {
        if (isUnmapped()) {
            makeMapped();
            xapp->sync();
        }
        setState(WinStateFullscreen, WinStateFullscreen);
    }
}

void YFrameWindow::wmToggleTray() {
    if (getTrayOption() == WinTrayIgnore) {
        setTrayOption(WinTrayExclusive);
    } else {
        setTrayOption(WinTrayIgnore);
    }
}

void YFrameWindow::wmMove() {
    Window root, child;
    int rx, ry, wx, wy;
    unsigned int mask;
    XQueryPointer(xapp->display(), desktop->handle(),
                  &root, &child, &rx, &ry, &wx, &wy, &mask);
    if (wx > int(x() + width()))
        wx = x() + width();
    if (wy > int(y() + height()))
        wy = y() + height();
    if (wx < x())
        wx = x();
    if (wy < y())
        wy = y();
    startMoveSize(true, false,
                  0, 0,
                  wx - x(), wy - y());
}

void YFrameWindow::wmSize() {
    startMoveSize(false, false,
                  0, 0,
                  0, 0);
}

bool YFrameWindow::canRestore() const {
    return hasState(WinStateUnmapped | WinStateMaximizedBoth);
}

void YFrameWindow::wmRestore() {
    if (canRestore()) {
        wmapp->signalGuiEvent(geWindowRestore);
        setState(WinStateUnmapped | WinStateMaximizedBoth, 0);
    }
}

void YFrameWindow::wmMinimize() {
#ifdef DEBUG_S
    MSG(("wmMinimize - Frame: %d", visible()));
    MSG(("wmMinimize - Client: %d", client()->visible()));
#endif
    manager->lockFocus();
    if (isMinimized()) {
        wmapp->signalGuiEvent(geWindowRestore);
        makeMapped();
    } else {
        wmapp->signalGuiEvent(geWindowMin);
        setState(WinStateUnmapped, WinStateMinimized);
        wmLower();
    }
    manager->unlockFocus();
    manager->focusLastWindow();
}

void YFrameWindow::minimizeTransients() {
    for (YFrameWindow *w = transient(); w; w = w->nextTransient()) {
// Since a) YFrameWindow::setState is too heavy but b) we want to save memory
        MSG(("> isMinimized: %d\n", w->isMinimized()));
        if (w->isMinimized()) {
            w->fWinState |= WinStateWasMinimized;
        } else {
            w->fWinState &= ~WinStateWasMinimized;
            w->wmMinimize();
        }
    }
}

void YFrameWindow::restoreMinimizedTransients() {
    for (YFrameWindow *w = transient(); w; w = w->nextTransient())
        if (w->isMinimized() && !w->wasMinimized())
            w->setState(WinStateMinimized, 0);
}

void YFrameWindow::hideTransients() {
    for (YFrameWindow *w = transient(); w; w = w->nextTransient()) {
// See YFrameWindow::minimizeTransients() for reason
        MSG(("> isHidden: %d\n", w->isHidden()));
        if (w->isHidden()) {
            w->fWinState |= WinStateWasHidden;
        } else {
            w->fWinState&= ~WinStateWasHidden;
            w->wmHide();
        }
    }
}

void YFrameWindow::restoreHiddenTransients() {
    for (YFrameWindow *w = transient(); w; w = w->nextTransient())
        if (w->isHidden() && !w->wasHidden())
            w->setState(WinStateHidden, 0);
}

void YFrameWindow::doMaximize(int flags) {
    if (isUnmapped()) {
        makeMapped();
        xapp->sync();
    }
    if (flags == (getState() & WinStateMaximizedBoth)) {
        wmapp->signalGuiEvent(geWindowRestore);
        setState(flags, None);
    } else {
        wmapp->signalGuiEvent(geWindowMax);
        setState(WinStateMaximizedBoth, flags);
    }
}

void YFrameWindow::wmMaximize() {
    doMaximize(WinStateMaximizedBoth);
}

void YFrameWindow::wmMaximizeVert() {
    doMaximize(WinStateMaximizedVert);
}

void YFrameWindow::wmMaximizeHorz() {
    doMaximize(WinStateMaximizedHoriz);
}

void YFrameWindow::wmRollup() {
    if (isRollup()) {
        wmapp->signalGuiEvent(geWindowRestore);
        makeMapped();
    } else {
        //if (!canRollup())
        //    return ;
        wmapp->signalGuiEvent(geWindowRollup);
        setState(WinStateUnmapped, WinStateRollup);
    }
}

void YFrameWindow::wmHide() {
    if (isHidden()) {
        wmapp->signalGuiEvent(geWindowRestore);
        makeMapped();
    } else {
        wmapp->signalGuiEvent(geWindowHide);
        setState(WinStateUnmapped, WinStateHidden);
    }
}

void YFrameWindow::wmLower() {
    if (canLower()) {
        if (isMapped())
            wmapp->signalGuiEvent(geWindowLower);
        if (owner()) {
            for (YFrameWindow* w = this; w; w = w->owner()) {
                w->doLower();
            }
        }
        else if (hasState(WinStateModal) && client()->clientLeader()) {
            const Window leader = client()->clientLeader();
            YArray<YFrameWindow*> lower;
            lower += this;
            for (YFrameWindow* w = next(); w; w = w->next()) {
                if (leader == w->client()->clientLeader() && isGroupModalFor(w))
                    lower += w;
            }
            for (YFrameWindow* f : lower) {
                f->doLower();
            }
        }
        else {
            doLower();
        }
        manager->focusTopWindow();
    }
}

void YFrameWindow::doLower() {
    if (next()) {
        if (manager->setAbove(this, nullptr)) {
            beneath(prev());
        }
    }
}

void YFrameWindow::wmRaise() {
    if (canRaise()) {
        doRaise();
        manager->restackWindows();
    }
}

void YFrameWindow::doRaise() {
#ifdef DEBUG
    if (debug_z) dumpZorder("wmRaise: ", this);
#endif
    if (prev()) {
        YArray<YFrameWindow*> family;
        family += this;
        const int layer = getActiveLayer();
        const Window leader = client()->clientLeader();
        if (leader && owner() == nullptr && notState(WinStateModal)) {
            for (auto& modal : groupModals) {
                if (modal == leader &&
                    find(family, modal.frame) < 0 &&
                    layer == modal->getActiveLayer()) {
                    family += modal;
                }
            }
        }
        for (int i = 0; i < family.getCount(); ++i) {
            YFrameWindow* frame = family[i];
            int k = i;
            for (YFrameWindow* trans = frame->transient();
                 trans != nullptr; trans = trans->nextTransient()) {
                if (find(family, trans) < 0 &&
                    layer == trans->getActiveLayer()) {
                    family.insert(++k, trans);
                }
            }
        }

        YFrameWindow* topmost = manager->top(layer);
        while (topmost && topmost != this && 0 <= find(family, topmost)) {
            topmost = topmost->next();
        }

        YArray<YFrameWindow*> raise, other;
        for (YFrameWindow* frame = topmost; frame; frame = frame->next()) {
            if (find(family, frame) < 0)
                other += frame;
            else
                raise += frame;
            if (frame == this)
                break;
        }

        if (other.getCount() < raise.getCount()) {
            for (int i = other.getCount() - 1; 0 <= i; --i) {
                manager->setAbove(other[i], next());
            }
        }
        else {
            for (int i = 0; i < raise.getCount(); ++i) {
                manager->setAbove(raise[i], topmost);
            }
        }

#ifdef DEBUG
        if (debug_z) dumpZorder("wmRaise after raise: ", this);
#endif
    }
}

void YFrameWindow::wmClose() {
    if (!canClose())
        return ;

    manager->grabServer();
    client()->getProtocols(true);

    client()->sendPing();
    if (client()->protocol(YFrameClient::wpDeleteWindow)) {
        client()->sendDelete();
    } else {
        if (frameOption(foForcedClose)) {
            wmKill();
        } else {
            wmConfirmKill();
        }
    }
    manager->ungrabServer();
}

void YFrameWindow::wmConfirmKill(const char* message) {
    if (message == nullptr) {
        message = _("WARNING! All unsaved changes will be lost when\n"
                    "this client is killed. Do you wish to proceed?");
    }
    if (fKillMsgBox) {
        fKillMsgBox->unmanage();
    }
    fKillMsgBox = new YMsgBox(YMsgBox::mbBoth,
                      _("Kill Client: ") + getTitle(),
                      message,
                      this, "bomb");
}

void YFrameWindow::wmKill() {
    if (!canClose())
        return ;
#ifdef DEBUG
    if (debug)
        msg("No WM_DELETE_WINDOW protocol");
#endif
    XKillClient(xapp->display(), client()->handle());
}

void YFrameWindow::wmPrevWindow() {
    int flags = fwfNext | fwfVisible | fwfCycle |
                fwfFocusable | fwfWorkspace | fwfSame;
    YFrameWindow *f = findWindow(flags | fwfBackward);
    if (f && f != this) {
        f->wmRaise();
        manager->setFocus(f, true);
    }
}

void YFrameWindow::wmNextWindow() {
    int flags = fwfNext | fwfVisible | fwfCycle |
                fwfFocusable | fwfWorkspace | fwfSame;
    YFrameWindow *f = findWindow(flags);
    if (f && f != this) {
        wmLower();
        f->wmRaise();
        manager->setFocus(f, true);
    }
}

void YFrameWindow::loseWinFocus() {
    if (fFocused && fManaged) {
        fFocused = false;

        if (hasState(WinStateFocused)) {
            setState(WinStateFocused | WinStateUrgent, 0);
        }
        if (true || !clientMouseActions)
            if (focusOnClickClient || raiseOnClickClient)
                if (container())
                    container()->grabButtons();
        if (isIconic())
            fMiniIcon->repaint();
        else {
            setBackground(inactiveBorderBg);
            repaint();
            if (fTitleBar)
                fTitleBar->deactivate();
        }
        updateTaskBar();
    }
}

void YFrameWindow::setWinFocus() {
    if (!fFocused) {
        fFocused = true;

        setState(WinStateFocused | WinStateUrgent, WinStateFocused);
        if (isIconic())
            fMiniIcon->repaint();
        else {
            if (fTitleBar)
                fTitleBar->activate();
            setBackground(activeBorderBg);
            repaint();
        }
        updateTaskBar();

        if (true || !clientMouseActions) {
            if (!raiseOnClickClient || !canRaise() || !overlaps(bool(Below)))
                container()->releaseButtons();
        }
    }
}

void YFrameWindow::updateFocusOnMap(bool& doActivate) {
    bool onCurrentWorkspace = visibleNow();

    if (fDelayFocusTimer) {
        fDelayFocusTimer->stopTimer();
    }
    if (fAutoRaiseTimer) {
        fAutoRaiseTimer->stopTimer();
    }

    if (avoidFocus())
        doActivate = false;

    if (frameOption(foNoFocusOnMap))
        doActivate = false;

    if (!onCurrentWorkspace && !focusChangesWorkspace && !focusCurrentWorkspace)
        doActivate = false;

    if (owner() != nullptr) {
        if (owner()->focused() ||
           (nextTransient() && nextTransient()->focused()))
        {
            if (!focusOnMapTransientActive)
                doActivate = false;
        } else {
            if (!focusOnMapTransient)
                doActivate = false;
        }
    } else {
        bool mapUrgentGroupMember = false;
        if (isUrgent() && client()->clientLeader()) {
            YFrameWindow* f = manager->getFocus();
            if (f && f->client()->clientLeader() == client()->clientLeader()) {
                tlog("focus urgent group member %s", boolstr(doActivate));
                mapUrgentGroupMember = true;
            }
        }
        else if (manager->getFocus() && isGroupModalFor(manager->getFocus())) {
            if (getActiveLayer() >= manager->getFocus()->getActiveLayer()) {
                mapUrgentGroupMember = true;
            }
        }
        if (!focusOnMap && !mapUrgentGroupMember) {
            doActivate = false;
        }
    }

    manager->updateUserTime(fUserTime);
    if (doActivate && fUserTime.good())
        doActivate = (fUserTime.time() && fUserTime == manager->lastUserTime());
}

bool YFrameWindow::canShow() const {
    if (isUnmapped()) {
        return true;
    }

    int ax, ay, ar, ab;

    manager->getWorkArea(this, &ax, &ay, &ar, &ab);

    if ( !inrange(x(), ax - borderX(), ar - int(width()) + borderX()) ||
         !inrange(y(), ay - borderY() - titleY(), ab - int(height()) + borderY()))
    {
        return true;
    }

    return false;
}

void YFrameWindow::limitOuterPosition() {
    int ax, ay, ar, ab;
    if (affectsWorkArea() || client() == taskBar) {
        ax = 0; ay = 0; ar = desktop->width(); ab = desktop->height();
    } else {
        manager->getWorkArea(this, &ax, &ay, &ar, &ab);
    }
    ay -= min(borderY(), int(topSideVerticalOffset));
    if ( !inrange(x(), ax - borderX(), ar - int(width()) + borderX()) ||
         !inrange(y(), ay - borderY() - titleY(),
                       ab - int(height()) + borderY() + titleY()))
    {
        int newX = x();
        int newY = y();

        if (newX > ar - int(width()) + borderX())
            newX = ar - int(width());
        if (newX < ax)
            newX = ax;
        if (newY > ab - int(height()) + borderY() + titleY())
            newY = ab - int(height());
        if (newY < ay)
            newY = ay;

        setCurrentPositionOuter(newX, newY);
    }
}

void YFrameWindow::wmShow() {
    limitOuterPosition();
    if (isUnmapped()) {
        makeMapped();
    }
}

void YFrameWindow::focus(bool canWarp) {
    if (limitPosition &&
        (x() >= int(desktop->width()) - borderX() ||
         y() >= int(desktop->height()) - borderY() - titleY() ||
         x() <= - int(width()) + borderX() ||
         y() <= - int(height()) + borderY() + titleY()))
    {
        int newX = x();
        int newY = y();
        if (newX >= int(desktop->width()) - borderX())
            newX = int(desktop->width()) - int(width());
        if (newY >= int(desktop->height()) - borderY() - titleY())
            newY = int(desktop->height()) - int(height());
        if (newX < int(- borderX()))
            newX = int(- borderX());
        if (newY < int(- borderY()))
            newY = int(- borderY());
        setCurrentPositionOuter(newX, newY);
    }

    manager->setFocus(this, canWarp);
    if (raiseOnFocus && manager->wmState() == YWindowManager::wmRUNNING) {
        wmRaise();
    }
}

void YFrameWindow::activate(bool canWarp, bool curWork) {
    manager->lockFocus();
    if ( ! visibleNow()) {
        if (focusCurrentWorkspace && curWork)
            setWorkspace(manager->activeWorkspace());
        else {
            workspaces[getWorkspace()].focused = this;
            manager->activateWorkspace(getWorkspace());
            xapp->sync();
            XEvent ignored;
            while (XCheckWindowEvent(xapp->display(), xapp->root(),
                                     FocusChangeMask, &ignored)) { }
        }
    }
    if (isUnmapped()) {
        makeMapped();
    }
    manager->unlockFocus();
    focus(canWarp);
}

void YFrameWindow::activateWindow(bool raise, bool curWork) {
    if (raise)
        wmRaise();
    activate(true, curWork);
}

MiniIcon *YFrameWindow::getMiniIcon() {
    if (minimizeToDesktop && fMiniIcon == nullptr) {
        fMiniIcon = new MiniIcon(this);
    }
    return fMiniIcon;
}

void YFrameWindow::refresh() {
    repaint();
    if (fTitleBar) {
        fTitleBar->refresh();
    }
}

void YFrameWindow::repaint() {
    paint(getGraphics(), geometry());
}

void YFrameWindow::handleExpose(const XExposeEvent &expose) {
    if (expose.count == 0) {
        repaint();
    }
}

void YFrameWindow::paint(Graphics &g, const YRect& r) {
    if (!(frameDecors() & (fdResize | fdBorder)))
        return ;

    YColor bg = focused() ? activeBorderBg : inactiveBorderBg;
    g.setColor(bg);

    switch (wmLook) {
    case lookWin95:
    case lookWarp4:
    case lookNice:
        g.fillRect(1, 1, width() - 3, height() - 3);
        g.drawBorderW(0, 0, width() - 1, height() - 1, true);
        break;
    case lookMotif:
    case lookWarp3:
        g.draw3DRect(0, 0, width() - 1, height() - 1, true);
        g.draw3DRect(borderX() - 1, borderY() - 1,
                     width() - 2 * borderX() + 1, height() - 2 * borderY() + 1,
                     false);

        g.fillRect(1, 1, width() - 2, borderY() - 2);
        g.fillRect(1, 1, borderX() - 2, height() - 2);
        g.fillRect(1, (height() - 1) - (borderY() - 2), width() - 2, borderX() - 2);
        g.fillRect((width() - 1) - (borderX() - 2), 1, borderX() - 2, height() - 2);

        if (wmLook == lookMotif && canSize()) {
            g.setColor(bg.darker());
            g.drawLine(wsCornerX - 1, 0, wsCornerX - 1, height() - 1);
            g.drawLine(width() - wsCornerX - 1, 0, width() - wsCornerX - 1, height() - 1);
            g.drawLine(0, wsCornerY - 1, width(),wsCornerY - 1);
            g.drawLine(0, height() - wsCornerY - 1, width(), height() - wsCornerY - 1);
            g.setColor(bg.brighter());
            g.drawLine(wsCornerX, 0, wsCornerX, height() - 1);
            g.drawLine(width() - wsCornerX, 0, width() - wsCornerX, height() - 1);
            g.drawLine(0, wsCornerY, width(), wsCornerY);
            g.drawLine(0, height() - wsCornerY, width(), height() - wsCornerY);
        }
        break;
    case lookPixmap:
    case lookMetal:
    case lookFlat:
    case lookGtk:
        {
            int n = focused();
            int t = (frameDecors() & fdResize) ? 0 : 1;

            if ((frameT[t][n] != null || rgbFrameT[t][n] != null) &&
                (frameL[t][n] != null || rgbFrameL[t][n] != null) &&
                (frameR[t][n] != null || rgbFrameR[t][n] != null) &&
                (frameB[t][n] != null || rgbFrameB[t][n] != null) &&
                frameTL[t][n] != null && frameTR[t][n] != null &&
                frameBL[t][n] != null && frameBR[t][n] != null) {
                int const xtl(frameTL[t][n]->width());
                int const ytl(frameTL[t][n]->height());
                int const xtr(frameTR[t][n]->width());
                int const ytr(frameTR[t][n]->height());
                int const xbl(frameBL[t][n]->width());
                int const ybl(frameBL[t][n]->height());
                int const xbr(frameBR[t][n]->width());
                int const ybr(frameBR[t][n]->height());

                int const cx(width()/2);
                int const cy(height()/2);

                int mxtl = min(xtl, cx);
                int mytl = min(ytl, max(titleY() + borderY(), cy));
                int mxtr = min(xtr, cx);
                int mytr = min(ytr, max(titleY() + borderY(), cy));
                int mxbl = min(xbl, cx);
                int mybl = min(ybl, cy);
                int mxbr = min(xbr, cx);
                int mybr = min(ybr, cy);

                g.copyPixmap(frameTL[t][n], 0, 0,
                             mxtl, mytl, 0, 0);
                g.copyPixmap(frameTR[t][n], max(0, (int)xtr - (int)cx), 0,
                             mxtr, mytr,
                             width() - mxtr, 0);
                g.copyPixmap(frameBL[t][n], 0, max(0, (int)ybl - (int)cy),
                             mxbl, mybl,
                             0, height() - mybl);
                g.copyPixmap(frameBR[t][n],
                             max(0, (int)xbr - (int)cx), max(0, (int)ybr - (int)cy),
                             mxbr, mybr,
                             width() - mxbr, height() - mybr);

                if ((int) width() > (mxtl + mxtr)) {
                    if (frameT[t][n] != null)
                        g.repHorz(frameT[t][n],
                                  mxtl, 0, width() - mxtl - mxtr);
                    else g.drawGradient(rgbFrameT[t][n],
                        mxtl, 0, width() - mxtl - mxtr, borderY());
                }

                if ((int) height() > (mytl + mybl)) {
                    if (frameL[t][n] != null) g.repVert(frameL[t][n],
                        0, mytl, height() - mytl - mybl);
                    else g.drawGradient(rgbFrameL[t][n],
                        0, mytl, borderX(), height() - mytl - mybl);
                }

                if ((int) height() > (mytr + mybr)) {
                    if (frameR[t][n] != null) g.repVert(frameR[t][n],
                        width() - borderX(), mytr, height() - mytr - mybr);
                    else g.drawGradient(rgbFrameR[t][n],
                        width() - borderX(), mytr,
                        borderX(), height() - mytr - mybr);
                }

                if ((int) width() > (mxbl + mxbr)) {
                    if (frameB[t][n] != null) g.repHorz(frameB[t][n],
                        mxbl, height() - borderY(), width() - mxbl - mxbr);
                    else g.drawGradient(rgbFrameB[t][n],
                        mxbl, height() - borderY(),
                        width() - mxbl - mxbr, borderY());
                }

            } else {
                g.fillRect(1, 1, width() - 3, height() - 3);
                g.drawBorderW(0, 0, width() - 1, height() - 1, true);
            }
        }
        break;
    }
}

void YFrameWindow::handlePopDown(YPopupWindow *popup) {
    MSG(("popdown %p up %p", popup, fPopupActive));
    if (fPopupActive == popup)
        fPopupActive = nullptr;
}

void YFrameWindow::popupSystemMenu(YWindow *owner) {
    if (fPopupActive == nullptr) {
        if (fTitleBar &&
            fTitleBar->visible() &&
            fTitleBar->menuButton() &&
            fTitleBar->menuButton()->visible())
        {
            fTitleBar->menuButton()->popupMenu();
        }
        else {
            int ax = x() + container()->x();
            int ay = y() + container()->y();
            if (isIconic()) {
                ax = fMiniIcon->x();
                ay = fMiniIcon->y();
            }
            popupSystemMenu(owner, ax, ay,
                            YPopupWindow::pfCanFlipVertical);
        }
    }
}

void YFrameWindow::popupSystemMenu(YWindow *owner, int x, int y,
                                   unsigned int flags,
                                   YWindow *forWindow)
{
    if (fPopupActive == nullptr) {
        updateMenu();
        if (windowMenu()->itemCount() == 0)
            return;
        if (windowMenu()->popup(owner, forWindow, this,
                                x, y, flags))
            fPopupActive = windowMenu();
    }
}

void YFrameWindow::updateTitle() {
    layoutShape();
    if (fTitleBar)
        fTitleBar->repaint();
    if (fWinListItem && windowList)
        windowList->repaintItem(fWinListItem);
    if (fTaskBarApp) {
        fTaskBarApp->setToolTip(getTitle());
        fTaskBarApp->repaint();
    }
    if (fTrayApp)
        fTrayApp->setToolTip(getTitle());
}

void YFrameWindow::updateIconTitle() {
    if (fTaskBarApp)
        fTaskBarApp->repaint();
    if (isIconic())
        fMiniIcon->repaint();
}

void YFrameWindow::wmOccupyAllOrCurrent() {
    if (isAllWorkspaces()) {
        wmOccupyWorkspace(manager->activeWorkspace());
    } else {
        setAllWorkspaces();
    }
}

void YFrameWindow::wmOccupyAll() {
    if (!isAllWorkspaces())
        setAllWorkspaces();
}

void YFrameWindow::wmOccupyWorkspace(int workspace) {
    PRECONDITION(workspace < workspaceCount);
    mainOwner()->setWorkspace(workspace);
}

void YFrameWindow::updateAllowed() {
    Atom atoms[12];
    int i = 0;
    if ((fFrameFunctions & ffMove) || (fFrameDecors & fdTitleBar))
        atoms[i++] = _XA_NET_WM_ACTION_MOVE;
    if ((fFrameFunctions & ffResize) || (fFrameDecors & fdResize))
        atoms[i++] = _XA_NET_WM_ACTION_RESIZE;
    if ((fFrameFunctions & ffClose) || (fFrameDecors & fdClose))
        atoms[i++] = _XA_NET_WM_ACTION_CLOSE;
    if ((fFrameFunctions & ffMinimize) || (fFrameDecors & fdMinimize))
        atoms[i++] = _XA_NET_WM_ACTION_MINIMIZE;
    if ((fFrameFunctions & ffMaximize) || (fFrameDecors & fdMaximize)) {
        atoms[i++] = _XA_NET_WM_ACTION_MAXIMIZE_HORZ;
        atoms[i++] = _XA_NET_WM_ACTION_MAXIMIZE_VERT;
    }
    if ((fFrameFunctions & ffRollup) || (fFrameDecors & fdRollup))
        atoms[i++] = _XA_NET_WM_ACTION_SHADE;
    if (canFullscreen())
        atoms[i++] = _XA_NET_WM_ACTION_FULLSCREEN;
    if (true || (fFrameDecors & fdDepth)) {
        atoms[i++] = _XA_NET_WM_ACTION_ABOVE;
        atoms[i++] = _XA_NET_WM_ACTION_BELOW;
    }
    atoms[i++] = _XA_NET_WM_ACTION_STICK;
    atoms[i++] = _XA_NET_WM_ACTION_CHANGE_DESKTOP;
    client()->setNetWMAllowedActions(atoms,i);
}

void YFrameWindow::getFrameHints() {
    long decors = client()->mwmDecors();
    long functions = client()->mwmFunctions();
    long win_hints = client()->winHints();
    MwmHints *mwm_hints = client()->mwmHints();
    int functions_only = (mwm_hints && mwm_hints->onlyFuncs());

    unsigned long old_functions = fFrameFunctions;
    unsigned long old_decors = fFrameDecors;

    fFrameFunctions = 0;
    fFrameDecors = 0;
    fFrameOptions = 0;

    if (decors & MWM_DECOR_BORDER)      fFrameDecors |= fdBorder;
    if (decors & MWM_DECOR_RESIZEH)     fFrameDecors |= fdResize;
    if (decors & MWM_DECOR_TITLE)       fFrameDecors |= fdTitleBar | fdDepth;
    if (decors & MWM_DECOR_MENU)        fFrameDecors |= fdSysMenu;
    if (decors & MWM_DECOR_MAXIMIZE)    fFrameDecors |= fdMaximize;
    if (decors & MWM_DECOR_MINIMIZE)    fFrameDecors |= fdMinimize | fdHide | fdRollup;

    if (functions & MWM_FUNC_MOVE) {
        fFrameFunctions |= ffMove;
        if (functions_only)
            fFrameDecors |= fdBorder;
    }
    if (functions & MWM_FUNC_RESIZE)    {
        fFrameFunctions |= ffResize;
        if (functions_only)
            fFrameDecors |= fdResize | fdBorder;
    }
    if (functions & MWM_FUNC_MAXIMIZE) {
        fFrameFunctions |= ffMaximize;
        if (functions_only)
            fFrameDecors |= fdMaximize;
    }
    if (functions & MWM_FUNC_MINIMIZE) {
        fFrameFunctions |= ffMinimize | ffHide | ffRollup;
        if (functions_only)
            fFrameDecors |= fdMinimize | fdHide | fdRollup;
    }
    if (functions & MWM_FUNC_CLOSE) {
        fFrameFunctions |= ffClose;
        fFrameDecors |= fdClose;
    }

    /// !!! fFrameOptions needs refactoring
    if (win_hints & WinHintsSkipFocus)
        fFrameOptions |= foIgnoreQSwitch;
    if (win_hints & WinHintsSkipWindowMenu)
        fFrameOptions |= foIgnoreWinList;
    if (win_hints & WinHintsSkipTaskBar)
        fFrameOptions |= foIgnoreTaskBar;
    if (win_hints & WinHintsDoNotCover)
        fFrameOptions |= foDoNotCover;

/// TODO #warning "need initial window mapping cleanup"
    switch (fWindowType) {
    case wtCombo:
        fFrameFunctions = 0;
        fFrameDecors = 0;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch;
        break;
    case wtDesktop:
        fFrameFunctions = 0;
        fFrameDecors = 0;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch |
                         foNoFocusOnMap;
        break;
    case wtDialog:
        break;
    case wtDND:
        fFrameFunctions = ffMove;
        fFrameDecors = 0;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch |
                         foNoFocusOnMap | foDoNotFocus;
        break;
    case wtDock:
        fFrameFunctions = 0;
        fFrameDecors = 0;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch |
                         foNoFocusOnMap;
        break;
    case wtDropdownMenu:
        fFrameFunctions = 0;
        fFrameDecors = 0;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch;
        break;
    case wtMenu:
        fFrameFunctions &= ~(ffResize | ffMinimize | ffMaximize);
        fFrameDecors = fdTitleBar | fdSysMenu | fdClose | fdRollup;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch;
        break;
    case wtNormal:
        break;
    case wtNotification:
        fFrameFunctions = 0;
        fFrameDecors = 0;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch |
                         foNoFocusOnMap;
        break;
    case wtPopupMenu:
        fFrameFunctions = 0;
        fFrameDecors = 0;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch;
        break;
    case wtSplash:
        fFrameFunctions = 0;
        fFrameDecors = 0;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch |
                         foNoFocusOnMap;
        break;
    case wtToolbar:
        break;
    case wtTooltip:
        fFrameFunctions = 0;
        fFrameDecors = 0;
        fFrameOptions |= foIgnoreTaskBar | foIgnoreWinList | foIgnoreQSwitch |
                         foNoFocusOnMap | foDoNotFocus;
        break;
    case wtUtility:
        break;
    }

    if (client()->shaped())
        fFrameDecors &= ~(fdTitleBar | fdBorder);

    WindowOption wo(getWindowOption());

    /*msg("decor: %lX %lX %lX %lX %lX %lX",
            wo.function_mask, wo.functions,
            wo.decor_mask, wo.decors,
            wo.option_mask, wo.options);*/

    fFrameFunctions &= ~wo.function_mask;
    fFrameFunctions |= wo.functions;
    fFrameDecors &= ~wo.decor_mask;
    fFrameDecors |= wo.decors;
    fFrameOptions &= ~(wo.option_mask & fWinOptionMask);
    fFrameOptions |= (wo.options & fWinOptionMask);

    if (hasbit((fFrameFunctions | fFrameDecors) ^ (old_functions | old_decors), 63))
    {
        updateAllowed();
    }
}

WindowOption YFrameWindow::getWindowOption() {
    WindowOption wo;
    if (haveHintOption()) {
        wo = getHintOption();
    }
    if (defOptions) {
        getWindowOptions(defOptions, wo, false);
    }
    return wo;
}

void YFrameWindow::getWindowOptions(WindowOptions *list, WindowOption &opt,
                                    bool remove)
{
    const ClassHint* h = client()->classHint();
    mstring klass(h->res_class);
    mstring name(h->res_name);
    mstring role(client()->windowRole());

    if (klass != null) {
        if (name != null) {
            mstring klass_instance(h->res_class, ".", h->res_name);
            list->mergeWindowOption(opt, klass_instance, remove);

            mstring name_klass(h->res_name, ".", h->res_class);
            list->mergeWindowOption(opt, name_klass, remove);
        }
        list->mergeWindowOption(opt, klass, remove);
    }
    if (name != null) {
        if (role != null) {
            mstring name_role = name.append(".").append(role);
            list->mergeWindowOption(opt, name_role, remove);
        }
        list->mergeWindowOption(opt, name, remove);
    }
    if (role != null)
        list->mergeWindowOption(opt, role, remove);
    list->mergeWindowOption(opt, null, remove);
}

void YFrameWindow::getDefaultOptions(bool &requestFocus) {
    WindowOption wo(getWindowOption());

    if (wo.icon.nonempty()) {
        ref<YIcon> icon = YIcon::getIcon(wo.icon);
        if (icon != null)
            fFrameIcon = icon;
    }
    if (inrange(wo.workspace, 0, workspaceCount - 1)) {
        setWorkspace(wo.workspace);
        if (wo.workspace != manager->activeWorkspace())
            requestFocus = false;
    }
    if (inrange(wo.layer, 0, WinLayerCount - 1))
        setRequestedLayer(wo.layer);
    if (inrange(wo.tray, 0, WinTrayOptionCount - 1))
        setTrayOption(wo.tray);
    fTrayOrder = wo.order;
}

ref<YIcon> newClientIcon(int count, int reclen, long * elem) {
    ref<YImage> small;
    ref<YImage> large;
    ref<YImage> huge;

    if (reclen < 2)
        return null;
    for (int i = 0; i < count; i++, elem += reclen) {
        Pixmap pixmap(elem[0]), mask(elem[1]);
        if (pixmap == None) {
            warn("pixmap == None for subicon #%d", i);
            continue;
        }

        unsigned w = 0, h = 0, depth = 0;

        if (reclen >= 6) {
            w = elem[2];
            h = elem[3];
            depth = elem[4];
        } else {
            Window root;
            int x, y;
            unsigned w1, h1, border;
            if (XGetGeometry(xapp->display(), pixmap,
                             &root, &x, &y, &w1, &h1,
                             &border, &depth) != True) {
                warn("BadDrawable for subicon #%d", i);
                continue;
            }
            w = w1;
            h = h1;
        }

        MSG(("client icon: %ld %ux%u %d", pixmap, w, h, depth));
        if (inrange(w, 1U, 256U) + inrange(h, 1U, 256U) != 2) {
            MSG(("Invalid pixmap size for subicon #%d: %ux%u", i, w, h));
            continue;
        }

        if (depth == 1) {
            ref<YPixmap> img = YPixmap::create(w, h, xapp->depth());
            Graphics g(img, 0, 0);

            g.setColorPixel(0xffffffff);
            g.fillRect(0, 0, w, h);
            g.setColorPixel(0xff000000);
            g.setClipMask(pixmap);
            g.fillRect(0, 0, w, h);

            ref<YImage> img2 =
                YImage::createFromPixmapAndMask(img->pixmap(), mask, w, h);

            if (w <= YIcon::smallSize() || h <= YIcon::smallSize())
                small = img2;
            else if (small == null)
                small = img2->scale(YIcon::smallSize(), YIcon::smallSize());
            if (YIcon::smallSize() == YIcon::largeSize())
                large = small;
            else if (YIcon::smallSize() < w && w <= YIcon::largeSize()) {
                if (large == null || w > large->width()) {
                    large = img2;
                }
            }
            else if (YIcon::largeSize() < w && large == null)
                large = img2->scale(YIcon::largeSize(), YIcon::largeSize());
            if (YIcon::largeSize() == YIcon::hugeSize())
                huge = large;
            else if (YIcon::largeSize() < w && w <= YIcon::hugeSize()) {
                if (huge == null || w > huge->width()) {
                    huge = img2;
                }
            }
            else if (huge == null)
                huge = img2->scale(YIcon::hugeSize(), YIcon::hugeSize());
            img = null;
        }

        if (depth == xapp->depth() || depth == 24U) {
            if (w <= YIcon::smallSize()) {
                small = YImage::createFromPixmapAndMaskScaled(
                    pixmap, mask, w, h, YIcon::smallSize(), YIcon::smallSize());
            }
            else if (w <= YIcon::largeSize()) {
                large = YImage::createFromPixmapAndMaskScaled(
                    pixmap, mask, w, h, YIcon::largeSize(), YIcon::largeSize());
            }
            else if (huge == null) {
                huge = YImage::createFromPixmapAndMaskScaled(
                    pixmap, mask, w, h, YIcon::hugeSize(), YIcon::hugeSize());
            }
        }
    }

    if (huge != null) {
        if (small == null)
            small = huge->scale(YIcon::smallSize(), YIcon::smallSize());
        if (large == null)
            large = huge->scale(YIcon::largeSize(), YIcon::largeSize());
    }

    ref<YIcon> icon;
    if (small != null || large != null || huge != null)
        icon.init(new YIcon(small, large, huge));
    return icon;
}

void YFrameWindow::updateIcon() {
    long count;
    long* elem;
    Pixmap* pixmap;
    Atom type;

/// TODO #warning "think about winoptions specified icon here"

    ref<YIcon> oldFrameIcon = fFrameIcon;

    if (client()->getNetWMIcon(&count, &elem)) {
        ref<YImage> icons[3], largestIcon;
        const long sizes[3] = {
            long(YIcon::smallSize()),
            long(YIcon::largeSize()),
            long(YIcon::hugeSize())
        };
        long* largestOffset = nullptr;
        long largestSize = 0;

        // Find icons that match Small-/Large-/HugeIconSize and search
        // for the largest icon from NET_WM_ICON set.
        for (long *e = elem;
             e + 2 < elem + count && e[0] > 0 && e[1] > 0;
             e += 2 + e[0] * e[1]) {
            long w = e[0], h = e[1], *d = e + 2;
            if (w == h && d + w*h <= elem + count) {
                // Maybe huge=large=small, so examine all sizes[].
                for (int i = 0; i < 3; i++) {
                    if (w == sizes[i] && icons[i] == null) {
                        if (i >= 1 && sizes[i - 1] == sizes[i]) {
                            icons[i] = icons[i - 1];
                        } else {
                            icons[i] = YImage::createFromIconProperty(d, w, h);
                            if (w > largestSize) {
                                largestOffset = d;
                                largestSize = w;
                                largestIcon = icons[i];
                            }
                        }
                    }
                }
                if ((w > largestSize && largestSize < sizes[2]) ||
                    (w > sizes[2] && w < largestSize))
                {
                    largestOffset = d;
                    largestSize = w;
                }
            }
        }

        // Create missing icons by scaling the largest icon.
        for (int i = 0; i < 3; i++) {
            if (icons[i] == null) {
                // create the largest icon
                if (largestIcon == null && largestOffset && largestSize) {
                    largestIcon =
                        YImage::createFromIconProperty(largestOffset,
                                                       largestSize,
                                                       largestSize);
                }
                if (largestIcon != null) {
                    icons[i] = largestIcon->scale(sizes[i], sizes[i]);
                }
            }
        }
        fFrameIcon.init(new YIcon(icons[0], icons[1], icons[2]));
        XFree(elem);
    }
    else if (client()->getWinIcons(&type, &count, &elem)) {
        if (type == _XA_WIN_ICONS)
            fFrameIcon = newClientIcon(elem[0], elem[1], elem + 2);
        else // compatibility
            fFrameIcon = newClientIcon(count/2, 2, elem);
        XFree(elem);
    }
    else if (client()->getKwmIcon(&count, &pixmap) && count == 2) {
        long pix[4] = {
            long(pixmap[0]),
            long(pixmap[1]),
            long(client()->iconPixmapHint()),
            long(client()->iconMaskHint()),
        };
        XFree(pixmap);
        fFrameIcon = newClientIcon(1 + (pix[2] != None), 2, pix);
    }
    else if (client()->iconPixmapHint()) {
        long pix[2] = {
            long(client()->iconPixmapHint()),
            long(client()->iconMaskHint()),
        };
        fFrameIcon = newClientIcon(1, 2, pix);
    }

    if (fFrameIcon == null) {
        const char* name = client()->classHint()->res_name;
        if (nonempty(name)) {
            fFrameIcon = YIcon::getIcon(name);
        }
    }
    if (fFrameIcon == null && client()->adopted() == false) {
        fFrameIcon = YIcon::getIcon("icewm");
    }

    if (fFrameIcon != null) {
        if (fFrameIcon->small() == null && fFrameIcon->large() == null) {
            fFrameIcon = null;
        }
    }

    if (fFrameIcon == null) {
        fFrameIcon = oldFrameIcon;
    }

    if (fTitleBar && fTitleBar->menuButton())
        fTitleBar->menuButton()->repaint();
    if (fMiniIcon)
        fMiniIcon->updateIcon();
    if (fTrayApp)
        fTrayApp->repaint();
    if (fTaskBarApp)
        fTaskBarApp->repaint();
    if (windowList && fWinListItem && windowList->visible())
        windowList->repaintItem(fWinListItem);
}

YFrameWindow *YFrameWindow::nextLayer() {
    if (next()) return next();

    for (long l(getActiveLayer() - 1); l > -1; --l)
        if (manager->top(l)) return manager->top(l);

    return nullptr;
}

YFrameWindow *YFrameWindow::prevLayer() {
    if (prev()) return prev();

    for (long l(getActiveLayer() + 1); l < WinLayerCount; ++l)
        if (manager->bottom(l)) return manager->bottom(l);

    return nullptr;
}

YMenu *YFrameWindow::windowMenu() {
    //if (frameOption(foFullKeys))
    //    return windowMenuNoKeys;
    //else
    return wmapp->getWindowMenu();
}

bool YFrameWindow::addAsTransient() {
    Window ownerWindow(client()->ownerWindow());
    if (ownerWindow) {
        fOwner = manager->findFrame(ownerWindow);
        if (fOwner) {
            YArray<YFrameWindow*> owners;
            for (YFrameWindow* o = fOwner;
                 o && find(owners, o) < 0;
                 o = o->owner())
            {
                owners += o;
            }
            if (0 <= find(owners, this)) {
                fOwner = nullptr;
                return false;
            }

            MSG(("transient for 0x%lX: 0x%p", ownerWindow, fOwner));
            PRECONDITION(fOwner->transient() != this);

            fNextTransient = fOwner->transient();
            fOwner->setTransient(this);

            if (getActiveLayer() < fOwner->getActiveLayer()) {
                setRequestedLayer(fOwner->getActiveLayer());
            }
            if (fNextTransient &&
                fNextTransient->getActiveLayer() == getActiveLayer())
            {
                setAbove(fNextTransient);
            }
            else if (fOwner->getActiveLayer() == getActiveLayer()) {
                setAbove(owner());
            }
            setWorkspace(owner()->getWorkspace());
            return true;
        }
    }
    if (ownerWindow == xapp->root() ||
        (ownerWindow == None && hasState(WinStateModal)))
    {
        Window leader = client()->clientLeader();
        if (leader && leader != client()->handle() && leader != xapp->root()) {
            groupModals += GroupModal(leader, this);
            if (notState(WinStateModal)) {
                fWinState |= WinStateModal;
            }
        }
    }
    return false;
}

void YFrameWindow::removeAsTransient() {
    if (fOwner) {
        MSG(("removeAsTransient"));

        for (YFrameWindow *curr = fOwner->transient(), *prev = nullptr;
             curr; prev = curr, curr = curr->nextTransient()) {
            if (curr == this) {
                if (prev)
                    prev->setNextTransient(nextTransient());
                else
                    fOwner->setTransient(nextTransient());
                break;
            }
        }

        fOwner = nullptr;
        fNextTransient = nullptr;
    }
    if (groupModals.nonempty() && hasState(WinStateModal)) {
        for (int i = groupModals.getCount() - 1; 0 <= i; --i) {
            if (groupModals[i] == this) {
                groupModals.remove(i);
            }
        }
    }
}

void YFrameWindow::addTransients() {
    YArray<YFrameWindow*> owners;
    for (YFrameWindow* w(manager->bottomLayer()); w; w = w->prevLayer()) {
        if (w->owner() == nullptr) {
            Window cow = w->client()->ownerWindow();
            if (cow && cow == client()->handle()) {
                if (owner()) {
                    if (owners.isEmpty()) {
                        for (YFrameWindow* o = owner();
                             o && find(owners, o) < 0;
                             o = o->owner())
                        {
                            owners += o;
                        }
                    }
                    if (0 <= find(owners, w)) {
                        continue;
                    }
                }
                w->addAsTransient();
            }
        }
    }
}

void YFrameWindow::removeTransients() {
    if (transient()) {
        MSG(("removeTransients"));
        for (YFrameWindow* tran = transient(), *next; tran; tran = next) {
            next = tran->nextTransient();
            tran->setNextTransient(nullptr);
            tran->setOwner(nullptr);
        }
        fTransient = nullptr;
    }
}

bool YFrameWindow::isModal() {
    if (hasState(WinStateModal))
        return true;

    MwmHints *mwmHints = client()->mwmHints();
    if (mwmHints && (mwmHints->flags & MWM_HINTS_INPUT_MODE))
        if (mwmHints->input_mode != MWM_INPUT_MODELESS)
            return true;

    if (hasModal())
        return true;

    return false;
}

bool YFrameWindow::hasModal() {
    for (YFrameWindow *w = transient(); w; w = w->nextTransient()) {
        if (w->isModal())
            return true;
    }

    /* search for app modal dialogs */
    if (groupModals.nonempty()) {
        bool self = false, that = false;
        for (const auto& modal : groupModals) {
            if (modal == client()->clientLeader())
                that = true;
            if (modal == this)
                self = true;
        }
        if (that && !self)
            return true;
    }

    return false;
}

bool YFrameWindow::canFocus() {
    if (hasModal())
        return false;

    return true;
}

bool YFrameWindow::canFocusByMouse() {
    return canFocus() && !avoidFocus();
}

bool YFrameWindow::avoidFocus() {
    if (frameOption(foDoNotFocus))
        return true;

    if (getInputFocusHint())
        return false;

    if (frameOption(foIgnoreNoFocusHint))
        return false;

    if (client()->protocol(YFrameClient::wpTakeFocus) ||
        frameOption(foAppTakesFocus))
        return false;

    return true;
}

bool YFrameWindow::getInputFocusHint() {
    XWMHints* hints = client()->hints();
    bool input = true;

    if ( !frameOption(foIgnoreNoFocusHint) &&
        (hints && (hints->flags & InputHint) && !hints->input)) {
        input = false;
    }
    if (frameOption(foDoNotFocus)) {
        input = false;
    }
    return input;
}


void YFrameWindow::setWorkspace(int workspace) {
    if ( ! inrange(workspace + 1, 0, int(workspaceCount)))
        return ;
    if (workspace != fWinWorkspace) {
        int activeWS = int(manager->activeWorkspace());
        bool otherWS = (workspace != AllWorkspaces && workspace != activeWS);
        bool refocus = (this == manager->getFocus() && otherWS);
        if (otherWS) {
            int ws = (fWinWorkspace >= 0 ? fWinWorkspace : activeWS);
            if (workspaces[ws].focused == this) {
                workspaces[ws].focused = nullptr;
            }
        }
        fWinWorkspace = workspace;
        if (isAllWorkspaces())
            fWinState |= WinStateSticky;
        else
            fWinState &= ~WinStateSticky;
        client()->setWorkspaceHint(fWinWorkspace);
        updateState();
        if (refocus)
            manager->focusLastWindow();
        updateTaskBar();
        if (windowList && fWinListItem)
            windowList->updateWindowListApp(fWinListItem);
        for (YFrameWindow *t = transient(); t; t = t->nextTransient()) {
            t->setWorkspace(getWorkspace());
        }
        if (taskBar)
            taskBar->workspacesRepaint();
    }
}

YFrameWindow *YFrameWindow::mainOwner() {
    YFrameWindow *f = this;
    while (f->owner()) {
        f = f->owner();
    }
    return f;
}


void YFrameWindow::setRequestedLayer(int layer) {
    if (inrange<int>(layer, WinLayerDesktop, WinLayerAboveAll)) {
        if (fWinRequestedLayer != layer ||
            (hasState(WinStateAbove) && layer != WinLayerOnTop) ||
            (hasState(WinStateBelow) && layer != WinLayerBelow))
        {
            fWinRequestedLayer = layer;

            int state = (fWinState & ~(WinStateAbove | WinStateBelow));
            if (layer == WinLayerOnTop) {
                state |= WinStateAbove;
            }
            if (layer == WinLayerBelow) {
                state |= WinStateBelow;
            }
            if (fWinState != state) {
                fWinState = state;
            }

            updateLayer();
        }
    }
}

int YFrameWindow::windowTypeLayer() const {
    int newLayer = fWinRequestedLayer;

    switch (fWindowType) {
    case wtCombo:
        newLayer = WinLayerMenu;
        break;
    case wtDesktop:
        newLayer = WinLayerDesktop;
        break;
    case wtDialog:
        break;
    case wtDND:
        newLayer = WinLayerAboveAll;
        break;
    case wtDock:
        newLayer = WinLayerDock;
        break;
    case wtDropdownMenu:
        newLayer = WinLayerMenu;
        break;
    case wtMenu:
        newLayer = WinLayerMenu;
        break;
    case wtNormal:
        break;
    case wtNotification:
        newLayer = WinLayerAboveDock;
        break;
    case wtPopupMenu:
        newLayer = WinLayerMenu;
        break;
    case wtSplash:
        break;
    case wtToolbar:
        break;
    case wtTooltip:
        newLayer = WinLayerAboveAll;
        break;
    case wtUtility:
        break;
    }
    return newLayer;
}

void YFrameWindow::updateLayer(bool restack) {
    long oldLayer = fWinActiveLayer;
    long newLayer = windowTypeLayer();

    if (hasState(WinStateBelow))
        newLayer = WinLayerBelow;
    if (hasState(WinStateAbove))
        newLayer = WinLayerOnTop;
    if (fOwner) {
        if (newLayer < fOwner->getActiveLayer())
            newLayer = fOwner->getActiveLayer();
    }
    if (isFullscreen() && manager->fullscreenEnabled()) {
        for (YFrameWindow *f = manager->getFocus(); f; f = f->owner()) {
            if (f == this) {
                newLayer = WinLayerFullscreen;
                break;
            }
        }
    }

    if (newLayer != fWinActiveLayer) {
        removeFrame();
        fWinActiveLayer = newLayer;
        insertFrame(oldLayer != WinLayerFullscreen
                || !manager->switchWindowVisible());

        if (client() && !client()->destroyed())
            client()->setLayerHint(fWinActiveLayer);

        if (limitByDockLayer &&
           (newLayer == WinLayerDock || oldLayer == WinLayerDock) &&
            client() != taskBar)
            manager->requestWorkAreaUpdate();

        for (YFrameWindow *w = transient(); w; w = w->nextTransient()) {
            w->updateLayer(false);
        }

        if (restack)
            manager->restackWindows();
        if (taskBar)
            taskBar->workspacesRepaint();
    }
}

void YFrameWindow::setTrayOption(int option) {
    if (option >= WinTrayOptionCount || option < 0)
        return ;
    if (option != fWinTrayOption) {
        fWinTrayOption = option;
        updateTaskBar();
    }
}

void YFrameWindow::updateState() {
    if (!isManaged() || client()->destroyed())
        return ;

    client()->setStateHint();

    // some code is probably against the ICCCM.
    // some applications misbehave either way.
    // (some hide windows on iconize, this is bad when switching workspaces
    // or rolling up the window).

    bool iconic = isHidden() || isMinimized();
    bool hidden = iconic || isRollup() || !visibleNow();

    MSG(("updateState: winState=0x%X, client=%d", fWinState, !hidden));

    client()->setFrameState(iconic ? IconicState : NormalState);

    if (hidden) {
        setVisible(isRollup() && visibleNow());
        container()->hide();
        client()->hide();

        if (fDelayFocusTimer)
            fDelayFocusTimer->disableTimerListener(this);
        if (fAutoRaiseTimer)
            fAutoRaiseTimer->disableTimerListener(this);
    }
    else {
        client()->show();
        container()->show();
        show();
    }
}

bool YFrameWindow::affectsWorkArea() const {
    bool affects = ((fHaveStruts || doNotCover() ||
                     getActiveLayer() == WinLayerDock) &&
                    client() != taskBar);
    return affects;
}

bool YFrameWindow::inWorkArea() const {
    if (doNotCover())
        return false;
    if (isFullscreen())
        return false;
    if (getActiveLayer() >= WinLayerDock) {
        if (getActiveLayer() != WinLayerFullscreen)
            return false;
    }
    return !fHaveStruts;
}

void YFrameWindow::getNormalGeometryInner(int *x, int *y, int *w, int *h) const {
    XSizeHints *sh = client()->sizeHints();
    *x = normalX;
    *y = normalY;
    *w = sh ? normalW * max(1, sh->width_inc) + sh->base_width : normalW;
    *h = sh ? normalH * max(1, sh->height_inc) + sh->base_height : normalH;
}

void YFrameWindow::setNormalGeometryOuter(int ox, int oy, int ow, int oh) {
    int ix = ox + borderXN();
    int iy = oy + borderYN();
    int iw = ow - (2 * borderXN());
    int ih = oh - (2 * borderYN() + titleYN());
    setNormalGeometryInner(ix, iy, iw, ih);
}

void YFrameWindow::setNormalPositionOuter(int x, int y) {
    XSizeHints *sh = client()->sizeHints();
    x += borderXN();
    y += borderYN();
    int w = sh ? normalW * max(1, sh->width_inc) + sh->base_width : normalW;
    int h = sh ? normalH * max(1, sh->height_inc) + sh->base_height : normalH;
    setNormalGeometryInner(x, y, w, h);
}

void YFrameWindow::setNormalGeometryInner(int x, int y, int w, int h) {
    XSizeHints *sh = client()->sizeHints();
    normalX = x;
    normalY = y;
    normalW = sh ? (w - sh->base_width) / max(1, sh->width_inc) : w;
    normalH = sh ? (h - sh->base_height) / max(1, sh->height_inc) : h ;

    updateDerivedSize(getState() & WinStateMaximizedBoth);
    updateLayout();
}

void YFrameWindow::updateDerivedSize(int flagmask) {
    XSizeHints *sh = client()->sizeHints();

    int nx = normalX;
    int ny = normalY;
    int nw = sh ? normalW * max(1, sh->width_inc) + sh->base_width : normalW;
    int nh = sh ? normalH * max(1, sh->height_inc) + sh->base_height : normalH;

    int xiscreen = desktop->getScreenForRect(nx, ny, nw, nh);
    int mx, my, Mx, My;
    manager->getWorkArea(this, &mx, &my, &Mx, &My, xiscreen);
    int Mw = Mx - mx;
    int Mh = My - my;

    Mh -= titleYN();

    if (true) { // aspect of maximization
        int aMw, aMh;
        aMw = Mw;
        aMh = Mh;

        client()->constrainSize(aMw, aMh, YFrameClient::csKeepX);
        if (aMh < Mh) {
            Mh = aMh;
        } else {
            client()->constrainSize(aMw, aMh, YFrameClient::csKeepY);
            if (aMw < Mw) {
                Mw = aMw;
            }
        }
    }

    bool horiz = false;
    bool vert = false;

    if (isMaximizedHoriz()) {
        nw = Mw;
        if (considerHorizBorder) {
            nw -= 2 * borderXN();
        }
        horiz = true;
    }

    if (isMaximizedVert()) {
        nh = Mh;
        if (considerVertBorder) {
            nh -= 2 * borderYN();
        }
        vert = true;
    }

    if (horiz || vert) {
        client()->constrainSize(nw, nh, ///getLayer(),
                                (nw >= Mw) ? YFrameClient::csKeepY
                                : YFrameClient::csKeepX);
    }
    nw += 2 * borderXN();
    nh += 2 * borderYN();

    if (isFullscreen() || (flagmask & WinStateMinimized))
        horiz = vert = false;

    if (horiz) {
        int cx = mx;

        if (centerMaximizedWindows && !(sh && (sh->flags & PMaxSize)))
            cx = mx + (Mw - nw) / 2;
        else if (!considerHorizBorder)
            cx -= borderXN();
        if (flagmask & WinStateMaximizedHoriz || isMaximizedHoriz())
            nx = cx;
        else
            nx = x();
    } else {
        nx -= borderXN();
    }
    if (vert) {
        int cy = my;

        if (centerMaximizedWindows && !(sh && (sh->flags & PMaxSize)))
            cy = my + (Mh - nh) / 2;
        else if (!considerVertBorder)
            cy -= borderYN();

        if (flagmask & WinStateMaximizedVert || isMaximizedVert())
            ny = cy;
        else
            ny = y();
    } else {
        ny -= borderYN();
    }

    bool cx = true;
    bool cy = true;
    bool cw = true;
    bool ch = true;

    if (isFullscreen()) {
        cy = ch = false;
        cx = cw = false;
    }

    if (isMaximizedVert() && !vert)
        cy = ch = false;
    if (isMaximizedHoriz() && !horiz)
        cx = cw = false;

    if (cx)
        posX = nx;
    if (cy)
        posY = ny;
    if (cw)
        posW = nw;
    if (ch)
        posH = nh + titleYN();
}

void YFrameWindow::updateNormalSize() {
    MSG(("updateNormalSize: %d %d %d %d", normalX, normalY, normalW, normalH));
    XSizeHints *sh = client()->sizeHints();

    bool cx = true;
    bool cy = true;
    bool cw = true;
    bool ch = true;

    if (isFullscreen())
        cy = ch = cx = cw = false;
    if (isMaximizedHoriz())
        cx = cw = false;
    if (isMaximizedVert())
        cy = ch = false;
    if (isRollup())
        ch = false;

    if (cx)
        normalX = posX + borderXN();
    if (cy)
        normalY = posY + borderYN();
    if (cw) {
        normalW = posW - 2 * borderXN();
        if (sh) {
            normalW = (normalW - sh->base_width) / max(1, sh->width_inc);
        }
    }
    if (ch) {
        normalH = posH - (2 * borderYN() + titleYN());
        if (sh) {
            normalH = (normalH - sh->base_height) / max(1, sh->height_inc);
        }
    }
    MSG(("updateNormalSize> %d %d %d %d", normalX, normalY, normalW, normalH));
}

void YFrameWindow::setCurrentGeometryOuter(YRect newSize) {
    MSG(("setCurrentGeometryOuter: %d %d %d %d",
         newSize.x(), newSize.y(), newSize.width(), newSize.height()));
    setWindowGeometry(newSize);

    if ( ! isFullscreen()) {
        posX = x();
        posY = y();
        posW = width();
    }
    if ( ! hasState(WinStateFullscreen | WinStateRollup)) {
        posH = height();
    }

    updateNormalSize();
    MSG(("setCurrentGeometryOuter> %d %d %d %d",
         posX, posY, posW, posH));
}

void YFrameWindow::setCurrentPositionOuter(int px, int py) {
    if (px != x() || py != y()) {
        YWindow::setPosition(px, py);
        if ( ! isFullscreen()) {
            posX = px;
            posY = py;
            if ( !isIconic() && !isMaximizedHoriz())
                normalX = posX + borderXN();
            if ( !isIconic() && !isMaximizedVert())
                normalY = posY + borderYN();
        }
        sendConfigure();
    }
}

void YFrameWindow::updateIconPosition() {
    if (fMiniIcon) {
        if (minimizeToDesktop && isMinimized()) {
            fMiniIcon->hide();
            fMiniIcon->setPosition(-1, -1);
            fMiniIcon->show();
        }
        else {
            delete fMiniIcon;
            fMiniIcon = nullptr;
        }
    }
}

void YFrameWindow::updateLayout() {
    if (isIconic()) {
        fMiniIcon->show();
    }
    else if (isRollup()) {
        setWindowGeometry(YRect(posX, posY, posW, 2 * borderY() + titleY()));
    }
    else if (isFullscreen()) {
        // for _NET_WM_FULLSCREEN_MONITORS
        const int limit = desktop->getScreenCount() - 1;
        if (inrange(fFullscreenMonitorsTop,    0, limit) &&
            inrange(fFullscreenMonitorsBottom, 0, limit) &&
            inrange(fFullscreenMonitorsLeft,   0, limit) &&
            inrange(fFullscreenMonitorsRight,  0, limit))
        {
            YRect t(desktop->getScreenGeometry(fFullscreenMonitorsTop));
            YRect b(desktop->getScreenGeometry(fFullscreenMonitorsBottom));
            YRect l(desktop->getScreenGeometry(fFullscreenMonitorsLeft));
            YRect r(desktop->getScreenGeometry(fFullscreenMonitorsRight));
            int x = l.xx;
            int y = t.yy;
            int w = r.xx + int(r.ww) > x ? r.xx + int(r.ww) - x : int(l.ww);
            int h = b.yy + int(b.hh) > y ? b.yy + int(b.hh) - y : int(t.hh);
            setWindowGeometry(YRect(x, y, w, h));
        }
        else if (fullscreenUseAllMonitors) {
            YRect geo(desktop->getScreenGeometry(0));
            for (int screen = 1; screen <= limit; ++screen) {
                geo += desktop->getScreenGeometry(screen);
            }
            setWindowGeometry(geo);
        }
        else {
            setWindowGeometry(desktop->getScreenGeometry(getScreen()));
        }
    }
    else {
        MSG(("updateLayout %d %d %d %d", posX, posY, posW, posH));
        setWindowGeometry(YRect(posX, posY, posW, posH));
    }
    if (affectsWorkArea())
        manager->updateWorkArea();
}

void YFrameWindow::setState(int mask, int state) {
    int fOldState = fWinState;
    int fNewState = (fWinState & ~mask) | (state & mask);
    int deltaState = fOldState ^ fNewState;
    fWinState = fNewState;

    MSG(("setState: oldState: 0x%X, newState: 0x%X, mask: 0x%X, state: 0x%X",
         fOldState, fNewState, mask, state));
    //msg("normal1: (%d:%d %dx%d)", normalX, normalY, normalWidth, normalHeight);
    if (deltaState & WinStateMinimized) {
        MSG(("WinStateMinimized: %d", isMinimized()));
        if (fNewState & WinStateMinimized)
            minimizeTransients();
        else if (owner() && owner()->isMinimized())
            owner()->setState(WinStateMinimized, 0);
    }
    if (deltaState & WinStateHidden) {
        MSG(("WinStateHidden: %d", isHidden()));
        if (fNewState & WinStateHidden)
            hideTransients();
        else if (owner() && owner()->isHidden())
            owner()->setState(WinStateHidden, 0);
    }

    manager->lockWorkArea();
    updateDerivedSize(deltaState);
    updateLayout();
    updateState();
    updateLayer();
    if (hasbit(fOldState | fNewState, WinStateFullscreen) ||
        manager->top(WinLayerFullscreen))
    {
        manager->updateFullscreenLayer();
    }
    manager->unlockWorkArea();

    if (hasbit(deltaState, WinStateRollup | WinStateMinimized)) {
        setShape();
    }
    if (deltaState & fOldState & WinStateMinimized) {
        restoreMinimizedTransients();
    }
    if (deltaState & fOldState & WinStateHidden) {
        restoreHiddenTransients();
    }
    if ((deltaState & WinStateRollup) &&
        (clickFocus || !strongPointerFocus) &&
        this == manager->getFocus()) {
        manager->setFocus(this);
    }
    if (deltaState & fNewState & WinStateFullscreen) {
        if (notbit(deltaState & fNewState, WinStateUnmapped)) {
            activate();
        }
    }
    if (deltaState & fNewState & WinStateFocused) {
        if (this != manager->getFocus())
            manager->setFocus(this);
    }

    if (deltaState & WinStateUrgent) {
        if (notbit(fNewState, WinStateUrgent) && client()->urgencyHint()) {
            client()->hints()->flags &= ~XUrgencyHint;
        }
        updateTaskBar();
    }
    if (deltaState & WinStateModal) {
        if (notbit(fNewState, WinStateModal)) {
            for (int i = groupModals.getCount() - 1; 0 <= i; --i) {
                if (groupModals[i] == this) {
                    groupModals.remove(i);
                }
            }
        }
    }
    if (hasbit(deltaState, WinStateMinimized) && minimizeToDesktop) {
        if (isMinimized()) {
            if (getMiniIcon()) {
                fMiniIcon->show();
            }
        }
        else if (fMiniIcon) {
            fMiniIcon->hide();
        }
    }
    if (hasbit(deltaState, WinStateMaximizedBoth) && fTitleBar) {
        YFrameButton* maxi = fTitleBar->maximizeButton();
        if (maxi) {
            maxi->setKind(YFrameTitleBar::Maxi);
            maxi->repaint();
        }
    }
    if (hasbit(deltaState, WinStateRollup) && fTitleBar) {
        YFrameButton* rollup = fTitleBar->rollupButton();
        if (rollup) {
            rollup->setKind(YFrameTitleBar::Roll);
            rollup->repaint();
        }
    }
    if (hasbit(deltaState,
               WinStateMinimized | WinStateHidden | WinStateSkipTaskBar))
    {
        updateTaskBar();
    }
    if (hasbit(deltaState, WinStateUnmapped)) {
        layoutResizeIndicators();
        if (taskBar)
            taskBar->workspacesRepaint();
    }
}

void YFrameWindow::setAllWorkspaces() {
    if ( ! isAllWorkspaces()) {
        setWorkspace(AllWorkspaces);

        if (affectsWorkArea())
            manager->updateWorkArea();
        if (taskBar)
            taskBar->relayoutTasks();
        if (taskBar)
            taskBar->relayoutTray();
    }
}

bool YFrameWindow::visibleNow() const {
    return visibleOn(manager->activeWorkspace());
}

#if DO_NOT_COVER_OLD
void YFrameWindow::setDoNotCover(bool doNotCover) {
    fWinOptionMask &= ~foDoNotCover;

    if (doNotCover) {
        fFrameOptions |= foDoNotCover;
    } else {
        fFrameOptions &= ~foDoNotCover;
    }
    manager->updateWorkArea();
}
#endif

void YFrameWindow::updateMwmHints(XSizeHints* sh) {
    YDimension old(dimension());
    getFrameHints();
    int nwidth = sh ? normalW * max(1, sh->width_inc) + sh->base_width
                    : client()->width();
    int height = sh ? normalH * max(1, sh->height_inc) + sh->base_height
                    : client()->height();
    setNormalGeometryInner(normalX, normalY, nwidth, height);
    if (old == dimension()) {
        performLayout();
    }
}

ref<YIcon> YFrameWindow::clientIcon() const {
    for (YFrameWindow const *f(this); f != nullptr; f = f->owner())
        if (f->getClientIcon() != null)
            return f->getClientIcon();

    return wmapp->getDefaultAppIcon();
}

void YFrameWindow::updateProperties() {
    client()->setWorkspaceHint(fWinWorkspace);
    client()->setLayerHint(fWinActiveLayer);
    client()->setStateHint();
}

void YFrameWindow::updateTaskBar() {
    if (taskBar && fManaged) {
        taskBar->updateFrame(this);
    }
}

void YFrameWindow::updateAppStatus() {
    if (taskBar && fManaged) {
        bool needTrayApp(false);

        if (!isHidden() &&
            (notbit(frameOptions(), foIgnoreTaskBar) || isMinimized()) &&
            (getTrayOption() != WinTrayIgnore))
            if (trayShowAllWindows || visibleNow())
                needTrayApp = true;

        if (needTrayApp && fTrayApp == nullptr)
            fTrayApp = taskBar->addTrayApp(this);

        if (fTrayApp) {
            fTrayApp->setShown(needTrayApp);
            if (fTrayApp->getShown()) ///!!! optimize
                fTrayApp->repaint();
        }
        taskBar->relayoutTray();

        bool needTaskBarApp = true;
        bool grouping = false;

        if (isSkipTaskBar())
            needTaskBarApp = false;
        if (isHidden())
            needTaskBarApp = false;
        if (getTrayOption() == WinTrayExclusive)
            needTaskBarApp = false;
        if (getTrayOption() == WinTrayMinimized && isMinimized())
            needTaskBarApp = false;
        if (owner() != nullptr && !taskBarShowTransientWindows)
            needTaskBarApp = false;
        if (!visibleNow() && !taskBarShowAllWindows) {
            grouping = bool(taskBarTaskGrouping);
            needTaskBarApp = false;
        }
        if (isUrgent())
            needTaskBarApp = true;

        if (frameOption(foIgnoreTaskBar))
            needTaskBarApp = false;
        if (frameOption(foNoIgnoreTaskBar))
            needTaskBarApp = true;

        if ((needTaskBarApp || grouping) && fTaskBarApp == nullptr)
            fTaskBarApp = taskBar->addTasksApp(this);

        if (fTaskBarApp) {
            fTaskBarApp->setFlash(isUrgent());
            fTaskBarApp->setShown(needTaskBarApp);
            if (fTaskBarApp->getShown()) ///!!! optimize
                fTaskBarApp->repaint();
        }
        taskBar->relayoutTasks();
    }
}

void YFrameWindow::removeAppStatus() {
    if (taskBar) {
        taskBar->delistFrame(this, fTaskBarApp, fTrayApp);
        fTaskBarApp = nullptr;
        fTrayApp = nullptr;
    }
}

void YFrameWindow::handleMsgBox(YMsgBox *msgbox, int operation) {
    //msg("msgbox operation %d", operation);
    if (msgbox == fKillMsgBox) {
        msgbox->unmanage();
        fKillMsgBox = nullptr;
        if (operation == YMsgBox::mbOK && !client()->destroyed()) {
            if ( !client()->timedOut() || !client()->killPid()) {
                wmKill();
            }
        }
    }
}

void YFrameWindow::updateNetWMStrut() {
    int l = fStrutLeft;
    int r = fStrutRight;
    int t = fStrutTop;
    int b = fStrutBottom;
    client()->getNetWMStrut(&l, &r, &t, &b);
    if (l != fStrutLeft ||
        r != fStrutRight ||
        t != fStrutTop ||
        b != fStrutBottom)
    {
        fStrutLeft = l;
        fStrutRight = r;
        fStrutTop = t;
        fStrutBottom = b;
        fHaveStruts = l | r | t | b;
        MSG(("strut: %d %d %d %d", l, r, t, b));
        manager->updateWorkArea();
    }
}

void YFrameWindow::updateNetWMStrutPartial() {
    int l = fStrutLeft;
    int r = fStrutRight;
    int t = fStrutTop;
    int b = fStrutBottom;
    client()->getNetWMStrutPartial(&l, &r, &t, &b);
    if (l != fStrutLeft ||
        r != fStrutRight ||
        t != fStrutTop ||
        b != fStrutBottom)
    {
        fStrutLeft = l;
        fStrutRight = r;
        fStrutTop = t;
        fStrutBottom = b;
        fHaveStruts = l | r | t | b;
        MSG(("strut: %d %d %d %d", l, r, t, b));
        manager->updateWorkArea();
    }
}

void YFrameWindow::updateNetStartupId() {
    unsigned long time = (unsigned long) -1;
    if (client()->getNetStartupId(time)) {
        if (fUserTime.update(time))
            manager->updateUserTime(fUserTime);
    }
}

void YFrameWindow::updateNetWMUserTime() {
    unsigned long time = (unsigned long) -1;
    Window window = fUserTimeWindow ? fUserTimeWindow : client()->handle();
    if (client()->getNetWMUserTime(window, time)) {
        if (fUserTime.update(time))
            manager->updateUserTime(fUserTime);
    }
}

void YFrameWindow::updateNetWMUserTimeWindow() {
    Window window = fUserTimeWindow;
    if (client()->getNetWMUserTimeWindow(window) && window != fUserTimeWindow) {
        if (fUserTimeWindow != None) {
            windowContext.remove(fUserTimeWindow);
        }
        fUserTimeWindow = window;
        if (window != None) {
            windowContext.save(window, client());
            XWindowAttributes wa;
            if (XGetWindowAttributes(xapp->display(), window, &wa))
                XSelectInput(xapp->display(), window,
                             wa.your_event_mask | PropertyChangeMask);
        }
        updateNetWMUserTime();
    }
}

void YFrameWindow::updateNetWMWindowOpacity() {
    long data[1] = { 0, };
    if (client()->getNetWMWindowOpacity(data[0]))
        XChangeProperty(xapp->display(), handle(),
                _XA_NET_WM_WINDOW_OPACITY, XA_CARDINAL,
                32, PropModeReplace,
                (unsigned char *) data, 1);
    else
        XDeleteProperty(xapp->display(), handle(), _XA_NET_WM_WINDOW_OPACITY);
}

void YFrameWindow::updateNetWMFullscreenMonitors(int t, int b, int l, int r) {
    if (t != fFullscreenMonitorsTop ||
        b != fFullscreenMonitorsBottom ||
        l != fFullscreenMonitorsLeft ||
        r != fFullscreenMonitorsRight)
    {
        MSG(("fullscreen monitors: %d %d %d %d", t, b, l, r));
        fFullscreenMonitorsTop = t;
        fFullscreenMonitorsBottom = b;
        fFullscreenMonitorsLeft = l;
        fFullscreenMonitorsRight = r;
        if (isFullscreen())
            updateLayout();
    }
}

void YFrameWindow::setWmUrgency(bool wmUrgency) {
    if ( !frameOption(foIgnoreUrgent)) {
        if (wmUrgency != hasState(WinStateUrgent)) {
            fWinState ^= WinStateUrgent;
            client()->setStateHint();
            updateTaskBar();
        }
    }
}

bool YFrameWindow::isUrgent() const {
    return hasState(WinStateUrgent) || client()->urgencyHint();
}

int YFrameWindow::getScreen() const {
    int nx, ny, nw, nh;
    getNormalGeometryInner(&nx, &ny, &nw, &nh);
    return desktop->getScreenForRect(nx, ny, nw, nh);
}

void YFrameWindow::wmArrange(int tcb, int lcr) {
    int mx, my, Mx, My, newX = 0, newY = 0;

    int xiscreen = desktop->getScreenForRect(x(), y(), width(), height());

    manager->getWorkArea(this, &mx, &my, &Mx, &My, xiscreen);

    switch (tcb) {
    case waTop:
        newY = my - (considerVertBorder ? 0 : borderY());
        break;
    case waCenter:
        newY = my + ((My - my) >> 1) - (height() >> 1) - (considerVertBorder ? 0 : borderY());
        break;
    case waBottom:
        newY = My - height() + (considerVertBorder ? 0 : borderY());
        break;
    }

    switch (lcr) {
    case waLeft:
        newX = mx - (considerHorizBorder ? 0 : borderX());
        break;
    case waCenter:
        newX = mx + ((Mx - mx) >> 1) - ((width()) >> 1) - (considerHorizBorder ? 0 : borderX());
        break;
    case waRight:
        newX = Mx - width() + (considerHorizBorder ? 0 : borderX());
        break;
    }

    MSG(("wmArrange: setPosition(x = %d, y = %d)", newX, newY));

    setCurrentPositionOuter(newX, newY);
}

void YFrameWindow::wmSnapMove(int tcb, int lcr) {
   int mx, my, Mx, My, newX = 0, newY = 0;

    int xiscreen = desktop->getScreenForRect(x(), y(), width(), height());

    YFrameWindow **w = nullptr;
    int count = 0;

    manager->getWindowsToArrange(&w, &count);

    manager->getWorkArea(this, &mx, &my, &Mx, &My, xiscreen);

    MSG(("WorkArea: mx = %d, my = %d, Mx = %d, My = %d", mx, my, Mx, My));
    MSG(("thisFrame: x = %d, y = %d, w = %d, h = %d, bx = %d, by = %d, ty = %d",
         x(), y(), width(), height(), borderX(), borderY(), titleY()));
    MSG(("thisClient: w = %d, h = %d", client()->width(), client()->height()));

    switch (tcb) {
       case waTop:
           newY = getTopCoord(my, w, count);
           if (!considerVertBorder && newY == my)
               newY -= borderY();
           break;
       case waCenter:
           newY = y();
           break;
       case waBottom:
           newY = getBottomCoord(My, w, count) - height();
           if (!considerVertBorder && (newY + (int) height()) == My)
               newY += borderY();
           break;
    }

    switch (lcr) {
       case waLeft:
           newX = getLeftCoord(mx, w, count);
           if (!considerHorizBorder && newX == mx)
               newX -= borderX();
           break;
       case waCenter:
           newX = x();
           break;
       case waRight:
           newX = getRightCoord(Mx, w, count) - width();
           if (!considerHorizBorder && (newX + (int) width()) == Mx)
               newX += borderX();
           break;
    }

    MSG(("NewPosition: x = %d, y = %d", newX, newY));

    setCurrentPositionOuter(newX, newY);

    delete [] w;
}

int YFrameWindow::getTopCoord(int my, YFrameWindow **w, int count)
{
    int i, n;

    if (y() < my)
        return y();

    for (i = y() - 2; i > my; i--) {
        for (n = 0; n < count; n++) {
            if (    (this != w[n])
                 && (i == (w[n]->y() + (int) w[n]->height()))
                 && ( x()            < (w[n]->x() + (int) w[n]->width()))
                 && ((x() + (int) width()) > (w[n]->x())) ) {
                return i;
            }
        }
    }

    return my;
}

int YFrameWindow::getBottomCoord(int My, YFrameWindow **w, int count)
{
    int i, n;

    if ((y() + (int) height()) > My)
        return y() + height();

    for (i = y() + height() + 2; i < My; i++) {
        for (n = 0; n < count; n++) {
            if (    (this != w[n])
                 && (i == w[n]->y())
                 && ( x()            < (w[n]->x() + (int) w[n]->width()))
                 && ((x() + (int) width()) > (w[n]->x())) ) {
                return i;
            }
        }
    }

    return My;
}

int YFrameWindow::getLeftCoord(int mx, YFrameWindow **w, int count)
{
    int i, n;

    if (x() < mx)
        return x();

    for (i = x() - 2; i > mx; i--) {
        for (n = 0; n < count; n++) {
            if (    (this != w[n])
                 && (i == (w[n]->x() + (int) w[n]->width()))
                 && ( y()             < (w[n]->y() + (int) w[n]->height()))
                 && ((y() + (int) height()) > (w[n]->y())) ) {
                return i;
            }
        }
    }

    return mx;
}

int YFrameWindow::getRightCoord(int Mx, YFrameWindow **w, int count)
{
    int i, n;

    if ((x() + (int) width()) > Mx)
        return x() + width();

    for (i = x() + width() + 2; i < Mx; i++) {
        for (n = 0; n < count; n++) {
            if (    (this != w[n])
                 && (i == w[n]->x())
                 && ( y()             < (w[n]->y() + (int) w[n]->height()))
                 && ((y() + (int) height()) > (w[n]->y())) ) {
                return i;
            }
        }
    }

    return Mx;
}

// vim: set sw=4 ts=4 et:
