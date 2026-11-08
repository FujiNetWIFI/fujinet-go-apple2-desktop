/*
 * Apple2HelpWindow -- see HelpWindow.h.
 *
 * The topics in a sidebar, with a search that lists every section having
 * all the words typed; the topic in a text view, as an attributed string
 * built from its blocks (tables and bullets on tab stops with a hanging
 * indent), its matches highlighted when it came from a search. The Help
 * menu's own search field finds the same sections.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "HelpWindow.h"

#import <objc/runtime.h>

#include <float.h>

#include "apple2help.h"

#define ME APPLE2HELP_MACOS
#define SIDE_W 250.0
#define BODY_PT 13.0
#define MAX_HITS 64

static char kContextKey, kTargetKey;

static NSString *str(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

void Apple2SetHelpContext(NSWindow *window, NSString *context)
{
    if (window) objc_setAssociatedObject(window, &kContextKey, context, OBJC_ASSOCIATION_COPY_NONATOMIC);
}

static NSString *contextOf(NSWindow *window)
{
    NSString *c = window ? objc_getAssociatedObject(window, &kContextKey) : nil;
    return c ? c : @"main";
}

/* A button's target is weak: the button keeps this one. */
@interface Apple2HelpTarget : NSObject
@property (nonatomic, copy) NSString *context;
- (void)help:(NSButton *)sender;
@end

@implementation Apple2HelpTarget
- (void)help:(NSButton *)sender
{
    [[Apple2HelpWindow shared] showAt:self.context ? self.context : contextOf(sender.window)];
}
@end

NSButton *Apple2HelpButton(NSString *context)
{
    Apple2HelpTarget *target = [[Apple2HelpTarget alloc] init];
    target.context = context;
    NSButton *b = [[NSButton alloc] initWithFrame:NSZeroRect];
    b.bezelStyle = NSBezelStyleHelpButton;
    b.title = @"";
    b.toolTip = @"Help";
    b.target = target;
    b.action = @selector(help:);
    objc_setAssociatedObject(b, &kTargetKey, target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [b sizeToFit];
    return b;
}

/* Command+F finds. */
@interface Apple2HelpPanelWindow : NSWindow
@property (nonatomic, copy) void (^onFind)(void);
@end

@implementation Apple2HelpPanelWindow
- (BOOL)performKeyEquivalent:(NSEvent *)e
{
    const NSEventModifierFlags mods = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask &
        (NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption |
         NSEventModifierFlagControl);
    if (mods == NSEventModifierFlagCommand && [e.charactersIgnoringModifiers isEqualToString:@"f"] && self.onFind) {
        self.onFind();
        return YES;
    }
    return [super performKeyEquivalent:e];
}
@end

@interface Apple2HelpWindow () <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate, NSTextViewDelegate,
                               NSSearchFieldDelegate, NSSplitViewDelegate>
@end

@implementation Apple2HelpWindow {
    Apple2HelpPanelWindow *_window;
    NSView *_side;
    NSSearchField *_search;
    NSTableView *_list;
    NSTextView *_text;
    int _topic, _block;
    NSString *_query;
    NSMutableArray<NSNumber *> *_starts;        /* each block's first character */
    /* the list's rows: a topic, or a section a search found */
    NSMutableArray<NSNumber *> *_rowTopic, *_rowBlock;
    BOOL _searching, _filling;
}

+ (Apple2HelpWindow *)shared
{
    static Apple2HelpWindow *help;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ help = [[Apple2HelpWindow alloc] init]; });
    return help;
}

- (instancetype)init
{
    if ((self = [super init])) {
        _topic = -1;
        _block = -1;
        _query = @"";
        _starts = [NSMutableArray array];
        _rowTopic = [NSMutableArray array];
        _rowBlock = [NSMutableArray array];
    }
    return self;
}

/* ---- the page ------------------------------------------------------------------- */

- (NSDictionary *)attrsFor:(int)kind bold:(BOOL)boldRun
{
    NSFont *body = [NSFont systemFontOfSize:BODY_PT];
    NSFont *font = boldRun ? [NSFont boldSystemFontOfSize:BODY_PT] : body;
    NSMutableDictionary *a = [@{ NSForegroundColorAttributeName: NSColor.labelColor } mutableCopy];
    switch (kind) {
    case APPLE2HELP_BOLD: font = [NSFont boldSystemFontOfSize:BODY_PT]; break;
    case APPLE2HELP_ITALIC: font = [[NSFontManager sharedFontManager] convertFont:body toHaveTrait:NSItalicFontMask]; break;
    case APPLE2HELP_CODE:
    case APPLE2HELP_KBD:
        font = [NSFont monospacedSystemFontOfSize:BODY_PT - 1
                                           weight:kind == APPLE2HELP_KBD ? NSFontWeightSemibold : NSFontWeightRegular];
        a[NSBackgroundColorAttributeName] = NSColor.quaternaryLabelColor;
        break;
    default: break;
    }
    a[NSFontAttributeName] = font;
    return a;
}

- (void)append:(NSMutableAttributedString *)out text:(NSString *)text attrs:(NSDictionary *)attrs
{
    [out appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:attrs]];
}

- (void)append:(NSMutableAttributedString *)out spans:(const apple2help_block *)b bold:(BOOL)bold
{
    for (int i = 0; i < b->nspans; i++) {
        const apple2help_span *sp = &b->spans[i];
        if (sp->kind == APPLE2HELP_CELL) {
            [self append:out text:@"\t" attrs:[self attrsFor:APPLE2HELP_TEXT bold:bold]];
            continue;
        }
        NSMutableDictionary *a = [[self attrsFor:sp->kind bold:bold] mutableCopy];
        NSString *text = str(apple2help_span_text(sp, ME));
        if (sp->kind == APPLE2HELP_CODE || sp->kind == APPLE2HELP_KBD)
            text = [NSString stringWithFormat:@" %@ ", text];
        if (sp->kind == APPLE2HELP_LINK) {
            NSString *target = str(sp->target);
            NSURL *url = [target hasPrefix:@"#"] ? [NSURL URLWithString:[@"help:" stringByAppendingString:[target substringFromIndex:1]]]
                                                 : [NSURL URLWithString:target];
            if (url) a[NSLinkAttributeName] = url;
        }
        [self append:out text:text attrs:a];
    }
}

static NSMutableParagraphStyle *paraStyle(CGFloat before, CGFloat after, CGFloat first, CGFloat head,
                                          NSArray<NSNumber *> *tabs)
{
    NSMutableParagraphStyle *p = [[NSMutableParagraphStyle alloc] init];
    NSMutableArray *stops = [NSMutableArray array];
    for (NSNumber *t in tabs)
        [stops addObject:[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:t.doubleValue options:@{}]];
    p.paragraphSpacingBefore = before;
    p.paragraphSpacing = after;
    p.firstLineHeadIndent = first;
    p.headIndent = head;
    p.tabStops = stops;
    p.defaultTabInterval = 28;
    return p;
}

- (NSAttributedString *)page
{
    const apple2help_topic *tp = apple2help_topic_at(_topic);
    NSMutableAttributedString *out = [[NSMutableAttributedString alloc] init];
    [_starts removeAllObjects];
    if (!tp) return out;
    [self append:out text:[str(tp->title) stringByAppendingString:@"\n"]
           attrs:@{ NSFontAttributeName: [NSFont boldSystemFontOfSize:26],
                    NSForegroundColorAttributeName: NSColor.labelColor,
                    NSParagraphStyleAttributeName: paraStyle(0, 10, 0, 0, @[]) }];
    for (int i = 0; i < tp->nblocks; i++) {
        const apple2help_block *b = &tp->blocks[i];
        const NSUInteger start = out.length;
        NSParagraphStyle *style = nil;
        [_starts addObject:@(start)];
        if (!(b->platforms & ME)) continue;
        switch (b->kind) {
        case APPLE2HELP_H2:
        case APPLE2HELP_H3:
            [self append:out text:str(apple2help_heading(b))
                   attrs:@{ NSFontAttributeName: [NSFont boldSystemFontOfSize:b->kind == APPLE2HELP_H2 ? 18 : 14],
                            NSForegroundColorAttributeName: NSColor.labelColor }];
            style = paraStyle(b->kind == APPLE2HELP_H2 ? 16 : 10, 6, 0, 0, @[]);
            break;
        case APPLE2HELP_BULLET:
        case APPLE2HELP_BULLET2:
        case APPLE2HELP_NUMBERED: {
            const CGFloat base = b->kind == APPLE2HELP_BULLET2 ? 18 : 0;
            NSString *mark = b->kind == APPLE2HELP_NUMBERED ? [NSString stringWithFormat:@"%d.\t", b->number]
                           : b->kind == APPLE2HELP_BULLET ? @"•\t" : @"◦\t";
            [self append:out text:mark attrs:[self attrsFor:APPLE2HELP_TEXT bold:NO]];
            [self append:out spans:b bold:NO];
            style = paraStyle(0, 4, base, base + 18, @[@(base + 18)]);
            break;
        }
        case APPLE2HELP_PRE:
            [self append:out text:str(b->spans[0].text)
                   attrs:@{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:BODY_PT - 1 weight:NSFontWeightRegular],
                            NSForegroundColorAttributeName: NSColor.labelColor,
                            NSBackgroundColorAttributeName: NSColor.quaternaryLabelColor }];
            style = paraStyle(4, 10, 18, 18, @[]);
            break;
        case APPLE2HELP_TABLE_HEAD:
        case APPLE2HELP_TABLE_ROW: {
            NSMutableArray<NSNumber *> *tabs = [NSMutableArray array];
            CGFloat at = 0;
            for (int k = 0; k < b->ncol_em; k++) {
                at += b->col_em[k] * BODY_PT;
                [tabs addObject:@(at)];
            }
            [self append:out spans:b bold:b->kind == APPLE2HELP_TABLE_HEAD];
            style = paraStyle(b->kind == APPLE2HELP_TABLE_HEAD ? 4 : 0, 3, 0, at, tabs);
            break;
        }
        case APPLE2HELP_NOTE:
            [self append:out spans:b bold:NO];
            style = paraStyle(4, 10, 18, 18, @[]);
            break;
        default:
            [self append:out spans:b bold:NO];
            style = paraStyle(0, 8, 0, 0, @[]);
            break;
        }
        [self append:out text:@"\n" attrs:[self attrsFor:APPLE2HELP_TEXT bold:NO]];
        [out addAttribute:NSParagraphStyleAttributeName value:style range:NSMakeRange(start, out.length - start)];
    }
    /* the search's words */
    for (NSString *w in [_query componentsSeparatedByString:@" "]) {
        if (!w.length) continue;
        NSRange from = NSMakeRange(0, out.length), r;
        while ((r = [out.string rangeOfString:w options:NSCaseInsensitiveSearch range:from]).location != NSNotFound) {
            [out addAttributes:@{ NSBackgroundColorAttributeName: NSColor.findHighlightColor,
                                  NSForegroundColorAttributeName: NSColor.blackColor } range:r];
            from = NSMakeRange(NSMaxRange(r), out.length - NSMaxRange(r));
        }
    }
    return out;
}

/* the block's heading at the top */
- (void)scrollToBlock
{
    if (_block < 0 || _block >= (int)_starts.count) {
        [_text scrollPoint:NSZeroPoint];
        return;
    }
    NSLayoutManager *lm = _text.layoutManager;
    const NSUInteger at = _starts[_block].unsignedIntegerValue;
    if (!lm || at >= _text.string.length) return;
    [lm ensureLayoutForTextContainer:_text.textContainer];
    const NSRange glyphs = [lm glyphRangeForCharacterRange:NSMakeRange(at, 1) actualCharacterRange:NULL];
    const NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:_text.textContainer];
    [_text scrollPoint:NSMakePoint(0, r.origin.y + _text.textContainerOrigin.y - 8)];
}

- (void)showTopic:(int)topic block:(int)block query:(NSString *)query
{
    if (!apple2help_topic_at(topic)) return;
    _block = block;
    if (topic != _topic || ![query isEqualToString:_query]) {
        _topic = topic;
        _query = [query copy];
        [_text.textStorage setAttributedString:[self page]];
    }
    __weak Apple2HelpWindow *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf scrollToBlock]; });
}

- (BOOL)textView:(NSTextView *)view clickedOnLink:(id)link atIndex:(NSUInteger)index
{
    (void)view; (void)index;
    NSURL *url = [link isKindOfClass:[NSURL class]] ? link : [NSURL URLWithString:[link description]];
    if ([url.scheme isEqualToString:@"help"]) {
        [self showAt:[@"#" stringByAppendingString:url.resourceSpecifier]];
        return YES;
    }
    return NO;      /* AppKit opens it in the browser */
}

/* ---- the list ------------------------------------------------------------------- */

- (NSString *)searchText
{
    return [_search.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
}

- (void)fillList
{
    NSString *q = [self searchText];
    _filling = YES;
    [_rowTopic removeAllObjects];
    [_rowBlock removeAllObjects];
    _searching = q.length > 0;
    if (!_searching) {
        for (int t = 0; t < apple2help_topic_count(); t++) {
            [_rowTopic addObject:@(t)];
            [_rowBlock addObject:@(-1)];
        }
    } else {
        apple2help_hit hits[MAX_HITS];
        const int n = apple2help_search(ME, q.UTF8String, hits, MAX_HITS);
        for (int i = 0; i < n; i++) {
            [_rowTopic addObject:@(hits[i].topic)];
            [_rowBlock addObject:@(hits[i].block)];
        }
    }
    [_list reloadData];
    if (!_searching && _topic >= 0)
        [_list selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)_topic] byExtendingSelection:NO];
    _filling = NO;
}

- (void)selectRow:(NSInteger)row
{
    if (row < 0 || row >= (NSInteger)_rowTopic.count) return;
    [_list selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    [_list scrollRowToVisible:row];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table
{
    (void)table;
    return _searching && !_rowTopic.count ? 1 : (NSInteger)_rowTopic.count;
}

- (CGFloat)tableView:(NSTableView *)table heightOfRow:(NSInteger)row
{
    (void)table;
    return (_searching && row < (NSInteger)_rowBlock.count && _rowBlock[row].intValue >= 0) ? 38 : 24;
}

- (BOOL)tableView:(NSTableView *)table shouldSelectRow:(NSInteger)row
{
    (void)table;
    return row < (NSInteger)_rowTopic.count;
}

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    (void)column;
    NSTableCellView *cell = [table makeViewWithIdentifier:@"row" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = @"row";
        NSTextField *tf = [NSTextField labelWithString:@""];
        tf.lineBreakMode = NSLineBreakByTruncatingTail;
        tf.maximumNumberOfLines = 2;
        tf.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:tf];
        cell.textField = tf;
        [tf.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:4].active = YES;
        [tf.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4].active = YES;
        [tf.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor].active = YES;
    }
    /* plain text in the label's own colours, which turn with the selection */
    NSString *text;
    cell.textField.textColor = NSColor.labelColor;
    if (row >= (NSInteger)_rowTopic.count) {
        text = @"No matches";
        cell.textField.textColor = NSColor.secondaryLabelColor;
    } else {
        const apple2help_topic *tp = apple2help_topic_at(_rowTopic[row].intValue);
        const int b = _rowBlock[row].intValue;
        text = b < 0 ? str(tp->title)
                     : [NSString stringWithFormat:@"%@\n%@", str(apple2help_heading(&tp->blocks[b])), str(tp->title)];
    }
    cell.textField.font = [NSFont systemFontOfSize:BODY_PT];
    cell.textField.stringValue = text;
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification *)note
{
    (void)note;
    const NSInteger row = _list.selectedRow;
    if (_filling || row < 0 || row >= (NSInteger)_rowTopic.count) return;
    [self showTopic:_rowTopic[row].intValue block:_rowBlock[row].intValue query:_searching ? [self searchText] : @""];
}

- (void)searchChanged:(id)sender
{
    (void)sender;
    [self fillList];
}

/* in the search: Return opens the best match, Down goes into the list */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)view doCommandBySelector:(SEL)command
{
    (void)control; (void)view;
    if (command == @selector(insertNewline:)) {
        [self fillList];
        if (_searching) [self selectRow:0];
        return YES;
    }
    if (command == @selector(moveDown:)) {
        [_window makeFirstResponder:_list];
        if (_list.selectedRow < 0) [self selectRow:0];
        return YES;
    }
    return NO;
}

/* ---- the window ------------------------------------------------------------------ */

/* Resizing the window resizes the page, not the sidebar. */
- (BOOL)splitView:(NSSplitView *)split shouldAdjustSizeOfSubview:(NSView *)view
{
    (void)split;
    return view != _side;
}

- (CGFloat)splitView:(NSSplitView *)split constrainMinCoordinate:(CGFloat)p ofSubviewAt:(NSInteger)i
{
    (void)split; (void)p; (void)i;
    return 180;
}

- (CGFloat)splitView:(NSSplitView *)split constrainMaxCoordinate:(CGFloat)p ofSubviewAt:(NSInteger)i
{
    (void)split; (void)p; (void)i;
    return 420;
}

- (void)buildWindow
{
    const NSRect frame = NSMakeRect(0, 0, 980, 720);
    _window = [[Apple2HelpPanelWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"FujiNet Go Apple II Help";
    _window.releasedWhenClosed = NO;
    _window.tabbingMode = NSWindowTabbingModeDisallowed;
    _window.minSize = NSMakeSize(560, 360);
    _window.delegate = self;
    __weak Apple2HelpWindow *weakSelf = self;
    _window.onFind = ^{ [weakSelf showFor:nil]; };

    NSSplitView *split = [[NSSplitView alloc] initWithFrame:frame];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.delegate = self;

    NSVisualEffectView *side = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, SIDE_W, frame.size.height)];
    side.material = NSVisualEffectMaterialSidebar;
    side.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    _side = side;
    _search = [[NSSearchField alloc] initWithFrame:NSMakeRect(8, frame.size.height - 34, SIDE_W - 16, 24)];
    _search.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    _search.placeholderString = @"Search Help";
    _search.delegate = self;
    _search.target = self;
    _search.action = @selector(searchChanged:);
    [side addSubview:_search];
    NSScrollView *listScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, SIDE_W, frame.size.height - 42)];
    listScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    listScroll.hasVerticalScroller = YES;
    listScroll.drawsBackground = NO;
    _list = [[NSTableView alloc] initWithFrame:listScroll.bounds];
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"topic"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    column.width = SIDE_W - 20;
    [_list addTableColumn:column];
    _list.headerView = nil;
    _list.style = NSTableViewStyleSourceList;
    _list.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    _list.backgroundColor = NSColor.clearColor;
    _list.dataSource = self;
    _list.delegate = self;
    listScroll.documentView = _list;
    [side addSubview:listScroll];

    NSScrollView *textScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(SIDE_W + 1, 0, frame.size.width - SIDE_W - 1, frame.size.height)];
    textScroll.hasVerticalScroller = YES;
    const NSSize cs = textScroll.contentSize;
    /* TextKit 1: tab stops and the scroll to a heading behave the same on
     * every macOS this runs on */
    _text = [NSTextView textViewUsingTextLayoutManager:NO];
    _text.frame = NSMakeRect(0, 0, cs.width, cs.height);
    _text.minSize = NSMakeSize(0, cs.height);
    _text.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _text.verticallyResizable = YES;
    _text.horizontallyResizable = NO;
    _text.autoresizingMask = NSViewWidthSizable;
    _text.textContainer.containerSize = NSMakeSize(cs.width, FLT_MAX);
    _text.textContainer.widthTracksTextView = YES;
    _text.textContainerInset = NSMakeSize(24, 18);
    _text.editable = NO;
    _text.selectable = YES;
    _text.richText = YES;
    _text.drawsBackground = YES;
    _text.backgroundColor = NSColor.textBackgroundColor;
    _text.delegate = self;
    textScroll.documentView = _text;

    [split addSubview:side];
    [split addSubview:textScroll];
    _window.contentView = split;
    [split adjustSubviews];
    [split setPosition:SIDE_W ofDividerAtIndex:0];
    [_window center];
    [self fillList];
}

- (void)showAt:(NSString *)ident
{
    if (!_window) [self buildWindow];
    if ([ident hasPrefix:@"?"]) {
        _search.stringValue = [ident substringFromIndex:1];
        [self fillList];
        [self selectRow:0];
    } else if (ident || _topic < 0) {
        int t = 0, b = -1;
        if (!ident || apple2help_find(ident.UTF8String, &t, &b) != 0) {
            t = 0;
            b = -1;
        }
        if (_search.stringValue.length) {
            _search.stringValue = @"";
            [self fillList];
        }
        [self showTopic:t block:b query:@""];
        _filling = YES;
        [self selectRow:t];
        _filling = NO;
    }
    [_window makeKeyAndOrderFront:nil];
}

- (void)showFor:(NSWindow *)window
{
    if (!window || window == _window) {
        if (!_window) [self showAt:nil];
        [_window makeKeyAndOrderFront:nil];
        [_window makeFirstResponder:_search];
        return;
    }
    [self showAt:contextOf(window)];
}

/* ---- the Help menu's search (on any thread: no AppKit here) ---------------------------- */

- (void)searchForItemsWithSearchString:(NSString *)searchString
                           resultLimit:(NSInteger)resultLimit
                    matchedItemHandler:(void (^)(NSArray *items))handleMatchedItems
{
    apple2help_hit hits[MAX_HITS];
    NSMutableArray *items = [NSMutableArray array];
    const int max = resultLimit > 0 && resultLimit < MAX_HITS ? (int)resultLimit : MAX_HITS;
    const int n = apple2help_search(ME, searchString.UTF8String, hits, max);
    for (int i = 0; i < n; i++) {
        const apple2help_topic *tp = apple2help_topic_at(hits[i].topic);
        [items addObject:str(hits[i].block < 0 ? tp->id : tp->blocks[hits[i].block].id)];
    }
    handleMatchedItems(items);
}

- (NSArray<NSString *> *)localizedTitlesForItem:(id)item
{
    int t, b;
    if (![item isKindOfClass:[NSString class]] || apple2help_find([item UTF8String], &t, &b) != 0) return @[];
    const apple2help_topic *tp = apple2help_topic_at(t);
    if (b < 0) return @[str(tp->title)];
    return @[[NSString stringWithFormat:@"%@ ▸ %@", str(tp->title), str(apple2help_heading(&tp->blocks[b]))]];
}

- (void)performActionForItem:(id)item
{
    [self showAt:item];
}

- (void)showAllHelpTopicsForSearchString:(NSString *)searchString
{
    [self showAt:[@"?" stringByAppendingString:searchString]];
}

/* APPLE2_HELP_SCREENSHOT: the window as it looks */
- (BOOL)savePng:(NSString *)path
{
    NSView *v = _window.contentView;
    if (!v) return NO;
    NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    if (!rep) return NO;
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png && [png writeToFile:path atomically:YES];
}
@end
