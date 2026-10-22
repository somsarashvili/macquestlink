// mkaudiodev <out|in> [create "<name>" [sub-device UID]] : aggregate audio device wrapping the sub-device (default: the
// default output, or input), so Wine lists endpoints with that name (vrlink looks for "Steam Streaming Speakers" and
// "Steam Streaming Microphone"). Over a loopback device (output fed back to its input) the aggregate has both endpoints.
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>


static AudioObjectID find_device(CFStringRef uid)
{
    AudioObjectID dev = kAudioObjectUnknown;
    AudioValueTranslation t = { &uid, sizeof(uid), &dev, sizeof(dev) };
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(t);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &t);
    return dev;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    int input = !strcmp(argv[1], "in");
    CFStringRef uid = input ? CFSTR("macquestlink.aggregate.in") : CFSTR("macquestlink.aggregate");
    argc--, argv++;
    AudioObjectID old = find_device(uid);
    if (old != kAudioObjectUnknown) {
        printf("destroy existing -> %d\n", (int)AudioHardwareDestroyAggregateDevice(old));
    }
    if (argc < 3 || strcmp(argv[1], "create")) {
        return 0;
    }
    AudioObjectID out;
    UInt32 size = sizeof(out);
    AudioObjectPropertyAddress a = { input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &out);
    CFStringRef out_uid;
    size = sizeof(out_uid);
    a.mSelector = kAudioDevicePropertyDeviceUID;
    if (argc > 3) {
        out_uid = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
    } else {
        AudioObjectGetPropertyData(out, &a, 0, NULL, &size, &out_uid);
    }

    CFStringRef name = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
    CFMutableDictionaryRef sub = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sub, CFSTR(kAudioSubDeviceUIDKey), out_uid);
    CFArrayRef subs = CFArrayCreate(NULL, (const void **)&sub, 1, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), name);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), uid);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subs);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), out_uid);
    AudioObjectID dev;
    OSStatus st = AudioHardwareCreateAggregateDevice(desc, &dev);
    char buf[256];
    CFStringGetCString(out_uid, buf, sizeof(buf), kCFStringEncodingUTF8);
    printf("create \"%s\" over %s -> %d (id %u)\n", argv[2], buf, (int)st, (unsigned)dev);
    if (st) {
        return 1;
    }
    Float64 rate = 48000;
    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    printf("48 kHz -> %d\n", (int)AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(rate), &rate));
    return 0;
}
