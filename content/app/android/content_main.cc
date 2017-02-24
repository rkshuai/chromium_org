// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/app/android/content_main.h"

#include "base/at_exit.h"
#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/debug/trace_event.h"
#include "base/lazy_instance.h"
#include "content/public/app/content_main.h"
#include "content/public/app/content_main_delegate.h"
#include "content/public/app/content_main_runner.h"
#include "content/public/common/content_switches.h"
#include "jni/ContentMain_jni.h"

using base::LazyInstance;

namespace content {

namespace {
LazyInstance<scoped_ptr<ContentMainRunner> > g_content_runner =
    LAZY_INSTANCE_INITIALIZER;

LazyInstance<scoped_ptr<ContentMainDelegate> > g_content_main_delegate =
    LAZY_INSTANCE_INITIALIZER;

}  // namespace

static void InitApplicationContext(JNIEnv* env, jclass clazz, jobject context) {
  base::android::ScopedJavaLocalRef<jobject> scoped_context(env, context);
  base::android::InitApplicationContext(env, scoped_context);
}

/*主要做了下面几件事：

1.使用前面创建和设置的ShellMainDelegate初始化g_content_main_delegate，并初始化结构体ContentMainParams params的成员变量delegate。

2.创建并初始化Class ContentMainRunner(content/app/content_main_runner.cc)的实例g_content_runner，初始化函数Initialize(...)参数正是前面创建的params。

3.运行g_content_runner(ContentMainRunnerImpl)*/
static jint Start(JNIEnv* env, jclass clazz) {
  TRACE_EVENT0("startup", "content::Start");

  // On Android we can have multiple requests to start the browser in process
  // simultaneously. If we get an asynchonous request followed by a synchronous
  // request then we have to call this a second time to finish starting the
  // browser synchronously.
  if (!g_content_runner.Get().get()) {
    ContentMainParams params(g_content_main_delegate.Get().get());
    g_content_runner.Get().reset(ContentMainRunner::Create());
    g_content_runner.Get()->Initialize(params);//初始化ContentMainRunnerImpl对象
  }
  return g_content_runner.Get()->Run();
}

void SetContentMainDelegate(ContentMainDelegate* delegate) {
  DCHECK(!g_content_main_delegate.Get().get());
  g_content_main_delegate.Get().reset(delegate);
}

bool RegisterContentMain(JNIEnv* env) {
  return RegisterNativesImpl(env);
}

}  // namespace content
