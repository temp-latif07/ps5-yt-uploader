#include "notify.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#ifdef PS5
typedef struct {
    int type;                         // 0x00
    int req_id;                       // 0x04
    int priority;                     // 0x08
    int msg_id;                       // 0x0C
    int target_id;                    // 0x10 (-1 for active logged-in user)
    int user_id;                      // 0x14
    int unk1;                         // 0x18
    int unk2;                         // 0x1C
    int app_id;                       // 0x20
    int error_num;                    // 0x24
    int unk3;                         // 0x28
    unsigned char use_icon_image_uri; // 0x2C (1 to use custom/system icon)
    char message[1024];               // 0x2D Notification text
    char uri[1024];                   // 0x42D System icon URI
    char unkstr[1024];                // 0x82D Extra padding / data
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
    req.target_id = -1;
    req.use_icon_image_uri = 1;
    snprintf(req.uri, sizeof(req.uri), "cxml://psnotification/tex_icon_system");
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
