/*
 * voxtral_menubar.m - Menu bar status item (macOS)
 *
 * Menu bar app using GCD for concurrency and SF Symbols for icons.
 * States: idle (outline mic), recording (filled mic), error (warning triangle).
 */

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include "voxtral_menubar.h"
#include "voxtral_version.h"
#import <dispatch/dispatch.h>
#include <pthread.h>
#include <string.h>

static dispatch_queue_t g_work_queue;
static dispatch_semaphore_t g_quit_sem;

/* Protects g_last_text — written from worker thread, read on main thread */
static pthread_mutex_t g_text_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_last_text_buf[8192];
static int  g_has_last_text = 0;

@interface VoxtralDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSStatusItem *statusItem;
@property (strong) NSMenuItem *statusLineItem;   /* "Idle" / "Recording..." */
@property (strong) NSMenuItem *lastTextItem;
@property (strong) NSMenuItem *errorItem;        /* shown only on permission error */
@end

@implementation VoxtralDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;

    self.statusItem = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSVariableStatusItemLength];

    [self setIcon:@"mic" tooltip:@"Idle"];

    NSMenu *menu = [[NSMenu alloc] init];

    /* Title row: version */
    NSMenuItem *titleItem = [[NSMenuItem alloc] initWithTitle:@"Voxtral v" VOXTRAL_VERSION
                                                       action:nil
                                                keyEquivalent:@""];
    titleItem.enabled = NO;
    [menu addItem:titleItem];

    [menu addItem:[NSMenuItem separatorItem]];

    /* Status row */
    self.statusLineItem = [[NSMenuItem alloc] initWithTitle:@"Idle — press Command+R to dictate"
                                                     action:nil
                                              keyEquivalent:@""];
    self.statusLineItem.enabled = NO;
    [menu addItem:self.statusLineItem];

    /* Permission error row (hidden until needed) */
    self.errorItem = [[NSMenuItem alloc] initWithTitle:@""
                                                action:@selector(openPrivacySettings:)
                                         keyEquivalent:@""];
    self.errorItem.target = self;
    self.errorItem.hidden = YES;
    [menu addItem:self.errorItem];

    [menu addItem:[NSMenuItem separatorItem]];

    /* Copy last transcription */
    self.lastTextItem = [[NSMenuItem alloc] initWithTitle:@"Copy Last Transcription"
                                                   action:@selector(copyLastText:)
                                            keyEquivalent:@"c"];
    self.lastTextItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    self.lastTextItem.target = self;
    self.lastTextItem.enabled = NO;
    [menu addItem:self.lastTextItem];

    [menu addItem:[NSMenuItem separatorItem]];

    /* History actions */
    NSMenuItem *historyItem = [[NSMenuItem alloc] initWithTitle:@"Open History Log"
                                                         action:@selector(openHistoryLog:)
                                                  keyEquivalent:@""];
    historyItem.target = self;
    [menu addItem:historyItem];

    NSMenuItem *clearItem = [[NSMenuItem alloc] initWithTitle:@"Clear History…"
                                                       action:@selector(clearHistory:)
                                                keyEquivalent:@""];
    clearItem.target = self;
    [menu addItem:clearItem];

    [menu addItem:[NSMenuItem separatorItem]];

    /* Quit */
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit Voxtral"
                                                       action:@selector(quitApp:)
                                                keyEquivalent:@"q"];
    quitItem.target = self;
    [menu addItem:quitItem];

    self.statusItem.menu = menu;
}

/* ---- Icon helpers ---- */

- (void)setIcon:(NSString *)symbolName tooltip:(NSString *)tip {
    NSImage *img = [NSImage imageWithSystemSymbolName:symbolName
                               accessibilityDescription:@"Voxtral Dictation"];
    NSImageSymbolConfiguration *cfg = [NSImageSymbolConfiguration
        configurationWithPointSize:16 weight:NSFontWeightMedium];
    self.statusItem.button.image = [img imageWithSymbolConfiguration:cfg];
    self.statusItem.button.toolTip = [NSString stringWithFormat:@"Voxtral — %@", tip];
}

/* ---- Actions ---- */

- (void)copyLastText:(id)sender {
    (void)sender;
    pthread_mutex_lock(&g_text_mutex);
    NSString *text = g_has_last_text ? @(g_last_text_buf) : nil;
    pthread_mutex_unlock(&g_text_mutex);
    if (text) {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:text forType:NSPasteboardTypeString];
    }
}

- (void)openHistoryLog:(id)sender {
    (void)sender;
    NSString *home = NSHomeDirectory();
    NSString *path = [home stringByAppendingPathComponent:@".config/voxtral/history.log"];
    /* Create the file if it doesn't exist so the open doesn't fail */
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        [[NSFileManager defaultManager] createFileAtPath:path contents:nil attributes:nil];
    }
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
}

- (void)clearHistory:(id)sender {
    (void)sender;
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"Clear transcription history?";
    alert.informativeText = @"This will permanently delete ~/.config/voxtral/history.log.";
    [alert addButtonWithTitle:@"Clear History"];
    [alert addButtonWithTitle:@"Cancel"];
    alert.alertStyle = NSAlertStyleWarning;
    NSModalResponse resp = [alert runModal];
    if (resp == NSAlertFirstButtonReturn) {
        NSString *home = NSHomeDirectory();
        NSString *path = [home stringByAppendingPathComponent:@".config/voxtral/history.log"];
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    }
}

- (void)openPrivacySettings:(id)sender {
    (void)sender;
    NSURL *url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)quitApp:(id)sender {
    (void)sender;
    vox_menubar_quit();
}

@end

/* ---- Public API ---- */

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
    dispatch_async(dispatch_get_main_queue(), ^{
        VoxtralDelegate *d = (VoxtralDelegate *)NSApp.delegate;
        if (active) {
            [d setIcon:@"mic.fill" tooltip:@"Recording…"];
            d.statusLineItem.title = @"Recording… (Command+R or silence to finish)";
        } else {
            [d setIcon:@"mic" tooltip:@"Idle"];
            d.statusLineItem.title = @"Idle — press Command+R to dictate";
        }
    });
}

void vox_menubar_set_processing(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        VoxtralDelegate *d = (VoxtralDelegate *)NSApp.delegate;
        [d setIcon:@"waveform" tooltip:@"Transcribing…"];
        d.statusLineItem.title = @"Transcribing…";
    });
}

void vox_menubar_set_status(const char *msg) {
    NSString *nsMsg = (msg && msg[0]) ? @(msg) : @"Idle — press Command+R to dictate";
    dispatch_async(dispatch_get_main_queue(), ^{
        VoxtralDelegate *d = (VoxtralDelegate *)NSApp.delegate;
        d.statusLineItem.title = nsMsg;
        d.statusItem.button.toolTip = [NSString stringWithFormat:@"Voxtral — %@", nsMsg];
    });
}

void vox_menubar_set_error(const char *msg) {
    NSString *nsMsg = msg ? @(msg) : @"Permission error — check System Settings";
    dispatch_async(dispatch_get_main_queue(), ^{
        VoxtralDelegate *d = (VoxtralDelegate *)NSApp.delegate;
        /* Switch to warning icon */
        NSImage *img = [NSImage imageWithSystemSymbolName:@"exclamationmark.triangle.fill"
                                   accessibilityDescription:@"Voxtral — Permission Error"];
        NSImageSymbolConfiguration *cfg = [NSImageSymbolConfiguration
            configurationWithPointSize:16 weight:NSFontWeightMedium];
        d.statusItem.button.image = [img imageWithSymbolConfiguration:cfg];
        d.statusItem.button.toolTip = @"Voxtral — Permission required";

        /* Show error item with clickable "open System Settings" action */
        d.statusLineItem.title = @"Permission required";
        d.errorItem.title = nsMsg;
        d.errorItem.hidden = NO;
    });
}

void vox_menubar_set_last_text(const char *text) {
    int enable;

    pthread_mutex_lock(&g_text_mutex);
    if (text && text[0]) {
        strncpy(g_last_text_buf, text, sizeof(g_last_text_buf) - 1);
        g_last_text_buf[sizeof(g_last_text_buf) - 1] = '\0';
        g_has_last_text = 1;
    } else {
        g_last_text_buf[0] = '\0';
        g_has_last_text = 0;
    }
    enable = g_has_last_text ? YES : NO;
    pthread_mutex_unlock(&g_text_mutex);

    dispatch_async(dispatch_get_main_queue(), ^{
        VoxtralDelegate *d = (VoxtralDelegate *)NSApp.delegate;
        d.lastTextItem.enabled = enable;
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

#else /* non-Apple stubs */

#include "voxtral_menubar.h"
#include <stdio.h>

static dispatch_semaphore_t g_quit_sem;

int vox_menubar_run(vox_worker_fn worker_fn, void *user_data) {
    g_quit_sem = dispatch_semaphore_create(0);
    worker_fn(user_data);
    return 0;
}

void vox_menubar_set_recording(int active)        { (void)active; }
void vox_menubar_set_error(const char *msg)       { if (msg) fprintf(stderr, "Error: %s\n", msg); }
void vox_menubar_set_last_text(const char *text)  { (void)text; }
void vox_menubar_quit(void)                       { dispatch_semaphore_signal(g_quit_sem); }
int  vox_menubar_wait_for_quit(void)              { return dispatch_semaphore_wait(g_quit_sem, DISPATCH_TIME_FOREVER); }

#endif
