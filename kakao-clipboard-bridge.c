#define _GNU_SOURCE
#include "kakao-png-to-bmp.h"
#include <fcntl.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PNG "image/png"
#define BMP "image/bmp"
#define FILE_TRANSFER "application/vnd.portal.filetransfer"
#define PORTAL_FILES "application/vnd.portal.files"
#define GNOME_FILES "x-special/gnome-copied-files"
#define MAX_CLIPBOARD_BYTES (64U * 1024U * 1024U)
#define MAX_CLIPBOARD_FORMATS 32U

typedef struct {
    GdkContentProvider parent;
    GHashTable *contents;
    GdkContentFormats *formats;
    GdkContentFormats *storable;
    const char *generated_mime;
} ScreenshotProvider;

typedef GdkContentProviderClass ScreenshotProviderClass;
G_DEFINE_TYPE(ScreenshotProvider, screenshot_provider, GDK_TYPE_CONTENT_PROVIDER)

typedef struct {
    GOutputStream *stream;
    GBytes *converted;
    GCancellable *caller;
    GCancellable *cancel;
    gulong cancel_handler;
    guint timeout;
    int priority;
    gint64 started;
    const char *generated_mime;
} WriteRequest;

static GQueue conversion_queue = G_QUEUE_INIT;
static gboolean conversion_busy;
static GCancellable *current_read;

static void start_next_conversion(void);

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
    g_clear_pointer(&write->converted, g_bytes_unref);
    g_free(write);
}

static void finish_write(GTask *task, GError *error)
{
    WriteRequest *write = g_task_get_task_data(task);
    const char *generated_mime = write->generated_mime;
    if (error) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning("Could not serve clipboard image: %s", error->message);
        g_task_return_error(task, error);
    } else {
        if (generated_mime)
            g_message("Served requested %s (%" G_GINT64_FORMAT " ms)", generated_mime,
                      (g_get_monotonic_time() - write->started) / 1000);
        g_task_return_boolean(task, TRUE);
    }
    /* Release the decoded image before allowing a second conversion. */
    g_clear_pointer(&write->converted, g_bytes_unref);
    g_object_unref(task);
    if (generated_mime) {
        conversion_busy = FALSE;
        start_next_conversion();
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

static guint32 read_le32(const guint8 *bytes)
{
    guint32 value;
    memcpy(&value, bytes, sizeof value);
    return GUINT32_FROM_LE(value);
}

static GBytes *bmp_to_png(GBytes *bmp, GCancellable *cancel, GError **error)
{
    gsize length;
    const guint8 *data = g_bytes_get_data(bmp, &length);
    /* Wine exports a BMP file header followed by a Windows DIB. Limit the
     * decoded image size before handing it to the image loader. */
    if (length < 54 || data[0] != 'B' || data[1] != 'M' ||
        read_le32(data + 14) < 40 || read_le32(data + 14) > length - 14) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid Windows BMP header");
        return NULL;
    }
    gint64 width = (gint32)read_le32(data + 18);
    gint64 height = (gint32)read_le32(data + 22);
    if (height < 0)
        height = -height;
    if (width <= 0 || height == 0 || width > 16384 || height > 16384 ||
        width * height > 64000000) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "BMP exceeds image size limit");
        return NULL;
    }

    g_autoptr(GInputStream) input = g_memory_input_stream_new_from_bytes(bmp);
    g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_stream(input, cancel, error);
    if (!pixbuf || g_cancellable_set_error_if_cancelled(cancel, error))
        return NULL;
    char *png = NULL;
    gsize size;
    /* A low compression level saves CPU without changing the decoded pixels. */
    if (!gdk_pixbuf_save_to_buffer(pixbuf, &png, &size, "png", error, "compression", "1", NULL))
        return NULL;
    return g_bytes_new_take(png, size);
}

static void convert_image(GTask *task, gpointer source, gpointer unused, GCancellable *cancel)
{
    (void)unused;
    ScreenshotProvider *provider = source;
    GError *error = NULL;
    gboolean to_bmp = g_str_equal(provider->generated_mime, BMP);
    GBytes *original = g_hash_table_lookup(provider->contents, to_bmp ? PNG : BMP);
    GBytes *converted = to_bmp ? kakao_png_to_bmp(original, cancel, &error)
                               : bmp_to_png(original, cancel, &error);
    if (converted)
        g_task_return_pointer(task, converted, (GDestroyNotify)g_bytes_unref);
    else
        g_task_return_error(task, error);
}

static void conversion_ready(GObject *provider, GAsyncResult *result, gpointer data)
{
    (void)provider;
    GTask *task = data;
    WriteRequest *write = g_task_get_task_data(task);
    GError *error = NULL;
    write->converted = g_task_propagate_pointer(G_TASK(result), &error);
    if (write->converted)
        send_bytes(task, write->converted);
    else
        finish_write(task, error);
}

static void start_next_conversion(void)
{
    while (!conversion_busy && !g_queue_is_empty(&conversion_queue)) {
        GTask *task = g_queue_pop_head(&conversion_queue);
        if (g_task_return_error_if_cancelled(task)) {
            g_object_unref(task);
            continue;
        }
        conversion_busy = TRUE;
        g_autoptr(GTask) decode = g_task_new(g_task_get_source_object(task),
                                             g_task_get_cancellable(task), conversion_ready, task);
        g_task_run_in_thread(decode, convert_image);
    }
}

static void provider_write(GdkContentProvider *provider, const char *mime,
                           GOutputStream *stream, int priority, GCancellable *caller,
                           GAsyncReadyCallback callback, gpointer data)
{
    ScreenshotProvider *image = (ScreenshotProvider *)provider;
    GBytes *original = g_hash_table_lookup(image->contents, mime);
    gboolean generate = !original && g_str_equal(mime, image->generated_mime);
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    GTask *task = g_task_new(provider, cancel, callback, data);
    g_task_set_priority(task, priority);
    if ((!generate && !original) || (generate && conversion_queue.length >= 4)) {
        g_task_return_new_error(task, G_IO_ERROR, generate ? G_IO_ERROR_BUSY : G_IO_ERROR_NOT_SUPPORTED,
                                "%s", generate ? "Too many image conversion requests" : "Unsupported clipboard format");
        g_object_unref(task);
        return;
    }

    WriteRequest *write = g_new0(WriteRequest, 1);
    write->stream = g_object_ref(stream);
    write->cancel = g_object_ref(cancel);
    write->priority = priority;
    write->started = g_get_monotonic_time();
    write->generated_mime = generate ? image->generated_mime : NULL;
    if (caller) {
        write->caller = g_object_ref(caller);
        write->cancel_handler = g_cancellable_connect(caller, G_CALLBACK(cancel_write), cancel, NULL);
    }
    write->timeout = g_timeout_add_seconds(15, time_out_write, write);
    g_task_set_task_data(task, write, free_write);
    if (generate) {
        g_queue_push_tail(&conversion_queue, task);
        start_next_conversion();
    } else if (g_bytes_get_size(original)) {
        send_bytes(task, original);
    } else {
        finish_write(task, NULL);
    }
}

static gboolean provider_finish(GdkContentProvider *provider, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, provider), FALSE);
    return g_task_propagate_boolean(G_TASK(result), error);
}

static GdkContentFormats *provider_formats(GdkContentProvider *provider)
{
    return gdk_content_formats_ref(((ScreenshotProvider *)provider)->formats);
}

static GdkContentFormats *provider_storable(GdkContentProvider *provider)
{
    return gdk_content_formats_ref(((ScreenshotProvider *)provider)->storable);
}

static void provider_finalize(GObject *object)
{
    ScreenshotProvider *provider = (ScreenshotProvider *)object;
    g_hash_table_unref(provider->contents);
    gdk_content_formats_unref(provider->formats);
    gdk_content_formats_unref(provider->storable);
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

typedef struct {
    GdkClipboard *clipboard;
    GCancellable *cancel;
    char **mimes;
    gsize index;
    gsize remaining;
    GHashTable *contents;
    GInputStream *stream;
    const char *generated_mime;
    gboolean file_copy;
    GFile *file;
} Capture;

static void free_capture(Capture *capture)
{
    g_clear_object(&capture->stream);
    g_clear_object(&capture->file);
    g_hash_table_unref(capture->contents);
    g_strfreev(capture->mimes);
    g_object_unref(capture->cancel);
    g_object_unref(capture->clipboard);
    g_free(capture);
}

static void read_part(GTask *task, gpointer clipboard, gpointer data, GCancellable *cancel)
{
    (void)clipboard;
    Capture *capture = data;
    g_autoptr(GByteArray) bytes = g_byte_array_new();
    GError *error = NULL;
    guint8 buffer[65536];
    gssize n;
    while ((n = g_input_stream_read(capture->stream, buffer, sizeof buffer, cancel, &error)) > 0) {
        if ((gsize)n > capture->remaining - bytes->len) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Clipboard image exceeds size limit");
            break;
        }
        g_byte_array_append(bytes, buffer, n);
    }
    g_input_stream_close(capture->stream, NULL, NULL);
    if (error)
        g_task_return_error(task, error);
    else
        g_task_return_pointer(task, g_byte_array_free_to_bytes(g_steal_pointer(&bytes)),
                              (GDestroyNotify)g_bytes_unref);
}

static void capture_next(Capture *capture);

static void publish_capture(Capture *capture)
{
    ScreenshotProvider *provider = g_object_new(screenshot_provider_get_type(), NULL);
    provider->contents = g_hash_table_ref(capture->contents);
    provider->generated_mime = capture->generated_mime;
    GdkContentFormatsBuilder *formats = gdk_content_formats_builder_new();
    GHashTableIter iterator;
    gpointer mime;
    g_hash_table_iter_init(&iterator, capture->contents);
    while (g_hash_table_iter_next(&iterator, &mime, NULL))
        gdk_content_formats_builder_add_mime_type(formats, mime);
    provider->storable = gdk_content_formats_builder_free_to_formats(formats);
    provider->formats = gdk_content_formats_union(
        gdk_content_formats_new(&provider->generated_mime, 1), provider->storable);
    GdkContentProvider *output = GDK_CONTENT_PROVIDER(provider);
    if (capture->file) {
        /* The old portal token belongs to the original clipboard owner. */
        GFile *files[] = {capture->file};
        GdkFileList *list = gdk_file_list_new_from_array(files, G_N_ELEMENTS(files));
        GValue value = G_VALUE_INIT;
        g_value_init(&value, GDK_TYPE_FILE_LIST);
        g_value_take_boxed(&value, list);
        GdkContentProvider *file_provider = gdk_content_provider_new_for_value(&value);
        g_value_unset(&value);
        output = gdk_content_provider_new_union(
            (GdkContentProvider *[]) {output, file_provider}, 2);
    }
    if (!gdk_clipboard_set_content(capture->clipboard, output))
        g_warning("Could not offer clipboard image with %s", capture->generated_mime);
    g_object_unref(output);
    free_capture(capture);
}

static GBytes *load_file_image(const char *path, GCancellable *cancel)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    struct stat status;
    if (fstat(fd, &status) || !S_ISREG(status.st_mode) || status.st_size == 0 ||
        status.st_size > MAX_CLIPBOARD_BYTES) {
        close(fd);
        return NULL;
    }

    guint8 buffer[65536];
    ssize_t count = read(fd, buffer, 4096);
    if (count <= 0) {
        close(fd);
        return NULL;
    }
    g_autofree char *type = g_content_type_guess(NULL, buffer, count, NULL);
    g_autofree char *mime = g_content_type_get_mime_type(type);
    gboolean is_png = g_strcmp0(mime, PNG) == 0;
    if (!is_png && g_strcmp0(mime, BMP) && g_strcmp0(mime, "image/jpeg") &&
        g_strcmp0(mime, "image/webp")) {
        close(fd);
        return NULL;
    }
    g_autoptr(GByteArray) bytes = g_byte_array_sized_new(status.st_size);
    g_byte_array_append(bytes, buffer, count);
    while (!g_cancellable_is_cancelled(cancel) &&
           (count = read(fd, buffer, sizeof buffer)) > 0) {
        if ((gsize)count > MAX_CLIPBOARD_BYTES - bytes->len)
            break;
        g_byte_array_append(bytes, buffer, count);
    }
    close(fd);
    if (count != 0 || !bytes->len || g_cancellable_is_cancelled(cancel))
        return NULL;
    if (is_png)
        return g_byte_array_free_to_bytes(g_steal_pointer(&bytes));

    g_autoptr(GBytes) image = g_byte_array_free_to_bytes(g_steal_pointer(&bytes));
    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes(image);
    g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_stream(stream, cancel, NULL);
    if (!pixbuf || (guint64)gdk_pixbuf_get_width(pixbuf) * gdk_pixbuf_get_height(pixbuf) > 64000000 ||
        g_cancellable_is_cancelled(cancel))
        return NULL;
    g_autoptr(GdkPixbuf) oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf);
    if (!oriented)
        return NULL;
    g_autofree char *png = NULL;
    gsize length;
    if (!gdk_pixbuf_save_to_buffer(oriented, &png, &length, "png", NULL, "compression", "1", NULL) ||
        length > MAX_CLIPBOARD_BYTES)
        return NULL;
    return g_bytes_new_take(g_steal_pointer(&png), length);
}

static GBytes *retrieve_file_image(Capture *capture, GCancellable *cancel)
{
    GBytes *copy = g_hash_table_lookup(capture->contents, GNOME_FILES);
    GBytes *transfer = g_hash_table_lookup(capture->contents, FILE_TRANSFER);
    gsize length;
    const char *marker = g_bytes_get_data(copy, &length);
    const char *token = g_bytes_get_data(transfer, NULL);
    if (length < 5 || memcmp(marker, "copy\n", 5) != 0 ||
        !g_bytes_get_size(transfer) || g_bytes_get_size(transfer) > 256)
        return NULL;
    g_autofree char *key = g_strndup(token, g_bytes_get_size(transfer));
    if (!*key || !g_utf8_validate(key, -1, NULL))
        return NULL;

    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancel, NULL);
    if (!bus)
        return NULL;
    g_autoptr(GVariant) response = g_dbus_connection_call_sync(
        bus, "org.freedesktop.portal.Documents", "/org/freedesktop/portal/documents",
        "org.freedesktop.portal.FileTransfer", "RetrieveFiles",
        g_variant_new("(sa{sv})", key, NULL), G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE, 3000, cancel, NULL);
    if (!response)
        return NULL;
    g_auto(GStrv) files = NULL;
    g_variant_get(response, "(^as)", &files);
    if (!files[0] || files[1] || !g_path_is_absolute(files[0]))
        return NULL;
    GBytes *png = load_file_image(files[0], cancel);
    if (png)
        capture->file = g_file_new_for_path(files[0]);
    return png;
}

static void resolve_file_copy(GTask *task, gpointer clipboard, gpointer data, GCancellable *cancel)
{
    (void)clipboard;
    g_task_return_pointer(task, retrieve_file_image(data, cancel), (GDestroyNotify)g_bytes_unref);
}

static void file_copy_ready(GObject *clipboard, GAsyncResult *result, gpointer data)
{
    (void)clipboard;
    Capture *capture = data;
    GBytes *png = g_task_propagate_pointer(G_TASK(result), NULL);
    if (!png || capture->cancel != current_read) {
        g_clear_pointer(&png, g_bytes_unref);
        free_capture(capture);
        return;
    }
    g_hash_table_remove(capture->contents, FILE_TRANSFER);
    g_hash_table_insert(capture->contents, g_strdup(PNG), png);
    publish_capture(capture);
}

static void part_ready(GObject *clipboard, GAsyncResult *result, gpointer data)
{
    (void)clipboard;
    Capture *capture = data;
    g_autoptr(GError) error = NULL;
    GBytes *bytes = g_task_propagate_pointer(G_TASK(result), &error);
    g_clear_object(&capture->stream);
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning("Could not read clipboard image data: %s", error->message);
    if (!bytes) {
        free_capture(capture);
        return;
    }
    const char *original_mime = g_str_equal(capture->generated_mime, BMP) ? PNG : BMP;
    if (g_str_equal(capture->mimes[capture->index], original_mime) && g_bytes_get_size(bytes) == 0) {
        g_bytes_unref(bytes);
        free_capture(capture);
        return;
    }
    capture->remaining -= g_bytes_get_size(bytes);
    g_hash_table_insert(capture->contents, g_strdup(capture->mimes[capture->index]), bytes);
    capture->index++;
    capture_next(capture);
}

static void clipboard_opened(GObject *clipboard, GAsyncResult *result, gpointer data)
{
    Capture *capture = data;
    g_autoptr(GError) error = NULL;
    capture->stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(clipboard), result, NULL, &error);
    if (!capture->stream) {
        if (!g_cancellable_is_cancelled(capture->cancel) && error)
            g_warning("Could not open clipboard image data: %s", error->message);
        free_capture(capture);
        return;
    }
    g_autoptr(GTask) task = g_task_new(clipboard, capture->cancel, part_ready, capture);
    g_task_set_task_data(task, capture, NULL);
    g_task_run_in_thread(task, read_part);
}

static void capture_next(Capture *capture)
{
    if (capture->cancel != current_read || g_cancellable_is_cancelled(capture->cancel)) {
        free_capture(capture);
        return;
    }
    if (capture->mimes[capture->index]) {
        const char *mimes[] = {capture->mimes[capture->index], NULL};
        gdk_clipboard_read_async(capture->clipboard, mimes, G_PRIORITY_DEFAULT,
                                 capture->cancel, clipboard_opened, capture);
        return;
    }

    if (capture->file_copy) {
        g_autoptr(GTask) task = g_task_new(capture->clipboard, capture->cancel, file_copy_ready, capture);
        g_task_set_task_data(task, capture, NULL);
        g_task_run_in_thread(task, resolve_file_copy);
    } else {
        publish_capture(capture);
    }
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
    gboolean has_png = gdk_content_formats_contain_mime_type(formats, PNG);
    gboolean has_bmp = gdk_content_formats_contain_mime_type(formats, BMP);
    gboolean has_transfer = gdk_content_formats_contain_mime_type(formats, FILE_TRANSFER);
    gboolean has_gnome_files = gdk_content_formats_contain_mime_type(formats, GNOME_FILES);
    gboolean file_copy = !has_png && !has_bmp && has_transfer && has_gnome_files;
    if (count > MAX_CLIPBOARD_FORMATS ||
        (!file_copy && (has_png == has_bmp || has_transfer || has_gnome_files)))
        return;
    current_read = g_cancellable_new();
    Capture *capture = g_new0(Capture, 1);
    capture->generated_mime = has_png || file_copy ? BMP : PNG;
    capture->file_copy = file_copy;
    capture->clipboard = g_object_ref(clipboard);
    capture->cancel = g_object_ref(current_read);
    if (file_copy) {
        capture->mimes = g_strdupv((char *[]) {GNOME_FILES, FILE_TRANSFER, NULL});
        capture->remaining = 65536;
    } else {
        capture->mimes = g_new0(char *, count + 1);
        for (gsize i = 0; i < count; i++)
            capture->mimes[i] = g_strdup(mimes[i]);
        capture->remaining = MAX_CLIPBOARD_BYTES;
    }
    capture->contents = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref);
    capture_next(capture);
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
    g_message("Listening for clipboard images with PNG or BMP on XWayland");
    g_main_loop_run(loop);
    return 0;
}
