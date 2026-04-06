/*
 * voxtral_menubar.m - Menu bar status item (macOS)
 *
 * Minimal Objective-C: NSStatusItem with programmatic template images.
 * Two states: idle (outline mic) and recording (filled mic).
 * NSApplication runs on main thread, worker runs on background pthread.
 */

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include "voxtral_menubar.h"
#include <pthread.h>

/* Forward declaration for the quit callback */
extern volatile sig_atomic_t mic_interrupted;
extern pthread_mutex_t wf_mutex;
extern pthread_cond_t  wf_cond;

/* ---- App delegate ---- */

@interface VoxtralDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSStatusItem *statusItem;
@property (strong) NSImage *idleImage;
@property (strong) NSImage *recordImage;
@end

@implementation VoxtralDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;

    self.statusItem = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSSquareStatusItemLength];

    /* Draw mic icons programmatically (template images, no assets) */
    self.idleImage = [self drawMicIcon:NO];
    self.recordImage = [self drawMicIcon:YES];
    self.statusItem.button.image = self.idleImage;

    /* Menu */
    NSMenu *menu = [[NSMenu alloc] init];
    [menu addItemWithTitle:@"Voxtral Dictation"
                    action:nil keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Quit Voxtral"
                    action:@selector(quitApp:) keyEquivalent:@"q"];
    [[menu itemAtIndex:2] setTarget:self];
    self.statusItem.menu = menu;
}

- (NSImage *)drawMicIcon:(BOOL)filled {
    NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
    [img lockFocus];

    NSBezierPath *body = [NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(6, 6, 6, 9) xRadius:3 yRadius:3];
    if (filled) {
        [[NSColor labelColor] setFill];
        [body fill];
    } else {
        [[NSColor labelColor] setStroke];
        [body setLineWidth:1.2];
        [body stroke];
    }

    /* Stand */
    NSBezierPath *stand = [NSBezierPath bezierPath];
    [stand moveToPoint:NSMakePoint(4, 10)];
    [stand curveToPoint:NSMakePoint(14, 10)
          controlPoint1:NSMakePoint(4, 3)
          controlPoint2:NSMakePoint(14, 3)];
    [[NSColor labelColor] setStroke];
    [stand setLineWidth:1.2];
    [stand stroke];

    /* Base line */
    NSBezierPath *base = [NSBezierPath bezierPath];
    [base moveToPoint:NSMakePoint(9, 3)];
    [base lineToPoint:NSMakePoint(9, 1)];
    [base moveToPoint:NSMakePoint(6, 1)];
    [base lineToPoint:NSMakePoint(12, 1)];
    [base setLineWidth:1.2];
    [base stroke];

    [img unlockFocus];
    [img setTemplate:YES];
    return img;
}

- (void)quitApp:(id)sender {
    (void)sender;
    vox_menubar_quit();
}

@end

/* ---- C API ---- */

static VoxtralDelegate *g_delegate = nil;
static pthread_t g_worker_thread;

struct worker_args {
    vox_worker_fn fn;
    void *data;
};

static void *worker_wrapper(void *arg) {
    struct worker_args *wa = (struct worker_args *)arg;
    void *result = wa->fn(wa->data);
    free(wa);
    /* Worker finished — quit the app */
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
    return result;
}

int vox_menubar_run(vox_worker_fn worker_fn, void *user_data) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        g_delegate = [[VoxtralDelegate alloc] init];
        [NSApp setDelegate:g_delegate];

        /* Spawn worker on background thread */
        struct worker_args *wa = malloc(sizeof(*wa));
        wa->fn = worker_fn;
        wa->data = user_data;
        pthread_create(&g_worker_thread, NULL, worker_wrapper, wa);

        /* Run NSApplication (blocks until termination) */
        [NSApp run];

        pthread_join(g_worker_thread, NULL);
    }
    return 0;
}

void vox_menubar_set_recording(int active) {
    if (!g_delegate) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        g_delegate.statusItem.button.image =
            active ? g_delegate.recordImage : g_delegate.idleImage;
    });
}

void vox_menubar_quit(void) {
    /* Signal the wexproflow thread to exit */
    mic_interrupted = 1;
    pthread_mutex_lock(&wf_mutex);
    pthread_cond_signal(&wf_cond);
    pthread_mutex_unlock(&wf_mutex);
}

#else /* !__APPLE__ */

#include "voxtral_menubar.h"
#include <stdio.h>

int vox_menubar_run(vox_worker_fn worker_fn, void *user_data) {
    /* No menu bar on non-macOS — just run worker directly */
    worker_fn(user_data);
    return 0;
}

void vox_menubar_set_recording(int active) { (void)active; }
void vox_menubar_quit(void) {}

#endif
