// main.m - the macOS Chocobar bar.
//
// This is the front-end that replaces p_ui.c on macOS. It draws the bar, tracks
// the terminal it belongs to, and shows the token numbers the shared core
// computes. It is deliberately a NEW implementation rather than a port: the
// Windows bar is GDI into a layered window with a hand-rolled layout, and none
// of that carries over. What carries over is the tracking contract the probe
// measured (macos/probe/probe.m, which the macOS workflow runs on a schedule):
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
#include <unistd.h>

// ---- the core bridge (compiled into chocobar-core.o) ------------------------
// Declared here rather than in a shared header: this is the entire surface the
// macOS bar has on the portable core, and keeping it in one block makes the
// coupling auditable at a glance. (A generated cb_bridge.h shared with p_agg.c
// would turn signature drift into a compile error instead of a link error -
// planned, not rushed.)
extern void cbCoreInit(void);
extern int  cbLoadConfig(const wchar_t *path);
extern void cbTokensInit(void);
extern long long cbScanTokens(void);
extern long long cbTokAll(void);
extern int  cbTokAppCount(void);
extern const char *cbTokAppName(int i);
extern long long cbTokAppSum(int i);
extern int  cbTokSourceCount(void);
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
    // The adapter's constructor exports USERPROFILE from the same
    // profileDir() the core's "~" expansion reads (p_utils.c subsPathExpand),
    // so the config file and the stores, cursors and log resolve under ONE
    // home directory. Reading HOME here instead put the config in /tmp while
    // every core path used the real home whenever HOME was unset or empty
    // (a Finder launch), so the bar reported a config whose stores it could
    // never find.
    const char *home = getenv("USERPROFILE");
    snprintf(g_configPath, sizeof(g_configPath), "%s/.wizbar/config.json", home && *home ? home : "/tmp");}

// ---- headless self-test --------------------------------------------------------
//
// CHOCOBAR_SELFTEST=1 runs the ENTIRE core seam with no GUI: adapter init,
// config load, cursor load, one full scan. Prints one line, exits 0.
//
// CHOCOBAR_SELFTEST=2 additionally performs a SECOND scan after waiting for
// the file named by CHOCOBAR_SELFTEST_MARKER: the harness appends a record to
// the store and creates that file, so the second scan exercises the real warm
// path - unchanged files skipped by cursor size+mtime, the appended tail read
// by resuming from the stored byte offset through SetFilePointerEx, and the
// aggregates PRESERVED (only the new record may add). This is the regression
// net for the whole seam: path translation, directory enumeration, seek
// semantics, codecs, cursor persistence, aggregation. A bar that merely
// launches proves none of it.
// The seam startup both entry points share: init the config lock, load byte
// cursors, encode the resolved config path wide (the -1 pass converts and
// counts the terminating NUL), and load it. Returns -1 when the path cannot
// be encoded, 0 when the load fails, 1 on success. The self-test refuses on
// any non-1; the GUI path logs instead of proceeding silently.
static int loadCoreConfig(wchar_t *wideCfg, size_t cap) {
    cbCoreInit();
    cbTokensInit();
    int wl = MultiByteToWideChar(CP_UTF8, 0, g_configPath, -1, wideCfg, (int)cap);
    if (wl <= 0) return -1;
    return cbLoadConfig(wideCfg) ? 1 : 0;
}

static int runSelfTest(void) {
    resolveConfigPath();   // the app delegate normally does this; headless mode has none
    wchar_t wideCfg[1024];
    int r = loadCoreConfig(wideCfg, 1024);
    if (r < 0) { fprintf(stderr, "SELFTEST FAIL cannot encode config path\n"); return 1; }
    if (r == 0) { fprintf(stderr, "SELFTEST FAIL config did not load\n"); return 1; }
    long long first = cbScanTokens();
    printf("SELFTEST SCAN1 %lld TOK_ALL %lld APPS %d SOURCES %d\n",
           first, cbTokAll(), cbTokAppCount(), cbTokSourceCount());
    fflush(stdout);
    const char *mode = getenv("CHOCOBAR_SELFTEST");
    if (mode && strcmp(mode, "2") == 0) {
        const char *marker = getenv("CHOCOBAR_SELFTEST_MARKER");
        for (int i = 0; marker && i < 200 && access(marker, F_OK) != 0; i++)
            usleep(50000);   // up to 10s for the harness to append and signal
        long long second = cbScanTokens();
        printf("SELFTEST SCAN2 %lld TOK_ALL %lld\n", second, cbTokAll());
    }
    return 0;
}

// ---- terminal discovery --------------------------------------------------------
//
// Which window to follow. The Windows bar identifies the terminal by window
// CLASS then by a title substring (terminal.className / terminal.title in the
// config). macOS has no window classes, so the frontmost app is the target:
// the bar belongs to whatever you are looking at, which needs no configuration
// at all. A config-selected bundle-id allowlist is the natural next step and
// is why the tracking helpers take a PID instead of a global.

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

static const double kCorner = 8.0;

// One bar: a rounded, translucent strip with the token numbers. Drawn with
// CoreGraphics into the panel's view; no GDI, no layered window. The bar's
// height and gap come from the config through g_height/g_gap; there are no
// separate constants to keep in sync.
@interface BarView : NSView
@property(nonatomic, assign) long long tokAll;
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
    // leads with. 0 means nothing has been counted yet (fresh start, or the
    // master switch reset everything) - print an em-dash-like placeholder,
    // never a bare 0 that reads like a real measurement.
    NSString *head = self.tokAll > 0
        ? [NSString stringWithFormat:@"%@", [BarView compact:self.tokAll]]
        : @"--";
    [head drawAtPoint:NSMakePoint(x, midY - 7) withAttributes:attrs];
    x += [head sizeWithAttributes:attrs].width + 14.0;

    // Per-app breakdown, capped at four rows AND at the strip's width: the
    // count only advances for rows actually drawn, so blank or zero-sum apps
    // cannot eat the budget and leave the strip showing fewer real rows.
    int shown = 0;
    for (int i = 0; i < self.appCount && shown < 4; i++) {
        const char *name = cbTokAppName(i);
        if (!name || !*name) continue;
        long long sum = cbTokAppSum(i);
        if (sum <= 0) continue;
        NSString *line = [NSString stringWithFormat:@"%s %@", name, [BarView compact:sum]];
        [line drawAtPoint:NSMakePoint(x, midY - 6.5) withAttributes:dim];
        x += [line sizeWithAttributes:dim].width + 12.0;
        shown++;
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
static int g_height = 30, g_gap = 4;

// The tracking tick's cadence. The rescan period is computed from this same
// constant (see tick), so retuning the tick rescales scans deliberately - the
// two numbers can no longer drift apart silently.
static const double kTickInterval = 0.25;   // seconds: 4 Hz

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
//
// WHAT THE PROBE DID AND DID NOT MEASURE (probe.m carries the matching note):
// the probe proved that a panel CAN track another process's window with zero
// focus theft. It runs a 200x200 predicate at 20 Hz for ~6 s and hides on its
// own schedule. This bar runs a 200x120 predicate at 4 Hz forever, and hides
// when the target has no qualifying window (minimized, or on another Space).
// The probe is evidence the mechanism works; only this code defines the bar's
// rules.
static void tick(void) {
    // Rescan on the configured cadence, BEFORE any target-window early
    // return: the scan must not stop while the frontmost app has no
    // qualifying window (small, absent, or on another Space, whose windows
    // the on-screen list never returns). That is when the numbers most need
    // to stay current, and the Windows bar scans from an unconditional timer
    // the same way (p_ui.c's TIMER_METRICS -> buildChips -> scanTokenCache).
    static int tickCount = 0;
    int period = cbCfgRescanSec() > 0 ? cbCfgRescanSec() : 60;
    if (tickCount % (int)(period / kTickInterval) == 0) {
        cbScanTokens();
        refreshNumbers();
    }
    tickCount++;

    pid_t t = targetPID();
    if (!t) {
        [g_panel orderOut:nil];
        return;
    }
    WinRect target = largestWindowForPID(t);
    if (!target.valid) {
        [g_panel orderOut:nil];
        return;
    }
    // Anchor to the screen the target is ON, not whichever screen is main: a
    // secondary-display target must clamp against its own visible frame. The
    // target arrives as a Quartz rect (top-left origin, global) while
    // NSScreen.frame is AppKit (bottom-left origin, anchored at the main
    // screen), so the centre is converted BEFORE the containment test - and
    // the flip always uses the MAIN screen's height, because that is what the
    // global Quartz frame is measured from.
    NSScreen *screen = [NSScreen mainScreen];
    if (!screen) { [g_panel orderOut:nil]; return; }   // headless/background session: nowhere to draw
    CGFloat mainH = NSHeight(screen.frame);
    NSPoint centre = NSMakePoint(target.x + target.w / 2,
                                 mainH - (target.y + target.h / 2));
    for (NSScreen *s in [NSScreen screens])
        if (NSPointInRect(centre, s.frame)) { screen = s; break; }
    NSRect vf = screen.visibleFrame;
    CGFloat appY = mainH - (target.y - g_gap) - g_height;
    // Clamp into the visible frame: never above the menu bar's band (a
    // status-level panel would COVER the menu), never below the Dock.
    CGFloat maxY = NSMaxY(vf) - g_height;
    CGFloat minY = NSMinY(vf);
    if (appY > maxY) appY = maxY;
    if (appY < minY) appY = minY;
    NSRect want = NSMakeRect(target.x, appY, target.w, g_height);
    if (![g_panel isVisible]) [g_panel orderFrontRegardless];
    [g_panel setFrame:want display:YES];
    [g_panel displayIfNeeded];
}

@implementation Chocobar
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    resolveConfigPath();

    // Load cursors before the config, then the config, then the first scan:
    // the same startup ordering the Windows bar uses (tokLiveInit is called
    // before the first scan so a warm start resumes from its byte cursors).
    wchar_t wideCfg[1024];
    int r = loadCoreConfig(wideCfg, 1024);
    if (r < 0) {
        NSLog(@"[wizbar] mac bar: cannot encode config path, aborting");
        [NSApp terminate:nil];
        return;
    }
    if (r == 0)
        NSLog(@"[wizbar] mac bar: config did not load - running on the core defaults");

    int h = cbCfgHeight();
    int gp = cbCfgGap();
    if (h > 8 && h < 200) g_height = h;
    if (gp >= 0 && gp < 200) g_gap = gp;

    g_panel = makePanel();
    g_view = (BarView *)g_panel.contentView;

    refreshNumbers();
    cbScanTokens();
    refreshNumbers();

    [NSTimer scheduledTimerWithTimeInterval:kTickInterval
                                     target:self
                                   selector:@selector(onTick:)
                                   userInfo:nil
                                    repeats:YES];
    NSLog(@"[wizbar] mac bar up: config=%s height=%d gap=%d", g_configPath, g_height, g_gap);
}

- (void)onTick:(NSTimer *)t {
    (void)t;
    tick();
}

- (void)applicationWillTerminate:(NSNotification *)note {
    (void)note;
    [g_panel orderOut:nil];
}
@end

int main(int argc, const char *argv[]) {
    g_argc = argc;
    g_argv = argv;
    // Headless self-test: exercise the core seam and exit before any AppKit
    // work, so CI can run it on a runner with no window server interaction.
    if (getenv("CHOCOBAR_SELFTEST")) return runSelfTest();
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
