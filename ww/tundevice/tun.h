#pragma once
#include "buffer_pool.h"
#include "master_pool.h"
#include "hloop.h"
#include "hplatform.h"
#include "hthread.h"
#include <stdint.h>

#define TUN_LOG_EVERYTHING true

#ifdef OS_UNIX
typedef int tun_handle_t;
#else
typedef void *tun_handle_t; // Windows handle (void* can hold HANDLE)
#endif

struct tun_device_s;

typedef void (*TunReadEventHandle)(struct tun_device_s *tdev, void *userdata, shift_buffer_t *buf, tid_t tid);

typedef struct tun_device_s
{
    char *name;
    // hio_t       *io; not using fd multiplexer
    tun_handle_t       handle;
    hthread_t          readThread;
    hthread_t          writeThread;
    hthread_routine    routine_reader;
    hthread_routine    routine_writer;
    master_pool_t     *reader_message_pool;
    generic_pool_t    *reader_shift_buffer_pool;
    buffer_pool_t     *reader_buffer_pool;
    struct hchan_s    *writer_buffer_channel;
    TunReadEventHandle read_event_callback;
    void              *userdata;
    atomic_bool        notstop;
    atomic_bool        up;

} tun_device_t;

tun_device_t *createTunDevice(const char *name, bool offload, void *userdata, TunReadEventHandle cb);
void          bringTunDeviceUP(tun_device_t *tdev);
void          writeToTunDevce(tun_device_t *tdev, shift_buffer_t *buf);
