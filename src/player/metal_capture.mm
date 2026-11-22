// Programmatic Metal frame capture, so a run can produce a .gputrace without
// attaching Xcode by hand: MTLCaptureManager starts a device-wide capture and the
// trace is written to the path given by MTL_CAPTURE.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "player/metal_capture.h"

#include <cstdio>

bool phxplayer_start_metal_capture(const char *path)
{
  if (@available(macOS 10.15, *))
  {
    MTLCaptureManager *manager = [MTLCaptureManager sharedCaptureManager];
    if ([manager isCapturing])
    {
      std::fprintf(stderr, "[capture] a capture is already running\n");
      return false;
    }

    NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
    if (devices.count == 0)
    {
      std::fprintf(stderr, "[capture] no Metal device to capture\n");
      return false;
    }

    MTLCaptureDescriptor *descriptor = [MTLCaptureDescriptor new];
    descriptor.captureObject = devices[0];
    descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
    descriptor.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];

    NSError *error = nil;
    if (![manager startCaptureWithDescriptor:descriptor error:&error])
    {
      std::fprintf(stderr, "[capture] start failed: %s\n",
                   error ? error.localizedDescription.UTF8String : "unknown error");
      return false;
    }
    std::fprintf(stderr, "[capture] writing %s\n", path);
    return true;
  }
  return false;
}

void phxplayer_stop_metal_capture(void)
{
  if (@available(macOS 10.15, *))
  {
    MTLCaptureManager *manager = [MTLCaptureManager sharedCaptureManager];
    if ([manager isCapturing]) [manager stopCapture];
  }
}
