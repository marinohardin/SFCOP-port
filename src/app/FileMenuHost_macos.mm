// The File menu in the menu bar (AppKit). SDL's default application menu is already there once
// its video system is up; the File menu is added after it. Choosing an item opens the folder in
// Finder through activateFileMenuEntry (FileMenu.h).
#include "app/FileMenu.h"

#import <AppKit/AppKit.h>

#include <vector>

namespace {
std::vector<sfcop::app::FileMenuEntry> gEntries;
} // namespace

@interface SfcopFileMenuTarget : NSObject
- (void)reveal:(NSMenuItem *)item;
@end

@implementation SfcopFileMenuTarget
- (void)reveal:(NSMenuItem *)item {
    (void)sfcop::app::activateFileMenuEntry(gEntries, static_cast<std::size_t>(item.tag),
                                            sfcop::app::revealInFileManager);
}
@end

namespace sfcop::app {

bool revealInFileManager(const std::string &path) {
    @autoreleasepool {
        NSString *text = [NSString stringWithUTF8String:path.c_str()];
        if (text == nil || path.empty()) {
            return false;
        }
        // Opens the folder itself, so its contents are what the window shows.
        return [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:text isDirectory:YES]];
    }
}

void installFileMenu(const std::vector<FileMenuEntry> &entries) {
    if (entries.empty()) {
        return;
    }
    @autoreleasepool {
        static SfcopFileMenuTarget *target = [[SfcopFileMenuTarget alloc] init];
        NSMenu *bar = [NSApp mainMenu];
        if (bar == nil) {
            bar = [[NSMenu alloc] init];
            [NSApp setMainMenu:bar];
        }
        for (NSMenuItem *existing in bar.itemArray) {
            if ([existing.title isEqualToString:@"File"]) {
                return; // installed already
            }
        }
        gEntries = entries;
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"File"];
        for (std::size_t i = 0; i < entries.size(); ++i) {
            NSString *title = [NSString stringWithUTF8String:entries[i].title.c_str()];
            NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                          action:@selector(reveal:)
                                                   keyEquivalent:@""];
            item.target = target;
            item.tag = static_cast<NSInteger>(i);
            [menu addItem:item];
        }
        NSMenuItem *fileItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
        fileItem.submenu = menu;
        // After the application menu, before the rest (SDL adds Window and others).
        [bar insertItem:fileItem atIndex:bar.numberOfItems > 0 ? 1 : 0];
    }
}

} // namespace sfcop::app
