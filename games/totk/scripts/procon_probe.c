/* The Switch Pro Controller's IMU over IOKit HID, as a second non-seizing client (2026-10-19).
   Why: macOS's GameController framework reports GCMotion for the Pro Controller but never streams it
   (measured on macOS 26.6.2: 0 samples a second, acceleration (0,0,0) G while the game reads states).
   The IMU data is in the controller's own HID input reports (0x30); this probe checks a second client
   can read them and send subcommands while the system gamepad driver keeps the buttons working.
   Protocol per SDL_hidapi_switch.c (libsdl-org/SDL, Valve's driver): output report 0x01 carries a
   packet counter, neutral rumble, a subcommand; EnableIMU (0x40) and SetInputReportMode 0x30 (0x03)
   turn on the IMU; SPIFlashRead (0x10) reads the factory IMU calibration at 0x6020 (and the user one
   at 0x8026, magic B2 A1); replies come as input report 0x21. Report 0x30: IMU at offset 12, three
   samples of accel XYZ then gyro XYZ, int16 LE, oldest first (imuState[2] is the oldest, per SDL).
   Scales: 4 G / (coeff - raw) and 936 deg/s / (coeff - raw); gyro zero-rate = raw. Axis mapping to
   the GCMotion frame (x right, y top, z out of the face) is SDL's: x = -Y, y = +Z, z = -X.
   Run: make procon-probe && build/procon-probe. Hold the controller flat, buttons up: accel z should
   read -1 G (gravity down through the face); tip the right side down: x goes positive. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static IOHIDManagerRef manager;
static IOHIDDeviceRef dev;
static int devIsUSB, packetNumber, seen30, seen3f, otherReports;
static int16_t aRaw[3], aCoeff[3], gRaw[3], gCoeff[3];   /* factory, then user-overridden raws */
static double aScale[3], gScale[3];                      /* per raw axis: to G, to rad/s */
static int haveCal;
static long printed;
static uint8_t inBuf[64], outBuf[64];
static int replySub, replyAck, spiWant; static uint8_t spiGot[32];

static void spin(double seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
}

static void sendSub(uint8_t sub, const uint8_t *args, int len) {
    memset(outBuf, 0, sizeof outBuf);
    outBuf[0] = packetNumber++ & 0xF;
    outBuf[1] = 0x00; outBuf[2] = 0x01; outBuf[3] = 0x40; outBuf[4] = 0x40;   /* neutral rumble, left */
    outBuf[5] = 0x00; outBuf[6] = 0x01; outBuf[7] = 0x40; outBuf[8] = 0x40;   /* neutral rumble, right */
    outBuf[9] = sub;
    if (len) memcpy(outBuf + 10, args, len);
    IOReturn r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x01, outBuf, devIsUSB ? 63 : 48);
    printf("sub %02x -> IOReturn 0x%08x\n", sub, (unsigned)r);
}

static void spiRead(uint32_t addr, uint8_t len) {
    uint8_t args[5] = { addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >> 24) & 0xff, len };
    spiWant = addr;
    sendSub(0x10, args, 5);
}

static int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

static void onReport(void *ctx, IOReturn res, void *sender, IOHIDReportType type, uint32_t reportID, uint8_t *report, CFIndex len) {
    (void)ctx; (void)res; (void)sender; (void)type;
    static int dbgN;
    if (getenv("PROCON_DEBUG_ALL") && dbgN++ < 20) printf("  report id %02x len %ld\n", reportID, (long)len);
    /* Apple's callback hands us the raw report with the report ID as byte 0 (0x30/0x21: len 49). */
    const uint8_t *r = report + (len > 1 && report[0] == (uint8_t)reportID ? 1 : 0);
    len -= (int)(r - report);
    if (reportID == 0x21 && len >= 19) {           /* subcommand reply */
        static int dumpN;
        if (getenv("PROCON_DEBUG_ALL") && dumpN++ < 5) {
            printf("  0x21:");
            for (int i = 0; i < len; i++) printf(" %02x", r[i]);
            printf("\n");
        }
        replySub = r[13]; replyAck = r[12];
        /* this firmware echoes the report mode, not the subcommand id; match SPI replies by their address+len echo */
        if (spiWant >= 0 && r[18] > 0 && r[18] <= 32 && len >= 19 + r[18] &&
            (int)(r[14] | (r[15] << 8) | (r[16] << 16) | (r[17] << 24)) == spiWant) {
            memcpy(spiGot, r + 19, r[18]); spiWant = -1;
        }
    } else if (reportID == 0x30 && len >= 48) {
        seen30++;
        if (!haveCal) return;
        for (int s = 2; s >= 0; s--) {             /* oldest first, like SDL */
            const uint8_t *m = r + 12 + s * 12;
            int16_t a[3] = { le16(m), le16(m + 2), le16(m + 4) }, g[3] = { le16(m + 6), le16(m + 8), le16(m + 10) };
            double acc[3] = { -a[1] * aScale[1], a[2] * aScale[2], -a[0] * aScale[0] };
            double gyr[3] = { -(g[1] - gRaw[1]) * gScale[1], (g[2] - gRaw[2]) * gScale[2], -(g[0] - gRaw[0]) * gScale[0] };
            if (++printed % 40 == 0)   /* about 5 lines a second */
                printf("acc (%+.2f %+.2f %+.2f) G   gyro (%+.3f %+.3f %+.3f) rad/s\n",
                       acc[0], acc[1], acc[2], gyr[0], gyr[1], gyr[2]);
            fflush(stdout);
        }
    } else if (reportID == 0x3f) seen3f++;
    else if (reportID != 0x21) otherReports++;
}

static CFStringRef ckey(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }   /* CFSTR() stringifies; these keys are #defines */

static int intProp(IOHIDDeviceRef d, const char *key) {
    CFStringRef k = ckey(key);
    CFNumberRef n = IOHIDDeviceGetProperty(d, k);
    CFRelease(k);
    int v = 0;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberSInt32Type, &v);
    return v;
}

static void onDeviceAdded(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef device) {
    (void)ctx; (void)res; (void)sender;
    int vid = intProp(device, kIOHIDVendorIDKey);
    int pid = intProp(device, kIOHIDProductIDKey);
    if (getenv("PROCON_DEBUG_ALL")) {
        CFStringRef prod = IOHIDDeviceGetProperty(device, ckey(kIOHIDProductKey));
        char pbuf[128] = "?";
        if (prod) CFStringGetCString(prod, pbuf, sizeof pbuf, kCFStringEncodingUTF8);
        printf("hid device: %s vid %04x pid %04x\n", pbuf, vid, pid);
    }
    if (vid != 0x057E || (pid != 0x2009 && pid != 0x200E)) return;
    devIsUSB = pid == 0x200E;
    if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess) { fprintf(stderr, "open failed\n"); return; }
    dev = device;
    IOHIDDeviceRegisterInputReportCallback(dev, inBuf, sizeof inBuf, onReport, NULL);
    printf("opened Pro Controller (VID %04x PID %04x, %s)\n", vid, pid, devIsUSB ? "USB" : "Bluetooth");
}

static int waitForReply(int sub, double seconds) {
    replySub = 0;
    for (double t = 0; t < seconds; t += 0.02) {
        spin(0.02);
        if (replySub == sub) return replyAck & 0x80;
    }
    return 0;
}

static void loadCal(void) {
    for (int tryN = 0; tryN < 8; tryN++) {   /* retry: the controller dedups output reports by packet counter, and Apple's driver is sending them too */
        spiRead(0x6020, 24);
        for (double t = 0; t < 0.5 && spiWant >= 0; t += 0.02) spin(0.02);
        if (spiWant == -1) break;
    }
    if (spiWant != -1) {
        fprintf(stderr, "factory IMU calibration read failed; using default scales (SDL fallback)\n");
        for (int i = 0; i < 3; i++) { aScale[i] = 1.0 / 4096.0; gScale[i] = M_PI / 180.0 / 14.2842; }
        haveCal = 1;
        return;
    }
    for (int i = 0; i < 3; i++) {
        aRaw[i] = le16(spiGot + i * 2); aCoeff[i] = le16(spiGot + 6 + i * 2);
        gRaw[i] = le16(spiGot + 12 + i * 2); gCoeff[i] = le16(spiGot + 18 + i * 2);
    }
    spiRead(0x8026, 20);
    for (double t = 0; t < 1.0 && spiWant >= 0; t += 0.02) spin(0.02);
    if (spiWant == -1 && le16(spiGot) == (int16_t)0xA1B2) {   /* user calibration wins */
        for (int i = 0; i < 3; i++) { aRaw[i] = le16(spiGot + 2 + i * 2); gRaw[i] = le16(spiGot + 14 + i * 2); }
        printf("using user IMU calibration\n");
    }
    for (int i = 0; i < 3; i++) {
        aScale[i] = (aCoeff[i] - aRaw[i]) ? 4.0 / (aCoeff[i] - aRaw[i]) : 1.0 / 4096.0;
        gScale[i] = (gCoeff[i] - gRaw[i]) ? 936.0 / (gCoeff[i] - gRaw[i]) * M_PI / 180.0 : M_PI / 180.0 / 14.2842;
    }
    printf("cal: accel raw (%d %d %d) coeff (%d %d %d), gyro zero (%d %d %d) coeff (%d %d %d)\n",
           aRaw[0], aRaw[1], aRaw[2], aCoeff[0], aCoeff[1], aCoeff[2], gRaw[0], gRaw[1], gRaw[2], gCoeff[0], gCoeff[1], gCoeff[2]);
    haveCal = 1;
}

int main(void) {
    setbuf(stdout, NULL);
    manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef match = NULL;
    if (!getenv("PROCON_DEBUG_ALL")) {
        match = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &(int){ 0x057E });
        CFStringRef vk = ckey(kIOHIDVendorIDKey);
        CFDictionarySetValue(match, vk, v);
        CFRelease(vk);
    }
    IOHIDManagerSetDeviceMatching(manager, match);
    IOHIDManagerRegisterDeviceMatchingCallback(manager, onDeviceAdded, NULL);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) { fprintf(stderr, "manager open failed\n"); return 1; }
    for (int t = 0; t < 50 && !dev; t++) spin(0.05);
    if (!dev) { fprintf(stderr, "no Pro Controller found (connect it and retry)\n"); return 1; }

    spin(2.0);   /* what is the system driver asking for? */
    printf("before touching anything: reports 0x30 %d, 0x3f %d, other %d\n", seen30, seen3f, otherReports);

    sendSub(0x40, (uint8_t[]){ 1 }, 1);                 /* EnableIMU */
    printf("EnableIMU ack: %d\n", waitForReply(0x40, 1.0));
    uint8_t mode = 0x30;
    sendSub(0x03, &mode, 1);                            /* SetInputReportMode: full, with IMU */
    printf("SetInputReportMode ack: %d\n", waitForReply(0x03, 1.0));
    loadCal();

    printf("streaming; hold flat buttons up (expect z about -1 G), then tip it. Ctrl-C stops.\n");
    for (;;) { spin(0.05); }
}
