#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <pixman.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-server-protocol.h"
#include "webos-shell-client-protocol.h"

#define OUT_W 1920
#define OUT_H 1080
#define APP_ID "com.webosbrew.shim"
#define BTN_LEFT 0x110
#define KEY_Q 16
#define KEY_ENTER 28
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_UP 103
#define KEY_DOWN 108
#define MOD_CTRL 4
#define MOD_ALT 8
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

struct shim;
struct ssurf;

struct hbuf {
    struct shim *s;
    struct wl_buffer *buf;
    void *data;
    int busy;
};

struct hseat {
    struct shim *s;
    struct wl_seat *seat;
    struct wl_pointer *ptr;
    struct wl_keyboard *kb;
};

enum srole { ROLE_NONE, ROLE_TOPLEVEL, ROLE_POPUP, ROLE_CURSOR };

struct shim {
    struct wl_display *hdpy;
    struct wl_display *sdpy;
    struct wl_event_source *timer;
    int timer_armed;

    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_shm *hshm;
    struct wl_shell *hshell;
    struct wl_shell_surface *hss;
    struct wl_surface *hsurf;
    struct wl_webos_shell *wshell;
    struct wl_webos_shell_surface *wsurf;
    struct wl_output *houtput;
    int have_output;
    int debug;

    struct hseat seats[8];
    int nseats;

    int out_w, out_h;
    struct hbuf hbufs[2];
    int nhbufs;

    struct wl_list stack;
    struct ssurf *kb_focus, *ptr_focus;
    struct wl_resource *ckb, *cptr;

    int ptr_x, ptr_y;
    int host_ptr_inside;
    int ptr_seen, kb_seen, kb_keys, kb_vendor;
    int dirty;
    int ntoplevels, casc;
    int zseq;

    struct xkb_context *xctx;
    struct xkb_keymap *xkm;
    struct xkb_state *xst;

    int kmfd;
    uint32_t kmsize;
    uint32_t mods_depressed, mods_latched, mods_locked, mods_group;
    int host_kb_focus;
    uint32_t pressed[32];
    int npressed;

    uint64_t start_ms;
};

struct fcb {
    struct wl_resource *res;
    struct wl_list link;
};

struct spos {
    int32_t w, h;
    int32_t ax, ay, aw, ah;
    int32_t ox, oy;
    uint32_t anchor, gravity, constraint;
};

struct ssurf {
    struct shim *s;
    struct wl_resource *res;
    struct wl_resource *buf_res, *pending_res;
    struct wl_shm_buffer *shm, *pending_shm;
    struct wl_listener buf_listener;
    int has_pending;
    int buf_released;
    int mapped;
    int32_t x, y, w, h;
    enum srole role;
    int configured;
    int32_t sub_dx, sub_dy;
    struct ssurf *parent;
    struct wl_list sublist;
    struct wl_list stack_link;
    struct wl_list frames;
    int z;
    struct wl_resource *xdg_surface, *xdg_toplevel, *xdg_popup;
    struct spos pos;
    int32_t win_x, win_y, win_w, win_h;
};

static struct shim *S;
static void schedule(struct shim *s);
static void ptr_move(struct shim *s, int x, int y);
static void ptr_button(struct shim *s, uint32_t button, uint32_t state);
static void set_kb_focus(struct shim *s, struct ssurf *sfn);
static void send_tl_configure(struct ssurf *sfn);
static void send_pop_configure(struct ssurf *sfn);
static void paint(struct shim *s);

static void die(const char *m) { fprintf(stderr, "shim: %s: %s\n", m, strerror(errno)); exit(1); }

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void flush_host(struct shim *s) {
    if (s->hdpy) wl_display_flush(s->hdpy);
}

/* ---------------- client buffers (libwayland's built-in wl_shm) ---------------- */
static void release_buf(struct ssurf *sfn) {
    if (!sfn->buf_res || sfn->buf_released) return;
    wl_buffer_send_release(sfn->buf_res);
    sfn->buf_released = 1;
}

static void buf_gone(struct wl_listener *l, void *data) {
    (void)data;
    struct ssurf *sfn = wl_container_of(l, sfn, buf_listener);
    sfn->buf_res = NULL;
    sfn->buf_released = 1;
}

/* ---------------- region ---------------- */
static void region_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void region_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static void region_sub(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static const struct wl_region_interface region_impl = { .destroy = region_destroy, .add = region_add, .subtract = region_sub };

/* ---------------- forward declarations for roles ---------------- */
static void xdg_surface_res_destroy(struct wl_resource *r);
static void xdg_toplevel_res_destroy(struct wl_resource *r);
static void xdg_popup_res_destroy(struct wl_resource *r);
static void set_ptr_focus(struct shim *s, struct ssurf *sfn);
static const struct xdg_toplevel_interface xdg_toplevel_impl;
static const struct xdg_popup_interface xdg_popup_impl;

/* ---------------- wl_surface ---------------- */
static void srf_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

static void srf_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y) {
    (void)c; (void)x; (void)y;
    struct ssurf *sfn = wl_resource_get_user_data(r);
    struct wl_shm_buffer *shm = buf ? wl_shm_buffer_get(buf) : NULL;
    if (buf && !shm) {
        fprintf(stderr, "shim: ignoring non-shm buffer\n");
        return;
    }
    if (sfn->pending_shm) wl_shm_buffer_unref(sfn->pending_shm);
    sfn->pending_res = buf;
    sfn->pending_shm = shm;
    if (shm) wl_shm_buffer_ref(shm);
    sfn->has_pending = 1;
}

static void srf_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
    if (S) schedule(S);
}

static void frame_res_destroy(struct wl_resource *r) {
    struct fcb *f = wl_resource_get_user_data(r);
    if (f) { wl_list_remove(&f->link); free(f); }
}

static void srf_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct ssurf *sfn = wl_resource_get_user_data(r);
    struct wl_resource *cr = wl_resource_create(c, &wl_callback_interface, 1, id);
    if (!cr) return;
    struct fcb *f = calloc(1, sizeof(*f));
    if (!f) { wl_resource_destroy(cr); return; }
    f->res = cr;
    wl_list_insert(sfn->frames.prev, &f->link);
    wl_resource_set_implementation(cr, NULL, f, frame_res_destroy);
}

static void srf_set_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) { (void)c; (void)r; (void)reg; }
static void srf_set_transform(struct wl_client *c, struct wl_resource *r, int32_t t) { (void)c; (void)r; (void)t; }
static void srf_set_scale(struct wl_client *c, struct wl_resource *r, int32_t sc) { (void)c; (void)r; (void)sc; }

static void popup_anchor(struct ssurf *sfn, int32_t *px, int32_t *py, int32_t *pw, int32_t *ph) {
    struct spos *p = &sfn->pos;
    int32_t w = p->w > 0 ? p->w : sfn->w;
    int32_t h = p->h > 0 ? p->h : sfn->h;
    int32_t ax, ay;

    if (p->anchor & XDG_POSITIONER_ANCHOR_LEFT) ax = p->ax;
    else if (p->anchor & XDG_POSITIONER_ANCHOR_RIGHT) ax = p->ax + p->aw;
    else ax = p->ax + p->aw / 2;

    if (p->anchor & XDG_POSITIONER_ANCHOR_TOP) ay = p->ay;
    else if (p->anchor & XDG_POSITIONER_ANCHOR_BOTTOM) ay = p->ay + p->ah;
    else ay = p->ay + p->ah / 2;

    /* gravity is the direction the popup grows from the anchor point:
     * LEFT/RIGHT/TOP/BOTTOM mean the popup extends left/right/up/down. */
    if (p->gravity & XDG_POSITIONER_GRAVITY_LEFT) *px = ax - w;
    else if (p->gravity & XDG_POSITIONER_GRAVITY_RIGHT) *px = ax;
    else *px = ax - w / 2;

    if (p->gravity & XDG_POSITIONER_GRAVITY_TOP) {
        *py = ay - h;
    } else if (p->gravity & XDG_POSITIONER_GRAVITY_BOTTOM) {
        *py = ay;
    } else {
        /* GTK often omits the vertical gravity for menus.  A menu in the
         * upper half should open downwards, one near the bottom upwards. */
        *py = (ay < sfn->s->out_h / 2) ? ay : ay - h;
    }

    *px += p->ox;
    *py += p->oy;
    *pw = w;
    *ph = h;
}

static void popup_global(struct ssurf *sfn, int32_t *gx, int32_t *gy, int32_t *pw, int32_t *ph) {
    int32_t px, py;
    popup_anchor(sfn, &px, &py, pw, ph);
    int32_t ox = sfn->parent ? sfn->parent->x + sfn->parent->win_x : 0;
    int32_t oy = sfn->parent ? sfn->parent->y + sfn->parent->win_y : 0;
    int32_t x = ox + px;
    int32_t y = oy + py;
    int32_t w = *pw > 0 ? *pw : 1;
    int32_t h = *ph > 0 ? *ph : 1;
    if (x + w > sfn->s->out_w) x = sfn->s->out_w - w;
    if (y + h > sfn->s->out_h) y = sfn->s->out_h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    *gx = x;
    *gy = y;
}

static void place_surface(struct ssurf *sfn) {
    if (sfn->role == ROLE_POPUP && sfn->parent) {
        int32_t gx, gy, pw, ph;
        popup_global(sfn, &gx, &gy, &pw, &ph);
        /* requested coordinates refer to the window geometry, not the buffer */
        sfn->x = gx - sfn->win_x;
        sfn->y = gy - sfn->win_y;
        if (sfn->w <= 0 && pw > 0) sfn->w = pw;
        if (sfn->h <= 0 && ph > 0) sfn->h = ph;
    }
}

static int is_main_toplevel(struct ssurf *sfn) {
    return sfn->role == ROLE_TOPLEVEL &&
           sfn->w >= sfn->s->out_w - 2 && sfn->h >= sfn->s->out_h - 2;
}

static void stack_insert(struct shim *s, struct ssurf *sfn) {
    struct ssurf *it;
    wl_list_remove(&sfn->stack_link);
    wl_list_for_each(it, &s->stack, stack_link) {
        if (it->z > sfn->z) {
            /* insert before 'it' (wl_list_insert places the node after list) */
            wl_list_insert(it->stack_link.prev, &sfn->stack_link);
            return;
        }
    }
    wl_list_insert(s->stack.prev, &sfn->stack_link);
}

static void raise_toplevel(struct shim *s, struct ssurf *sfn) {
    if (sfn->role != ROLE_TOPLEVEL || is_main_toplevel(sfn)) return;
    sfn->z = 100 + (++s->zseq);
    stack_insert(s, sfn);
    s->dirty = 1;
    schedule(s);
}

static void srf_commit(struct wl_client *c, struct wl_resource *r) {
    (void)c;
    struct ssurf *sfn = wl_resource_get_user_data(r);
    struct shim *s = sfn->s;

    if (sfn->has_pending) {
        sfn->has_pending = 0;
        if (sfn->pending_res && sfn->pending_res == sfn->buf_res) {
            /* same buffer re-attached: drop the extra reference from attach */
            if (sfn->pending_shm) wl_shm_buffer_unref(sfn->pending_shm);
            sfn->pending_shm = NULL;
            sfn->pending_res = NULL;
            sfn->buf_released = 0;
        } else {
            if (sfn->buf_res) {
                release_buf(sfn);
                wl_list_remove(&sfn->buf_listener.link);
            }
            if (sfn->shm) wl_shm_buffer_unref(sfn->shm);
            sfn->buf_res = sfn->pending_res;
            sfn->shm = sfn->pending_shm;
            sfn->pending_res = NULL;
            sfn->pending_shm = NULL;
            sfn->buf_released = 0;
            if (sfn->buf_res) {
                wl_list_init(&sfn->buf_listener.link);
                wl_resource_add_destroy_listener(sfn->buf_res, &sfn->buf_listener);
            }
            if (sfn->shm) {
                sfn->w = wl_shm_buffer_get_width(sfn->shm);
                sfn->h = wl_shm_buffer_get_height(sfn->shm);
            } else {
                sfn->w = 0;
                sfn->h = 0;
            }
        }
    }

    if (sfn->role == ROLE_TOPLEVEL && !sfn->configured) send_tl_configure(sfn);
    if (sfn->role == ROLE_POPUP && !sfn->configured) send_pop_configure(sfn);

    int was = sfn->mapped;
    sfn->mapped = (sfn->shm != NULL) && sfn->role != ROLE_CURSOR && sfn->role != ROLE_NONE;

    if (sfn->mapped) {
        place_surface(sfn);
        if (!was) {
            if (sfn->role == ROLE_TOPLEVEL)
                sfn->z = is_main_toplevel(sfn) ? 0 : 100 + (++s->zseq);
            else if (sfn->role == ROLE_POPUP)
                sfn->z = 1000 + (++s->zseq);
            else
                sfn->z = 500;
            stack_insert(s, sfn);
            fprintf(stderr, "shim: map role=%d %dx%d at %d,%d z=%d\n",
                    (int)sfn->role, sfn->w, sfn->h, sfn->x, sfn->y, sfn->z);
        }
        if (!was && sfn->role == ROLE_TOPLEVEL) {
            s->ntoplevels++;
            if (s->ntoplevels == 1) { sfn->x = 0; sfn->y = 0; }
            else { sfn->x = 60 * (s->casc % 6); sfn->y = 60 * (s->casc % 6); s->casc++; }
            set_kb_focus(s, sfn);
        }
    } else if (was && sfn->role == ROLE_TOPLEVEL) {
        if (s->ntoplevels > 0) s->ntoplevels--;
        wl_list_remove(&sfn->stack_link);
    }

    s->dirty = 1;
    schedule(s);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = srf_destroy, .attach = srf_attach, .damage = srf_damage, .frame = srf_frame,
    .set_opaque_region = srf_set_region, .set_input_region = srf_set_region,
    .commit = srf_commit, .set_buffer_transform = srf_set_transform,
    .set_buffer_scale = srf_set_scale, .damage_buffer = srf_damage,
};

static void srf_res_destroy(struct wl_resource *r) {
    struct ssurf *sfn = wl_resource_get_user_data(r);
    if (!sfn) return;
    struct shim *s = sfn->s;
    if (s->kb_focus == sfn) s->kb_focus = NULL;
    if (s->ptr_focus == sfn) s->ptr_focus = NULL;

    struct fcb *f, *tmp;
    wl_list_for_each_safe(f, tmp, &sfn->frames, link)
        wl_resource_destroy(f->res);

    if (sfn->buf_res) wl_list_remove(&sfn->buf_listener.link);
    if (sfn->shm) { wl_shm_buffer_unref(sfn->shm); sfn->shm = NULL; }
    if (sfn->pending_shm) { wl_shm_buffer_unref(sfn->pending_shm); sfn->pending_shm = NULL; }
    if (sfn->mapped && sfn->role == ROLE_TOPLEVEL && s->ntoplevels > 0) s->ntoplevels--;
    wl_list_remove(&sfn->stack_link);

    if (sfn->xdg_toplevel) { wl_resource_set_user_data(sfn->xdg_toplevel, NULL); wl_resource_destroy(sfn->xdg_toplevel); sfn->xdg_toplevel = NULL; }
    if (sfn->xdg_popup) { wl_resource_set_user_data(sfn->xdg_popup, NULL); wl_resource_destroy(sfn->xdg_popup); sfn->xdg_popup = NULL; }
    if (sfn->xdg_surface) { wl_resource_set_user_data(sfn->xdg_surface, NULL); wl_resource_destroy(sfn->xdg_surface); sfn->xdg_surface = NULL; }

    s->dirty = 1;
    free(sfn);
}

static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct shim *s = wl_resource_get_user_data(r);
    struct ssurf *sfn = calloc(1, sizeof(*sfn));
    if (!sfn) { wl_client_post_no_memory(c); return; }
    sfn->s = s;
    wl_list_init(&sfn->stack_link); wl_list_init(&sfn->sublist); wl_list_init(&sfn->frames);
    sfn->buf_listener.notify = buf_gone;
    wl_list_init(&sfn->buf_listener.link);
    sfn->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    if (!sfn->res) { free(sfn); return; }
    wl_resource_set_implementation(sfn->res, &surface_impl, sfn, srf_res_destroy);
}

static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *rr = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(r), id);
    if (rr) wl_resource_set_implementation(rr, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = comp_create_surface, .create_region = comp_create_region,
};

static void bind_compositor(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    uint32_t v = ver > 4 ? 4 : ver;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, v, id);
    if (r) wl_resource_set_implementation(r, &compositor_impl, d, NULL);
}

/* ---------------- wl_output ---------------- */
static void output_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_output_interface output_impl = { .release = output_release };

static void bind_output(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct shim *s = d;
    uint32_t v = ver > 3 ? 3 : ver;
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, v, id);
    if (!r) return;
    wl_resource_set_implementation(r, &output_impl, s, NULL);
    wl_output_send_geometry(r, 0, 0, 340, 190, WL_OUTPUT_SUBPIXEL_UNKNOWN, "LG", "webOS", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, s->out_w, s->out_h, 60000);
    if (v >= 2) wl_output_send_scale(r, 1);
    if (v >= 2) wl_output_send_done(r);
}

/* ---------------- client seat ---------------- */
static void ckb_destroy_res(struct wl_resource *r) {
    struct shim *s = wl_resource_get_user_data(r);
    if (s && s->ckb == r) s->ckb = NULL;
}
static void ckb_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_keyboard_interface keyboard_impl = { .release = ckb_release };

static void cptr_destroy_res(struct wl_resource *r) {
    struct shim *s = wl_resource_get_user_data(r);
    if (s && s->cptr == r) s->cptr = NULL;
}
static void cptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial,
                            struct wl_resource *surf, int32_t hx, int32_t hy) {
    (void)c; (void)r; (void)serial; (void)hx; (void)hy;
    if (!surf) return;
    struct ssurf *sfn = wl_resource_get_user_data(surf);
    if (sfn && sfn->role == ROLE_NONE) {
        sfn->role = ROLE_CURSOR;
        sfn->mapped = 0;
        wl_list_remove(&sfn->stack_link);
        sfn->s->dirty = 1;
        schedule(sfn->s);
    }
}
static void cptr_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = { .set_cursor = cptr_set_cursor, .release = cptr_release };

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct shim *s = wl_resource_get_user_data(r);
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    if (!p) return;
    wl_resource_set_implementation(p, &pointer_impl, s, cptr_destroy_res);
    s->cptr = p;
    if (s->ptr_focus) {
        int lx = s->ptr_x - s->ptr_focus->x;
        int ly = s->ptr_y - s->ptr_focus->y;
        wl_pointer_send_enter(p, wl_display_next_serial(s->sdpy), s->ptr_focus->res,
                              wl_fixed_from_int(lx), wl_fixed_from_int(ly));
    }
}

static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct shim *s = wl_resource_get_user_data(r);
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    if (!k) return;
    wl_resource_set_implementation(k, &keyboard_impl, s, ckb_destroy_res);
    s->ckb = k;
    if (s->kmfd >= 0 && s->kmsize > 0)
        wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, s->kmfd, s->kmsize);
    if (wl_resource_get_version(k) >= 4)
        wl_keyboard_send_repeat_info(k, 25, 400);
    wl_keyboard_send_modifiers(k, wl_display_next_serial(s->sdpy),
                               s->mods_depressed, s->mods_latched, s->mods_locked, s->mods_group);
    if (s->kb_focus) {
        struct wl_array a; wl_array_init(&a);
        for (int i = 0; i < s->npressed; i++) {
            uint32_t *v = wl_array_add(&a, sizeof(uint32_t));
            if (v) *v = s->pressed[i];
        }
        wl_keyboard_send_enter(k, wl_display_next_serial(s->sdpy), s->kb_focus->res, &a);
        wl_array_release(&a);
    }
}

static void ctouch_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_touch_interface touch_impl = { .release = ctouch_release };
static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *t = wl_resource_create(c, &wl_touch_interface, wl_resource_get_version(r), id);
    if (t) wl_resource_set_implementation(t, &touch_impl, NULL, NULL);
}
static void seat_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer, .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch, .release = seat_release,
};

/* ---------------- minimal wl_data_device_manager (GTK requires the global) ---------------- */
static void dsrc_offer(struct wl_client *c, struct wl_resource *r, const char *mime) { (void)c; (void)r; (void)mime; }
static void dsrc_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void dsrc_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; (void)r; (void)a; }
static const struct wl_data_source_interface data_source_impl = {
    .offer = dsrc_offer, .destroy = dsrc_destroy, .set_actions = dsrc_set_actions,
};

static void ddev_start_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *src,
                            struct wl_resource *origin, struct wl_resource *icon, uint32_t serial) {
    (void)c; (void)r; (void)src; (void)origin; (void)icon; (void)serial;
}
static void ddev_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, uint32_t serial) {
    (void)c; (void)r; (void)src; (void)serial;
}
static void ddev_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_data_device_interface data_device_impl = {
    .start_drag = ddev_start_drag, .set_selection = ddev_set_selection, .release = ddev_release,
};

static void ddm_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *sr = wl_resource_create(c, &wl_data_source_interface, 1, id);
    if (sr) wl_resource_set_implementation(sr, &data_source_impl, NULL, NULL);
}
static void ddm_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    (void)seat;
    struct wl_resource *dr = wl_resource_create(c, &wl_data_device_interface, 1, id);
    if (dr) wl_resource_set_implementation(dr, &data_device_impl, NULL, NULL);
}
static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_source, .get_data_device = ddm_get_device,
};
static void bind_ddm(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    (void)ver; (void)d;
    struct wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, 1, id);
    if (r) wl_resource_set_implementation(r, &ddm_impl, NULL, NULL);
}

static void bind_seat(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    struct shim *s = d;
    uint32_t v = ver > 4 ? 4 : ver;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, v, id);
    if (!r) return;
    wl_resource_set_implementation(r, &seat_impl, s, NULL);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (v >= 2) wl_seat_send_name(r, "seat0");
}

/* ---------------- xdg_positioner ---------------- */
static void pos_destroy_res(struct wl_resource *r) {
    struct spos *p = wl_resource_get_user_data(r);
    free(p);
}
static void pos_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void pos_set_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; struct spos *p = wl_resource_get_user_data(r); p->w = w; p->h = h; }
static void pos_set_anchor_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; struct spos *p = wl_resource_get_user_data(r); p->ax = x; p->ay = y; p->aw = w; p->ah = h; }
static void pos_set_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; struct spos *p = wl_resource_get_user_data(r); p->anchor = a; }
static void pos_set_gravity(struct wl_client *c, struct wl_resource *r, uint32_t g) { (void)c; struct spos *p = wl_resource_get_user_data(r); p->gravity = g; }
static void pos_set_constraint(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; struct spos *p = wl_resource_get_user_data(r); p->constraint = a; }
static void pos_set_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c; struct spos *p = wl_resource_get_user_data(r); p->ox = x; p->oy = y; }
static void pos_set_reactive(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void pos_set_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; (void)r; (void)w; (void)h; }
static void pos_set_parent_configure(struct wl_client *c, struct wl_resource *r, uint32_t s) { (void)c; (void)r; (void)s; }
static const struct xdg_positioner_interface positioner_impl = {
    .destroy = pos_destroy, .set_size = pos_set_size, .set_anchor_rect = pos_set_anchor_rect,
    .set_anchor = pos_set_anchor, .set_gravity = pos_set_gravity,
    .set_constraint_adjustment = pos_set_constraint, .set_offset = pos_set_offset,
    .set_reactive = pos_set_reactive, .set_parent_size = pos_set_parent_size,
    .set_parent_configure = pos_set_parent_configure,
};

static void wm_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct spos *p = calloc(1, sizeof(*p));
    if (!p) { wl_client_post_no_memory(c); return; }
    struct wl_resource *pr = wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(r), id);
    if (!pr) { free(p); return; }
    wl_resource_set_implementation(pr, &positioner_impl, p, pos_destroy_res);
}

/* ---------------- xdg_surface ---------------- */
static void xdg_surface_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }

static void send_tl_configure(struct ssurf *sfn) {
    if (!sfn->xdg_toplevel || !sfn->xdg_surface) return;
    uint32_t states[2] = { XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_ACTIVATED };
    struct wl_array a;
    wl_array_init(&a);
    for (int i = 0; i < 2; i++) {
        uint32_t *v = wl_array_add(&a, sizeof(uint32_t));
        if (v) *v = states[i];
    }
    xdg_toplevel_send_configure(sfn->xdg_toplevel, sfn->s->out_w, sfn->s->out_h, &a);
    wl_array_release(&a);
    xdg_surface_send_configure(sfn->xdg_surface, wl_display_next_serial(sfn->s->sdpy));
    sfn->configured = 1;
}

static void send_pop_configure(struct ssurf *sfn) {
    if (!sfn->xdg_popup || !sfn->xdg_surface) return;
    int32_t gx, gy, pw, ph;
    popup_global(sfn, &gx, &gy, &pw, &ph);
    int32_t ox = sfn->parent ? sfn->parent->x + sfn->parent->win_x : 0;
    int32_t oy = sfn->parent ? sfn->parent->y + sfn->parent->win_y : 0;
    xdg_popup_send_configure(sfn->xdg_popup, gx - ox, gy - oy, pw, ph);
    xdg_surface_send_configure(sfn->xdg_surface, wl_display_next_serial(sfn->s->sdpy));
    sfn->configured = 1;
}

static void xs_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct ssurf *sfn = wl_resource_get_user_data(r);
    if (!sfn) { wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "no surface"); return; }
    if (sfn->role != ROLE_NONE) { wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "role already set"); return; }
    struct wl_resource *tr = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    if (!tr) return;
    sfn->role = ROLE_TOPLEVEL;
    sfn->xdg_toplevel = tr;
    wl_resource_set_implementation(tr, &xdg_toplevel_impl, sfn, xdg_toplevel_res_destroy);
}

static void xs_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id,
                         struct wl_resource *parent, struct wl_resource *positioner) {
    struct ssurf *sfn = wl_resource_get_user_data(r);
    if (!sfn) return;
    if (sfn->role != ROLE_NONE) { wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "role already set"); return; }
    struct ssurf *ps = parent ? wl_resource_get_user_data(parent) : NULL;
    struct spos *pos = positioner ? wl_resource_get_user_data(positioner) : NULL;
    struct wl_resource *pr = wl_resource_create(c, &xdg_popup_interface, wl_resource_get_version(r), id);
    if (!pr) return;
    sfn->role = ROLE_POPUP;
    sfn->parent = ps;
    if (pos) sfn->pos = *pos;
    if (sfn->s->debug)
        fprintf(stderr, "shim: popup size=%dx%d anchor_rect=%d,%d %dx%d anchor=0x%x gravity=0x%x constraint=0x%x off=%d,%d\n",
                sfn->pos.w, sfn->pos.h, sfn->pos.ax, sfn->pos.ay, sfn->pos.aw, sfn->pos.ah,
                sfn->pos.anchor, sfn->pos.gravity, sfn->pos.constraint, sfn->pos.ox, sfn->pos.oy);
    sfn->xdg_popup = pr;
    wl_resource_set_implementation(pr, &xdg_popup_impl, sfn, xdg_popup_res_destroy);
}

static void xs_set_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c;
    struct ssurf *sfn = wl_resource_get_user_data(r);
    if (sfn) {
        if ((sfn->win_x != x || sfn->win_y != y || sfn->win_w != w || sfn->win_h != h) &&
            sfn->s->debug)
            fprintf(stderr, "shim: geometry role=%d %d,%d %dx%d\n",
                    (int)sfn->role, x, y, w, h);
        sfn->win_x = x;
        sfn->win_y = y;
        sfn->win_w = w;
        sfn->win_h = h;
    }
}
static void xs_ack_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
    (void)c; (void)serial;
    struct ssurf *sfn = wl_resource_get_user_data(r);
    if (sfn) sfn->configured = 2;
}
static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy, .get_toplevel = xs_get_toplevel,
    .get_popup = xs_get_popup, .set_window_geometry = xs_set_geometry,
    .ack_configure = xs_ack_configure,
};

static void xdg_surface_res_destroy(struct wl_resource *r) {
    struct ssurf *sfn = wl_resource_get_user_data(r);
    if (sfn && sfn->xdg_surface == r) sfn->xdg_surface = NULL;
}

static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface) {
    struct ssurf *sfn = wl_resource_get_user_data(surface);
    if (!sfn) return;
    if (sfn->xdg_surface) { wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE, "xdg_surface exists"); return; }
    struct wl_resource *sr = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    if (!sr) return;
    sfn->xdg_surface = sr;
    wl_resource_set_implementation(sr, &xdg_surface_impl, sfn, xdg_surface_res_destroy);
}

/* ---------------- xdg_toplevel ---------------- */
static void tl_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void tl_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) { (void)c; (void)r; (void)p; }
static void tl_set_title(struct wl_client *c, struct wl_resource *r, const char *t) { (void)c; (void)r; (void)t; }
static void tl_set_app_id(struct wl_client *c, struct wl_resource *r, const char *a) { (void)c; (void)r; (void)a; }
static void tl_show_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) { (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y; }
static void tl_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) { (void)c; (void)r; (void)seat; (void)serial; }
static void tl_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges) { (void)c; (void)r; (void)seat; (void)serial; (void)edges; }
static void tl_set_max_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; (void)r; (void)w; (void)h; }
static void tl_set_min_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; (void)r; (void)w; (void)h; }
static void tl_set_maximized(struct wl_client *c, struct wl_resource *r) { (void)c; struct ssurf *sfn = wl_resource_get_user_data(r); if (sfn) { sfn->configured = 0; send_tl_configure(sfn); } }
static void tl_unset_maximized(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void tl_set_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *o) { (void)c; (void)o; struct ssurf *sfn = wl_resource_get_user_data(r); if (sfn) { sfn->configured = 0; send_tl_configure(sfn); } }
static void tl_unset_fullscreen(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void tl_set_minimized(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy = tl_destroy, .set_parent = tl_set_parent, .set_title = tl_set_title,
    .set_app_id = tl_set_app_id, .show_window_menu = tl_show_menu,
    .move = tl_move, .resize = tl_resize, .set_max_size = tl_set_max_size,
    .set_min_size = tl_set_min_size, .set_maximized = tl_set_maximized,
    .unset_maximized = tl_unset_maximized, .set_fullscreen = tl_set_fullscreen,
    .unset_fullscreen = tl_unset_fullscreen, .set_minimized = tl_set_minimized,
};

static void xdg_toplevel_res_destroy(struct wl_resource *r) {
    struct ssurf *sfn = wl_resource_get_user_data(r);
    if (sfn && sfn->xdg_toplevel == r) sfn->xdg_toplevel = NULL;
}

/* ---------------- xdg_popup ---------------- */
static void pop_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void pop_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) { (void)c; (void)r; (void)seat; (void)serial; }
static void pop_reposition(struct wl_client *c, struct wl_resource *r, struct wl_resource *pos, uint32_t token) {
    (void)c;
    struct ssurf *sfn = wl_resource_get_user_data(r);
    struct spos *p = wl_resource_get_user_data(pos);
    if (!sfn) return;
    if (p) sfn->pos = *p;
    sfn->configured = 0;
    send_pop_configure(sfn);
    if (sfn->xdg_popup) xdg_popup_send_repositioned(sfn->xdg_popup, token);
}
static const struct xdg_popup_interface xdg_popup_impl = {
    .destroy = pop_destroy, .grab = pop_grab, .reposition = pop_reposition,
};

static void xdg_popup_res_destroy(struct wl_resource *r) {
    struct ssurf *sfn = wl_resource_get_user_data(r);
    if (sfn && sfn->xdg_popup == r) sfn->xdg_popup = NULL;
}

/* ---------------- xdg_wm_base ---------------- */
static void wm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) { (void)c; (void)r; (void)serial; }
static const struct xdg_wm_base_interface xdg_wm_base_impl = {
    .destroy = wm_destroy, .create_positioner = wm_create_positioner,
    .get_xdg_surface = wm_get_xdg_surface, .pong = wm_pong,
};

static void bind_xdg_wm_base(struct wl_client *c, void *d, uint32_t ver, uint32_t id) {
    uint32_t v = ver > 3 ? 3 : ver;
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, v, id);
    if (r) wl_resource_set_implementation(r, &xdg_wm_base_impl, d, NULL);
}

/* ---------------- focus / input ---------------- */
static struct ssurf *toplevel_of(struct ssurf *sfn) {
    while (sfn && sfn->role == ROLE_POPUP) sfn = sfn->parent;
    return sfn;
}

static struct ssurf *find_surface(struct shim *s, int x, int y) {
    struct ssurf *sfn;
    wl_list_for_each_reverse(sfn, &s->stack, stack_link) {
        if (!sfn->mapped || !sfn->shm) continue;
        if (sfn->role != ROLE_TOPLEVEL && sfn->role != ROLE_POPUP) continue;
        if (x >= sfn->x && x < sfn->x + sfn->w &&
            y >= sfn->y && y < sfn->y + sfn->h)
            return sfn;
    }
    return NULL;
}

static void set_ptr_focus(struct shim *s, struct ssurf *sfn) {
    if (s->ptr_focus == sfn) return;
    uint32_t serial = wl_display_next_serial(s->sdpy);
    if (s->ptr_focus && s->cptr)
        wl_pointer_send_leave(s->cptr, serial, s->ptr_focus->res);
    s->ptr_focus = sfn;
    if (sfn && s->cptr) {
        int lx = s->ptr_x - sfn->x;
        int ly = s->ptr_y - sfn->y;
        wl_pointer_send_enter(s->cptr, serial, sfn->res,
                              wl_fixed_from_int(lx), wl_fixed_from_int(ly));
    }
}

static void ptr_move(struct shim *s, int x, int y) {
    s->ptr_x = MAX(0, MIN(x, s->out_w - 1));
    s->ptr_y = MAX(0, MIN(y, s->out_h - 1));
    struct ssurf *t = find_surface(s, s->ptr_x, s->ptr_y);
    set_ptr_focus(s, t);
    if (s->ptr_focus && s->cptr)
        wl_pointer_send_motion(s->cptr, (uint32_t)now_ms(),
                               wl_fixed_from_int(s->ptr_x - s->ptr_focus->x),
                               wl_fixed_from_int(s->ptr_y - s->ptr_focus->y));
    schedule(s);
}

static void dismiss_popups(struct shim *s, struct ssurf *keep) {
    struct ssurf *sfn;
    wl_list_for_each(sfn, &s->stack, stack_link) {
        if (sfn->role == ROLE_POPUP && sfn->mapped && sfn != keep && sfn->xdg_popup) {
            xdg_popup_send_popup_done(sfn->xdg_popup);
            sfn->mapped = 0;
        }
    }
}

static void ptr_button(struct shim *s, uint32_t button, uint32_t state) {
    struct ssurf *t = find_surface(s, s->ptr_x, s->ptr_y);
    set_ptr_focus(s, t);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (t && t->role != ROLE_POPUP) dismiss_popups(s, NULL);
        else if (!t) dismiss_popups(s, NULL);
        if (t && t->role == ROLE_TOPLEVEL) {
            set_kb_focus(s, t);
            raise_toplevel(s, t);
        }
    }
    if (s->ptr_focus && s->cptr)
        wl_pointer_send_button(s->cptr, wl_display_next_serial(s->sdpy),
                               (uint32_t)now_ms(), button, state);
    schedule(s);
}

static void set_kb_focus(struct shim *s, struct ssurf *sfn) {
    if (s->kb_focus == sfn) return;
    uint32_t serial = wl_display_next_serial(s->sdpy);
    if (s->kb_focus && s->ckb)
        wl_keyboard_send_leave(s->ckb, serial, s->kb_focus->res);
    s->kb_focus = sfn;
    if (sfn && s->ckb) {
        struct wl_array a; wl_array_init(&a);
        for (int i = 0; i < s->npressed; i++) {
            uint32_t *v = wl_array_add(&a, sizeof(uint32_t));
            if (v) *v = s->pressed[i];
        }
        wl_keyboard_send_enter(s->ckb, serial, sfn->res, &a);
        wl_array_release(&a);
    }
}

/* ---------------- host input listeners ---------------- */
static void host_ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                           struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y) {
    (void)p; (void)serial;
    struct hseat *hs = d;
    if (surf != hs->s->hsurf) return;
    if (!hs->s->ptr_seen++) {
        if (hs->s->debug) fprintf(stderr, "shim: first pointer ENTER\n");
    }
    hs->s->host_ptr_inside = 1;
    ptr_move(hs->s, wl_fixed_to_int(x), wl_fixed_to_int(y));
}
static void host_ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surf) {
    (void)p; (void)serial; (void)surf;
    struct hseat *hs = d;
    hs->s->host_ptr_inside = 0;
}
static void host_ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)p; (void)time;
    struct hseat *hs = d;
    if (!hs->s->host_ptr_inside) return;
    ptr_move(hs->s, wl_fixed_to_int(x), wl_fixed_to_int(y));
}
static void host_ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
                            uint32_t button, uint32_t state) {
    (void)p; (void)serial; (void)time;
    struct hseat *hs = d;
    if (!hs->s->host_ptr_inside) return;
    if (hs->s->debug)
        fprintf(stderr, "shim: pointer button %u state %u at %d,%d\n",
                button, state, hs->s->ptr_x, hs->s->ptr_y);
    ptr_button(hs->s, button, state);
}
static void host_ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)p;
    struct hseat *hs = d;
    if (hs->s->cptr && hs->s->ptr_focus)
        wl_pointer_send_axis(hs->s->cptr, time, axis, value);
}
static const struct wl_pointer_listener host_ptr_listener = {
    .enter = host_ptr_enter, .leave = host_ptr_leave, .motion = host_ptr_motion,
    .button = host_ptr_button, .axis = host_ptr_axis,
};

static void host_kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t sz) {
    (void)k;
    struct hseat *hs = d;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    int nfd = dup(fd);
    close(fd);
    if (nfd < 0) return;
    if (hs->s->kmfd >= 0) close(hs->s->kmfd);
    hs->s->kmfd = nfd;
    hs->s->kmsize = sz;
    if (hs->s->ckb)
        wl_keyboard_send_keymap(hs->s->ckb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, nfd, sz);
}
static void host_kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surf, struct wl_array *keys) {
    (void)k; (void)serial; (void)surf; (void)keys;
    struct hseat *hs = d;
    hs->s->host_kb_focus = 1;
}
static void host_kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surf) {
    (void)k; (void)serial; (void)surf;
    struct hseat *hs = d;
    hs->s->host_kb_focus = 0;
}

static void press_add(struct shim *s, uint32_t key) {
    for (int i = 0; i < s->npressed; i++) if (s->pressed[i] == key) return;
    if (s->npressed < (int)(sizeof(s->pressed) / sizeof(s->pressed[0])))
        s->pressed[s->npressed++] = key;
}
static void press_remove(struct shim *s, uint32_t key) {
    for (int i = 0; i < s->npressed; i++) {
        if (s->pressed[i] == key) {
            memmove(&s->pressed[i], &s->pressed[i + 1], (s->npressed - i - 1) * sizeof(uint32_t));
            s->npressed--;
            return;
        }
    }
}

static void host_kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time,
                        uint32_t key, uint32_t state) {
    (void)k; (void)serial;
    struct hseat *hs = d;
    struct shim *s = hs->s;
    int ctrl = (s->mods_depressed & MOD_CTRL) != 0;
    int alt = (s->mods_depressed & MOD_ALT) != 0;

    /* webOS remote/vendor keys live above the Linux KEY_MAX range.  They are
     * not real keyboard symbols; forwarding them makes GTK/LibreOffice
     * insert replacement characters. */
    if (key > 0x2ff) {
        if (s->kb_vendor < 8) {
            fprintf(stderr, "shim: dropping vendor key %u state %u\n", key, state);
            s->kb_vendor++;
        }
        return;
    }

    if (!s->kb_seen++) {
        if (s->debug) fprintf(stderr, "shim: first keyboard KEY\n");
    }
    if (s->debug && s->kb_keys < 20) {
        fprintf(stderr, "shim: key code=%u state=%u mods=0x%x\n", key, state, s->mods_depressed);
        s->kb_keys++;
    }

    if (ctrl && alt) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            if (key == KEY_Q) { wl_display_terminate(s->sdpy); return; }
            if (key == KEY_LEFT)  { ptr_move(s, s->ptr_x - 40, s->ptr_y); return; }
            if (key == KEY_RIGHT) { ptr_move(s, s->ptr_x + 40, s->ptr_y); return; }
            if (key == KEY_UP)    { ptr_move(s, s->ptr_x, s->ptr_y - 40); return; }
            if (key == KEY_DOWN)  { ptr_move(s, s->ptr_x, s->ptr_y + 40); return; }
            if (key == KEY_ENTER) {
                ptr_button(s, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
                ptr_button(s, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                return;
            }
        } else if (key == KEY_ENTER) {
            return;
        }
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) press_add(s, key);
    else press_remove(s, key);

    if (s->ckb && s->kb_focus)
        wl_keyboard_send_key(s->ckb, wl_display_next_serial(s->sdpy), time, key, state);
}

static void host_kb_mods(void *d, struct wl_keyboard *k, uint32_t serial,
                         uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
    (void)k; (void)serial;
    struct hseat *hs = d;
    hs->s->mods_depressed = dep;
    hs->s->mods_latched = lat;
    hs->s->mods_locked = lock;
    hs->s->mods_group = grp;
    if (hs->s->ckb)
        wl_keyboard_send_modifiers(hs->s->ckb, wl_display_next_serial(hs->s->sdpy), dep, lat, lock, grp);
}

static const struct wl_keyboard_listener host_kb_listener = {
    .keymap = host_kb_keymap, .enter = host_kb_enter, .leave = host_kb_leave,
    .key = host_kb_key, .modifiers = host_kb_mods,
};

static void host_seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
    struct hseat *hs = d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !hs->ptr) {
        hs->ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(hs->ptr, &host_ptr_listener, hs);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && hs->ptr) {
        wl_pointer_destroy(hs->ptr); hs->ptr = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !hs->kb) {
        hs->kb = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(hs->kb, &host_kb_listener, hs);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && hs->kb) {
        wl_keyboard_destroy(hs->kb); hs->kb = NULL;
    }
}
static void host_seat_name(void *d, struct wl_seat *seat, const char *name) {
    (void)d; (void)seat;
    fprintf(stderr, "shim: seat name: %s\n", name ? name : "(null)");
}
static const struct wl_seat_listener host_seat_listener = {
    .capabilities = host_seat_caps, .name = host_seat_name,
};

/* ---------------- host shell listeners ---------------- */
static void hss_ping(void *d, struct wl_shell_surface *ss, uint32_t serial) { (void)d; wl_shell_surface_pong(ss, serial); }
static void hss_configure(void *d, struct wl_shell_surface *ss, uint32_t edges, int32_t w, int32_t h) {
    (void)d; (void)ss; (void)edges;
    fprintf(stderr, "shim: host configure %dx%d\n", w, h);
}
static void hss_popup_done(void *d, struct wl_shell_surface *ss) { (void)d; (void)ss; }
static const struct wl_shell_surface_listener host_ss_listener = {
    .ping = hss_ping, .configure = hss_configure, .popup_done = hss_popup_done,
};

static void ws_state_changed(void *d, struct wl_webos_shell_surface *ws, uint32_t st) {
    (void)d; (void)ws;
    fprintf(stderr, "shim: webos state %u\n", st);
}
static void ws_position_changed(void *d, struct wl_webos_shell_surface *ws, int32_t x, int32_t y) {
    (void)d; (void)ws; (void)x; (void)y;
}
static void ws_close(void *d, struct wl_webos_shell_surface *ws) {
    (void)ws;
    struct shim *s = d;
    fprintf(stderr, "shim: close requested by compositor\n");
    if (s->sdpy) wl_display_terminate(s->sdpy);
}
static void ws_exposed(void *d, struct wl_webos_shell_surface *ws, struct wl_array *r) { (void)d; (void)ws; (void)r; }
static void ws_about(void *d, struct wl_webos_shell_surface *ws, uint32_t st) { (void)d; (void)ws; (void)st; }
static void ws_addon(void *d, struct wl_webos_shell_surface *ws, uint32_t st) { (void)d; (void)ws; (void)st; }
static const struct wl_webos_shell_surface_listener host_ws_listener = {
    .state_changed = ws_state_changed, .position_changed = ws_position_changed,
    .close = ws_close, .exposed = ws_exposed,
    .state_about_to_change = ws_about, .addon_status_changed = ws_addon,
};

/* ---------------- host output ---------------- */
static void hout_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                          int32_t pw, int32_t ph, int32_t subpixel,
                          const char *make, const char *model, int32_t transform) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel;
    (void)make; (void)model; (void)transform;
}
static void hout_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    (void)o; (void)refresh;
    struct shim *s = d;
    if ((flags & WL_OUTPUT_MODE_CURRENT) && w > 0 && h > 0) {
        s->out_w = w;
        s->out_h = h;
        s->have_output = 1;
    }
}
static void hout_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void hout_scale(void *d, struct wl_output *o, int32_t factor) { (void)d; (void)o; (void)factor; }
static const struct wl_output_listener host_output_listener = {
    .geometry = hout_geometry, .mode = hout_mode, .done = hout_done, .scale = hout_scale,
};

/* ---------------- host registry ---------------- */
static void hreg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    struct shim *s = d;
    if (!strcmp(iface, "wl_compositor")) {
        s->comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(iface, "wl_shm")) {
        s->hshm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, "wl_shell")) {
        s->hshell = wl_registry_bind(r, name, &wl_shell_interface, 1);
    } else if (!strcmp(iface, "wl_webos_shell")) {
        s->wshell = wl_registry_bind(r, name, &wl_webos_shell_interface, ver < 2 ? ver : 2);
    } else if (!strcmp(iface, "wl_output") && !s->houtput) {
        s->houtput = wl_registry_bind(r, name, &wl_output_interface, ver < 2 ? ver : 2);
        if (s->houtput) wl_output_add_listener(s->houtput, &host_output_listener, s);
    } else if (!strcmp(iface, "wl_seat")) {
        if (s->nseats >= (int)(sizeof(s->seats) / sizeof(s->seats[0]))) return;
        struct hseat *hs = &s->seats[s->nseats++];
        hs->s = s;
        hs->seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 4 ? ver : 4);
        if (!hs->seat) { s->nseats--; return; }
        wl_seat_add_listener(hs->seat, &host_seat_listener, hs);
        fprintf(stderr, "shim: bound seat %u (v%u)\n", name, ver);
    }
}
static void hreg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener host_reg_listener = { .global = hreg_global, .global_remove = hreg_remove };

/* ---------------- host framebuffer ---------------- */
static void hbuf_release(void *d, struct wl_buffer *b) {
    (void)b;
    struct hbuf *hb = d;
    hb->busy = 0;
    if (hb->s->dirty) schedule(hb->s);
}
static const struct wl_buffer_listener hbuf_listener = { .release = hbuf_release };

static int create_host_buffers(struct shim *s) {
    size_t stride = (size_t)s->out_w * 4;
    size_t one = stride * (size_t)s->out_h;
    size_t total = one * 2;
    int fd = memfd_create("shim-fb", MFD_CLOEXEC);
    if (fd < 0) { perror("shim: memfd_create"); return -1; }
    if (ftruncate(fd, (off_t)total) < 0) { perror("shim: ftruncate"); close(fd); return -1; }
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("shim: mmap fb"); close(fd); return -1; }
    memset(map, 0, total);
    struct wl_shm_pool *pool = wl_shm_create_pool(s->hshm, fd, (int32_t)total);
    if (!pool) { close(fd); return -1; }
    for (int i = 0; i < 2; i++) {
        struct hbuf *hb = &s->hbufs[i];
        hb->s = s;
        hb->data = (char *)map + (size_t)i * one;
        hb->busy = 0;
        hb->buf = wl_shm_pool_create_buffer(pool, (int32_t)((size_t)i * one),
                                            s->out_w, s->out_h, (int32_t)stride,
                                            WL_SHM_FORMAT_XRGB8888);
        if (!hb->buf) { wl_shm_pool_destroy(pool); close(fd); return -1; }
        wl_buffer_add_listener(hb->buf, &hbuf_listener, hb);
    }
    wl_shm_pool_destroy(pool);
    close(fd);
    s->nhbufs = 2;
    return 0;
}

static void draw_cursor(struct shim *s, uint32_t *fb) {
    int cx = s->ptr_x, cy = s->ptr_y;
    for (int dy = -12; dy <= 12; dy++) {
        for (int dx = -12; dx <= 12; dx++) {
            int d2 = dx * dx + dy * dy;
            uint32_t c = 0;
            if (d2 <= 144 && d2 >= 81) c = 0xff000000;
            else if (d2 < 81 && d2 >= 49) c = 0xffffffff;
            if ((dx >= -1 && dx <= 1 && dy >= -12 && dy <= 12) ||
                (dy >= -1 && dy <= 1 && dx >= -12 && dx <= 12))
                c = 0xffffffff;
            if (!c) continue;
            int x = cx + dx, y = cy + dy;
            if (x >= 0 && x < s->out_w && y >= 0 && y < s->out_h)
                fb[y * s->out_w + x] = c;
        }
    }
}

static void paint(struct shim *s) {
    int idx = -1;
    for (int i = 0; i < s->nhbufs; i++) if (!s->hbufs[i].busy) { idx = i; break; }
    if (idx < 0) {
        if (!s->timer_armed) { s->timer_armed = 1; wl_event_source_timer_update(s->timer, 8); }
        return;
    }
    struct hbuf *hb = &s->hbufs[idx];
    pixman_image_t *dst = pixman_image_create_bits(PIXMAN_x8r8g8b8, s->out_w, s->out_h,
                                                   hb->data, s->out_w * 4);
    if (!dst) return;
    pixman_color_t bg = { 0x0808, 0x1010, 0x1818, 0xffff };
    pixman_rectangle16_t rect = { 0, 0, (uint16_t)s->out_w, (uint16_t)s->out_h };
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, dst, &bg, 1, &rect);

    struct ssurf *sfn;
    wl_list_for_each(sfn, &s->stack, stack_link) {
        if (!sfn->mapped || !sfn->shm) continue;
        struct wl_shm_buffer *b = sfn->shm;
        int bw = wl_shm_buffer_get_width(b);
        int bh = wl_shm_buffer_get_height(b);
        int bstride = wl_shm_buffer_get_stride(b);
        uint32_t bfmt = wl_shm_buffer_get_format(b);
        pixman_format_code_t fc = (bfmt == WL_SHM_FORMAT_XRGB8888)
                                  ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8;
        wl_shm_buffer_begin_access(b);
        pixman_image_t *src = pixman_image_create_bits(fc, bw, bh,
                                                       wl_shm_buffer_get_data(b), bstride);
        if (src) {
            int x1 = MAX(sfn->x, 0), y1 = MAX(sfn->y, 0);
            int x2 = MIN(sfn->x + bw, s->out_w), y2 = MIN(sfn->y + bh, s->out_h);
            if (x2 > x1 && y2 > y1) {
                pixman_image_composite32(PIXMAN_OP_OVER, src, NULL, dst,
                                         x1 - sfn->x, y1 - sfn->y, 0, 0,
                                         x1, y1, x2 - x1, y2 - y1);
            }
            pixman_image_unref(src);
        }
        wl_shm_buffer_end_access(b);
    }
    pixman_image_unref(dst);
    draw_cursor(s, hb->data);

    wl_surface_attach(s->hsurf, hb->buf, 0, 0);
    wl_surface_damage(s->hsurf, 0, 0, s->out_w, s->out_h);
    wl_surface_commit(s->hsurf);
    hb->busy = 1;
    flush_host(s);
    s->dirty = 0;

    uint32_t t = (uint32_t)now_ms();
    wl_list_for_each(sfn, &s->stack, stack_link) {
        if (sfn->mapped) {
            struct fcb *f, *tmp;
            wl_list_for_each_safe(f, tmp, &sfn->frames, link) {
                wl_callback_send_done(f->res, t);
                wl_resource_destroy(f->res);
            }
            /* Keep the current buffer attached until the client commits a
             * replacement; releasing it here lets the client draw into the
             * pixels we may still composite on the next pointer-driven frame. */
        }
    }
}

static int timer_cb(void *d) {
    struct shim *s = d;
    s->timer_armed = 0;
    if (s->dirty) paint(s);
    return 0;
}

static void schedule(struct shim *s) {
    s->dirty = 1;
    if (!s->timer_armed && s->timer) {
        s->timer_armed = 1;
        wl_event_source_timer_update(s->timer, 16);
    }
}

/* ---------------- host display fd ---------------- */
static int host_fd_cb(int fd, uint32_t mask, void *d) {
    (void)fd;
    struct shim *s = d;
    if (mask & WL_EVENT_READABLE) {
        if (wl_display_dispatch(s->hdpy) < 0) {
            fprintf(stderr, "shim: host display error: %s\n", strerror(errno));
            wl_display_terminate(s->sdpy);
            return 0;
        }
    }
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        fprintf(stderr, "shim: host display hung up\n");
        wl_display_terminate(s->sdpy);
        return 0;
    }
    flush_host(s);
    return 0;
}

/* ---------------- setup ---------------- */
static int setup_host(struct shim *s) {
    s->hdpy = wl_display_connect(NULL);
    if (!s->hdpy) return -1;

    s->reg = wl_display_get_registry(s->hdpy);
    wl_registry_add_listener(s->reg, &host_reg_listener, s);
    if (wl_display_roundtrip(s->hdpy) < 0) return -1;
    if (wl_display_roundtrip(s->hdpy) < 0) return -1;

    if (!s->comp || !s->hshm || !s->hshell || !s->wshell) {
        fprintf(stderr, "shim: missing host globals (comp=%p shm=%p shell=%p webos=%p)\n",
                (void *)s->comp, (void *)s->hshm, (void *)s->hshell, (void *)s->wshell);
        return -1;
    }

    s->hsurf = wl_compositor_create_surface(s->comp);
    if (!s->hsurf) return -1;
    s->hss = wl_shell_get_shell_surface(s->hshell, s->hsurf);
    if (!s->hss) return -1;
    wl_shell_surface_add_listener(s->hss, &host_ss_listener, s);
    wl_shell_surface_set_title(s->hss, "Wayland Shim");
    wl_shell_surface_set_class(s->hss, "shim");

    s->wsurf = wl_webos_shell_get_shell_surface(s->wshell, s->hsurf);
    if (s->wsurf) {
        wl_webos_shell_surface_add_listener(s->wsurf, &host_ws_listener, s);
        wl_webos_shell_surface_set_property(s->wsurf, "appId", APP_ID);
        wl_webos_shell_surface_set_property(s->wsurf, "_WEBOS_WINDOW_TYPE", "_WEBOS_WINDOW_TYPE_CARD");
        wl_webos_shell_surface_set_state(s->wsurf, WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
    }
    wl_shell_surface_set_fullscreen(s->hss, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0, NULL);

    if (create_host_buffers(s) < 0) return -1;
    flush_host(s);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    struct shim *s = calloc(1, sizeof(*s));
    if (!s) die("calloc");
    S = s;
    s->out_w = OUT_W; s->out_h = OUT_H;
    s->kmfd = -1;
    s->start_ms = now_ms();
    s->ptr_x = OUT_W / 2;
    s->ptr_y = OUT_H / 2;
    s->debug = getenv("SHIM_DEBUG") != NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--debug")) s->debug = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s [--debug]\n"
                   "  SHIM_DEBUG=1 or --debug enables verbose input/popup logging\n"
                   "  Requires XDG_RUNTIME_DIR and WAYLAND_DISPLAY of the host compositor.\n",
                   argv[0]);
            return 0;
        }
    }
    wl_list_init(&s->stack);

    s->xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    if (setup_host(s) < 0) {
        fprintf(stderr, "shim: cannot set up host Wayland connection\n");
        return 1;
    }
    if (!s->have_output || s->out_w <= 0 || s->out_h <= 0) {
        s->out_w = OUT_W;
        s->out_h = OUT_H;
        fprintf(stderr, "shim: no host output mode, assuming %dx%d\n", s->out_w, s->out_h);
    }
    s->ptr_x = s->out_w / 2;
    s->ptr_y = s->out_h / 2;

    s->sdpy = wl_display_create();
    if (!s->sdpy) die("wl_display_create");
    if (wl_display_add_socket(s->sdpy, "wayland-shim") < 0)
        die("wl_display_add_socket (is XDG_RUNTIME_DIR set?)");

    struct wl_event_loop *loop = wl_display_get_event_loop(s->sdpy);

    if (wl_display_init_shm(s->sdpy) < 0) die("wl_display_init_shm");
    wl_global_create(s->sdpy, &wl_compositor_interface, 4, s, bind_compositor);
    wl_global_create(s->sdpy, &wl_data_device_manager_interface, 1, s, bind_ddm);
    wl_global_create(s->sdpy, &wl_seat_interface, 4, s, bind_seat);
    wl_global_create(s->sdpy, &wl_output_interface, 3, s, bind_output);
    wl_global_create(s->sdpy, &xdg_wm_base_interface, 3, s, bind_xdg_wm_base);

    wl_event_loop_add_fd(loop, wl_display_get_fd(s->hdpy), WL_EVENT_READABLE, host_fd_cb, s);
    s->timer = wl_event_loop_add_timer(loop, timer_cb, s);

    paint(s);

    fprintf(stderr, "shim: ready on WAYLAND_DISPLAY=wayland-shim (PID %d)\n", (int)getpid());
    wl_display_run(s->sdpy);
    fprintf(stderr, "shim: exiting\n");
    return 0;
}
