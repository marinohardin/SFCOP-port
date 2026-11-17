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

// Puts the File menu into the main menu when it is not there; false while it cannot (no NSApp or
// no main menu yet). Once it is in, each call is one pointer comparison.
static bool ensureFileMenu(bool &installedNow) {
    static SfcopFileMenuTarget *target = [[SfcopFileMenuTarget alloc] init];
    static NSMenuItem *fileItem = nil;
    installedNow = false;
    NSMenu *bar = NSApp != nil ? [NSApp mainMenu] : nil;
    if (bar == nil || gEntries.empty()) {
        return false;
    }
    if (fileItem != nil && fileItem.menu == bar) {
        return true;
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
    installedNow = true;
    return true;
}

static std::string menuBarTitles() {
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    for (NSMenuItem *entry in [NSApp mainMenu].itemArray) {
        [titles addObject:entry.title.length > 0 ? entry.title : @"(app)"];
    }
    return std::string([[titles componentsJoinedByString:@" | "] UTF8String]);
}

static unsigned gCalls = 0; // installFileMenu's call is call 0, then one per pump
static bool gLogged = false;

void installFileMenu(const std::vector<FileMenuEntry> &entries) {
    if (entries.empty()) {
        return;
    }
    @autoreleasepool {
        gEntries = entries;
        gCalls = 0;
        gLogged = false;
        bool now = false;
        const bool ready = ensureFileMenu(now);
        NSLog(@"sfcop: File menu install: NSApp %s, menu %s", NSApp != nil ? "set" : "nil",
              ready ? "in place" : "not yet");
        if (ready) {
            gLogged = true;
            NSLog(@"sfcop: File menu in place at the install call; menu bar: %s",
                  menuBarTitles().c_str());
        }
    }
}

void pumpFileMenu() {
    if (gLogged || gEntries.empty()) {
        // In place: a replaced main menu is still put right, silently, with one comparison.
        if (gEntries.empty()) {
            return;
        }
        @autoreleasepool {
            bool now = false;
            if (ensureFileMenu(now) && now) {
                NSLog(@"sfcop: File menu put back; menu bar: %s", menuBarTitles().c_str());
            }
        }
        return;
    }
    @autoreleasepool {
        ++gCalls;
        bool now = false;
        if (ensureFileMenu(now)) {
            gLogged = true;
            NSLog(@"sfcop: File menu in place after %u pumps; menu bar: %s", gCalls,
                  menuBarTitles().c_str());
        }
    }
}

} // namespace sfcop::app
