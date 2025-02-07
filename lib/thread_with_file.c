// SPDX-License-Identifier: GPL-2.0
/*
 * (C) 2022-2024 Kent Overstreet <kent.overstreet@linux.dev>
 */
#include <linux/anon_inodes.h>
#include <linux/darray.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/poll.h>
#include <linux/sched/sysctl.h>
#include <linux/thread_with_file.h>

/* stdio_redirect */

#define STDIO_REDIRECT_BUFSIZE		4096

static bool stdio_redirect_has_input(struct stdio_redirect *stdio)
{
	return stdio->input.buf.nr || stdio->done;
}

static bool stdio_redirect_has_output(struct stdio_redirect *stdio)
{
	return stdio->output.buf.nr || stdio->done;
}

static bool stdio_redirect_has_input_space(struct stdio_redirect *stdio)
{
	return stdio->input.buf.nr < STDIO_REDIRECT_BUFSIZE || stdio->done;
}

static bool stdio_redirect_has_output_space(struct stdio_redirect *stdio)
{
	return stdio->output.buf.nr < STDIO_REDIRECT_BUFSIZE || stdio->done;
}

static void stdio_buf_init(struct stdio_buf *buf)
{
	spin_lock_init(&buf->lock);
	init_waitqueue_head(&buf->wait);
	darray_init(&buf->buf);
}

int stdio_redirect_read(struct stdio_redirect *stdio, char *ubuf, size_t len)
{
	struct stdio_buf *buf = &stdio->input;

	/*
	 * we're waiting on user input (or for the file descriptor to be
	 * closed), don't want a hung task warning:
	 */
	do {
		wait_event_timeout(buf->wait, stdio_redirect_has_input(stdio),
				   sysctl_hung_task_timeout_secs * HZ / 2);
	} while (!stdio_redirect_has_input(stdio));

	if (stdio->done)
		return -1;

	spin_lock(&buf->lock);
	int ret = min(len, buf->buf.nr);
	memcpy(ubuf, buf->buf.data, ret);
	darray_remove_items(&buf->buf, buf->buf.data, ret);
	spin_unlock(&buf->lock);

	wake_up(&buf->wait);
	return ret;
}
EXPORT_SYMBOL_GPL(stdio_redirect_read);

int stdio_redirect_readline(struct stdio_redirect *stdio, char *ubuf, size_t len)
{
	struct stdio_buf *buf = &stdio->input;
	size_t copied = 0;
	ssize_t ret = 0;
again:
	do {
		wait_event_timeout(buf->wait, stdio_redirect_has_input(stdio),
				   sysctl_hung_task_timeout_secs * HZ / 2);
	} while (!stdio_redirect_has_input(stdio));

	if (stdio->done) {
		ret = -1;
		goto out;
	}

	spin_lock(&buf->lock);
	size_t b = min(len, buf->buf.nr);
	char *n = memchr(buf->buf.data, '\n', b);
	if (n)
		b = min_t(size_t, b, n + 1 - buf->buf.data);
	memcpy(ubuf, buf->buf.data, b);
	darray_remove_items(&buf->buf, buf->buf.data, b);
	ubuf += b;
	len -= b;
	copied += b;
	spin_unlock(&buf->lock);

	wake_up(&buf->wait);

	if (!n && len)
		goto again;
out:
	return copied ?: ret;
}
EXPORT_SYMBOL_GPL(stdio_redirect_readline);

__printf(3, 0)
static void darray_vprintf(darray_char *out, gfp_t gfp, const char *fmt, va_list args)
{
	size_t len;

	do {
		va_list args2;
		va_copy(args2, args);

		len = vsnprintf(out->data + out->nr, darray_room(*out), fmt, args2);
	} while (len + 1 > darray_room(*out) && !darray_make_room_gfp(out, len + 1, gfp));

	out->nr += min(len, darray_room(*out));
}

void stdio_redirect_vprintf(struct stdio_redirect *stdio, bool nonblocking,
			    const char *fmt, va_list args)
{
	struct stdio_buf *buf = &stdio->output;
	unsigned long flags;

	if (!nonblocking)
		wait_event(buf->wait, stdio_redirect_has_output_space(stdio));
	else if (!stdio_redirect_has_output_space(stdio))
		return;
	if (stdio->done)
		return;

	spin_lock_irqsave(&buf->lock, flags);
	darray_vprintf(&buf->buf, nonblocking ? GFP_NOWAIT : GFP_KERNEL, fmt, args);
	spin_unlock_irqrestore(&buf->lock, flags);

	wake_up(&buf->wait);
}
EXPORT_SYMBOL_GPL(stdio_redirect_vprintf);

void stdio_redirect_printf(struct stdio_redirect *stdio, bool nonblocking,
			   const char *fmt, ...)
{

	va_list args;
	va_start(args, fmt);
	stdio_redirect_vprintf(stdio, nonblocking, fmt, args);
	va_end(args);
}
EXPORT_SYMBOL_GPL(stdio_redirect_printf);

/* thread with file: */

void thread_with_file_exit(struct thread_with_file *thr)
{
	if (thr->task) {
		kthread_stop(thr->task);
		put_task_struct(thr->task);
	}
}
EXPORT_SYMBOL_GPL(thread_with_file_exit);

int run_thread_with_file(struct thread_with_file *thr,
			 const struct file_operations *fops,
			 int (*fn)(void *))
{
	struct file *file = NULL;
	int ret, fd = -1;
	unsigned fd_flags = O_CLOEXEC;

	if (fops->read && fops->write)
		fd_flags |= O_RDWR;
	else if (fops->read)
		fd_flags |= O_RDONLY;
	else if (fops->write)
		fd_flags |= O_WRONLY;

	char name[TASK_COMM_LEN];
	get_task_comm(name, current);

	thr->ret = 0;
	thr->task = kthread_create(fn, thr, "%s", name);
	ret = PTR_ERR_OR_ZERO(thr->task);
	if (ret)
		return ret;

	ret = get_unused_fd_flags(fd_flags);
	if (ret < 0)
		goto err;
	fd = ret;

	file = anon_inode_getfile(name, fops, thr, fd_flags);
	ret = PTR_ERR_OR_ZERO(file);
	if (ret)
		goto err;

	get_task_struct(thr->task);
	wake_up_process(thr->task);
	fd_install(fd, file);
	return fd;
err:
	if (fd >= 0)
		put_unused_fd(fd);
	if (thr->task)
		kthread_stop(thr->task);
	return ret;
}
EXPORT_SYMBOL_GPL(run_thread_with_file);

/* thread_with_stdio */

static void thread_with_stdio_done(struct thread_with_stdio *thr)
{
	thr->thr.done = true;
	thr->stdio.done = true;
	wake_up(&thr->stdio.input.wait);
	wake_up(&thr->stdio.output.wait);
}

static ssize_t thread_with_stdio_read(struct file *file, char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	struct thread_with_stdio *thr =
		container_of(file->private_data, struct thread_with_stdio, thr);
	struct stdio_buf *buf = &thr->stdio.output;
	size_t copied = 0, b;
	int ret = 0;

	if (!(file->f_flags & O_NONBLOCK)) {
		ret = wait_event_interruptible(buf->wait, stdio_redirect_has_output(&thr->stdio));
		if (ret)
			return ret;
	} else if (!stdio_redirect_has_output(&thr->stdio))
		return -EAGAIN;

	while (len && buf->buf.nr) {
		if (fault_in_writeable(ubuf, len) == len) {
			ret = -EFAULT;
			break;
		}

		spin_lock_irq(&buf->lock);
		b = min_t(size_t, len, buf->buf.nr);

		if (b && !copy_to_user_nofault(ubuf, buf->buf.data, b)) {
			ubuf	+= b;
			len	-= b;
			copied	+= b;
			darray_remove_items(&buf->buf, buf->buf.data, b);
		}
		spin_unlock_irq(&buf->lock);
	}

	return copied ?: ret;
}

static ssize_t thread_with_stdio_write(struct file *file, const char __user *ubuf,
				       size_t len, loff_t *ppos)
{
	struct thread_with_stdio *thr =
		container_of(file->private_data, struct thread_with_stdio, thr);
	struct stdio_buf *buf = &thr->stdio.input;
	size_t copied = 0;
	ssize_t ret = 0;

	while (len) {
		if (thr->thr.done) {
			ret = -EPIPE;
			break;
		}

		size_t b = len - fault_in_readable(ubuf, len);
		if (!b) {
			ret = -EFAULT;
			break;
		}

		spin_lock(&buf->lock);
		if (buf->buf.nr < STDIO_REDIRECT_BUFSIZE)
			darray_make_room_gfp(&buf->buf,
				min(b, STDIO_REDIRECT_BUFSIZE - buf->buf.nr), GFP_NOWAIT);
		b = min(len, darray_room(buf->buf));

		if (b && !copy_from_user_nofault(&darray_top(buf->buf), ubuf, b)) {
			buf->buf.nr += b;
			ubuf	+= b;
			len	-= b;
			copied	+= b;
		}
		spin_unlock(&buf->lock);

		if (b) {
			wake_up(&buf->wait);
		} else {
			if ((file->f_flags & O_NONBLOCK)) {
				ret = -EAGAIN;
				break;
			}

			ret = wait_event_interruptible(buf->wait,
					stdio_redirect_has_input_space(&thr->stdio));
			if (ret)
				break;
		}
	}

	return copied ?: ret;
}

static __poll_t thread_with_stdio_poll(struct file *file, struct poll_table_struct *wait)
{
	struct thread_with_stdio *thr =
		container_of(file->private_data, struct thread_with_stdio, thr);

	poll_wait(file, &thr->stdio.output.wait, wait);
	poll_wait(file, &thr->stdio.input.wait, wait);

	__poll_t mask = 0;

	if (stdio_redirect_has_output(&thr->stdio))
		mask |= EPOLLIN;
	if (stdio_redirect_has_input_space(&thr->stdio))
		mask |= EPOLLOUT;
	if (thr->thr.done)
		mask |= EPOLLHUP|EPOLLERR;
	return mask;
}

static int thread_with_stdio_release(struct inode *inode, struct file *file)
{
	struct thread_with_stdio *thr =
		container_of(file->private_data, struct thread_with_stdio, thr);

	thread_with_stdio_done(thr);
	thread_with_file_exit(&thr->thr);
	darray_exit(&thr->stdio.input.buf);
	darray_exit(&thr->stdio.output.buf);
	thr->exit(thr);
	return 0;
}

static const struct file_operations thread_with_stdio_fops = {
	.llseek		= no_llseek,
	.read		= thread_with_stdio_read,
	.write		= thread_with_stdio_write,
	.poll		= thread_with_stdio_poll,
	.release	= thread_with_stdio_release,
};

static int thread_with_stdio_fn(void *arg)
{
	struct thread_with_stdio *thr = arg;

	thr->fn(thr);

	thread_with_stdio_done(thr);
	return 0;
}

int run_thread_with_stdio(struct thread_with_stdio *thr,
			  void (*exit)(struct thread_with_stdio *),
			  void (*fn)(struct thread_with_stdio *))
{
	stdio_buf_init(&thr->stdio.input);
	stdio_buf_init(&thr->stdio.output);
	thr->exit	= exit;
	thr->fn		= fn;

	return run_thread_with_file(&thr->thr, &thread_with_stdio_fops, thread_with_stdio_fn);
}
EXPORT_SYMBOL_GPL(run_thread_with_stdio);

MODULE_AUTHOR("Kent Overstreet");
MODULE_LICENSE("GPL");
