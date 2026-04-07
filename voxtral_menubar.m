/*
 * voxtral_menubar.m - Menu bar status item (macOS)
 *
 * Minimal menu bar app using GCD for concurrency and SF Symbols for icons.
 * Two states: idle (outline mic) and recording (filled mic).
 */

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include "voxtral_menubar.h"
#import <dispatch/dispatch.h>

static dispatch_queue_t g_work_queue;
static dispatch_semaphore_t g_quit_sem;
static BOOL g_is_recording = NO;

@interface VoxtralDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSStatusItem *statusItem;
@end

@implementation VoxtralDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;

    self.statusItem = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSVariableStatusItemLength];

    NSImage *micImage = [NSImage imageWithSystemSymbolName:@"mic"
                                   accessibilityDescription:@"Voxtral Dictation"];
    NSImageSymbolConfiguration *config = [NSImageSymbolConfiguration configurationWithPointSize:16 weight:NSFontWeightMedium];
    self.statusItem.button.image = [micImage imageWithSymbolConfiguration:config];
    self.statusItem.button.toolTip = @"Voxtral Dictation - Idle";

    NSMenu *menu = [[NSMenu alloc] init];
    [menu addItemWithTitle:@"Voxtral Dictation" action:nil keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit Voxtral"
                                                       action:@selector(quitApp:)
                                                keyEquivalent:@"q"];
    quitItem.target = self;
    [menu addItem:quitItem];
    self.statusItem.menu = menu;
}

- (void)quitApp:(id)sender {
    (void)sender;
    vox_menubar_quit();
}

@end

int vox_menubar_run(vox_worker_fn worker_fn, void *user_data) {
    @autoreleasepool {
        g_quit_sem = dispatch_semaphore_create(0);
        g_work_queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp setDelegate:[[VoxtralDelegate alloc] init]];

        dispatch_async(g_work_queue, ^{
            worker_fn(user_data);
            dispatch_async(dispatch_get_main_queue(), ^{
                [NSApp terminate:nil];
            });
        });

        [NSApp run];
    }
    return 0;
}

void vox_menubar_set_recording(int active) {
    g_is_recording = active;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSStatusItem *item = [(VoxtralDelegate *)NSApp.delegate statusItem];
        NSString *symbolName = active ? @"mic.fill" : @"mic";
        NSImage *img = [NSImage imageWithSystemSymbolName:symbolName
                                  accessibilityDescription:@"Voxtral Dictation"];
        NSImageSymbolConfiguration *config = [NSImageSymbolConfiguration configurationWithPointSize:16 weight:NSFontWeightMedium];
        item.button.image = [img imageWithSymbolConfiguration:config];
        item.button.toolTip = active ? @"Voxtral Dictation - Recording..." : @"Voxtral Dictation - Idle";
    });
}

void vox_menubar_quit(void) {
    dispatch_semaphore_signal(g_quit_sem);
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
}

int vox_menubar_wait_for_quit(void) {
    return dispatch_semaphore_wait(g_quit_sem, DISPATCH_TIME_FOREVER);
}

#else

#include "voxtral_menubar.h"
#include <stdio.h>

static dispatch_semaphore_t g_quit_sem;

int vox_menubar_run(vox_worker_fn worker_fn, void *user_data) {
    g_quit_sem = dispatch_semaphore_create(0);
    worker_fn(user_data);
    return 0;
}

void vox_menubar_set_recording(int active) { (void)active; }
void vox_menubar_quit(void) { dispatch_semaphore_signal(g_quit_sem); }
int vox_menubar_wait_for_quit(void) { return dispatch_semaphore_wait(g_quit_sem, DISPATCH_TIME_FOREVER); }

#endif
