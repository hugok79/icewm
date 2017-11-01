#ifndef __SWITCH_H
#define __SWITCH_H

#include "base.h"
#include "ymenu.h"

class YFrameWindow;
class YWindowManager;

// XXX: this only exists because there are no lambda functions
class IClosablePopup
{
public:
    // report true if window was up; close in any case
    virtual bool close()=0;
    virtual ~IClosablePopup(){};
};

// data model, default implementation is list of windows
//class ISwitchItems;
class ISwitchItems
{
public:
    virtual void updateList() =0;
    virtual int getCount() =0;
    virtual ~ISwitchItems() {}

    // move the focused target up or down and return the new focused element
    virtual int moveTarget(bool zdown)=0;
    /// Show changed focus preview to user
    virtual void displayFocusChange(int idxFocused)=0;

    // set target cursor and implementation specific stuff in the beginning
    virtual void begin(bool zdown)=0;
    virtual void cancel()=0;
    virtual void accept(IClosablePopup *parent)=0;

    // XXX: convert to iterator
    virtual int getActiveItem()=0;
    virtual ustring getTitle(int idx) =0;
    virtual ref<YIcon> getIcon(int idx) =0;

    // Manager notification about windows disappearing under the fingers
    virtual void destroyedItem(void* framePtr) =0;

    virtual bool isKey(KeySym k, unsigned int mod) =0;
};

class SwitchWindow: public YPopupWindow, IClosablePopup {
    ISwitchItems* zItems;
    bool m_verticalStyle;
public:
    SwitchWindow(YWindow *parent = 0,
                 ISwitchItems *items = 0, bool verticalStyle=true);
    virtual ~SwitchWindow();

    virtual void paint(Graphics &g, const YRect &r);

    void begin(bool zdown, int mods);

    virtual void activatePopup(int flags);
    virtual void deactivatePopup();

    virtual bool handleKey(const XKeyEvent &key);
    virtual void handleButton(const XButtonEvent &button);

    void destroyedFrame(YFrameWindow *frame);

private:

#ifdef CONFIG_GRADIENTS
    ref<YImage> fGradient;
#endif

    static YColor *switchFg;
    static YColor *switchBg;
    static YColor *switchHl;
    static YColor *switchMbg;
    static YColor *switchMfg;
    static ref<YFont> switchFont;

    int modsDown;

    bool isUp;

    bool modDown(int m);
    bool isModKey(KeyCode c);
    void resize(int xiscreen);

    void cancel();
    bool close();
    void accept();
    void displayFocus(int itemIdx);
    //YFrameWindow *nextWindow(YFrameWindow *from, bool zdown, bool next);
    YFrameWindow *nextWindow(bool zdown);

private: // not-used
    SwitchWindow(const SwitchWindow &);
    SwitchWindow &operator=(const SwitchWindow &);
};

#endif

// vim: set sw=4 ts=4 et:
