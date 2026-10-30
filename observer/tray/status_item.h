// Public AppKit presentation; status-window occlusion is not popup readiness.
#import <Cocoa/Cocoa.h>

inline NSImage* shin_status_image(NSImage* base, bool update) {
    if (!update) return base;
    NSImage* image = [NSImage imageWithSize:base.size flipped:NO drawingHandler:^BOOL(NSRect rect) {
        [base drawInRect:rect];
        // A small update dot stays inside the same template canvas and slot.
        NSRect badge = NSMakeRect(NSMaxX(rect) - 6, NSMaxY(rect) - 6, 5, 5);
        [NSColor.blackColor setFill];
        [[NSBezierPath bezierPathWithOvalInRect:badge] fill];
        return YES;
    }];
    [image setTemplate:YES];
    image.accessibilityDescription = base.accessibilityDescription;
    return image;
}

inline void shin_status_presentation(NSStatusItem* item, NSImage* image) {
    if (item.length != NSSquareStatusItemLength) item.length = NSSquareStatusItemLength;
    if (item.button.title.length) item.button.title = @"";
    if (item.button.image != image) item.button.image = image;
    if (item.button.imagePosition != NSImageOnly) item.button.imagePosition = NSImageOnly;
}
