#include "notify.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#ifdef PS5
typedef struct {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t* req, size_t size, int blocking);
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
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", message);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif

    printf("[NOTIFY] %s\n", message);

    pthread_mutex_lock(&s_notify_mutex);
    notify_spy_fn spy = s_notify_spy;
    pthread_mutex_unlock(&s_notify_mutex);

    if (spy) {
        spy(message);
    }
}
