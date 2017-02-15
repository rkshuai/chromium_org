/*
 * Copyright (C) 2013, 2014 Hisilicon. All rights reserved.
 *
 * Common XLOG definitions for webkit
 *
 */

#include <utils/Log.h>
#include <utils/CallStack.h>


#ifndef LOG_BASE_DEBUG_H
#define LOG_BASE_DEBUG_H

//#define BLINK_LOG_DEBUG

#ifdef BLINK_LOG_DEBUG

#define XLOGD(TAG,...) android_printLog(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

#else

#define XLOGD(...) {}
//#define XLOGD(TAG,...) android_printLog(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
//#define DUMP_CALL_STACK(LOGTAG) {}
#define DUMP_CALL_STACK(LOGTAG) {\
    android::CallStack stack; \
        stack.update(); \
    stack.log(LOGTAG); }

#endif

#define XXLOGD(TAG,...) android_printLog(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define XXLOGE(TAG,...) android_printLog(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define XXLOGF(TAG,...) android_printLog(ANDROID_LOG_FATAL, TAG, __VA_ARGS__)


// Global LOGTAG definitions for WebCore
namespace WebCore {
    //const char* const VERSION = "AppleWebKit/534.30";
    const char* const LOGTAG  = "blink";
    const char* const HTML5   = "html5";
}

#endif // LOG_BASE_DEBUG_H
