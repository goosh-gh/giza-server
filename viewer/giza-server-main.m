#import <Cocoa/Cocoa.h>
#include <pthread.h>

extern int  giza_server_worker_main(void);
extern void giza_server_cocoa_init(void);

static void *_worker_thread(void *arg __attribute__((unused)))
{
    giza_server_worker_main();
    return NULL;
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
    giza_server_cocoa_init();

    pthread_t thr;
    pthread_create(&thr, NULL, _worker_thread, NULL);
    pthread_detach(thr);

    [NSApp run];
    return 0;
}

