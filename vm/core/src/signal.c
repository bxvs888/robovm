/*
 * Copyright (C) 2012 Trillian AB
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <robovm.h>
#include <signal.h>
#include <errno.h>
#include "private.h"

#define LOG_TAG "core.signal"

/*
 * The common way to implement stack overflow detection is to catch SIGSEGV and see if the
 * address that generated the fault is in the current thread's stack guard page. Since the stack
 * has been consumed at this point one usually uses an alternate signal stack for the signal
 * handler to run on using sigaltstack(). Unfortunately sigaltstack() seems to be broken on
 * iOS. Even if it completes without errors the alternate stack won't be used by the signal
 * handler. In RoboVM we work around this bug by inserting code in the prologue of
 * compiled methods which tries to read from the stack area at sp-64k. If this read hits the 
 * guard page a fault will occur but we'll still have about 64k left of stack space to run the signal 
 * handler on.
 */

static InstanceField* stackStateField = NULL;

static void signalHandler(int signum, siginfo_t* info, void* context);

jboolean rvmInitSignals(Env* env) {
    stackStateField = rvmGetInstanceField(env, java_lang_Throwable, "stackState", "J");
    if (!stackStateField) return FALSE;
    return TRUE;
}

static jboolean installSignalHandlers(Env* env) {
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = &signalHandler;

#if defined(DARWIN)
    // On Darwin SIGBUS is generated when dereferencing NULL pointers
    if (sigaction(SIGBUS, &sa, NULL) != 0) {
        rvmThrowInternalErrorErrno(env, errno);
        rvmTearDownSignals(env);
        return FALSE;
    }
#endif

    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        rvmThrowInternalErrorErrno(env, errno);
        rvmTearDownSignals(env);
        return FALSE;
    }

    int err;
    if ((err = pthread_sigmask(0, NULL, &env->currentThread->signalMask)) != 0) {
        rvmThrowInternalErrorErrno(env, err);
        rvmTearDownSignals(env);
        return FALSE;        
    }

    return TRUE;
}

jboolean rvmSetupSignals(Env* env) {
    if (!installSignalHandlers(env)) {
        return FALSE;
    }
    return TRUE;
}

void rvmRestoreSignalMask(Env* env) {
    pthread_sigmask(SIG_SETMASK, &env->currentThread->signalMask, NULL);
}

void rvmTearDownSignals(Env* env) {
}

static inline void* getFramePointer(ucontext_t* context) {
#if defined(DARWIN)
#   if defined(RVM_X86)
        return (void*) (ptrdiff_t) context->uc_mcontext->__ss.__ebp;
#   elif defined(RVM_THUMBV7)
        return (void*) (ptrdiff_t) context->uc_mcontext->__ss.__r[7];
#   endif
#elif defined(LINUX)
#   if defined(RVM_X86)
        return (void*) (ptrdiff_t) context->uc_mcontext.gregs[REG_EBP];
#   endif
#endif
}

static inline void* getPC(ucontext_t* context) {
#if defined(DARWIN)
#   if defined(RVM_X86)
        return (void*) (ptrdiff_t) context->uc_mcontext->__ss.__eip;
#   elif defined(RVM_THUMBV7)
        return (void*) (ptrdiff_t) context->uc_mcontext->__ss.__pc;
#   endif
#elif defined(LINUX)
#   if defined(RVM_X86)
        return (void*) (ptrdiff_t) context->uc_mcontext.gregs[REG_EIP];
#   endif
#endif
}

static void signalHandler(int signum, siginfo_t* info, void* context) {
    Env* env = rvmGetEnv();
    if (env && rvmIsNonNativeFrame(env)) {
        void* faultAddr = info->si_addr;
        void* stackAddr = env->currentThread->stackAddr;
        Class* exClass = NULL;
        if (!faultAddr) {
            // NullPointerException
            exClass = java_lang_NullPointerException;
        } else if (faultAddr < stackAddr && faultAddr >= (void*) (stackAddr - THREAD_STACK_GUARD_SIZE)) {
            // StackOverflowError
            exClass = java_lang_StackOverflowError;
        }

        if (exClass) {
            Object* throwable = rvmAllocateObject(env, exClass);
            if (!throwable) {
                throwable = rvmExceptionClear(env);
            }
            Frame fakeFrame;
            fakeFrame.prev = (Frame*) getFramePointer((ucontext_t*) context);
            fakeFrame.returnAddress = getPC((ucontext_t*) context);
            CallStack* callStack = rvmCaptureCallStack(env, &fakeFrame);
            rvmSetLongInstanceFieldValue(env, throwable, stackStateField, PTR_TO_LONG(callStack));
            rvmRaiseException(env, throwable);
                puts("foo7\n");

        }
    }

    struct sigaction sa;
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;
    sigaction(signum, &sa, NULL);
    kill(0, signum);
}
