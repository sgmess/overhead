#pragma once
// One thread: semaphores are counters, and a task notification calls
// notify_hook(), which a test can use to run the task's work at once
#include <stdint.h>
#include <stdlib.h>
typedef int *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int BaseType_t;
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
#define pdMS_TO_TICKS(ms) (ms)
static inline int *fake_sem(int v)
{
    int *s = malloc(sizeof(*s));
    *s = v;
    return s;
}
static inline int fake_take(int *s)
{
    if (*s > 0) {
        (*s)--;
        return 1;
    }
    return 0;
}
#define xSemaphoreCreateMutex() fake_sem(1)
#define xSemaphoreCreateBinary() fake_sem(0)
#define xSemaphoreTake(s, t) fake_take(s)
#define xSemaphoreGive(s) ((void)(++*(s)))
#define vSemaphoreDelete(s) free(s)
void notify_hook(void);
#define xTaskNotifyGive(t) notify_hook()
static inline uint32_t ulTaskNotifyTake(int clear, uint32_t wait)
{
    (void)clear;
    (void)wait;
    return 1;
}
#define xTaskCreateWithCaps(f, n, s, a, p, h, c) ((void)(f), *(h) = (void *)1, pdPASS)
#define xTaskCreate(f, n, s, a, p, h) ((void)(f), *(h) = (void *)1, pdPASS)
