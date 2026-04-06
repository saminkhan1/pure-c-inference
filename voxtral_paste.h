/*
 * voxtral_paste.h - Text injection API
 *
 * macOS implementation uses CGEventPost (CoreGraphics).
 * Other platforms: stubs that return errors.
 */

#ifndef VOXTRAL_PASTE_H
#define VOXTRAL_PASTE_H

/* Type text directly into the frontmost app via keyboard event injection.
 * No pasteboard involvement — suitable for streaming token-by-token output.
 * Returns 0 on success, -1 on error. */
int vox_type_text(const char *text);

/* Check if Accessibility permission is granted (needed for event injection).
 * If not granted, prints instructions to stderr and prompts the macOS dialog.
 * Returns 1 if trusted, 0 if not. */
int vox_paste_check_access(void);

#endif /* VOXTRAL_PASTE_H */
