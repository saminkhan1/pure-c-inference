/*
 * voxtral_paste_macos.c - Text injection via keyboard events (macOS)
 *
 * Uses CGEventPost for key injection.
 * Requires Accessibility permission for CGEventPost.
 */

#ifdef __APPLE__

#include "voxtral_paste.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>

int vox_type_text(const char *text) {
    if (!text || !text[0]) return 0;

    /* Convert UTF-8 to UTF-16 via CoreFoundation */
    CFStringRef str = CFStringCreateWithCString(kCFAllocatorDefault,
                                                text, kCFStringEncodingUTF8);
    if (!str) return -1;

    CFIndex len = CFStringGetLength(str);
    UniChar stack_buf[128];
    UniChar *chars = (len <= 128) ? stack_buf :
                     (UniChar *)malloc((size_t)len * sizeof(UniChar));
    if (!chars) {
        CFRelease(str);
        return -1;
    }
    CFStringGetCharacters(str, CFRangeMake(0, len), chars);
    CFRelease(str);

    /* Strip control characters (U+0000-001F except tab and newline) */
    CFIndex dst = 0;
    for (CFIndex j = 0; j < len; j++) {
        UniChar c = chars[j];
        if (c < 0x20 && c != 0x09 && c != 0x0A) continue;
        chars[dst++] = c;
    }
    len = dst;
    if (len == 0) {
        if (chars != stack_buf) free(chars);
        return 0;
    }

    /* Post keyboard events in chunks (CGEventKeyboardSetUnicodeString
     * handles up to ~20 UniChars reliably per event) */
    for (CFIndex i = 0; i < len; ) {
        CFIndex chunk = len - i;
        if (chunk > 20) chunk = 20;
        /* Don't split a UTF-16 surrogate pair across chunks */
        if (chunk < (len - i) && chars[i + chunk - 1] >= 0xD800 &&
            chars[i + chunk - 1] <= 0xDBFF)
            chunk--;

        CGEventRef down = CGEventCreateKeyboardEvent(NULL, 0, true);
        CGEventRef up   = CGEventCreateKeyboardEvent(NULL, 0, false);
        CGEventKeyboardSetUnicodeString(down, (UniCharCount)chunk, chars + i);
        CGEventKeyboardSetUnicodeString(up,   (UniCharCount)chunk, chars + i);
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(down);
        CFRelease(up);

        i += chunk;
    }

    if (chars != stack_buf) free(chars);
    return 0;
}

int vox_paste_check_access(void) {
    /* AXIsProcessTrustedWithOptions prompts the user if not yet trusted */
    CFStringRef key = kAXTrustedCheckOptionPrompt;
    CFBooleanRef val = kCFBooleanTrue;
    CFDictionaryRef options = CFDictionaryCreate(
        kCFAllocatorDefault,
        (const void **)&key, (const void **)&val, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    Boolean trusted = AXIsProcessTrustedWithOptions(options);
    CFRelease(options);

    if (!trusted) {
        fprintf(stderr,
            "Error: Accessibility permission required.\n"
            "  System Settings → Privacy & Security → Accessibility\n"
            "  Enable the terminal or voxtral binary, then restart.\n");
    }
    return trusted ? 1 : 0;
}

#else /* !__APPLE__ */

#include "voxtral_paste.h"
#include <stdio.h>

int vox_type_text(const char *text) {
    (void)text;
    fprintf(stderr, "Error: Text injection is only supported on macOS\n");
    return -1;
}

int vox_paste_check_access(void) {
    fprintf(stderr, "Error: Accessibility check is only supported on macOS\n");
    return 0;
}

#endif
