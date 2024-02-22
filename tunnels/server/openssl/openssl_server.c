#include "openssl_server.h"
#include "buffer_pool.h"
#include "managers/socket_manager.h"
#include "loggers/network_logger.h"
#include "utils/jsonutils.h"
#include "hv/hssl.h"
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#define STATE(x) ((oss_server_state_t *)((x)->state))
#define CSTATE(x) ((oss_server_con_state_t *)((((x)->line->chains_state)[self->chain_index])))
#define CSTATE_MUT(x) ((x)->line->chains_state)[self->chain_index]
#define ISALIVE(x) (CSTATE(x) != NULL)

typedef struct oss_server_state_s
{

    hssl_ctx_t ssl_context;
    // settings

} oss_server_state_t;

typedef struct oss_server_con_state_s
{

    bool handshake_completed;
    SSL *ssl;

    BIO *rbio;
    BIO *wbio; 
    int fd;

    bool init_sent;
    bool first_sent;

} oss_server_con_state_t;


enum sslstatus
{
    SSLSTATUS_OK,
    SSLSTATUS_WANT_IO,
    SSLSTATUS_FAIL
};

static enum sslstatus get_sslstatus(SSL *ssl, int n)
{
    switch (SSL_get_error(ssl, n))
    {
    case SSL_ERROR_NONE:
        return SSLSTATUS_OK;
    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_WANT_READ:
        return SSLSTATUS_WANT_IO;
    case SSL_ERROR_ZERO_RETURN:
    case SSL_ERROR_SYSCALL:
    default:
        return SSLSTATUS_FAIL;
    }
}

static void cleanup(tunnel_t *self, context_t *c)
{
    if (CSTATE(c) != NULL)
    {
        SSL_free(CSTATE(c)->ssl); /* free the SSL object and its BIO's */
        free(CSTATE(c));
        CSTATE_MUT(c) = NULL;
        destroyContext(c);
    }
}

static inline void upStream(tunnel_t *self, context_t *c)
{
    oss_server_state_t *state = STATE(self);

    if (c->payload != NULL)
    {
        oss_server_con_state_t *cstate = CSTATE(c);

        enum sslstatus status;
        int n;
        size_t len = bufLen(c->payload);
        while (len > 0)
        {
            n = BIO_write(cstate->rbio, rawBuf(c->payload), len);

            if (n <= 0)
            {
                /* if BIO write fails, assume unrecoverable */
                DISCARD_CONTEXT(c);
                goto failed;
            }
            shiftr(c->payload,n);
            len -= n;

            if (!SSL_is_init_finished(cstate->ssl))
            {
                n = SSL_accept(cstate->ssl);
                status = get_sslstatus(cstate->ssl, n);

                /* Did SSL request to write bytes? */
                if (status == SSLSTATUS_WANT_IO)
                    do
                    {
                        shift_buffer_t *buf = popBuffer(buffer_pools[c->line->tid]);
                        size_t avail = rCap(buf);
                        n = BIO_read(cstate->wbio, rawBuf(buf), avail);
                        if (n > 0)
                        {
                            setLen(buf, n);
                            context_t *answer = newContext(c->line);
                            answer->payload = buf;
                            self->dw->downStream(self->dw, answer);
                            if (!ISALIVE(c))
                            {
                                DISCARD_CONTEXT(c);
                                return;
                            }
                        }
                        else if (!BIO_should_retry(cstate->wbio))
                        {
                            // If BIO_should_retry() is false then the cause is an error condition.
                            DISCARD_CONTEXT(c);
                            reuseBuffer(buffer_pools[c->line->tid], buf);
                            goto failed;
                        }
                        else
                        {
                            reuseBuffer(buffer_pools[c->line->tid], buf);
                        }
                    } while (n > 0);

                if (status == SSLSTATUS_FAIL)
                {
                    DISCARD_CONTEXT(c);
                    goto failed;
                }

                if (!SSL_is_init_finished(cstate->ssl))
                {
                    DISCARD_CONTEXT(c);
                    return;
                }
                else
                {
                    LOGD("Tls handshake complete");
                    cstate->handshake_completed = true;
                    context_t *up_init_ctx = newContext(c->line);
                    up_init_ctx->init = true;
                    self->up->upStream(self->up, up_init_ctx);
                    if (!ISALIVE(c))
                    {
                        LOGW("Openssl server: next node instantly closed the init with fin");
                        DISCARD_CONTEXT(c);
                        return;
                    }
                }
            }

            /* The encrypted data is now in the input bio so now we can perform actual
             * read of unencrypted data. */

            do
            {
                shift_buffer_t *buf = popBuffer(buffer_pools[c->line->tid]);
                size_t avail = rCap(buf);
                n = SSL_read(cstate->ssl, rawBuf(buf), avail);
                if (n > 0)
                {
                    setLen(buf, n);
                    context_t *up_ctx = newContext(c->line);
                    up_ctx->payload = buf;
                    if (!(cstate->first_sent))
                    {
                        up_ctx->first = true;
                        cstate->first_sent = true;
                    }
                    self->up->upStream(self->up, up_ctx);
                    if (!ISALIVE(c))
                    {
                        DISCARD_CONTEXT(c);
                        return;
                    }
                }
                else
                {
                    reuseBuffer(buffer_pools[c->line->tid], buf);
                }

            } while (n > 0);

            status = get_sslstatus(cstate->ssl, n);

            /* Did SSL request to write bytes? This can happen if peer has requested SSL
             * renegotiation. */
            if (status == SSLSTATUS_WANT_IO)
                do
                {
                    shift_buffer_t *buf = popBuffer(buffer_pools[c->line->tid]);
                    size_t avail = rCap(buf);

                    n = BIO_read(cstate->wbio, rawBuf(buf), avail);
                    if (n > 0)
                    {
                        setLen(buf, n);
                        context_t *answer = newContext(c->line);
                        answer->payload = buf;
                        self->dw->downStream(self->dw, answer);
                        if (!ISALIVE(c))
                        {
                            DISCARD_CONTEXT(c);
                            return;
                        }
                    }
                    else if (!BIO_should_retry(cstate->wbio))
                    {
                        // If BIO_should_retry() is false then the cause is an error condition.
                        reuseBuffer(buffer_pools[c->line->tid], buf);
                        DISCARD_CONTEXT(c);
                        goto failed_after_establishment;
                    }
                    else
                    {
                        reuseBuffer(buffer_pools[c->line->tid], buf);
                    }
                } while (n > 0);

            if (status == SSLSTATUS_FAIL)
            {
                DISCARD_CONTEXT(c);
                goto failed_after_establishment;
            }
        }
        // done with socket data
        DISCARD_CONTEXT(c);

        if (c->first)
        {
        }
    }
    else
    {

        if (c->init)
        {
            CSTATE_MUT(c) = malloc(sizeof(oss_server_con_state_t));
            memset(CSTATE(c), 0, sizeof(oss_server_con_state_t));
            oss_server_con_state_t *cstate = CSTATE(c);
            cstate->fd = hio_fd(c->src_io);
            cstate->rbio = BIO_new(BIO_s_mem());
            cstate->wbio = BIO_new(BIO_s_mem());

            cstate->ssl = SSL_new(state->ssl_context);
            SSL_set_accept_state(cstate->ssl); /* sets ssl to work in server mode. */
            SSL_set_bio(cstate->ssl, cstate->rbio, cstate->wbio);
            hio_read(c->src_io);
        }
        if (c->fin)
        {
            context_t *fail_context_up = newContext(c->line);
            fail_context_up->fin = true;
            fail_context_up->src_io = c->src_io;
            cleanup(self, c);
            self->up->upStream(self->up, fail_context_up);
            return;
        }
    }

    return;

failed_after_establishment:
    context_t *fail_context_up = newContext(c->line);
    fail_context_up->fin = true;
    fail_context_up->src_io = c->src_io;
    self->up->upStream(self->up, fail_context_up);
failed:
    context_t *fail_context = newContext(c->line);
    fail_context->fin = true;
    fail_context->src_io = NULL;
    cleanup(self, c);
    self->dw->downStream(self->dw, fail_context);
    return;
}

static inline void downStream(tunnel_t *self, context_t *c)
{
    oss_server_con_state_t *cstate = CSTATE(c);
    if (c->payload != NULL)
    {
        // self->dw->downStream(self->dw, ctx);
        // char buf[DEFAULT_BUF_SIZE];
        enum sslstatus status;

        if (!SSL_is_init_finished(cstate->ssl))
        {
            LOGF("How is it possilbe to received data before sending init to upstream?");
            exit(1);
        }
        size_t len = bufLen(c->payload);
        while (len)
        {
            int n = SSL_write(cstate->ssl, rawBuf(c->payload), len);
            status = get_sslstatus(cstate->ssl, n);

            if (n > 0)
            {
                /* consume the waiting bytes that have been used by SSL */
                if ((size_t)n < len)
                {
                    shiftr(c->payload, n);
                }
                // memmove(cstate->encrypt_buf, cstate->encrypt_buf + n, cstate->encrypt_len - n);

                len -= n;
                // cstate->encrypt_buf = (char *)realloc(cstate->encrypt_buf, cstate->encrypt_len);

                /* take the output of the SSL object and queue it for socket write */
                do
                {

                    shift_buffer_t *buf = popBuffer(buffer_pools[c->line->tid]);
                    size_t avail = rCap(buf);
                    n = BIO_read(cstate->wbio, rawBuf(buf), avail);
                    if (n > 0)
                    {
                        setLen(buf, n);
                        context_t *dw_context = newContext(c->line);
                        dw_context->payload = buf;
                        self->dw->downStream(self->dw, dw_context);
                        if (!ISALIVE(c))
                        {
                            DISCARD_CONTEXT(c);
                            return;
                        }
                    }
                    else if (!BIO_should_retry(cstate->wbio))
                    {
                        // If BIO_should_retry() is false then the cause is an error condition.
                        reuseBuffer(buffer_pools[c->line->tid], buf);
                        DISCARD_CONTEXT(c);
                        goto failed_after_establishment;
                    }
                    else
                    {
                        reuseBuffer(buffer_pools[c->line->tid], buf);
                    }
                } while (n > 0);
            }

            if (status == SSLSTATUS_FAIL)
            {
                DISCARD_CONTEXT(c);
                goto failed_after_establishment;
            }

            if (n == 0)
                break;
        }
        return;
    }
    else
    {
        if (c->est)
        {
            self->dw->downStream(self->dw, c);
            return;
        }
        else if (c->fin)
        {
            goto failed;
        }
    }
    return;

failed_after_establishment:
    context_t *fail_context_up = newContext(c->line);
    fail_context_up->fin = true;
    fail_context_up->src_io = NULL;
    self->up->upStream(self->up, fail_context_up);
failed:
    context_t *fail_context = newContext(c->line);
    fail_context->fin = true;
    fail_context->src_io = c->src_io;
    cleanup(self, c);
    self->dw->downStream(self->dw, fail_context);

    return;
}

static bool check_libhv()
{
    if (!HV_WITH_SSL)
    {
        LOGF("OpenSSL-Server: libhv compiled without ssl backend");
        return false;
    }
    if (strcmp(hssl_backend(), "openssl") != 0)
    {
        LOGF("OpenSSL-Server: wrong ssl backend in libhv, expecetd: %s but found %s", "openssl", hssl_backend());
        return false;
    }
    return true;
}
static void openSSLUpStream(tunnel_t *self, context_t *c)
{
    upStream(self, c);
}
static void openSSLPacketUpStream(tunnel_t *self, context_t *c)
{
    upStream(self, c);
}
static void openSSLDownStream(tunnel_t *self, context_t *c)
{
    downStream(self, c);
}
static void openSSLPacketDownStream(tunnel_t *self, context_t *c)
{
    downStream(self, c);
}

tunnel_t *newOpenSSLServer(node_instance_context_t *instance_info)
{
    if (!check_libhv())
    {
        return NULL;
    }

    oss_server_state_t *state = malloc(sizeof(oss_server_state_t));
    memset(state, 0, sizeof(oss_server_state_t));

    hssl_ctx_opt_t ssl_param;
    memset(&ssl_param, 0, sizeof(ssl_param));
    ssl_param.crt_file = "cert/fullchain.pem";
    ssl_param.key_file = "cert/privkey.pem";
    ssl_param.endpoint = HSSL_SERVER;
    state->ssl_context = hssl_ctx_new(&ssl_param);
    if (state->ssl_context == NULL)
    {
        LOGF("Could not create node ssl context");
        return NULL;
    }
    tunnel_t *t = newTunnel();
    t->state = state;

    
    t->upStream = &openSSLUpStream;
    t->packetUpStream = &openSSLPacketUpStream;
    t->downStream = &openSSLDownStream;
    t->packetDownStream = &openSSLPacketDownStream;
    return t;
}

void apiOpenSSLServer(tunnel_t *self, char *msg)
{
    LOGE("openssl-server API NOT IMPLEMENTED"); // TODO
}

tunnel_t *destroyOpenSSLServer(tunnel_t *self)
{
    LOGE("openssl-server DESTROY NOT IMPLEMENTED"); // TODO
    return NULL;
}