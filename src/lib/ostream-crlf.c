/* Copyright (c) 2004 Timo Sirainen */

/* The code is quite ugly because we want the send functions to return correcly
   the number of input bytes consumed, not number of bytes actually sent. */

#include "lib.h"
#include "buffer.h"
#include "istream.h"
#include "ostream-internal.h"
#include "ostream-crlf.h"

#define IOVBUF_COUNT 64

struct crlf_ostream {
	struct _ostream ostream;

        struct ostream *output;
	int last_cr;
};

static const struct const_iovec cr_iov = { "\r", 1 };

static void _close(struct _iostream *stream)
{
	struct crlf_ostream *cstream = (struct crlf_ostream *)stream;

	o_stream_close(cstream->output);
}

static void _destroy(struct _iostream *stream)
{
	struct crlf_ostream *cstream = (struct crlf_ostream *)stream;

	o_stream_ref(cstream->output);
}

static void _set_max_buffer_size(struct _iostream *stream, size_t max_size)
{
	struct crlf_ostream *cstream = (struct crlf_ostream *)stream;

	o_stream_set_max_buffer_size(cstream->output, max_size);
}

static void _cork(struct _ostream *stream, int set)
{
	struct crlf_ostream *cstream = (struct crlf_ostream *)stream;

	if (set)
		o_stream_cork(cstream->output);
	else
		o_stream_uncork(cstream->output);
}

static int _flush(struct _ostream *stream)
{
	struct crlf_ostream *cstream = (struct crlf_ostream *)stream;

	return o_stream_flush(cstream->output);
}

static size_t _get_used_size(struct _ostream *stream)
{
	struct crlf_ostream *cstream = (struct crlf_ostream *)stream;

	return o_stream_get_buffer_used_size(cstream->output);
}

static int _seek(struct _ostream *stream, uoff_t offset)
{
	struct crlf_ostream *cstream = (struct crlf_ostream *)stream;
	int ret;

	cstream->last_cr = FALSE;
	ret = o_stream_seek(cstream->output, offset);
	stream->ostream.offset = cstream->output->offset;
	return ret;
}

static ssize_t
sendv_crlf(struct crlf_ostream *cstream, const struct const_iovec *iov,
	   size_t iov_count, const char *diff, ssize_t *total_r)
{
	ssize_t ret;
	size_t pos;

	ret = o_stream_sendv(cstream->output, iov, iov_count);
	if (ret > 0) {
		pos = (size_t)ret - 1;
		while (pos >= iov->iov_len) {
			*total_r += iov->iov_len + *diff;
			pos -= iov->iov_len;
			iov++;
			diff++;
		}

		cstream->last_cr = *((const char *)iov->iov_base + pos) == '\r';

		if (pos + 1 == iov->iov_len)
			*total_r += iov->iov_len + *diff;
		else
			*total_r += pos;
	}
	cstream->ostream.ostream.offset = cstream->output->offset;
	return ret;
}

static ssize_t
_sendv_crlf(struct _ostream *stream, const struct const_iovec *iov,
	    size_t iov_count)
{
	struct crlf_ostream *cstream = (struct crlf_ostream *)stream;
	buffer_t *iov_buf, *diff_buf;
	const unsigned char *data;
	struct const_iovec new_iov;
	size_t vec, i, len, start, new_iov_count = 0, new_iov_size = 0;
	ssize_t ret, total;
	int last_cr;

	last_cr = cstream->last_cr;

	t_push();
	iov_buf = buffer_create_dynamic(unsafe_data_stack_pool,
					sizeof(struct const_iovec *) *
					IOVBUF_COUNT, (size_t)-1);
	diff_buf = buffer_create_dynamic(unsafe_data_stack_pool,
					 IOVBUF_COUNT, (size_t)-1);
	total = 0;
	for (vec = 0; vec < iov_count; vec++) {
		data = iov[vec].iov_base;
		len = iov[vec].iov_len;

		for (i = start = 0;; i++) {
			if (i != len) {
				if (data[i] != '\n')
					continue;

				if (i > 0) {
					if (data[i-1] == '\r')
						continue;
				} else {
					if (last_cr)
						continue;
				}

				/* need to insert CR */
			}

			if (i != start) {
				new_iov.iov_base = data + start;
				new_iov.iov_len = i - start;

				buffer_append(iov_buf, &new_iov,
					      sizeof(new_iov));
				buffer_append_c(diff_buf, 0);
				new_iov_count++;
				new_iov_size += new_iov.iov_len;
			}
			start = i;

			if (i != len) {
				buffer_append(iov_buf, &cr_iov, sizeof(cr_iov));
				buffer_append_c(diff_buf, -1);
				new_iov_count++;
				new_iov_size++;
			}

			if (new_iov_count >= IOVBUF_COUNT-1) {
				ret = sendv_crlf(cstream, iov_buf->data,
						 new_iov_count, diff_buf->data,
						 &total);
				if (ret != (ssize_t)new_iov_size) {
					t_pop();
					return ret < 0 ? ret : total;
				}

				buffer_set_used_size(iov_buf, 0);
				buffer_set_used_size(diff_buf, 0);
				new_iov_count = 0;
				new_iov_size = 0;
			}

			if (i == len)
				break;
		}

		if (len != 0)
			last_cr = data[len-1] == '\r';
	}

	ret = sendv_crlf(cstream, iov_buf->data, new_iov_count,
			 diff_buf->data, &total);
	t_pop();
	return ret < 0 ? ret : total;
}

static ssize_t
sendv_lf(struct crlf_ostream *cstream, const struct const_iovec *iov,
	 size_t iov_count, const char *diff, ssize_t *total_r)
{
	ssize_t ret;
	size_t left;

	ret = o_stream_sendv(cstream->output, iov, iov_count);
	if (ret >= 0) {
		left = (size_t)ret;
		while (left >= iov->iov_len) {
			*total_r += iov->iov_len + *diff;
			left -= iov->iov_len;
			iov++;
			diff++;
		}
		*total_r += left;
	}
	cstream->ostream.ostream.offset = cstream->output->offset;
	return ret;
}

static ssize_t
_sendv_lf(struct _ostream *stream, const struct const_iovec *iov,
	  size_t iov_count)
{
	struct crlf_ostream *cstream = (struct crlf_ostream *)stream;
	buffer_t *iov_buf, *diff_buf;
	const unsigned char *data;
	struct const_iovec new_iov;
	size_t vec, i, len, start, next, new_iov_count = 0, new_iov_size = 0;
	ssize_t ret, total;
	int diff;

	t_push();
	iov_buf = buffer_create_dynamic(unsafe_data_stack_pool,
					sizeof(struct const_iovec *) *
					IOVBUF_COUNT, (size_t)-1);
	diff_buf = buffer_create_dynamic(unsafe_data_stack_pool,
					 IOVBUF_COUNT, (size_t)-1);
	total = 0;
	for (vec = 0; vec < iov_count; vec++) {
		data = iov[vec].iov_base;
		len = iov[vec].iov_len;

		for (i = start = 0;; i++) {
			if (i != len) {
				if (data[i] != '\n' || i == 0 ||
				    data[i-1] != '\r')
					continue;
			}

			if (start == 0 && i > 0 && data[0] != '\n' &&
			    cstream->last_cr) {
				/* bare CR, keep it */
				buffer_append(iov_buf, &cr_iov, sizeof(cr_iov));
				buffer_append_c(diff_buf, -1);
				new_iov_count++;
				new_iov_size++;
			}

			next = i;
			if (i != len) {
				/* skipping an CR */
				i--;
				cstream->last_cr = FALSE;
				diff = 1;
			} else if (i != start && data[i-1] == '\r') {
				/* data ends with CR, don't add it yet */
				i--;
				cstream->last_cr = TRUE;
				diff = 1;
			} else {
				/* data doesn't end with CR */
				cstream->last_cr = FALSE;
				diff = 0;
			}

			new_iov.iov_base = data + start;
			new_iov.iov_len = i - start;

			buffer_append(iov_buf, &new_iov, sizeof(new_iov));
			buffer_append_c(diff_buf, diff);
			new_iov_count++;
			new_iov_size += new_iov.iov_len;

			start = i = next;

			if (new_iov_count == IOVBUF_COUNT) {
				ret = sendv_lf(cstream, iov_buf->data,
					       new_iov_count, diff_buf->data,
					       &total);
				stream->ostream.offset =
					cstream->output->offset;
				if (ret != (ssize_t)new_iov_size) {
					t_pop();
					return ret < 0 ? ret : total;
				}

				buffer_set_used_size(iov_buf, 0);
				buffer_set_used_size(diff_buf, 0);
				new_iov_count = 0;
				new_iov_size = 0;
			}

			if (i == len)
				break;
		}
	}

	if (new_iov_count == 0) {
		/* Tried to send only CR. */
		ret = 0;
		total++;
	} else {
		ret = sendv_lf(cstream, iov_buf->data, new_iov_count,
			       diff_buf->data, &total);
	}
	stream->ostream.offset = cstream->output->offset;

	t_pop();
	return ret < 0 ? ret : total;
}

static off_t
_send_istream(struct _ostream *outstream, struct istream *instream)
{
	struct const_iovec iov;
        const unsigned char *data;
	size_t sent = 0;
	ssize_t ret;

	while ((ret = i_stream_read_data(instream, &data,
					 &iov.iov_len, 0)) != -1) {
		if (iov.iov_len == 0)
			return sent;

		iov.iov_base = data;
		ret = o_stream_sendv(&outstream->ostream, &iov, 1);
		if (ret <= 0)
			return ret < 0 && sent == 0 ? -1 : (ssize_t)sent;

		i_stream_skip(instream, ret);
		sent += ret;

		if ((size_t)ret != iov.iov_len)
			return sent;
	}

	return sent == 0 ? -1 : (ssize_t)sent;
}

static struct crlf_ostream *
o_stream_create_common(pool_t pool, struct ostream *output)
{
	struct crlf_ostream *cstream;

	cstream = p_new(pool, struct crlf_ostream, 1);
	cstream->output = output;
	o_stream_ref(output);

	cstream->ostream.iostream.close = _close;
	cstream->ostream.iostream.destroy = _destroy;
	cstream->ostream.iostream.set_max_buffer_size = _set_max_buffer_size;

	cstream->ostream.cork = _cork;
	cstream->ostream.flush = _flush;
	cstream->ostream.get_used_size = _get_used_size;
	cstream->ostream.seek = _seek;
	cstream->ostream.send_istream = _send_istream;
	return cstream;
}

struct ostream *o_stream_create_crlf(pool_t pool, struct ostream *output)
{
	struct crlf_ostream *cstream;

	cstream = o_stream_create_common(pool, output);
	cstream->ostream.sendv = _sendv_crlf;
	return _o_stream_create(&cstream->ostream, pool);
}

struct ostream *o_stream_create_lf(pool_t pool, struct ostream *output)
{
	struct crlf_ostream *cstream;

	cstream = o_stream_create_common(pool, output);
	cstream->ostream.sendv = _sendv_lf;
	return _o_stream_create(&cstream->ostream, pool);
}
