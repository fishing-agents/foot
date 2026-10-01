/* Headless Weston has no input devices, but Foot requires a wl_seat.
 * This test-only module supplies a real, device-less Weston seat; it does not
 * replace any terminal/parser/rendering behavior or fabricate PTY responses.
 * Weston 14 exports these backend APIs without installing their declarations.
 * Build: cc -shared -fPIC $(pkg-config --cflags --libs libweston-14)
 *        tests/weston-seat.c -o build/weston-seat.so
 */
#include <libweston/libweston.h>

void weston_seat_init(struct weston_seat *, struct weston_compositor *, const char *);
void weston_seat_release(struct weston_seat *);

static struct weston_seat seat;
static struct wl_listener destroy_listener;

static void on_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    wl_list_remove(&listener->link);
    weston_seat_release(&seat);
}

WL_EXPORT int wet_module_init(struct weston_compositor *compositor,
                              int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    weston_seat_init(&seat, compositor, "foot-test-seat");
    destroy_listener.notify = on_destroy;
    wl_signal_add(&compositor->destroy_signal, &destroy_listener);
    return 0;
}
