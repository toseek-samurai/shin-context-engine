#pragma once
// Cocoa captions are retained objects, unlike ImGui's per-frame text. Bind only
// owned captions; never search/replace user paths, project names or draft input.
using NativeCaption = NSString* (^)(void);
NSMapTable<NSObject*, NSMutableDictionary*>* native_captions() {
    static NSMapTable* captions = [NSMapTable weakToStrongObjectsMapTable];
    return captions;
}
template<class T> T* live_caption(T* object, NSString* property, NativeCaption translate) {
    if (!object) return object;
    auto* captions = native_captions();
    NSMutableDictionary* fields = [captions objectForKey:object];
    if (!fields) { fields = [NSMutableDictionary dictionary]; [captions setObject:fields forKey:object]; }
    NSString* value = translate();
    fields[property] = @[[translate copy], value];
    [object setValue:value forKey:property];
    return object;
}
template<class T> T* live_text(T* object, NSString* property, const char* message) {
    NSString* key = [NSString stringWithUTF8String:message];
    return live_caption(object, property, ^{ return ui_text(key.UTF8String); });
}
NSTextField* live_label(const char* message, bool wrapping = true) {
    return live_text(wrapping ? [NSTextField wrappingLabelWithString:@""] :
        [NSTextField labelWithString:@""], @"stringValue", message);
}
NSButton* live_button(const char* message, id target, SEL action, bool checkbox = false) {
    return live_text(checkbox ? [NSButton checkboxWithTitle:@"" target:target action:action] :
        [NSButton buttonWithTitle:@"" target:target action:action], @"title", message);
}
void refresh_native_captions() {
    auto* captions = native_captions();
    for (NSObject* object in captions.keyEnumerator) {
        NSMutableDictionary* fields = [captions objectForKey:object];
        for (NSString* property in fields.allKeys) {
            NSArray* record = fields[property];
            // A later asynchronous result or user edit supersedes this binding.
            if (![[object valueForKey:property] isEqual:record[1]]) { [fields removeObjectForKey:property]; continue; }
            NativeCaption translate = record[0];
            NSString* value = translate();
            [object setValue:value forKey:property];
            fields[property] = @[record[0], value];
        }
    }
}
// Only this known status field may contain a catalog message assigned by an
// asynchronous operation. Composite diagnostic reports remain received snapshots.
void refresh_setup_status(NSTextField* field) {
    if (!field) return;
    for (const auto& row : shin::ui::messages) {
        if ([field.stringValue isEqualToString:[NSString stringWithUTF8String:row.en]] ||
            [field.stringValue isEqualToString:[NSString stringWithUTF8String:row.ru]]) {
            live_text(field, @"stringValue", row.en); return;
        }
    }
}
