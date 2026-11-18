#include "notify.h"
#include <stdio.h>
#include <pthread.h>

#ifdef PS5
// PS5 notification request definition
int sceNotificationRequest(const char* message);
#endif

static notify_spy_fn s_notify_spy = NULL;
static pthread_mutex_t s_notify_mutex = PTHREAD_MUTEX_INITIALIZER;

void notify_set_spy(notify_spy_fn spy) {
    pthread_mutex_lock(&s_notify_mutex);
    s_notify_spy = spy;
    pthread_mutex_unlock(&s_notify_mutex);
}

void notify_send(const char* message) {
    if (!message) {
        return;
    }

#ifdef PS5
    sceNotificationRequest(message);
#endif

    printf("[NOTIFY] %s\n", message);

    pthread_mutex_lock(&s_notify_mutex);
    notify_spy_fn spy = s_notify_spy;
    pthread_mutex_unlock(&s_notify_mutex);

    if (spy) {
        spy(message);
    }
}
