// main.m - the macOS Chocobar bar.
//
// This is the front-end that replaces p_ui.c on macOS. It draws the bar, tracks
// the terminal it belongs to, and shows the token numbers the shared core
// computes. It is deliberately a NEW implementation rather than a port: the
// Windows bar is GDI into a layered window with a hand-rolled layout, and none
// of that carries over. What carries over is the tracking contract the probe
// measured (docs/reviews/2026-10-11-window-attachment-research.md):
//
//   - the bar is NOT a child of the terminal. macOS has no cross-process
//     window parenting, so the bar is a non-activating NSPanel at status level
//     that tracks the target window's bounds from CGWindowListCopyWindowInfo.
//   - it never takes focus, survives Spaces, and hides when the target is
//     minimized or gone.
//
// What is shared with Windows is the core: config parsing, the token scan, and
// now the aggregation (p_agg.c), compiled unchanged behind chocobar_core.h.

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

// Declares the Win32 spellings the core compiles against. main.m needs exactly
// one of them: MultiByteToWideChar, to hand the core a wide config path the way
// it expects. Without this the codec call compiles as an implicit declaration
// and CP_UTF8 does not exist at all.
#include "chocobar_core.h"

#include <stdlib.h>
#include <string.h>

// ---- the core bridge (compiled into chocobar-core.o) ------------------------
// Declared here rather than in a shared header: this is the entire surface the
// macOS bar has on the portable core, and keeping it in one block makes the
// coupling auditable at a glance.
extern int  cbLoadConfig(const wchar_t *path);
extern void cbTokensInit(void);
extern long long cbScanTokens(void);
extern long long cbTokAll(void);
extern long long cbTokWeek(void);
extern long long cbTokMonth(void);
extern long long cbTokensToday(void);
extern int  cbTokAppCount(void);
extern const char *cbTokAppName(int i);
extern long long cbTokAppSum(int i);
extern int  cbTokModelCount(void);
extern const char *cbTokModelLabel(int i);
extern long long cbTokModelSum(int i);
extern int  cbTokSourceCount(void);
extern const char *cbTokSourceApp(int i);
extern int  cbCfgTokensEnabled(void);
extern int  cbCfgRescanSec(void);
extern int  cbCfgHeight(void);
extern int  cbCfgGap(void);

// ---- config -------------------------------------------------------------------
//
// The Windows bar reads %USERPROFILE%\.wizbar\config.json. macOS keeps the same
// file name and the same shape so one config works on both, at
// ~/.wizbar/config.json, with a --config override for testing.
static char g_configPath[1024];

// Real C argv, captured in main(). NSProcessInfo.arguments is an NSArray of
// NSString, which cannot be passed where the C loop below expects const char **.
static int g_argc = 0;
static const char *const *g_argv = NULL;

static void resolveConfigPath(void) {
    for (int i = 1; i < g_argc - 1; i++) {
        if (!strcmp(g_argv[i], "--config") && g_argv[i + 1][0]) {
            snprintf(g_configPath, sizeof(g_configPath), "%s", g_argv[i + 1]);
            return;
        }
    }
    const char *home = getenv("HOME");
    snprintf(g_configPath, sizeof(g_configPath), "%s/.wizbar/config.json", home ? home : "/tmp");
}

// ---- terminal discovery --------------------------------------------------------

// Which window to follow. The Windows bar identifies the terminal by window
// CLASS then by a title substring (terminal.className / terminal.title in the
// config). macOS has no window classes, so the equivalent is BUNDLE ID plus
// title substring: bundle id is the stable, user-visible identity of a terminal
// app, and it is what a person would put in a config.
//
// Defaults to the frontmost app that is not us, which is the macOS-native
// behaviour: the bar belongs to whatever you are looking at. That is a better
// default than Windows' class list because it needs no configuration at all.

static NSArray<NSString *> *terminalBundleIDs(void) {
    // Read from the config if the user named one, else follow the frontmost app.
    return nil; // frontmost-app mode
}

static pid_t frontmostPID(void) {
    return [NSWorkspace sharedWorkspace].frontmostApplication.processIdentifier;
}

static pid_t ourPID(void) { return getpid(); }

typedef struct { double x, y, w, h; BOOL valid; } WinRect;

static NSArray<NSValue *> *windowsForPID(pid_t pid) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list) return @[];
    NSMutableArray<NSValue *> *out = [NSMutableArray array];
    CFIndex n = CFArrayGetCount(list);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef info = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        CFNumberRef owner = (CFNumberRef)CFDictionaryGetValue(info, kCGWindowOwnerPID);
        int ownerPid = 0;
        if (!owner || !CFNumberGetValue(owner, kCFNumberIntType, &ownerPid) || ownerPid != pid) continue;
        CFNumberRef layer = (CFNumberRef)CFDictionaryGetValue(info, kCGWindowLayer);
        int layerNo = 0;
        if (layer && CFNumberGetValue(layer, kCFNumberIntType, &layerNo) && layerNo != 0) continue;
        CFDictionaryRef b = (CFDictionaryRef)CFDictionaryGetValue(info, kCGWindowBounds);
        if (!b) continue;
        CGRect rect;
        if (!CGRectMakeWithDictionaryRepresentation(b, &rect)) continue;
        if (rect.size.width < 200 || rect.size.height < 120) continue;
        WinRect r = {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height, YES};
        [out addObject:[NSValue valueWithBytes:&r objCType:@encode(WinRect)]];
    }
    CFRelease(list);
    return out;
}

static WinRect largestWindowForPID(pid_t pid) {
    WinRect best = {0, 0, 0, 0, NO};
    for (NSValue *v in windowsForPID(pid)) {
        WinRect r;
        [v getValue:&r];
        if (!best.valid || r.w * r.h > best.w * best.h) best = r;
    }
    return best;
}

// The PID whose window the bar belongs to right now.
static pid_t targetPID(void) {
    pid_t fm = frontmostPID();
    if (fm && fm != ourPID()) return fm;
    return 0;
}

// ---- the bar view ---------------------------------------------------------------

static const double kBarHeight = 30.0;
static const double kBarGap = 4.0;
static const double kCorner = 8.0;

// One bar: a rounded, translucent strip with the token numbers. Drawn with
// CoreGraphics into the panel's view; no GDI, no layered window.
@interface BarView : NSView
@property(nonatomic, assign) long long tokAll;
@property(nonatomic, assign) long long tokToday;
@property(nonatomic, assign) long long tokWeek;
@property(nonatomic, assign) long long tokMonth;
@property(nonatomic, assign) int appCount;
@property(nonatomic, copy) NSString *statusLine;
@end

@implementation BarView

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)canBecomeKeyWindow { return NO; }

// Match the Windows bar's palette: a warm off-white ("#FBF2E2" is the shipped
// backgroundTint default) with the deep pink accent for numbers.
- (void)drawRect:(NSRect)dirty {
    NSRect r = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:r xRadius:kCorner yRadius:kCorner];
    NSColor *fill = [NSColor colorWithCalibratedRed:0.984 green:0.949 blue:0.886 alpha:0.92];
    [fill setFill];
    [bg fill];
    [[NSColor colorWithCalibratedRed:0.85 green:0.72 blue:0.62 alpha:0.55] setStroke];
    bg.lineWidth = 1.0;
    [bg stroke];

    NSDictionary *attrs = @{
        NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.72 green:0.36 blue:0.45 alpha:1.0],
    };
    NSDictionary *dim = @{
        NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.42 green:0.38 blue:0.36 alpha:1.0],
    };

    double x = 12.0;
    double midY = NSHeight(self.bounds) / 2.0;

    // The headline number: all-time tokens, the same figure the Windows bar
    // leads with. -1 means "nothing read yet", which is an em dash on Windows
    // and must not print as a literal -1 here.
    NSString *head = self.tokAll > 0
        ? [NSString stringWithFormat:@"%@", [BarView compact:self.tokAll]]
        : @"--";
    [head drawAtPoint:NSMakePoint(x, midY - 7) withAttributes:attrs];
    x += [head sizeWithAttributes:attrs].width + 14.0;

    // Per-app breakdown, capped so a long source list cannot overflow the strip.
    int shown = 0;
    for (int i = 0; i < self.appCount && shown < 4; i++, shown++) {
        const char *name = cbTokAppName(i);
        if (!name || !*name) continue;
        long long sum = cbTokAppSum(i);
        if (sum <= 0) continue;
        NSString *line = [NSString stringWithFormat:@"%s %@", name, [BarView compact:sum]];
        [line drawAtPoint:NSMakePoint(x, midY - 6.5) withAttributes:dim];
        x += [line sizeWithAttributes:dim].width + 12.0;
        if (x > NSWidth(self.bounds) - 120) break;
    }

    // Right-aligned: the scan status, so a bar that is running but reading
    // nothing is visibly different from a bar that is not running.
    if (self.statusLine.length) {
        NSDictionary *sa = @{
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular],
            NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.55 green:0.50 blue:0.47 alpha:1.0],
        };
        NSSize sz = [self.statusLine sizeWithAttributes:sa];
        [self.statusLine drawAtPoint:NSMakePoint(NSWidth(self.bounds) - sz.width - 12.0, midY - 6)
                       withAttributes:sa];
    }
}

// 1234567 -> "1.2M", matching the Windows bar's fmtTokens shape.
+ (NSString *)compact:(long long)n {
    if (n >= 1000000000LL) return [NSString stringWithFormat:@"%.1fB", n / 1e9];
    if (n >= 1000000LL) return [NSString stringWithFormat:@"%.1fM", n / 1e6];
    if (n >= 1000LL) return [NSString stringWithFormat:@"%.1fk", n / 1e3];
    return [NSString stringWithFormat:@"%lld", n];
}

@end

// ---- the app controller ----------------------------------------------------------

@interface Chocobar : NSObject <NSApplicationDelegate>
@end

static NSPanel *g_panel = nil;
static BarView *g_view = nil;
static pid_t g_target = 0;
static BOOL g_quit = NO;
static int g_height = 30, g_gap = 4;

static NSPanel *makePanel(void) {
    NSPanel *p = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 10, g_height)
                                            styleMask:(NSWindowStyleMaskBorderless |
                                                       NSWindowStyleMaskNonactivatingPanel)
                                              backing:NSBackingStoreBuffered
                                                defer:YES];
    // Every flag here is load bearing, and each was a probe hypothesis:
    // nonactivatingPanel -> never takes focus; statusBar level -> floats above
    // ordinary and full-screen apps; canJoinAllSpaces + fullScreenAuxiliary ->
    // survives Spaces and shows over a full-screen terminal; stationary ->
    // does not jump when Spaces change; ignoresCycle -> not in Cmd+Tab.
    p.level = NSStatusWindowLevel;
    p.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                           NSWindowCollectionBehaviorFullScreenAuxiliary |
                           NSWindowCollectionBehaviorStationary |
                           NSWindowCollectionBehaviorIgnoresCycle;
    p.opaque = NO;
    p.backgroundColor = [NSColor clearColor];
    p.hasShadow = NO;
    p.releasedWhenClosed = NO;
    p.hidesOnDeactivate = NO;

    BarView *v = [[BarView alloc] initWithFrame:NSMakeRect(0, 0, 10, g_height)];
    v.wantsLayer = YES;
    p.contentView = v;
    return p;
}

static void refreshNumbers(void) {
    g_view.tokAll = cbTokAll();
    g_view.tokToday = cbTokensToday();
    g_view.tokWeek = cbTokWeek();
    g_view.tokMonth = cbTokMonth();
    g_view.appCount = cbTokAppCount();

    if (!cbCfgTokensEnabled()) {
        g_view.statusLine = @"tokens off";
    } else {
        int sources = cbTokSourceCount();
        g_view.statusLine = [NSString stringWithFormat:@"%d src%@", sources, sources == 1 ? @"" : @"s"];
    }
    [g_view setNeedsDisplay:YES];
}

// One tracking + refresh tick. The panel follows the target window's top edge,
// exactly as the probe measured.
static void tick(void) {
    pid_t t = targetPID();
    g_target = t;
    if (!t) {
        [g_panel orderOut:nil];
        return;
    }
    WinRect target = largestWindowForPID(t);
    if (!target.valid) {
        [g_panel orderOut:nil];
        return;
    }
    CGFloat screenH = NSHeight([NSScreen mainScreen].frame);
    NSRect want = NSMakeRect(target.x,
                             screenH - (target.y - g_gap) - g_height,
                             target.w,
                             g_height);
    if (![g_panel isVisible]) [g_panel orderFrontRegardless];
    [g_panel setFrame:want display:YES];
    [g_panel displayIfNeeded];

    // Rescan on the configured cadence. macOS runs the scan inline (no worker
    // thread): it reads JSONL session stores, which is milliseconds, and the
    // numbers must be current when the panel repaints.
    static int tickCount = 0;
    int period = cbCfgRescanSec() > 0 ? cbCfgRescanSec() : 60;
    if (tickCount % (period * 4) == 0) {   // the tick runs at 4 Hz
        cbScanTokens();
        refreshNumbers();
    }
    tickCount++;
}

static void timerFired(NSTimer *t) {
    (void)t;
    tick();
    if (g_quit) [NSApp terminate:nil];
}

@implementation Chocobar
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    resolveConfigPath();


    // Load cursors before the config, then the config, then the first scan:
    // the same startup ordering the Windows bar uses (tokLiveInit is called
    // before the first scan so a warm start resumes from its byte cursors).
    cbTokensInit();

    // The core takes a wide path, exactly as on Windows.
    wchar_t wideCfg[1024];
    MultiByteToWideChar(CP_UTF8, 0, g_configPath, -1, wideCfg, 1024);
    cbLoadConfig(wideCfg);

    int h = cbCfgHeight();
    int gp = cbCfgGap();
    if (h > 8 && h < 200) g_height = h;
    if (gp >= 0 && gp < 200) g_gap = gp;

    g_panel = makePanel();
    g_view = (BarView *)g_panel.contentView;

    refreshNumbers();
    cbScanTokens();
    refreshNumbers();

    [NSTimer scheduledTimerWithTimeInterval:0.25
                                     target:self
                                   selector:@selector(onTick:)
                                   userInfo:nil
                                    repeats:YES];
    NSLog(@"[wizbar] mac bar up: config=%s height=%d gap=%d", g_configPath, g_height, g_gap);
}

- (void)onTick:(NSTimer *)t { timerFired(t); }

- (void)applicationWillTerminate:(NSNotification *)note {
    (void)note;
    [g_panel orderOut:nil];
}
@end

int main(int argc, const char *argv[]) {
    g_argc = argc;
    g_argv = argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        // Accessory, not regular: no Dock icon, no menu bar takeover. The bar
        // is a floating panel, not an app the user manages.
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        Chocobar *delegate = [[Chocobar alloc] init];
        app.delegate = delegate;
        // A plain run loop, not a modal one: no windows are ever "opened" here.
        [app run];
    }
    return 0;
}
