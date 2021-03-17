//
// vncflinger - Copyright (C) 2021 Stefanie Kondik
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#define LOG_TAG "VNC-InputDevice"
#include <utils/Log.h>

#include <future>

#include "InputDevice.h"

#include <fcntl.h>
#include <stdio.h>

#include <sys/ioctl.h>

#include <linux/input.h>
#include <linux/uinput.h>

using namespace android;


static const struct UInputOptions {
    int cmd;
    int bit;
} kOptions[] = {
    {UI_SET_EVBIT, EV_KEY},
    {UI_SET_EVBIT, EV_REP},
    {UI_SET_EVBIT, EV_REL},
    {UI_SET_RELBIT, REL_X},
    {UI_SET_RELBIT, REL_Y},
    {UI_SET_RELBIT, REL_WHEEL},
    {UI_SET_EVBIT, EV_ABS},
    {UI_SET_ABSBIT, ABS_X},
    {UI_SET_ABSBIT, ABS_Y},
    {UI_SET_EVBIT, EV_SYN},
    {UI_SET_PROPBIT, INPUT_PROP_DIRECT},
};

status_t InputDevice::start_async(uint32_t width, uint32_t height) {
    // don't block the caller since this can take a few seconds
    std::async(&InputDevice::start, this, width, height);

    return NO_ERROR;
}

status_t InputDevice::start(uint32_t width, uint32_t height) {
    Mutex::Autolock _l(mLock);

    mLeftClicked = mMiddleClicked = mRightClicked = false;

    struct input_id id = {
        BUS_VIRTUAL, /* Bus type */
        1,           /* Vendor */
        1,           /* Product */
        4,           /* Version */
    };

    if (mFD >= 0) {
        ALOGE("Input device already open!");
        return NO_INIT;
    }

    mFD = open(UINPUT_DEVICE, O_WRONLY | O_NONBLOCK);
    if (mFD < 0) {
        ALOGE("Failed to open %s: err=%d", UINPUT_DEVICE, mFD);
        return NO_INIT;
    }

    unsigned int idx = 0;
    for (idx = 0; idx < sizeof(kOptions) / sizeof(kOptions[0]); idx++) {
        if (ioctl(mFD, kOptions[idx].cmd, kOptions[idx].bit) < 0) {
            ALOGE("uinput ioctl failed: %d %d", kOptions[idx].cmd, kOptions[idx].bit);
            goto err_ioctl;
        }
    }

    for (idx = 0; idx < KEY_MAX; idx++) {
        if (ioctl(mFD, UI_SET_KEYBIT, idx) < 0) {
            ALOGE("UI_SET_KEYBIT failed");
            goto err_ioctl;
        }
    }

    memset(&mUserDev, 0, sizeof(mUserDev));
    strncpy(mUserDev.name, "VNC-RemoteInput", UINPUT_MAX_NAME_SIZE);

    mUserDev.id = id;

    mUserDev.absmin[ABS_X] = 0;
    mUserDev.absmax[ABS_X] = width;
    mUserDev.absmin[ABS_Y] = 0;
    mUserDev.absmax[ABS_Y] = height;

    if (write(mFD, &mUserDev, sizeof(mUserDev)) != sizeof(mUserDev)) {
        ALOGE("Failed to configure uinput device");
        goto err_ioctl;
    }

    if (ioctl(mFD, UI_DEV_CREATE) == -1) {
        ALOGE("UI_DEV_CREATE failed");
        goto err_ioctl;
    }

    mOpened = true;

    ALOGD("Virtual input device created successfully (%dx%d)", width, height);
    return NO_ERROR;

err_ioctl:
    int prev_errno = errno;
    ::close(mFD);
    errno = prev_errno;
    mFD = -1;
    return NO_INIT;
}

status_t InputDevice::reconfigure(uint32_t width, uint32_t height) {
    stop();
    return start_async(width, height);
}

status_t InputDevice::stop() {
    Mutex::Autolock _l(mLock);

    mOpened = false;

    if (mFD < 0) {
        return OK;
    }

    ioctl(mFD, UI_DEV_DESTROY);
    close(mFD);
    mFD = -1;

    return OK;
}

status_t InputDevice::inject(uint16_t type, uint16_t code, int32_t value) {
    struct input_event event;
    memset(&event, 0, sizeof(event));
    gettimeofday(&event.time, 0); /* This should not be able to fail ever.. */
    event.type = type;
    event.code = code;
    event.value = value;
    if (write(mFD, &event, sizeof(event)) != sizeof(event)) return BAD_VALUE;
    return OK;
}

status_t InputDevice::injectSyn(uint16_t type, uint16_t code, int32_t value) {
    if (inject(type, code, value) != OK) {
        return BAD_VALUE;
    }
    return inject(EV_SYN, SYN_REPORT, 0);
}

status_t InputDevice::movePointer(int32_t x, int32_t y) {
    if (inject(EV_REL, REL_X, x) != OK) {
        return BAD_VALUE;
    }
    return injectSyn(EV_REL, REL_Y, y);
}

status_t InputDevice::setPointer(int32_t x, int32_t y) {
    if (inject(EV_ABS, ABS_X, x) != OK) {
        return BAD_VALUE;
    }
    return injectSyn(EV_ABS, ABS_Y, y);
}

status_t InputDevice::press(uint16_t code) {
    return inject(EV_KEY, code, 1);
}

status_t InputDevice::release(uint16_t code) {
    return inject(EV_KEY, code, 0);
}

status_t InputDevice::click(uint16_t code) {
    if (press(code) != OK) {
        return BAD_VALUE;
    }
    return release(code);
}

void InputDevice::keyEvent(bool down, uint32_t key) {
    int code;
    int sh = 0;
    int alt = 0;

    Mutex::Autolock _l(mLock);
    if (!mOpened) return;

    if ((code = keysym2scancode(key, &sh, &alt))) {
        int ret = 0;

        if (key && down) {
            if (sh) press(42);   // left shift
            if (alt) press(56);  // left alt

            inject(EV_SYN, SYN_REPORT, 0);

            ret = press(code);
            if (ret != 0) {
                ALOGE("Error: %d (%s)\n", errno, strerror(errno));
            }

            inject(EV_SYN, SYN_REPORT, 0);

            ret = release(code);
            if (ret != 0) {
                ALOGE("Error: %d (%s)\n", errno, strerror(errno));
            }

            inject(EV_SYN, SYN_REPORT, 0);

            if (alt) release(56);  // left alt
            if (sh) release(42);   // left shift

            inject(EV_SYN, SYN_REPORT, 0);
        }
    }
}

void InputDevice::pointerEvent(int buttonMask, int x, int y) {
    Mutex::Autolock _l(mLock);
    if (!mOpened) return;

    ALOGV("pointerEvent: buttonMask=%x x=%d y=%d", buttonMask, x, y);

    if ((buttonMask & 1) && mLeftClicked) {  // left btn clicked and moving
        inject(EV_ABS, ABS_X, x);
        inject(EV_ABS, ABS_Y, y);
        inject(EV_SYN, SYN_REPORT, 0);

    } else if (buttonMask & 1) {  // left btn clicked
        mLeftClicked = true;

        inject(EV_ABS, ABS_X, x);
        inject(EV_ABS, ABS_Y, y);
        inject(EV_KEY, BTN_TOUCH, 1);
        inject(EV_SYN, SYN_REPORT, 0);
    } else if (mLeftClicked)  // left btn released
    {
        mLeftClicked = false;
        inject(EV_ABS, ABS_X, x);
        inject(EV_ABS, ABS_Y, y);
        inject(EV_KEY, BTN_TOUCH, 0);
        inject(EV_SYN, SYN_REPORT, 0);
    }

    if (buttonMask & 4)  // right btn clicked
    {
        mRightClicked = true;
        press(158);  // back key
        inject(EV_SYN, SYN_REPORT, 0);
    } else if (mRightClicked)  // right button released
    {
        mRightClicked = false;
        release(158);
        inject(EV_SYN, SYN_REPORT, 0);
    }

    if (buttonMask & 2)  // mid btn clicked
    {
        mMiddleClicked = true;
        press(KEY_END);
        inject(EV_SYN, SYN_REPORT, 0);
    } else if (mMiddleClicked)  // mid btn released
    {
        mMiddleClicked = false;
        release(KEY_END);
        inject(EV_SYN, SYN_REPORT, 0);
    }

    if (buttonMask & 8) {
        inject(EV_REL, REL_WHEEL, 1);
        inject(EV_SYN, SYN_REPORT, 0);
    }

    if (buttonMask & 0x10) {
        inject(EV_REL, REL_WHEEL, -1);
        inject(EV_SYN, SYN_REPORT, 0);
    }
}

// q,w,e,r,t,y,u,i,o,p,a,s,d,f,g,h,j,k,l,z,x,c,v,b,n,m
static const int qwerty[] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                             49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
// ,!,",#,$,%,&,',(,),*,+,,,-,.,/
static const int spec1[] = {57, 2, 40, 4, 5, 6, 8, 40, 10, 11, 9, 13, 51, 12, 52, 52};
static const int spec1sh[] = {0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1};
// :,;,<,=,>,?,@
static const int spec2[] = {39, 39, 227, 13, 228, 53, 3};
static const int spec2sh[] = {1, 0, 1, 1, 1, 1, 1};
// [,\,],^,_,`
static const int spec3[] = {26, 43, 27, 7, 12, 399};
static const int spec3sh[] = {0, 0, 0, 1, 1, 0};
// {,|,},~
static const int spec4[] = {26, 43, 27, 215, 14};
static const int spec4sh[] = {1, 1, 1, 1, 0};

int InputDevice::keysym2scancode(uint32_t c, int* sh, int* alt) {
    int real = 1;
    if ('a' <= c && c <= 'z') return qwerty[c - 'a'];
    if ('A' <= c && c <= 'Z') {
        (*sh) = 1;
        return qwerty[c - 'A'];
    }
    if ('1' <= c && c <= '9') return c - '1' + 2;
    if (c == '0') return 11;
    if (32 <= c && c <= 47) {
        (*sh) = spec1sh[c - 32];
        return spec1[c - 32];
    }
    if (58 <= c && c <= 64) {
        (*sh) = spec2sh[c - 58];
        return spec2[c - 58];
    }
    if (91 <= c && c <= 96) {
        (*sh) = spec3sh[c - 91];
        return spec3[c - 91];
    }
    if (123 <= c && c <= 127) {
        (*sh) = spec4sh[c - 123];
        return spec4[c - 123];
    }
    switch (c) {
        case 0xff08:
            return 14;  // backspace
        case 0xff09:
            return 15;  // tab
        case 1:
            (*alt) = 1;
            return 34;  // ctrl+a
        case 3:
            (*alt) = 1;
            return 46;  // ctrl+c
        case 4:
            (*alt) = 1;
            return 32;  // ctrl+d
        case 18:
            (*alt) = 1;
            return 31;  // ctrl+r
        case 0xff0D:
            return 28;  // enter
        case 0xff1B:
            return 158;  // esc -> back
        case 0xFF51:
            return 105;  // left -> DPAD_LEFT
        case 0xFF53:
            return 106;  // right -> DPAD_RIGHT
        case 0xFF54:
            return 108;  // down -> DPAD_DOWN
        case 0xFF52:
            return 103;  // up -> DPAD_UP
        // case 360:
        //	return 232;// end -> DPAD_CENTER (ball click)
        case 0xff50:
            return KEY_HOME;  // home
        case 0xffff:
            return 158;  // del -> back
        case 0xff55:
            return 229;  // PgUp -> menu
        case 0xffcf:
            return 127;  // F2 -> search
        case 0xffe3:
            return 127;  // left ctrl -> search
        case 0xff56:
            return 61;  // PgUp -> call
        case 0xff57:
            return 107;  // End -> endcall
        case 0xffc2:
            return 211;  // F5 -> focus
        case 0xffc3:
            return 212;  // F6 -> camera
        case 0xffc4:
            return 150;  // F7 -> explorer
        case 0xffc5:
            return 155;  // F8 -> envelope

        case 50081:
        case 225:
            (*alt) = 1;
            if (real) return 48;  // a with acute
            return 30;            // a with acute -> a with ring above

        case 50049:
        case 193:
            (*sh) = 1;
            (*alt) = 1;
            if (real) return 48;  // A with acute
            return 30;            // A with acute -> a with ring above

        case 50089:
        case 233:
            (*alt) = 1;
            return 18;  // e with acute

        case 50057:
        case 201:
            (*sh) = 1;
            (*alt) = 1;
            return 18;  // E with acute

        case 50093:
        case 0xffbf:
            (*alt) = 1;
            if (real) return 36;  // i with acute
            return 23;            // i with acute -> i with grave

        case 50061:
        case 205:
            (*sh) = 1;
            (*alt) = 1;
            if (real) return 36;  // I with acute
            return 23;            // I with acute -> i with grave

        case 50099:
        case 243:
            (*alt) = 1;
            if (real) return 16;  // o with acute
            return 24;            // o with acute -> o with grave

        case 50067:
        case 211:
            (*sh) = 1;
            (*alt) = 1;
            if (real) return 16;  // O with acute
            return 24;            // O with acute -> o with grave

        case 50102:
        case 246:
            (*alt) = 1;
            return 25;  // o with diaeresis

        case 50070:
        case 214:
            (*sh) = 1;
            (*alt) = 1;
            return 25;  // O with diaeresis

        case 50577:
        case 245:
            (*alt) = 1;
            if (real) return 19;  // Hungarian o
            return 25;            // Hungarian o -> o with diaeresis

        case 50576:
        case 213:
            (*sh) = 1;
            (*alt) = 1;
            if (real) return 19;  // Hungarian O
            return 25;            // Hungarian O -> O with diaeresis

        case 50106:
        // case 0xffbe:
        //	(*alt)=1;
        // 	if (real)
        //		return 17; //u with acute
        // 	return 22; //u with acute -> u with grave
        case 50074:
        case 218:
            (*sh) = 1;
            (*alt) = 1;
            if (real) return 17;  // U with acute
            return 22;            // U with acute -> u with grave
        case 50108:
        case 252:
            (*alt) = 1;
            return 47;  // u with diaeresis

        case 50076:
        case 220:
            (*sh) = 1;
            (*alt) = 1;
            return 47;  // U with diaeresis

        case 50609:
        case 251:
            (*alt) = 1;
            if (real) return 45;  // Hungarian u
            return 47;            // Hungarian u -> u with diaeresis

        case 50608:
        case 219:
            (*sh) = 1;
            (*alt) = 1;
            if (real) return 45;  // Hungarian U
            return 47;            // Hungarian U -> U with diaeresis
    }
    return 0;
}
