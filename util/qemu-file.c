/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu-common.h"
#include "hw/hw.h"
#include "block/block.h"
#include "qemu/sockets.h"


#define IO_BUF_SIZE 32768

struct QEMUFile {
    const QEMUFileOps *ops;
    void *opaque;
    int is_write;

    int64_t bytes_xfer;
    int64_t xfer_limit;

    int64_t pos; /* start of buffer when writing, end of buffer
                    when reading */
    int buf_index;
    int buf_size; /* 0 when writing */
    uint8_t buf[IO_BUF_SIZE];

    int last_error;
};

typedef struct QEMUFileStdio
{
    FILE *stdio_file;
    QEMUFile *file;
} QEMUFileStdio;

typedef struct QEMUFileSocket
{
    int fd;
    QEMUFile *file;
} QEMUFileSocket;

typedef struct {
    Coroutine *co;
    int fd;
} FDYieldUntilData;

static void fd_coroutine_enter(void *opaque)
{
    FDYieldUntilData *data = opaque;
    qemu_set_fd_handler(data->fd, NULL, NULL, NULL);
    qemu_coroutine_enter(data->co, NULL);
}

/**
 * Yield until a file descriptor becomes readable
 *
 * Note that this function clobbers the handlers for the file descriptor.
 */
static void coroutine_fn yield_until_fd_readable(int fd)
{
    FDYieldUntilData data;

    assert(qemu_in_coroutine());
    data.co = qemu_coroutine_self();
    data.fd = fd;
    qemu_set_fd_handler(fd, fd_coroutine_enter, NULL, &data);
    qemu_coroutine_yield();
}

static int socket_get_fd(void *opaque)
{
    QEMUFileSocket *s = opaque;

    return s->fd;
}

static int socket_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    QEMUFileSocket *s = opaque;
    ssize_t len;

    for (;;) {
        len = qemu_recv(s->fd, buf, size, 0);
        if (len != -1) {
            break;
        }
        if (socket_error() == EAGAIN) {
            yield_until_fd_readable(s->fd);
        } else if (socket_error() != EINTR) {
            break;
        }
    }

    if (len == -1) {
        len = -socket_error();
    }
    return len;
}

static int socket_put_buffer(void *opaque, const uint8_t *buf, int64_t pos, int size)
{
    QEMUFileSocket *s = opaque;
    ssize_t len;

    len = qemu_send_full(s->fd, buf, size, 0);
    if (len < size) {
        len = -socket_error();
    }
    return len;
}

static int socket_close(void *opaque)
{
    QEMUFileSocket *s = opaque;
    closesocket(s->fd);
    g_free(s);
    return 0;
}

static int stdio_get_fd(void *opaque)
{
    QEMUFileStdio *s = opaque;

    return fileno(s->stdio_file);
}

static int stdio_put_buffer(void *opaque, const uint8_t *buf, int64_t pos, int size)
{
    QEMUFileStdio *s = opaque;
    return fwrite(buf, 1, size, s->stdio_file);
}

static int stdio_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    QEMUFileStdio *s = opaque;
    FILE *fp = s->stdio_file;
    int bytes;

    for (;;) {
        clearerr(fp);
        bytes = fread(buf, 1, size, fp);
        if (bytes != 0 || !ferror(fp)) {
            break;
        }
        if (errno == EAGAIN) {
            yield_until_fd_readable(fileno(fp));
        } else if (errno != EINTR) {
            break;
        }
    }
    return bytes;
}

static int stdio_pclose(void *opaque)
{
    QEMUFileStdio *s = opaque;
    int ret;
    ret = pclose(s->stdio_file);
    if (ret == -1) {
        ret = -errno;
    } else if (!WIFEXITED(ret) || WEXITSTATUS(ret) != 0) {
        /* close succeeded, but non-zero exit code: */
        ret = -EIO; /* fake errno value */
    }
    g_free(s);
    return ret;
}

static int stdio_fclose(void *opaque)
{
    QEMUFileStdio *s = opaque;
    int ret = 0;

    if (s->file->ops->put_buffer) {
        int fd = fileno(s->stdio_file);
        struct stat st;

        ret = fstat(fd, &st);
        if (ret == 0 && S_ISREG(st.st_mode)) {
            /*
             * If the file handle is a regular file make sure the
             * data is flushed to disk before signaling success.
             */
            ret = fsync(fd);
            if (ret != 0) {
                ret = -errno;
                return ret;
            }
        }
    }
    if (fclose(s->stdio_file) == EOF) {
        ret = -errno;
    }
    g_free(s);
    return ret;
}

static const QEMUFileOps stdio_pipe_read_ops = {
    .get_fd =     stdio_get_fd,
    .get_buffer = stdio_get_buffer,
    .close =      stdio_pclose
};

static const QEMUFileOps stdio_pipe_write_ops = {
    .get_fd =     stdio_get_fd,
    .put_buffer = stdio_put_buffer,
    .close =      stdio_pclose
};

QEMUFile *qemu_popen_cmd(const char *command, const char *mode)
{
    FILE *stdio_file;
    QEMUFileStdio *s;

    stdio_file = popen(command, mode);
    if (stdio_file == NULL) {
        return NULL;
    }

    if (mode == NULL || (mode[0] != 'r' && mode[0] != 'w') || mode[1] != 0) {
        fprintf(stderr, "qemu_popen: Argument validity check failed\n");
        return NULL;
    }

    s = g_malloc0(sizeof(QEMUFileStdio));

    s->stdio_file = stdio_file;

    if(mode[0] == 'r') {
        s->file = qemu_fopen_ops(s, &stdio_pipe_read_ops);
    } else {
        s->file = qemu_fopen_ops(s, &stdio_pipe_write_ops);
    }
    return s->file;
}

static const QEMUFileOps stdio_file_read_ops = {
    .get_fd =     stdio_get_fd,
    .get_buffer = stdio_get_buffer,
    .close =      stdio_fclose
};

static const QEMUFileOps stdio_file_write_ops = {
    .get_fd =     stdio_get_fd,
    .put_buffer = stdio_put_buffer,
    .close =      stdio_fclose
};

QEMUFile *qemu_fdopen(int fd, const char *mode)
{
    QEMUFileStdio *s;

    if (mode == NULL ||
	(mode[0] != 'r' && mode[0] != 'w') ||
	mode[1] != 'b' || mode[2] != 0) {
        fprintf(stderr, "qemu_fdopen: Argument validity check failed\n");
        return NULL;
    }

    s = g_malloc0(sizeof(QEMUFileStdio));
    s->stdio_file = fdopen(fd, mode);
    if (!s->stdio_file)
        goto fail;

    if(mode[0] == 'r') {
        s->file = qemu_fopen_ops(s, &stdio_file_read_ops);
    } else {
        s->file = qemu_fopen_ops(s, &stdio_file_write_ops);
    }
    return s->file;

fail:
    g_free(s);
    return NULL;
}

static const QEMUFileOps socket_read_ops = {
    .get_fd =     socket_get_fd,
    .get_buffer = socket_get_buffer,
    .close =      socket_close
};

static const QEMUFileOps socket_write_ops = {
    .get_fd =     socket_get_fd,
    .put_buffer = socket_put_buffer,
    .close =      socket_close
};

QEMUFile *qemu_fopen_socket(int fd, const char *mode)
{
    QEMUFileSocket *s = g_malloc0(sizeof(QEMUFileSocket));

    if (mode == NULL ||
        (mode[0] != 'r' && mode[0] != 'w') ||
        mode[1] != 'b' || mode[2] != 0) {
        fprintf(stderr, "qemu_fopen: Argument validity check failed\n");
        return NULL;
    }

    s->fd = fd;
    if (mode[0] == 'w') {
        socket_set_block(s->fd);
        s->file = qemu_fopen_ops(s, &socket_write_ops);
    } else {
        s->file = qemu_fopen_ops(s, &socket_read_ops);
    }
    return s->file;
}

QEMUFile *qemu_fopen(const char *filename, const char *mode)
{
    QEMUFileStdio *s;

    if (mode == NULL ||
	(mode[0] != 'r' && mode[0] != 'w') ||
	mode[1] != 'b' || mode[2] != 0) {
        fprintf(stderr, "qemu_fopen: Argument validity check failed\n");
        return NULL;
    }

    s = g_malloc0(sizeof(QEMUFileStdio));

    s->stdio_file = fopen(filename, mode);
    if (!s->stdio_file)
        goto fail;

    if(mode[0] == 'w') {
        s->file = qemu_fopen_ops(s, &stdio_file_write_ops);
    } else {
        s->file = qemu_fopen_ops(s, &stdio_file_read_ops);
    }
    return s->file;
fail:
    g_free(s);
    return NULL;
}

static int block_put_buffer(void *opaque, const uint8_t *buf,
                           int64_t pos, int size)
{
    bdrv_save_vmstate(opaque, buf, pos, size);
    return size;
}

static int block_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    return bdrv_load_vmstate(opaque, buf, pos, size);
}

static int bdrv_fclose(void *opaque)
{
    return bdrv_flush(opaque);
}

static const QEMUFileOps bdrv_read_ops = {
    .get_buffer = block_get_buffer,
    .close =      bdrv_fclose
};

static const QEMUFileOps bdrv_write_ops = {
    .put_buffer = block_put_buffer,
    .close =      bdrv_fclose
};

QEMUFile *qemu_fopen_bdrv(BlockDriverState *bs, int is_writable)
{
    if (is_writable)
        return qemu_fopen_ops(bs, &bdrv_write_ops);
    return qemu_fopen_ops(bs, &bdrv_read_ops);
}

QEMUFile *qemu_fopen_ops(void *opaque, const QEMUFileOps *ops)
{
    QEMUFile *f;

    f = g_malloc0(sizeof(QEMUFile));

    f->opaque = opaque;
    f->ops = ops;
    f->is_write = 0;
    return f;
}

int qemu_file_get_error(QEMUFile *f)
{
    return f->last_error;
}

void qemu_file_set_error(QEMUFile *f, int ret)
{
    if (f->last_error == 0) {
        f->last_error = ret;
    }
}

/** Flushes QEMUFile buffer
 *
 */
void qemu_fflush(QEMUFile *f)
{
    int ret = 0;

    if (!f->ops->put_buffer) {
        return;
    }
    if (f->is_write && f->buf_index > 0) {
        ret = f->ops->put_buffer(f->opaque, f->buf, f->pos, f->buf_index);
        if (ret >= 0) {
            f->pos += f->buf_index;
        }
        f->buf_index = 0;
    }
    if (ret < 0) {
        qemu_file_set_error(f, ret);
    }
}

static void qemu_fill_buffer(QEMUFile *f)
{
    int len;
    int pending;

    if (!f->ops->get_buffer)
        return;

    if (f->is_write)
        abort();

    pending = f->buf_size - f->buf_index;
    if (pending > 0) {
        memmove(f->buf, f->buf + f->buf_index, pending);
    }
    f->buf_index = 0;
    f->buf_size = pending;

    len = f->ops->get_buffer(f->opaque, f->buf + pending, f->pos,
                        IO_BUF_SIZE - pending);
    if (len > 0) {
        f->buf_size += len;
        f->pos += len;
    } else if (len == 0) {
        qemu_file_set_error(f, -EIO);
    } else if (len != -EAGAIN)
        qemu_file_set_error(f, len);
}

int qemu_get_fd(QEMUFile *f)
{
    if (f->ops->get_fd) {
        return f->ops->get_fd(f->opaque);
    }
    return -1;
}

/** Closes the file
 *
 * Returns negative error value if any error happened on previous operations or
 * while closing the file. Returns 0 or positive number on success.
 *
 * The meaning of return value on success depends on the specific backend
 * being used.
 */
int qemu_fclose(QEMUFile *f)
{
    int ret;
    qemu_fflush(f);
    ret = qemu_file_get_error(f);

    if (f->ops->close) {
        int ret2 = f->ops->close(f->opaque);
        if (ret >= 0) {
            ret = ret2;
        }
    }
    /* If any error was spotted before closing, we should report it
     * instead of the close() return value.
     */
    if (f->last_error) {
        ret = f->last_error;
    }
    g_free(f);
    return ret;
}

void qemu_put_buffer(QEMUFile *f, const uint8_t *buf, int size)
{
    int l;

    if (f->last_error) {
        return;
    }

    if (f->is_write == 0 && f->buf_index > 0) {
        fprintf(stderr,
                "Attempted to write to buffer while read buffer is not empty\n");
        abort();
    }

    while (size > 0) {
        l = IO_BUF_SIZE - f->buf_index;
        if (l > size)
            l = size;
        memcpy(f->buf + f->buf_index, buf, l);
        f->is_write = 1;
        f->buf_index += l;
        f->bytes_xfer += l;
        buf += l;
        size -= l;
        if (f->buf_index >= IO_BUF_SIZE) {
            qemu_fflush(f);
            if (qemu_file_get_error(f)) {
                break;
            }
        }
    }
}

void qemu_put_byte(QEMUFile *f, int v)
{
    if (f->last_error) {
        return;
    }

    if (f->is_write == 0 && f->buf_index > 0) {
        fprintf(stderr,
                "Attempted to write to buffer while read buffer is not empty\n");
        abort();
    }

    f->buf[f->buf_index++] = v;
    f->is_write = 1;
    if (f->buf_index >= IO_BUF_SIZE) {
        qemu_fflush(f);
    }
}

void qemu_file_skip(QEMUFile *f, int size)
{
    if (f->buf_index + size <= f->buf_size) {
        f->buf_index += size;
    }
}

int qemu_peek_buffer(QEMUFile *f, uint8_t *buf, int size, size_t offset)
{
    int pending;
    int index;

    if (f->is_write) {
        abort();
    }

    index = f->buf_index + offset;
    pending = f->buf_size - index;
    if (pending < size) {
        qemu_fill_buffer(f);
        index = f->buf_index + offset;
        pending = f->buf_size - index;
    }

    if (pending <= 0) {
        return 0;
    }
    if (size > pending) {
        size = pending;
    }

    memcpy(buf, f->buf + index, size);
    return size;
}

int qemu_get_buffer(QEMUFile *f, uint8_t *buf, int size)
{
    int pending = size;
    int done = 0;

    while (pending > 0) {
        int res;

        res = qemu_peek_buffer(f, buf, pending, 0);
        if (res == 0) {
            return done;
        }
        qemu_file_skip(f, res);
        buf += res;
        pending -= res;
        done += res;
    }
    return done;
}

int qemu_peek_byte(QEMUFile *f, int offset)
{
    int index = f->buf_index + offset;

    if (f->is_write) {
        abort();
    }

    if (index >= f->buf_size) {
        qemu_fill_buffer(f);
        index = f->buf_index + offset;
        if (index >= f->buf_size) {
            return 0;
        }
    }
    return f->buf[index];
}

int qemu_get_byte(QEMUFile *f)
{
    int result;

    result = qemu_peek_byte(f, 0);
    qemu_file_skip(f, 1);
    return result;
}

int64_t qemu_ftell(QEMUFile *f)
{
    qemu_fflush(f);
    return f->pos;
}

int qemu_file_rate_limit(QEMUFile *f)
{
    if (qemu_file_get_error(f)) {
        return 1;
    }
    if (f->xfer_limit > 0 && f->bytes_xfer > f->xfer_limit) {
        return 1;
    }
    return 0;
}

int64_t qemu_file_get_rate_limit(QEMUFile *f)
{
    return f->xfer_limit;
}

void qemu_file_set_rate_limit(QEMUFile *f, int64_t limit)
{
    f->xfer_limit = limit;
}

void qemu_file_reset_rate_limit(QEMUFile *f)
{
    f->bytes_xfer = 0;
}

void qemu_put_be16(QEMUFile *f, unsigned int v)
{
    qemu_put_byte(f, v >> 8);
    qemu_put_byte(f, v);
}

void qemu_put_be32(QEMUFile *f, unsigned int v)
{
    qemu_put_byte(f, v >> 24);
    qemu_put_byte(f, v >> 16);
    qemu_put_byte(f, v >> 8);
    qemu_put_byte(f, v);
}

void qemu_put_be64(QEMUFile *f, uint64_t v)
{
    qemu_put_be32(f, v >> 32);
    qemu_put_be32(f, v);
}

unsigned int qemu_get_be16(QEMUFile *f)
{
    unsigned int v;
    v = qemu_get_byte(f) << 8;
    v |= qemu_get_byte(f);
    return v;
}

unsigned int qemu_get_be32(QEMUFile *f)
{
    unsigned int v;
    v = qemu_get_byte(f) << 24;
    v |= qemu_get_byte(f) << 16;
    v |= qemu_get_byte(f) << 8;
    v |= qemu_get_byte(f);
    return v;
}

uint64_t qemu_get_be64(QEMUFile *f)
{
    uint64_t v;
    v = (uint64_t)qemu_get_be32(f) << 32;
    v |= qemu_get_be32(f);
    return v;
}

int qemu_read_bytes(QEMUFile *f, uint8_t *buf, int size)
{
    if (qemu_file_get_error(f)) {
        return -1;
    }
    return qemu_get_buffer(f, buf, size);
}

int qemu_peek_bytes(QEMUFile *f, uint8_t *buf, int size, size_t offset)
{
    if (qemu_file_get_error(f)) {
        return -1;
    }
    return qemu_peek_buffer(f, buf, size, offset);
}

int qemu_write_bytes(QEMUFile *f, const uint8_t *buf, int size)
{
    if (qemu_file_get_error(f)) {
        return -1;
    }

    qemu_put_buffer(f, buf, size);

    if (qemu_file_get_error(f)) {
        return -1;
    }

    return size;
}
