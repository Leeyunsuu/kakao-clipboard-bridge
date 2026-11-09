#include "kakao-png-to-bmp.h"
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <signal.h>

#define PNG "image/png"
#define BMP "image/bmp"
#define MAX_PNG_BYTES (64U * 1024U * 1024U)

typedef struct {
    GdkContentProvider parent;
    GBytes *png;
} ScreenshotProvider;

typedef GdkContentProviderClass ScreenshotProviderClass;
G_DEFINE_TYPE(ScreenshotProvider, screenshot_provider, GDK_TYPE_CONTENT_PROVIDER)

typedef struct {
    GOutputStream *stream;
    GBytes *bmp;
    GCancellable *caller;
    GCancellable *cancel;
    gulong cancel_handler;
    guint timeout;
    int priority;
    gint64 started;
    gboolean is_bmp;
} WriteRequest;

static GQueue bmp_queue = G_QUEUE_INIT;
static gboolean bmp_busy;
static GCancellable *current_read;

static void start_next_bmp(void);

static void cancel_write(GCancellable *caller, gpointer data)
{
    (void)caller;
    g_cancellable_cancel(data);
}

static gboolean time_out_write(gpointer data)
{
    WriteRequest *write = data;
    write->timeout = 0;
    g_cancellable_cancel(write->cancel);
    return G_SOURCE_REMOVE;
}

static void free_write(gpointer data)
{
    WriteRequest *write = data;
    if (write->timeout)
        g_source_remove(write->timeout);
    if (write->cancel_handler)
        g_cancellable_disconnect(write->caller, write->cancel_handler);
    g_clear_object(&write->caller);
    g_object_unref(write->cancel);
    g_object_unref(write->stream);
    g_clear_pointer(&write->bmp, g_bytes_unref);
    g_free(write);
}

static void finish_write(GTask *task, GError *error)
{
    WriteRequest *write = g_task_get_task_data(task);
    gboolean is_bmp = write->is_bmp;
    if (error) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning("Could not serve clipboard image: %s", error->message);
        g_task_return_error(task, error);
    } else {
        if (is_bmp)
            g_message("Served requested BMP (%" G_GINT64_FORMAT " ms)",
                      (g_get_monotonic_time() - write->started) / 1000);
        g_task_return_boolean(task, TRUE);
    }
    /* Release the decoded image before allowing a second conversion. */
    g_clear_pointer(&write->bmp, g_bytes_unref);
    g_object_unref(task);
    if (is_bmp) {
        bmp_busy = FALSE;
        start_next_bmp();
    }
}

static void write_done(GObject *stream, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    g_output_stream_write_all_finish(G_OUTPUT_STREAM(stream), result, NULL, &error);
    finish_write(data, error);
}

static void send_bytes(GTask *task, GBytes *bytes)
{
    WriteRequest *write = g_task_get_task_data(task);
    gsize size;
    const void *payload = g_bytes_get_data(bytes, &size);
    g_output_stream_write_all_async(write->stream, payload, size, write->priority,
                                    write->cancel, write_done, task);
}

static void decode_bmp(GTask *task, gpointer source, gpointer unused, GCancellable *cancel)
{
    (void)unused;
    ScreenshotProvider *provider = source;
    GError *error = NULL;
    GBytes *bmp = kakao_png_to_bmp(provider->png, cancel, &error);
    if (bmp)
        g_task_return_pointer(task, bmp, (GDestroyNotify)g_bytes_unref);
    else
        g_task_return_error(task, error);
}

static void bmp_ready(GObject *provider, GAsyncResult *result, gpointer data)
{
    (void)provider;
    GTask *task = data;
    WriteRequest *write = g_task_get_task_data(task);
    GError *error = NULL;
    write->bmp = g_task_propagate_pointer(G_TASK(result), &error);
    if (write->bmp)
        send_bytes(task, write->bmp);
    else
        finish_write(task, error);
}

static void start_next_bmp(void)
{
    while (!bmp_busy && !g_queue_is_empty(&bmp_queue)) {
        GTask *task = g_queue_pop_head(&bmp_queue);
        if (g_task_return_error_if_cancelled(task)) {
            g_object_unref(task);
            continue;
        }
        bmp_busy = TRUE;
        g_autoptr(GTask) decode = g_task_new(g_task_get_source_object(task),
                                             g_task_get_cancellable(task), bmp_ready, task);
        g_task_run_in_thread(decode, decode_bmp);
    }
}

static void provider_write(GdkContentProvider *provider, const char *mime,
                           GOutputStream *stream, int priority, GCancellable *caller,
                           GAsyncReadyCallback callback, gpointer data)
{
    gboolean is_bmp = g_str_equal(mime, BMP);
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    GTask *task = g_task_new(provider, cancel, callback, data);
    g_task_set_priority(task, priority);
    if ((!is_bmp && !g_str_equal(mime, PNG)) || (is_bmp && bmp_queue.length >= 4)) {
        g_task_return_new_error(task, G_IO_ERROR, is_bmp ? G_IO_ERROR_BUSY : G_IO_ERROR_NOT_SUPPORTED,
                                "%s", is_bmp ? "Too many BMP requests" : "Unsupported clipboard format");
        g_object_unref(task);
        return;
    }

    WriteRequest *write = g_new0(WriteRequest, 1);
    write->stream = g_object_ref(stream);
    write->cancel = g_object_ref(cancel);
    write->priority = priority;
    write->started = g_get_monotonic_time();
    write->is_bmp = is_bmp;
    if (caller) {
        write->caller = g_object_ref(caller);
        write->cancel_handler = g_cancellable_connect(caller, G_CALLBACK(cancel_write), cancel, NULL);
    }
    write->timeout = g_timeout_add_seconds(15, time_out_write, write);
    g_task_set_task_data(task, write, free_write);
    if (is_bmp) {
        g_queue_push_tail(&bmp_queue, task);
        start_next_bmp();
    } else {
        send_bytes(task, ((ScreenshotProvider *)provider)->png);
    }
}

static gboolean provider_finish(GdkContentProvider *provider, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, provider), FALSE);
    return g_task_propagate_boolean(G_TASK(result), error);
}

static GdkContentFormats *provider_formats(GdkContentProvider *provider)
{
    (void)provider;
    return gdk_content_formats_new((const char *[]) {BMP, PNG}, 2);
}

static GdkContentFormats *provider_storable(GdkContentProvider *provider)
{
    (void)provider;
    return gdk_content_formats_new((const char *[]) {PNG}, 1);
}

static void provider_finalize(GObject *object)
{
    g_bytes_unref(((ScreenshotProvider *)object)->png);
    G_OBJECT_CLASS(screenshot_provider_parent_class)->finalize(object);
}

static void screenshot_provider_class_init(ScreenshotProviderClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = provider_finalize;
    klass->ref_formats = provider_formats;
    klass->ref_storable_formats = provider_storable;
    klass->write_mime_type_async = provider_write;
    klass->write_mime_type_finish = provider_finish;
}

static void screenshot_provider_init(ScreenshotProvider *provider)
{
    (void)provider;
}

static void read_png(GTask *task, gpointer clipboard, gpointer data, GCancellable *cancel)
{
    (void)clipboard;
    GInputStream *stream = data;
    g_autoptr(GByteArray) bytes = g_byte_array_new();
    GError *error = NULL;
    guint8 buffer[65536];
    gssize n;
    while ((n = g_input_stream_read(stream, buffer, sizeof buffer, cancel, &error)) > 0) {
        if ((gsize)n > MAX_PNG_BYTES - bytes->len) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Clipboard PNG exceeds size limit");
            break;
        }
        g_byte_array_append(bytes, buffer, n);
    }
    g_input_stream_close(stream, NULL, NULL);
    if (error)
        g_task_return_error(task, error);
    else
        g_task_return_pointer(task, g_byte_array_free_to_bytes(g_steal_pointer(&bytes)),
                              (GDestroyNotify)g_bytes_unref);
}

static void png_ready(GObject *clipboard, GAsyncResult *result, gpointer unused)
{
    (void)unused;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) bytes = g_task_propagate_pointer(G_TASK(result), &error);
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning("Could not read clipboard PNG: %s", error->message);
    if (!bytes || g_task_get_cancellable(G_TASK(result)) != current_read)
        return;
    ScreenshotProvider *provider = g_object_new(screenshot_provider_get_type(), NULL);
    provider->png = g_steal_pointer(&bytes);
    if (!gdk_clipboard_set_content(GDK_CLIPBOARD(clipboard), GDK_CONTENT_PROVIDER(provider)))
        g_warning("Could not offer clipboard PNG and BMP");
    g_object_unref(provider);
}

static void clipboard_opened(GObject *clipboard, GAsyncResult *result, gpointer data)
{
    g_autoptr(GCancellable) request = data;
    g_autoptr(GError) error = NULL;
    GInputStream *stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(clipboard), result, NULL, &error);
    if (!stream) {
        if (!g_cancellable_is_cancelled(request) && error)
            g_warning("Could not open clipboard PNG: %s", error->message);
        return;
    }
    g_autoptr(GTask) task = g_task_new(clipboard, request, png_ready, NULL);
    g_task_set_task_data(task, stream, g_object_unref);
    g_task_run_in_thread(task, read_png);
}

static void clipboard_changed(GdkClipboard *clipboard, gpointer unused)
{
    (void)unused;
    if (current_read) {
        g_cancellable_cancel(current_read);
        g_clear_object(&current_read);
    }
    if (gdk_clipboard_is_local(clipboard))
        return;
    gsize count;
    GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
    const char *const *mimes = gdk_content_formats_get_mime_types(formats, &count);
    if (count != 1 || !g_str_equal(mimes[0], PNG))
        return;
    current_read = g_cancellable_new();
    gdk_clipboard_read_async(clipboard, (const char *[]) {PNG, NULL}, G_PRIORITY_DEFAULT,
                             current_read, clipboard_opened, g_object_ref(current_read));
}

static gboolean stop(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

int main(void)
{
    g_setenv("GDK_BACKEND", "x11", TRUE);
    if (!gtk_init_check()) {
        g_critical("XWayland display is unavailable");
        return 1;
    }
    GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    g_signal_connect(clipboard, "changed", G_CALLBACK(clipboard_changed), NULL);
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, stop, loop);
    g_unix_signal_add(SIGINT, stop, loop);
    g_message("Listening for PNG clipboard changes on XWayland");
    g_main_loop_run(loop);
    return 0;
}
