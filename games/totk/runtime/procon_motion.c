/* The gyro of the Nintendo Pro Controller (2026-10-19; started by runtime/gamepad_feedback.m, only with TOTK_GYRO=1).
   macOS gives that controller GCMotion sensors that report nothing (its log shows acceleration +0.00 +0.00 +0.00),
   unlike the DualShock and DualSense, so ask the controller itself instead. A second, non-seizing HID client (the
   system's gamepad driver keeps delivering buttons as usual) enables its IMU and selects input report mode 0x30, the
   full mode: reports then stream at 200 Hz, each carrying three six-axis samples 5 ms apart, the oldest last. The
   controller's SPI calibration does not answer through this second client (the subcommand replies come back with an
   empty payload, checked 2026-10-19 on macOS 26.6.2), so use the standard default scales and track the gyro's
   zero-rate bias ourselves: while the controller is still (gravity within 4 % of 1 G, rotation under 0.3 rad/s) pull
   the bias toward the reading with a 2 s time constant, as the Switch's own drift correction does. The raw axes (measured: x to
   the top edge, y to the left, z out of the face) map to the GCMotion frame the fusion expects (x right, y
   top edge, z out of the face), the way SDL maps them. Holding L3+R3 for half a second recenters the pose. */
#include "runtime.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PROCON_VENDOR = 0x057E, PROCON_BT = 0x2009, PROCON_USB = 0x200E };
/* The scales used when the SPI calibration cannot be read: 1/4096 G per LSB, 14.2842 LSB per degree per second. */
#define ACCEL_SCALE (1.0 / 4096.0)
#define GYRO_SCALE (M_PI / 180.0 / 14.2842)

static IOHIDManagerRef manager;
static IOHIDDeviceRef device;
static uint8_t report_buffer[64];
static double gyro_bias[3];
static unsigned out_counter;

static int16_t rd16(const uint8_t *p) { return (int16_t)((unsigned)p[0] | ((unsigned)p[1] << 8)); }

/* A subcommand over output report 0x01: packet counter, neutral rumble, the subcommand and its arguments. */
static void send_subcommand(IOHIDDeviceRef d, uint8_t sub, uint8_t arg) {
    uint8_t payload[48] = {0};
    payload[0] = (uint8_t)(out_counter++ & 0x0F);
    payload[1] = 0x00; payload[2] = 0x01; payload[3] = 0x40; payload[4] = 0x40;
    payload[5] = 0x00; payload[6] = 0x01; payload[7] = 0x40; payload[8] = 0x40;
    payload[9] = sub; payload[10] = arg;
    IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, 0x01, payload, sizeof payload);
}

static uint64_t prev_report_ns;   /* reports arrive ~24/s over BT, not the nominal 66/s: measure, don't assume */
static uint64_t sticks_down_ns;   /* how long both thumbsticks have been pressed */
static int recenter_armed = 1;

static void on_report(void *context, IOReturn result, void *sender, IOHIDReportType type, uint32_t reportID, uint8_t *r, CFIndex len) {
    (void)context; (void)sender; (void)type;
    if (result != kIOReturnSuccess || reportID != 0x30 || len < 1 + 12 + 36) return;
    r++;   /* the callback prefixes the report ID */
    uint64_t now = runtime_now_ns();
    uint64_t dt_ns = now - prev_report_ns;
    if (dt_ns < 5000000ull || dt_ns > 100000000ull) dt_ns = 15000000ull;   /* first report or a gap: assume nominal */
    prev_report_ns = now;
    double dt = (double)dt_ns / 1e9 / 3.0;
    /* Hold both thumbsticks (L3+R3) for half a second to recenter: the same restart the game itself does once at the
       title screen (StartSixAxisSensor -> identity), for when the pad's zero has drifted during a long session. */
    if ((r[3] & 0x0C) == 0x0C) {           /* report 0x30 shared button byte: [3] bit 3 = L3, bit 2 = R3 */
        sticks_down_ns += dt_ns;
        if (recenter_armed && sticks_down_ns >= 500000000ull) {
            runtime_motion_start();
            fprintf(stderr, "[gyro] recentered to the current pose (L3+R3)\n");
            runtime_toast("Gyro re-centered to the current pose");
            recenter_armed = 0;
        }
    } else {
        sticks_down_ns = 0;
        recenter_armed = 1;
    }
    for (int s = 2; s >= 0; s--) {   /* three samples per report, spread across the measured report interval; 2 is oldest */
        const uint8_t *imu = r + 12 + s * 12;
        double raw_a[3] = {rd16(imu), rd16(imu + 2), rd16(imu + 4)};
        double raw_g[3] = {rd16(imu + 6), rd16(imu + 8), rd16(imu + 10)};
        /* This pad's real axes (measured, see procon-view): raw X = top edge, Y = left, Z = out of the face.
           Feed the game GCMotion's frame: x = right, y = top edge, z = out of the face. */
        /* GCMotion reports the gravity vector (face-up on a desk reads z = -1 G, per the
         * author's own DualSense log), the opposite sign of an IMU's raw proper-force
         * reading; the game is written against GCMotion, so negate. Gyro keeps its sign. */
        double acc[3] = {raw_a[1] * ACCEL_SCALE, -raw_a[0] * ACCEL_SCALE, -raw_a[2] * ACCEL_SCALE};
        double gyro[3] = {-raw_g[1] * GYRO_SCALE, raw_g[0] * GYRO_SCALE, raw_g[2] * GYRO_SCALE};
        double am = sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
        double gm = sqrt(gyro[0] * gyro[0] + gyro[1] * gyro[1] + gyro[2] * gyro[2]);
        if (fabs(am - 1) < 0.04 && gm < 0.3)
            for (int i = 0; i < 3; i++) gyro_bias[i] += (gyro[i] - gyro_bias[i]) * dt / 2.0;   /* 2 s time constant */
        for (int i = 0; i < 3; i++) gyro[i] -= gyro_bias[i];
        runtime_motion_feed(acc, gyro, now - (uint64_t)(2 - s) * (dt_ns / 3));
    }
}

/* Open the controller (if there is one), ask it for its IMU reports. Runs on either thread; a fresh open is what a
   reconnect needs. */
static int open_current(void) {
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (!devices) return 0;
    IOHIDDeviceRef candidates[8], found = NULL;
    CFIndex count = CFSetGetCount(devices);
    if (count > 8) count = 8;
    CFSetGetValues(devices, (const void **)candidates);
    CFRelease(devices);
    for (CFIndex i = 0; i < count && !found; i++) {
        int32_t pid = 0;
        CFNumberRef n = IOHIDDeviceGetProperty(candidates[i], CFSTR("ProductID"));
        if (n) CFNumberGetValue(n, kCFNumberSInt32Type, &pid);
        if (pid == PROCON_BT || pid == PROCON_USB) found = candidates[i];
    }
    if (!found) return 0;
    if (found != device && IOHIDDeviceOpen(found, kIOHIDOptionsTypeNone) != kIOReturnSuccess) return 0;   /* already open: just re-ask */
    IOHIDDeviceRegisterInputReportCallback(found, report_buffer, sizeof report_buffer, on_report, NULL);
    send_subcommand(found, 0x40, 1);      /* enable the IMU */
    send_subcommand(found, 0x03, 0x30);   /* input report mode 0x30: the full reports, with the IMU */
    device = found;
    return 1;
}

static void device_added(void *context, IOReturn result, void *sender, IOHIDDeviceRef d) {
    (void)context; (void)result; (void)sender; (void)d;
    open_current();
}
static void device_removed(void *context, IOReturn result, void *sender, IOHIDDeviceRef d) {
    (void)context; (void)result; (void)sender;
    if (d == device) {
        device = NULL;
        fprintf(stderr, "[gyro] Pro Controller disconnected: motion stops until it reconnects\n");
    }
}

static void *run_loop(void *unused) {
    (void)unused;
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFRunLoopRun();
    return NULL;
}

int runtime_procon_motion_start(void) {
    if (manager) return open_current();
    manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) return 0;
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SInt32 vendor = PROCON_VENDOR;
    CFNumberRef vn = CFNumberCreate(NULL, kCFNumberSInt32Type, &vendor);
    CFDictionarySetValue(match, CFSTR("VendorID"), vn);
    CFRelease(vn);
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match);
    IOHIDManagerRegisterDeviceMatchingCallback(manager, device_added, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(manager, device_removed, NULL);
    if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess || !open_current()) {
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        CFRelease(manager);
        manager = NULL;
        return 0;
    }
    pthread_t thread;
    pthread_create(&thread, NULL, run_loop, NULL);
    pthread_detach(thread);
    return 1;
}
