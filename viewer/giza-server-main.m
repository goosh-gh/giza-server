/* viewer/giza-server-main.m
 *
 * giza_server — macOS Cocoa backend: main-thread bootstrapper
 *
 * NSApp MUST run on the main (OS-designated) thread.  On macOS this is the
 * thread that calls main().  We therefore hijack main() here, start a worker
 * thread that runs the real server logic (accept loop), then hand the main
 * thread over to NSApp's run loop.
 *
 * This follows exactly the same pattern as giza PR #86
 * (giza-osxcocoa-main.m).
 *
 * Copyright (c) 2026 goosh-gh — LGPL-2.1 (same as giza)
 */

#import <Cocoa/Cocoa.h>
#include <pthread.h>

/* declared in giza-server-cocoa.m */
extern int  giza_server_worker_main(void);
extern void giza_server_cocoa_init(void);

/* ------------------------------------------------------------------ */

static void *_worker_thread(void *arg __attribute__((unused)))
{
    giza_server_worker_main();
    return NULL;
}

int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
    /* Initialise NSApplication and Cocoa state from this thread before
       launching the worker — giza_server_cocoa_init() creates the
       NSApplication singleton and sets the activation policy.         */
    giza_server_cocoa_init();

    /* Launch the accept/connection worker thread */
    pthread_t thr;
    pthread_create(&thr, NULL, _worker_thread, NULL);
    pthread_detach(thr);

    /* Hand main thread to NSApp — never returns */
    [NSApp run];
    return 0;
}
