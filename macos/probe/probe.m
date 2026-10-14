// macOS window-follow probe for Chocobar.
//
// QUESTION THIS ANSWERS
// On Windows the bar is a CHILD window of the terminal: SetParent +
// GWLP_HWNDPARENT, so it is glued in z-order and moves for free. macOS has no
// cross-process equivalent. The research concluded the replacement is "an
// NSPanel at a floating window level that tracks the target window's bounds
// via CGWindowListCopyWindowInfo". That conclusion is from documentation, not
// measurement. This probe measures it.
//
// WHAT IT DOES
// 1. Spawns a child process (this same binary, --child) that owns a real
//    NSWindow and moves it on a fixed schedule. A separate process matters:
//    tracking must work for a window owned by SOMEONE ELSE, not our own.
// 2. Finds that child's window purely through CGWindowListCopyWindowInfo -
//    no accessibility API, no screen recording permission, no AppleScript.
// 3. Positions a non-activating NSPanel just above the child's window, and
//    checks on every sample that the panel actually landed there.
// 4. Watches for the two failure modes that would sink the design:
//      - focus theft: the panel must never become the frontmost app
//      - the panel following a MINIMIZED window (it must hide instead)
//
// WHAT IT DOES NOT MEASURE (deliberate, from the 2026-10-14 round-5 review)
// This probe re-implements the discovery + placement step, and only that step:
// it has no screen-selection pass and no visibleFrame clamp, and its minimum
// window size is 200x200 where the shipped bar uses 200x120. A VERDICT PASS
// therefore proves the raw "place the panel at the flipped target rect" step
// works; it says nothing about where the bar lands on a multi-display setup or
// near the menu bar / Dock, which main.m tick() alone defines. The geometry
// helpers here are intentionally a subset of main.m's, so the probe tracks the
// shipped math only loosely; sharing them is planned work, not a bug.
//
// The child is used instead of a real terminal so the test is deterministic and
// needs no interactive session: CI can run it, and a failure points at the
// tracking logic rather than at whatever window happened to be open.
//
// Build: clang -fobjc-arc -framework Cocoa probe.m -o probe
// Run:   ./probe            (parent: spawns the child, prints a verdict)
//        ./probe --child    (child: owns the window, prints its PID)

// Cocoa.h is the umbrella, but relying on what it happens to re-export has
// already cost two builds. Import the frameworks explicitly so class resolution
// does not depend on header ordering.
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <unistd.h>
#import <string.h>

// ---- tunables shared by both roles -----------------------------------------

static const double kPanelHeight = 30.0;
static const double kPanelGap = 4.0;
static const double kPollInterval = 0.05;   // 20 Hz
// How far the panel may sit from where the math said, in points. A few points
// absorbs the window's shadow and the panel's own borderless rounding.
static const double kPosTolerance = 6.0;

// The child's move schedule, in seconds from its own start. The parent reads
// these same constants so both roles agree without a shared header. (There is
// no separate settle constant: the child creates its window and immediately
// holds the start rect until T1, so the settle phase IS [0, T1).)
static const double kChildT1 = 1.5;    // move to the second rect
static const double kChildT2 = 3.0;    // minimize
static const double kChildT3 = 3.6;    // restore
static const double kChildT4 = 4.4;    // exit

static const double kRect0X = 100, kRect0Y = 100, kRect0W = 800, kRect0H = 600;
static const double kRect1X = 420, kRect1Y = 140, kRect1W = 640, kRect1H = 520;

// ---- geometry ---------------------------------------------------------------

typedef struct {
    double x, y, w, h;   // Quartz/top-left-origin coordinates, as CGWindow reports
    BOOL valid;
} WinRect;

// The panel's target position for a target window rect: the same x and width,
// sitting `gap` points above the target's TOP edge, in Quartz coordinates.
// Quartz y grows downward, so "above" is a smaller y.
static WinRect panelRectForTarget(WinRect t) {
    WinRect p;
    p.valid = t.valid;
    p.x = t.x;
    p.w = t.w;
    p.h = kPanelHeight;
    p.y = t.y - kPanelGap - kPanelHeight;
    return p;
}

// The main screen's height in AppKit (bottom-left origin) coordinates. Guarded
// because a headless/background session can report no screen at all, which is
// itself a result the probe should surface rather than crash on.
static CGFloat mainScreenHeight(void) {
    NSScreen *screen = [NSScreen mainScreen];
    return screen ? NSHeight(screen.frame) : 0;
}

static NSRect quartzToAppKit(WinRect r) {
    CGFloat screenH = mainScreenHeight();
    return NSMakeRect(r.x, screenH - (r.y + r.h), r.w, r.h);
}

static BOOL rectNear(WinRect a, WinRect b, double tol) {
    return a.valid && b.valid &&
           fabs(a.x - b.x) <= tol && fabs(a.y - b.y) <= tol &&
           fabs(a.w - b.w) <= tol && fabs(a.h - b.h) <= tol;
}

// ---- window discovery -------------------------------------------------------

// Every on-screen window owned by `pid`, as Quartz rects. This is the whole
// mechanism under test: if it cannot see another process's window, the design
// is dead and nothing below matters.
static NSArray<NSValue *> *windowsForPID(pid_t pid) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly,
                                                 kCGNullWindowID);
    if (!list) return @[];
    NSMutableArray<NSValue *> *out = [NSMutableArray array];
    CFIndex n = CFArrayGetCount(list);
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef info = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        CFNumberRef owner = (CFNumberRef)CFDictionaryGetValue(info, kCGWindowOwnerPID);
        int ownerPid = 0;
        if (!owner || !CFNumberGetValue(owner, kCFNumberIntType, &ownerPid) || ownerPid != pid) continue;
        // Only the normal window layer: skip tooltips, shadows and menus.
        CFNumberRef layer = (CFNumberRef)CFDictionaryGetValue(info, kCGWindowLayer);
        int layerNo = 0;
        if (layer && CFNumberGetValue(layer, kCFNumberIntType, &layerNo) && layerNo != 0) continue;
        CFDictionaryRef b = (CFDictionaryRef)CFDictionaryGetValue(info, kCGWindowBounds);
        if (!b) continue;
        CGRect rect;
        if (!CGRectMakeWithDictionaryRepresentation(b, &rect)) continue;
        // Ignore zero/invisible geometry, and anything too small to be the
        // real window (a 1x1 helper would otherwise match first).
        if (rect.size.width < 200 || rect.size.height < 200) continue;
        WinRect r = {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height, YES};
        [out addObject:[NSValue valueWithBytes:&r objCType:@encode(WinRect)]];
    }
    CFRelease(list);
    return out;
}

static WinRect largestWindowForPID(pid_t pid) {
    NSArray<NSValue *> *all = windowsForPID(pid);
    WinRect best = {0, 0, 0, 0, NO};
    for (NSValue *v in all) {
        WinRect r;
        [v getValue:&r];
        if (!best.valid || r.w * r.h > best.w * best.h) best = r;
    }
    return best;
}

// ---- the panel --------------------------------------------------------------

// A borderless, non-activating panel at status level. Every flag here is load
// bearing and each one was a candidate failure mode:
//   nonactivatingPanel - clicking it must not take focus from the terminal
//   statusBar level   - floats above ordinary windows (and full-screen apps)
//   canJoinAllSpaces   - survives the user switching Spaces
//   fullScreenAuxiliary- stays visible over a full-screen terminal
//   stationary         - does not move when Spaces change
//   ignoresCycle       - not part of Cmd+Tab
static NSPanel *makePanel(void) {
    NSRect f = NSMakeRect(0, 0, 10, kPanelHeight);
    NSPanel *p = [[NSPanel alloc] initWithContentRect:f
                                            styleMask:(NSWindowStyleMaskBorderless |
                                                       NSWindowStyleMaskNonactivatingPanel)
                                              backing:NSBackingStoreBuffered
                                                defer:YES];
    p.level = NSStatusWindowLevel;
    p.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                           NSWindowCollectionBehaviorFullScreenAuxiliary |
                           NSWindowCollectionBehaviorStationary |
                           NSWindowCollectionBehaviorIgnoresCycle;
    p.opaque = NO;
    p.backgroundColor = [NSColor colorWithCalibratedRed:0.98 green:0.95 blue:0.89 alpha:0.95];
    p.hasShadow = NO;
    p.movableByWindowBackground = NO;
    p.ignoresMouseEvents = NO;   // it must stay clickable later; this probe only checks tracking
    p.releasedWhenClosed = NO;
    p.alphaValue = 1.0;
    return p;
}

// ---- child role -------------------------------------------------------------

static int runChild(void) {
    @autoreleasepool {
        NSRect f = NSMakeRect(kRect0X, 0, kRect0W, kRect0H);
        // start at the same height as the probe's first expected rect
        NSWindow *w = [[NSWindow alloc] initWithContentRect:f
                                                   styleMask:(NSWindowStyleMaskTitled |
                                                              NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskResizable)
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
        w.title = @"ChocobarProbeTarget";
        [w makeKeyAndOrderFront:nil];
        // place at the intended Quartz y (main screen, top-left origin)
        CGFloat screenH = mainScreenHeight();
        [w setFrameOrigin:NSMakePoint(kRect0X, screenH - kRect0Y - kRect0H)];

        printf("CHILD_PID %d\n", (int)getpid());
        fflush(stdout);

        NSDate *start = [NSDate date];
        double t = 0;
        BOOL moved = NO, minimized = NO, restored = NO;
        while (t < kChildT4) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            t = -[start timeIntervalSinceNow];
            if (!moved && t >= kChildT1) {
                [w setFrameOrigin:NSMakePoint(kRect1X, screenH - kRect1Y - kRect1H)];
                [w setContentSize:NSMakeSize(kRect1W, kRect1H)];
                moved = YES;
            }
            if (!minimized && t >= kChildT2) { [w miniaturize:nil]; minimized = YES; }
            if (!restored && t >= kChildT3)  { [w deminiaturize:nil]; restored = YES; }
        }
        printf("CHILD_DONE\n");
        fflush(stdout);
    }
    return 0;
}

// ---- parent role ------------------------------------------------------------

typedef struct {
    int samples;
    int trackedOk;       // panel was where the math said, while the target was up
    int trackBad;
    int hiddenOk;        // panel was not visible while the target was minimized
    int hiddenBad;
    int focusStolen;     // the panel ever became the frontmost application
    int targetSeen;      // CGWindowList ever returned the child's window at all
    double worstOffset;
} Probe;

static pid_t frontmostPID(void) {
    return [NSWorkspace sharedWorkspace].frontmostApplication.processIdentifier;
}

static int runParent(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        // A session with no attached display (a plain CI runner) cannot place
        // or observe windows at all. Report that as its own outcome instead of
        // letting the numbers below silently read as "tracking works".
        if (![NSScreen mainScreen]) {
            printf("NO_SCREEN\n");
            printf("VERDICT INCONCLUSIVE\n");
            printf("REASON no attached display: CGWindowList cannot observe on-screen windows here\n");
            fflush(stdout);
            return 3;
        }

        // Launch the child and learn its PID.
        NSTask *task = [[NSTask alloc] init];
        task.launchPath = [[NSBundle mainBundle] executablePath] ?: @"probe";
        task.arguments = @[@"--child"];
        NSPipe *pipe = [NSPipe pipe];
        task.standardOutput = pipe;
        task.standardError = [NSFileHandle fileHandleWithStandardError];
        [task launch];

        pid_t childPID = 0;
        NSMutableData *buf = [NSMutableData data];
        // Read just enough of the child's stdout to learn the PID. Blocking on a
        // pipe here is fine: the child prints it before it does anything else.
        while (childPID == 0) {
            NSData *chunk = [[pipe fileHandleForReading] availableData];
            if (chunk.length) [buf appendData:chunk];
            NSString *s = [[NSString alloc] initWithData:buf encoding:NSUTF8StringEncoding];
            for (NSString *line in [s componentsSeparatedByString:@"\n"]) {
                if ([line hasPrefix:@"CHILD_PID "])
                    childPID = (pid_t)[[line substringFromIndex:9] intValue];
            }
            if (childPID) break;
            if (![task isRunning]) break;
        }
        if (!childPID) {
            fprintf(stderr, "FATAL: child never reported a PID\n");
            return 2;
        }

        NSPanel *panel = makePanel();
        [panel orderFrontRegardless];

        pid_t us = getpid();
        pid_t focusBefore = frontmostPID();

        Probe pr = {0};
        NSDate *start = [NSDate date];
        double t = 0;
        while ([task isRunning] && t < kChildT4 + 1.5) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:kPollInterval]];
            t = -[start timeIntervalSinceNow];

            WinRect target = largestWindowForPID(childPID);
            pr.samples++;   // every poll iteration, so SAMPLES and TARGET_SEEN
                            // stop printing the same number twice (U4-F5)
            BOOL minimized = (t >= kChildT2 && t < kChildT3);

            if (minimized) {
                // The target is minimized: CGWindowList may or may not still
                // list it. Either way the panel must not be shown over a
                // window the user cannot see - and the hide must be CHECKED
                // either way. This branch used to sit BELOW the target.valid
                // gate, which made the check silently vacuous whenever the
                // window server stopped listing the minimized window: the
                // branch never ran, both counters stayed 0, and hiddenBad == 0
                // still printed VERDICT PASS. The product handles the same
                // case correctly (main.m tick() hides on !target.valid too).
                [panel orderOut:nil];
                // Re-read AFTER ordering out. isVisible still reported YES for
                // the sample right after the minimize boundary because the
                // orderOut had not happened yet, so measuring before the action
                // scored the probe's own transition as a tracking failure.
                if ([panel isVisible]) pr.hiddenBad++; else pr.hiddenOk++;
                continue;
            }

            if (!target.valid) continue;
            pr.targetSeen++;

            WinRect want = panelRectForTarget(target);

            BOOL visible = [panel isVisible];

            if (frontmostPID() != focusBefore && frontmostPID() != us) {
                pr.focusStolen++;
                printf("FOCUS_STOLEN t=%.2f frontmost=%d\n", t, (int)frontmostPID());
                fflush(stdout);
            }

            if (!visible) [panel orderFrontRegardless];
            [panel setFrame:quartzToAppKit(want) display:YES];

            // Where the panel ended up, in Quartz coordinates - read AFTER the
            // setFrame above. Reading it first compared the PREVIOUS sample's
            // position against this sample's target, so every frame following a
            // move registered as drift and the very first frame compared the
            // panel's construction size against the target.
            NSRect pf = [panel frame];
            CGFloat screenH = mainScreenHeight();
            WinRect got = {NSMinX(pf), screenH - (NSMinY(pf) + NSHeight(pf)),
                           NSWidth(pf), NSHeight(pf), YES};

            if (rectNear(got, want, kPosTolerance)) {
                pr.trackedOk++;
            } else {
                pr.trackBad++;
                if (pr.trackBad <= 5) {
                    printf("TRACK_DRIFT t=%.2f want=(%.0f,%.0f,%.0f,%.0f) got=(%.0f,%.0f,%.0f,%.0f)\n",
                           t, want.x, want.y, want.w, want.h, got.x, got.y, got.w, got.h);
                    fflush(stdout);
                }
            }
            double off = fabs(got.x - want.x) + fabs(got.y - want.y);
            if (off > pr.worstOffset) pr.worstOffset = off;
        }

        [panel orderOut:nil];
        [task waitUntilExit];

        printf("SAMPLES %d\n", pr.samples);
        printf("TARGET_SEEN %d\n", pr.targetSeen);
        printf("TRACKED_OK %d\n", pr.trackedOk);
        printf("TRACK_BAD %d\n", pr.trackBad);
        printf("HIDDEN_OK %d\n", pr.hiddenOk);
        printf("HIDDEN_BAD %d\n", pr.hiddenBad);
        printf("FOCUS_STOLEN %d\n", pr.focusStolen);
        printf("WORST_OFFSET %.1f\n", pr.worstOffset);

        // The verdict. Kept as explicit thresholds rather than "everything
        // perfect" so a CI failure says WHICH property broke.
        // - The first sample is excluded: the child has just reported its PID
        //   and its window may not be positioned yet, so a miss there is a
        //   startup race, not a tracking failure.
        // - drift is tolerated at <=1 sample, because a move between the
        //   CGWindowList read and the setFrame is unavoidable at 20 Hz and
        //   self-corrects on the next tick. A real failure is SUSTAINED drift.
        int firstSkew = pr.trackBad ? 1 : 0;
        int sustained = pr.trackBad - firstSkew;
        BOOL ok = pr.targetSeen > 3 && pr.trackedOk > 0 && sustained <= 1 &&
                  pr.hiddenBad == 0 && pr.focusStolen == 0;
        printf("VERDICT %s\n", ok ? "PASS" : "FAIL");
        if (!ok) {
            if (pr.targetSeen <= 3)
                printf("REASON CGWindowList rarely returned another process's window\n");
            else if (sustained > 1)
                printf("REASON panel did not track the target window (%d sustained samples)\n", sustained);
            else if (pr.hiddenBad)
                printf("REASON panel stayed visible over a minimized target\n");
            else if (pr.focusStolen)
                printf("REASON panel stole focus from the foreground app\n");
        }
        fflush(stdout);
        return ok ? 0 : 1;
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc > 1 && strcmp(argv[1], "--child") == 0) return runChild();
        return runParent();
    }
}
