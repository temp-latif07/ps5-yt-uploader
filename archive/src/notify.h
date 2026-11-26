#ifndef NOTIFY_H
#define NOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*notify_spy_fn)(const char* message);

/**
 * Send a user notification toast.
 * On PS5 (#ifdef PS5): Dispatches via sceKernelSendNotificationRequest.
 * On Host (macOS/Linux): Formats log to stdout and calls registered spy if present.
 *
 * @param message Notification text to display
 */
void notify_send(const char* message);

/**
 * Register a spy/callback function to intercept notifications (primarily for testing).
 * Passing NULL removes the registered spy.
 *
 * @param spy Callback invoked upon notify_send
 */
void notify_set_spy(notify_spy_fn spy);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFY_H */
