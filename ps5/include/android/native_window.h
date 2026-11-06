/* XOG: the display stands in for Android's window. Our own code. */
#ifndef REXEMU_ANDROID_NATIVE_WINDOW_H
#define REXEMU_ANDROID_NATIVE_WINDOW_H

struct ANativeWindow;
static inline int ANativeWindow_getWidth(struct ANativeWindow *w) { (void)w; return 3840; }
static inline int ANativeWindow_getHeight(struct ANativeWindow *w) { (void)w; return 2160; }

#endif
