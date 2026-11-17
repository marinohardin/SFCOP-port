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
    const std::size_t index = static_cast<std::size_t>(item.tag);
    if (!sfcop::app::activateFileMenuEntry(gEntries, index, sfcop::app::revealInFileManager)) {
        // A folder that does not exist yet (logs before the first log) cannot be shown.
        NSLog(@"sfcop: could not show \"%@\" in Finder", item.title);
    }
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

// Puts the File menu into the main menu when it is not there. SDL builds (and can replace) the
// main menu while the application finishes launching, which depends on timing, so this runs now
// and again after every event the application handles until the File menu is in place; once it
// is, each call is one pointer comparison. Logs the menu bar's titles whenever it installs.
static void ensureFileMenu() {
    static SfcopFileMenuTarget *target = [[SfcopFileMenuTarget alloc] init];
    static NSMenuItem *fileItem = nil;
    NSMenu *bar = [NSApp mainMenu];
    if (bar == nil || gEntries.empty() || (fileItem != nil && fileItem.menu == bar)) {
        return;
    }
    if (fileItem == nil) {
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"File"];
        for (std::size_t i = 0; i < gEntries.size(); ++i) {
            NSString *title = [NSString stringWithUTF8String:gEntries[i].title.c_str()];
            NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                          action:@selector(reveal:)
                                                   keyEquivalent:@""];
            item.target = target;
            item.tag = static_cast<NSInteger>(i);
            [menu addItem:item];
        }
        fileItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
        fileItem.submenu = menu;
    } else if (fileItem.menu != nil) {
        [fileItem.menu removeItem:fileItem];
    }
    // After the application menu, before the rest (SDL adds Window and others).
    [bar insertItem:fileItem atIndex:bar.numberOfItems > 0 ? 1 : 0];
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    for (NSMenuItem *entry in bar.itemArray) {
        [titles addObject:entry.title.length > 0 ? entry.title : @"(app)"];
    }
    NSLog(@"sfcop: menu bar now: %@", [titles componentsJoinedByString:@" | "]);
}

void installFileMenu(const std::vector<FileMenuEntry> &entries) {
    if (entries.empty()) {
        return;
    }
    @autoreleasepool {
        gEntries = entries;
        static bool observing = false;
        if (!observing && NSApp != nil) {
            observing = true;
            [[NSNotificationCenter defaultCenter]
                addObserverForName:NSApplicationDidUpdateNotification
                            object:NSApp
                             queue:nil
                        usingBlock:^(NSNotification *) {
                          ensureFileMenu();
                        }];
        }
        ensureFileMenu();
    }
}

} // namespace sfcop::app
